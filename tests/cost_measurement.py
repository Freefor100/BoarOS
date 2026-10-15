"""One global lock for standalone boots and complete matched campaigns."""
import errno
import fcntl
import os
from pathlib import Path
import stat


def acquire_measurement_lock(path, inherited_fd=None):
    """Return a held lease; closing a child duplicate retains the parent's lease.

    A campaign keeps its file description open for the entire experiment and
    passes it with ``pass_fds``. An independent probe must be excluded, then the
    inherited description itself must already be able to hold the same lock.
    Validation never unlocks or closes the caller's descriptor.
    """
    path = Path(path)
    if inherited_fd is None:
        lease = path.open('a')
        try:
            fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return lease
        except BaseException:
            lease.close()
            raise
    if type(inherited_fd) is not int or inherited_fd < 0:
        raise ValueError('invalid inherited measurement lock descriptor')
    try:
        duplicate = os.dup(inherited_fd)
    except OSError as error:
        raise ValueError('invalid inherited measurement lock descriptor') from error
    lease = os.fdopen(duplicate, 'a')
    try:
        identity = os.fstat(lease.fileno())
        if not stat.S_ISREG(identity.st_mode):
            raise ValueError('inherited measurement lock is not a regular file')
        with path.open('a') as probe:
            expected = os.fstat(probe.fileno())
            if ((identity.st_dev, identity.st_ino) !=
                    (expected.st_dev, expected.st_ino)):
                raise ValueError('inherited measurement lock file differs')
            try:
                fcntl.flock(probe, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError as error:
                if error.errno not in (errno.EACCES, errno.EAGAIN):
                    raise
            else:
                raise ValueError('inherited measurement lock is not held')
        try:
            fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise ValueError('measurement lock belongs to another description') from error
        return lease
    except BaseException:
        lease.close()
        raise

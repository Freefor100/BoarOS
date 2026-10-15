"""Optional, paired QEMU guest PRNG inputs for independent cost boots."""


def replica_seed(base, replica):
    if type(replica) is not int or replica < 0:
        raise ValueError('invalid seed replica')
    if base is None:
        return None
    if type(base) is not int or not 0 <= base <= (1 << 64) - 1 - replica:
        raise ValueError('QEMU seed and replica span must fit uint64')
    return base + replica


def seed_arguments(seed):
    seed = replica_seed(seed, 0)
    return [] if seed is None else ['-seed', str(seed)]


def validate_resume_seed(record, requested):
    requested = replica_seed(requested, 0)
    actual = record.get('qemu_seed')
    if actual is not None:
        actual = replica_seed(actual, 0)
    if actual != requested:
        raise ValueError('resume QEMU seed differs')
    argv = record.get('argv', [])
    positions = [i for i, arg in enumerate(argv) if arg == '-seed']
    if requested is None:
        if positions:
            raise ValueError('unexpected resume QEMU seed argument')
        return
    if len(positions) != 1 or positions[0] + 1 >= len(argv):
        raise ValueError('missing or duplicate resume QEMU seed argument')
    try:
        argument = int(argv[positions[0] + 1], 0)
    except (TypeError, ValueError) as error:
        raise ValueError('invalid resume QEMU seed argument') from error
    if argument != requested:
        raise ValueError('resume QEMU seed argument differs')

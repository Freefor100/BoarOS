"""Keep the real measurement lease until the restoring helper has exited.

The guardian is unprivileged. sudo may close inherited descriptors, so the
guardian retains the flock rather than asking the root helper to inherit it.
The helper consumes the controller pipe directly: controller death becomes EOF
even while this guardian remains alive waiting for verified helper termination.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys

from cost_measurement import acquire_measurement_lock

ROOT = Path(__file__).resolve().parents[1]


def serve_guardian(lease, *, command=None, pass_fds=()):
    """Own and close lease only after the one helper process exits.

    command/pass_fds are a host-test API. The production CLI accepts only the
    parent's verified measurement fd and executes the fixed policy helper.
    """
    previous = {s: signal.signal(s, signal.SIG_IGN) for s in (signal.SIGINT, signal.SIGTERM)}
    process = None
    code = 2
    try:
        invocation = command or ['sudo', '-n', sys.executable, '-I',
                                 str(ROOT / 'tests/cost_policy.py'), '--lease']
        # No extra stdin writer: EOF reaches the helper after controller death.
        process = subprocess.Popen(invocation, stdin=sys.stdin, pass_fds=pass_fds)
        code = process.wait()
    except OSError as error:
        try:
            print(json.dumps({'event': 'blocked', 'error': str(error), 'restoration': None}), flush=True)
        except OSError:
            pass
    finally:
        if process is not None and process.poll() is None:
            process.wait()  # Do not abandon a live owner on a controller timeout.
        lease.close()
        for signum, handler in previous.items():
            signal.signal(signum, handler)
    try:
        print(json.dumps({'event': 'helper-exit', 'exit_status': code}), flush=True)
    except OSError:
        pass
    return code


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--measurement-lock-fd', type=int, required=True)
    args = parser.parse_args()
    if os.geteuid() == 0:
        parser.error('guardian must remain unprivileged')
    lease = acquire_measurement_lock(ROOT / 'build/cost/measurement.lock', args.measurement_lock_fd)
    os.close(args.measurement_lock_fd)
    outside = os.sched_getaffinity(0) - {2, 3}
    if outside:
        os.sched_setaffinity(0, outside)
    return serve_guardian(lease)


if __name__ == '__main__':
    sys.exit(main())

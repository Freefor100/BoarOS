"""Capture a guest and inject input only after its real console wait marker."""
import subprocess
import threading
import time


def run_guest(command, timeout):
    process = subprocess.Popen(command, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    output = bytearray()
    markers = [(b'LA console poll ready', b'g\n'),
               (b'LA console read ready', b'r\n')]

    def reader():
        pending = list(markers)
        while True:
            chunk = process.stdout.read(1)
            if not chunk:
                break
            output.extend(chunk)
            if pending and pending[0][0] in output:
                _, data = pending.pop(0)
                # Let read/ppoll actually block before providing input.
                time.sleep(0.2)
                try:
                    process.stdin.write(data)
                    process.stdin.flush()
                except (BrokenPipeError, OSError):
                    break

    thread = threading.Thread(target=reader)
    thread.start()
    try:
        code = process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
        thread.join()
        raise subprocess.TimeoutExpired(command, timeout,
                                        output.decode(errors='replace'))
    finally:
        thread.join()
        process.stdin.close()
        process.stdout.close()
    return code, output.decode(errors='replace')

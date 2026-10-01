"""Exercise fixed-newstyle/simple NBD and the stable/volatile fault contract."""

import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

INIT_MAGIC = 0x4E42444D41474943
OPTS_MAGIC = 0x49484156454F5054
REP_MAGIC = 0x0003E889045565A9


def exact(sock, count):
    data = bytearray()
    while len(data) < count:
        piece = sock.recv(count - len(data))
        assert piece, "NBD connection closed"
        data.extend(piece)
    return bytes(data)


class Session:
    def __init__(self, binary, image, socket_path, *options):
        self.process = subprocess.Popen(
            [binary, image, socket_path, *options], stderr=subprocess.PIPE, stdin=subprocess.PIPE
        )
        for _ in range(100):
            try:
                self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.socket.connect(socket_path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                self.socket.close()
                time.sleep(0.01)
        else:
            raise AssertionError("NBD socket not listening")
        magic, opts, flags = struct.unpack(">QQH", exact(self.socket, 18))
        assert (magic, opts, flags) == (INIT_MAGIC, OPTS_MAGIC, 3)
        self.socket.sendall(struct.pack(">I", 3))
        for option in (11, 8):
            self.socket.sendall(struct.pack(">QII", OPTS_MAGIC, option, 0))
            assert self.option_reply(option) == (0x80000001, b"")
        self.socket.sendall(struct.pack(">QII", OPTS_MAGIC, 7, 8) +
                            struct.pack(">IHH", 0, 1, 3))
        answers = [self.option_reply(7), self.option_reply(7),
                   self.option_reply(7)]
        assert answers[0] == (3, struct.pack(">HQH", 0, 8192, 5))
        assert answers[1] == (3, struct.pack(">HIII", 3, 512, 4096,
                                              1024 * 1024))
        assert answers[2] == (1, b"")
        self.cookie = 0

    def option_reply(self, option):
        magic, actual, kind, length = struct.unpack(">QIII", exact(self.socket, 20))
        assert magic == REP_MAGIC and actual == option
        return kind, exact(self.socket, length)

    def command(self, kind, offset=0, data=b"", length=None):
        self.cookie += 1
        if length is None:
            length = len(data)
        self.socket.sendall(struct.pack(">IHHQQI", 0x25609513, 0, kind,
                                         self.cookie, offset, length) +
                            (data if kind == 1 else b""))
        if kind == 2:
            return None
        magic, error, cookie = struct.unpack(">IIQ", exact(self.socket, 16))
        assert magic == 0x67446698 and cookie == self.cookie
        payload = exact(self.socket, length) if kind == 0 and not error else b""
        return error, payload

    def close(self, cut=False):
        if not cut:
            self.command(2)
        self.socket.close()
        # Keep the control pipe open until NBD disconnect has been consumed.
        self.process.wait(timeout=5)
        stderr = self.process.communicate(timeout=5)[1].decode()
        assert self.process.returncode == 0, stderr
        return stderr


def main(binary):
    with tempfile.TemporaryDirectory(prefix="boaros-nbd-") as directory:
        image = str(Path(directory) / "disk.img")
        address = str(Path(directory) / "disk.sock")
        Path(image).write_bytes(b"\0" * 8192)
        gated = Session(binary, image, address, "--control-stdin")
        gated.process.stdin.write(b"hold\n")
        gated.process.stdin.flush()
        assert gated.process.stderr.readline() == b"control=hold\n"
        for cookie, offset in ((1, 0), (2, 512)):
            gated.socket.sendall(struct.pack(">IHHQQI", 0x25609513, 0, 0, cookie, offset, 512))
            line = gated.process.stderr.readline()
            assert line.startswith(f"held={cookie} command=0 offset={offset} ".encode()), line
        gated.process.stdin.write(b"release 2\n")
        gated.process.stdin.flush()
        assert struct.unpack(">IIQ", exact(gated.socket, 16)) == (0x67446698, 0, 2)
        assert exact(gated.socket, 512) == b"\0" * 512
        assert gated.process.stderr.readline() == b"released=2\n"
        assert gated.process.stderr.readline() == b"control=release 2\n"
        gated.process.stdin.write(b"drain\n")
        gated.process.stdin.flush()
        assert struct.unpack(">IIQ", exact(gated.socket, 16)) == (0x67446698, 0, 1)
        assert exact(gated.socket, 512) == b"\0" * 512
        assert gated.process.stderr.readline() == b"released=1\n"
        assert gated.process.stderr.readline() == b"control=drain\n"
        gated.close()
        first, second = b"A" * 512, b"B" * 512
        session = Session(binary, image, address)
        assert session.command(1, data=first) == (0, b"")
        assert session.command(0, length=512) == (0, first)
        assert Path(image).read_bytes()[:512] == b"\0" * 512
        assert session.command(3) == (0, b"")
        assert Path(image).read_bytes()[:512] == first
        assert session.command(1, data=second) == (0, b"")
        session.close()
        assert Path(image).read_bytes()[:512] == first

        session = Session(binary, image, address, "--fail-write=1")
        assert session.command(1, data=second)[0] == 5
        assert session.command(0, length=512) == (0, first)
        session.close()

        session = Session(binary, image, address, "--fail-flush=1")
        assert session.command(1, data=second) == (0, b"")
        assert session.command(3)[0] == 5
        session.close()
        assert Path(image).read_bytes()[:512] == first

        for policy, expected in (("none", first), ("all", second),
                                 ("reverse", first), ("odd", second),
                                 ("even", first)):
            session = Session(binary, image, address,
                              "--cut-after=2", "--persist=" + policy)
            assert session.command(1, data=first) == (0, b"")
            session.socket.sendall(struct.pack(">IHHQQI", 0x25609513, 0, 1,
                                               2, 0, 512) + second)
            log = session.close(cut=True)
            assert "cut=2 policy=" + policy in log
            assert Path(image).read_bytes()[:512] == expected
            Path(image).write_bytes(b"\0" * 8192)
            first = b"A" * 512
        session = Session(binary, image, address, "--arm-on-signal",
                          "--control-stdin", "--cut-after=1", "--persist=none")
        assert session.command(1, data=first) == (0, b"")
        assert session.process.stderr.readline().startswith(b"event=0 type=WRITE")
        # Arming must acknowledge the reset while the NBD stream is idle;
        # releasing the guest before ACK can omit its first write.
        session.process.stdin.write(b"arm\n")
        session.process.stdin.flush()
        assert session.process.stderr.readline() == b"armed=1\n"
        assert session.process.stderr.readline() == b"control=arm\n"
        session.socket.sendall(struct.pack(">IHHQQI", 0x25609513, 0, 1,
                                           2, 0, 512) + second)
        log = session.close(cut=True)
        assert "event=1 type=WRITE" in log and "cut=1 policy=none" in log
        assert Path(image).read_bytes()[:512] == b"\0" * 512
        Path(image).write_bytes(bytes(8192))
        session = Session(binary, image, address, "--control-stdin")
        assert session.command(1, 0, b"A" * 512)[0] == 0
        assert session.command(3)[0] == 0
        assert session.command(1, 0, b"B" * 512)[0] == 0
        session.process.stdin.write(b"cut\n")
        session.process.stdin.flush()
        log = session.close(cut=True)
        assert "cause=control" in log and Path(image).read_bytes()[:512] == b"A" * 512
        print("NBD protocol, flush, errors and cut policies passed")


if __name__ == "__main__":
    main(sys.argv[1])

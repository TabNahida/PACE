#!/usr/bin/env python3
"""Independent RFC 6455 peers using only Python's standard library; no timing claims."""
import argparse
import base64
import hashlib
import os
from pathlib import Path
import socket
import struct
import subprocess
import time


GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class Peer:
    def __init__(self, connection):
        self.socket = connection
        self.pending = b""
        connection.settimeout(5)

    def take(self, size):
        while len(self.pending) < size:
            received = self.socket.recv(65536)
            if not received:
                raise AssertionError("unexpected WebSocket EOF")
            self.pending += received
        value, self.pending = self.pending[:size], self.pending[size:]
        return value

    def head(self):
        while b"\r\n\r\n" not in self.pending:
            received = self.socket.recv(4096)
            if not received:
                raise AssertionError("EOF before upgrade headers")
            self.pending += received
            assert len(self.pending) < 65536
        value, self.pending = self.pending.split(b"\r\n\r\n", 1)
        lines = value.decode("ascii").split("\r\n")
        return lines[0], {key.lower(): value.strip() for key, value in (line.split(":", 1) for line in lines[1:])}

    def frame(self, opcode, payload=b"", final=True, masked=False):
        header = bytes([(0x80 if final else 0) | opcode])
        flag = 0x80 if masked else 0
        if len(payload) < 126:
            header += bytes([flag | len(payload)])
        elif len(payload) < 65536:
            header += bytes([flag | 126]) + struct.pack("!H", len(payload))
        else:
            header += bytes([flag | 127]) + struct.pack("!Q", len(payload))
        if masked:
            key = os.urandom(4)
            header += key
            payload = bytes(value ^ key[index % 4] for index, value in enumerate(payload))
        self.socket.sendall(header + payload)

    def read_frame(self, masked):
        first, second = self.take(2)
        assert bool(second & 0x80) == masked, "incorrect mask direction"
        size = second & 127
        if size == 126:
            size = struct.unpack("!H", self.take(2))[0]
        elif size == 127:
            size = struct.unpack("!Q", self.take(8))[0]
        assert size < 2**20
        key = self.take(4) if masked else None
        payload = self.take(size)
        if key is not None:
            payload = bytes(value ^ key[index % 4] for index, value in enumerate(payload))
        return first, payload


def challenge(key):
    return base64.b64encode(hashlib.sha1((key + GUID).encode("ascii")).digest()).decode("ascii")


def client_against_pace(executable):
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    process = subprocess.Popen([executable, "serve", str(port)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        until = time.monotonic() + 5
        while True:
            try:
                connection = socket.create_connection(("127.0.0.1", port), timeout=0.2)
                break
            except OSError:
                if process.poll() is not None or time.monotonic() >= until:
                    raise AssertionError("PACE WebSocket server failed to start")
                time.sleep(0.01)
        with connection:
            peer = Peer(connection)
            key = base64.b64encode(os.urandom(16)).decode("ascii")
            connection.sendall(("GET /echo HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
                                "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                                "Sec-WebSocket-Protocol: echo\r\nSec-WebSocket-Key: " + key + "\r\n\r\n").encode("ascii"))
            status, headers = peer.head()
            assert status.startswith("HTTP/1.1 101 ")
            assert headers["sec-websocket-accept"] == challenge(key)
            assert headers["sec-websocket-protocol"] == "echo"
            peer.frame(1, b"\xe2", final=False, masked=True)
            peer.frame(9, b"independent ping", masked=True)
            peer.frame(0, b"\x82\xac", masked=True)
            assert peer.read_frame(False) == (0x8A, b"independent ping")
            assert peer.read_frame(False) == (0x81, "€".encode())
            payload = bytes(range(256)) * 200
            peer.frame(2, payload, masked=True)
            assert peer.read_frame(False) == (0x82, payload)
            peer.frame(8, b"\x03\xe8done", masked=True)
            assert peer.read_frame(False) == (0x88, b"\x03\xe8done")
    finally:
        process.terminate()
        try:
            process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()


def server_against_pace(executable, mode="valid"):
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        listener.settimeout(5)
        process = subprocess.Popen([executable, "client", str(listener.getsockname()[1])],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            connection, _ = listener.accept()
            with connection:
                peer = Peer(connection)
                status, headers = peer.head()
                assert status == "GET /echo HTTP/1.1"
                assert len(base64.b64decode(headers["sec-websocket-key"], validate=True)) == 16
                accept = challenge(headers["sec-websocket-key"]) if mode != "challenge" else "invalid"
                protocol = "echo" if mode != "subprotocol" else "unoffered"
                extension = "Sec-WebSocket-Extensions: permessage-deflate\r\n" if mode == "extension" else ""
                connection.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                    "Sec-WebSocket-Accept: " + accept + "\r\nSec-WebSocket-Protocol: " + protocol + "\r\n"
                                    + extension + "\r\n").encode("ascii"))
                if mode in ("valid", "masked"):
                    assembled = b""
                    while True:
                        first, payload = peer.read_frame(True)
                        opcode = first & 15
                        if opcode == 9:
                            peer.frame(10, payload)
                            continue
                        assert opcode in (0, 1)
                        assembled += payload
                        if first & 0x80:
                            break
                    assert assembled == b"Hello from PACE"
                    if mode == "masked":
                        peer.frame(1, assembled, masked=True)
                        first, payload = peer.read_frame(True)
                        assert first == 0x88 and payload[:2] == b"\x03\xea"  # 1002 protocol error.
                    else:
                        peer.frame(1, assembled[:4], final=False)
                        peer.frame(9, b"server ping")
                        peer.frame(0, assembled[4:])
                        assert peer.read_frame(True) == (0x8A, b"server ping")
                        first, payload = peer.read_frame(True)
                        assert first == 0x88 and payload[:2] == b"\x03\xe8"
                        peer.frame(8, payload)
            stdout, stderr = process.communicate(timeout=5)
            if mode == "valid":
                assert process.returncode == 0, (stdout, stderr)
                assert "WebSocket echo verified" in stdout
            else:
                assert process.returncode != 0, "malicious server was accepted: " + mode
                expected = {"challenge": "upgrade response", "subprotocol": "unoffered", "extension": "extension", "masked": "masking"}
                assert expected[mode] in stderr, stderr
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", nargs="?", help="path to tutorial_websocket binary")
    parser.add_argument("--build-dir", type=Path, help="find the tutorial binary under this build directory")
    args = parser.parse_args()
    if bool(args.executable) == bool(args.build_dir):
        parser.error("provide either executable or --build-dir")
    if args.build_dir:
        candidates = [path for name in ("tutorial_websocket", "tutorial_websocket.exe")
                      for path in args.build_dir.rglob(name) if path.is_file()]
        if len(candidates) != 1:
            parser.error(f"expected one tutorial_websocket binary under {args.build_dir}, found {len(candidates)}")
        args.executable = str(candidates[0].resolve())
    client_against_pace(args.executable)
    for mode in ("valid", "challenge", "subprotocol", "extension", "masked"):
        server_against_pace(args.executable, mode)
    print("Independent Python/PACE WebSocket interoperability passed (both roles + invalid peers)")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Run real QUIC exchanges between binaries built with different backends."""
import argparse
from pathlib import Path
import selectors
import subprocess


def exchange(server_binary: Path, client_binary: Path) -> None:
    server = subprocess.Popen([str(server_binary), "--interop-server"], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True)
    try:
        with selectors.DefaultSelector() as ready:
            ready.register(server.stdout, selectors.EVENT_READ)
            if not ready.select(timeout=15):
                raise RuntimeError("QUIC interoperability server did not start")
        port = server.stdout.readline().strip()
        if not port.isdecimal():
            raise RuntimeError(f"Invalid server port: {port!r}")
        subprocess.run([str(client_binary), "--interop-client", port], check=True, timeout=30)
        output, error = server.communicate(timeout=30)
        if server.returncode:
            raise RuntimeError(f"QUIC server failed: {output}\n{error}")
    finally:
        if server.poll() is None:
            server.kill()
        server.communicate()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("openssl", type=Path)
    parser.add_argument("ngtcp2", type=Path)
    args = parser.parse_args()
    exchange(args.openssl.resolve(), args.ngtcp2.resolve())
    exchange(args.ngtcp2.resolve(), args.openssl.resolve())
    print("OpenSSL ↔ ngtcp2 QUIC interoperability passed in both directions")


if __name__ == "__main__":
    main()

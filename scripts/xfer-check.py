#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""End-to-end payload check for omi0.

The driver marks received frames CHECKSUM_UNNECESSARY, so TCP will not
catch a corrupted frame. Hash the bytes on both ends instead.

  receiver:  xfer-check.py recv 10.20.0.14 5201
  sender:    xfer-check.py send 10.20.0.14 5201 /tmp/rand.bin
"""
import hashlib
import socket
import sys
import time

CHUNK = 1 << 20


def recv(addr, port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((addr, port))
    srv.listen(1)
    conn, _ = srv.accept()
    h = hashlib.sha256()
    total = 0
    t0 = time.monotonic()
    while True:
        buf = conn.recv(CHUNK)
        if not buf:
            break
        h.update(buf)
        total += len(buf)
    dt = time.monotonic() - t0
    print(f"recv {total} bytes in {dt:.2f}s sha256 {h.hexdigest()}")


def send(addr, port, path):
    h = hashlib.sha256()
    s = socket.create_connection((addr, port))
    total = 0
    with open(path, "rb") as f:
        while True:
            buf = f.read(CHUNK)
            if not buf:
                break
            h.update(buf)
            s.sendall(buf)
            total += len(buf)
    s.shutdown(socket.SHUT_WR)
    s.recv(1)
    s.close()
    print(f"sent {total} bytes sha256 {h.hexdigest()}")


if __name__ == "__main__":
    if sys.argv[1] == "recv":
        recv(sys.argv[2], int(sys.argv[3]))
    else:
        send(sys.argv[2], int(sys.argv[3]), sys.argv[4])

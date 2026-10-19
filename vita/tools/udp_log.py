#!/usr/bin/env python3
"""Vita UDP log receiver. For Windows, where `nc -lu 18194` is not available.

Usage: python tools/udp_log.py [--port 18194] [--out log.txt]
"""
import argparse
import socket
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=18194)
    ap.add_argument("--out", help="also append received log to this file")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", args.port))
    out = open(args.out, "a", encoding="utf-8") if args.out else None
    print(f"UDP {args.port} listening (Ctrl+C to quit)", file=sys.stderr)
    try:
        while True:
            data, (host, _) = sock.recvfrom(2048)
            line = data.decode("utf-8", "replace").rstrip("\n")
            print(f"{host} {line}", flush=True)
            if out:
                out.write(line + "\n")
                out.flush()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

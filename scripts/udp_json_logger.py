#!/usr/bin/env python3
"""Listen on a UDP port and append each datagram's JSON payload to a file.

A simple sink for ft8mon's `-json host:port` output: every FT8 decode
arrives as one JSON object per UDP datagram, and this script appends it,
one object per line (newline-delimited JSON), to a file.

  ./ft8mon -listen :8073 -json 127.0.0.1:5001
  ./scripts/udp_json_logger.py         # writes to ft8_decodes.ndjson
"""

import argparse
import json
import socket
import sys


def main():
    ap = argparse.ArgumentParser(
        description="Append JSON UDP datagrams to a file (newline-delimited).")
    ap.add_argument("-p", "--port", type=int, default=5001,
                    help="UDP port to listen on (default: 5001)")
    ap.add_argument("-b", "--bind", default="0.0.0.0",
                    help="address to bind (default: 0.0.0.0, all interfaces)")
    ap.add_argument("-o", "--outfile", default="ft8_decodes.ndjson",
                    help="file to append to (default: ft8_decodes.ndjson)")
    ap.add_argument("--no-validate", action="store_true",
                    help="append raw payloads without checking they are JSON")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.bind, args.port))
    print(f"listening on {args.bind}:{args.port}, appending to {args.outfile}",
          file=sys.stderr)

    # line-buffered append; each datagram is flushed so the file stays current.
    with open(args.outfile, "a", encoding="utf-8") as f:
        while True:
            data, addr = sock.recvfrom(65535)
            text = data.decode("utf-8", errors="replace").strip()
            if not text:
                continue
            if not args.no_validate:
                try:
                    json.loads(text)
                except json.JSONDecodeError as e:
                    print(f"skipping non-JSON datagram from "
                          f"{addr[0]}:{addr[1]}: {e}", file=sys.stderr)
                    continue
            f.write(text + "\n")
            f.flush()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass

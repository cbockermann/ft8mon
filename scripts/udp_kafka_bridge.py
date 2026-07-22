#!/usr/bin/env python3
"""Forward ft8mon JSON UDP datagrams to a Kafka topic.

A small bridge for ft8mon's `-json host:port` output: it listens on a
UDP port, and produces each datagram's JSON payload to Kafka. UDP is
fire-and-forget, so no receiver need be running for ft8mon; this bridge
gives you durable, replayable storage once the decodes reach Kafka.

  ./ft8mon -listen :8073 -json 127.0.0.1:5001 -tag band:40m
  ./scripts/udp_kafka_bridge.py --brokers localhost:9092 --topic ft8.decodes \
      --key band

Needs the confluent-kafka client:

  pip install confluent-kafka
"""

import argparse
import json
import signal
import socket
import sys


def extract_key(text, key_field):
    """Return (key_bytes_or_None, error_or_None) for a datagram.

    Parses `text` as JSON and pulls out `key_field`. If key_field is
    None, no JSON parse is required and (None, None) is returned.
    """
    if not key_field:
        return None, None
    try:
        obj = json.loads(text)
    except json.JSONDecodeError as e:
        return None, f"invalid JSON: {e}"
    val = obj.get(key_field)
    if val is None:
        return None, None  # send without a key
    return str(val).encode("utf-8"), None


def main():
    ap = argparse.ArgumentParser(
        description="Forward ft8mon JSON UDP datagrams to a Kafka topic.")
    ap.add_argument("-p", "--port", type=int, default=5001,
                    help="UDP port to listen on (default: 5001)")
    ap.add_argument("-b", "--bind", default="0.0.0.0",
                    help="address to bind (default: 0.0.0.0, all interfaces)")
    ap.add_argument("--brokers", default="localhost:9092",
                    help="Kafka bootstrap servers (default: localhost:9092)")
    ap.add_argument("--topic", default="ft8.decodes",
                    help="Kafka topic to produce to (default: ft8.decodes)")
    ap.add_argument("--key", metavar="FIELD", default=None,
                    help="use this JSON field as the Kafka message key, "
                         "e.g. --key band (default: no key)")
    ap.add_argument("--acks", default="all", choices=["0", "1", "all"],
                    help="producer acks / durability (default: all)")
    ap.add_argument("--validate", action="store_true",
                    help="parse every datagram as JSON and skip invalid ones "
                         "(implied when --key is set)")
    args = ap.parse_args()

    try:
        from confluent_kafka import Producer
    except ImportError:
        print("error: this bridge needs the confluent-kafka client.\n"
              "       install it with:  pip install confluent-kafka",
              file=sys.stderr)
        return 1

    n_ok = 0
    n_err = 0

    def on_delivery(err, msg):
        nonlocal n_ok, n_err
        if err is not None:
            n_err += 1
            print(f"delivery failed: {err}", file=sys.stderr)
        else:
            n_ok += 1

    producer = Producer({
        "bootstrap.servers": args.brokers,
        "acks": args.acks,
        "client.id": "ft8-udp-kafka-bridge",
    })

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.bind, args.port))
    print(f"listening on {args.bind}:{args.port}, producing to "
          f"'{args.topic}' at {args.brokers}"
          + (f", key='{args.key}'" if args.key else ""), file=sys.stderr)

    running = True

    def stop(signum, frame):
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    validate = args.validate or bool(args.key)

    try:
        while running:
            try:
                data, addr = sock.recvfrom(65535)
            except OSError:
                break
            text = data.decode("utf-8", errors="replace").strip()
            if not text:
                continue

            key, err = extract_key(text, args.key)
            if err is not None:
                print(f"skipping datagram from {addr[0]}:{addr[1]}: {err}",
                      file=sys.stderr)
                continue
            if validate and not args.key:
                # validation requested without keying: parse to check.
                try:
                    json.loads(text)
                except json.JSONDecodeError as e:
                    print(f"skipping datagram from {addr[0]}:{addr[1]}: "
                          f"invalid JSON: {e}", file=sys.stderr)
                    continue

            value = text.encode("utf-8")
            while True:
                try:
                    producer.produce(args.topic, value=value, key=key,
                                     on_delivery=on_delivery)
                    break
                except BufferError:
                    # local queue full; serve callbacks and retry.
                    producer.poll(0.5)
            producer.poll(0)  # serve delivery callbacks
    finally:
        # flush buffered messages before exiting.
        print("flushing...", file=sys.stderr)
        producer.flush(10)
        print(f"delivered {n_ok}, failed {n_err}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())

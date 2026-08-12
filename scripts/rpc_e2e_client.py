#!/usr/bin/env python3
"""MyRPC V2 wire protocol E2E client: 20-byte header + body.

The header layout (network byte order):
  magic(2) version(1) flags(1) msg_type(1) status(2) request_id(4)
  method_id(4) body_len(4) reserved(1)

Single request (keeps the V1 command line):
  python3 scripts/rpc_e2e_client.py --host 127.0.0.1 --port 12345 --body test

Concurrent multiplexing check: N requests over one connection with distinct
request_ids; every response must carry one of the sent ids, and each response
body must echo the corresponding request body.
  python3 scripts/rpc_e2e_client.py --concurrency 50 --timeout 20
"""
import argparse
import socket
import struct
import sys

MAGIC = 0x4D50
VERSION = 1
MSG_REQUEST = 0
MSG_RESPONSE = 1
K_HEADER = 20
K_MAX_BODY = 64 * 1024 * 1024
DEFAULT_METHOD = "demo.echo"


def fnv1a32(s: str) -> int:
    h = 2166136261
    for b in s.encode("utf-8"):
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def encode_frame(request_id: int, method_id: int, body: bytes,
                 msg_type: int = MSG_REQUEST, status: int = 0) -> bytes:
    header = struct.pack("!HBBBHIIIB", MAGIC, VERSION, 0, msg_type, status,
                         request_id, method_id, len(body), 0)
    return header + body


def decode_header(raw: bytes):
    (magic, version, flags, msg_type, status, request_id, method_id,
     body_len, _reserved) = struct.unpack("!HBBBHIIIB", raw)
    return magic, version, flags, msg_type, status, request_id, method_id, body_len


def recv_exact(sock, n: int) -> bytes:
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise ConnectionError("connection closed mid-frame")
        data += chunk
    return data


def read_response(sock):
    """Read one response frame; returns (request_id, status, body)."""
    header = recv_exact(sock, K_HEADER)
    magic, version, _f, msg_type, status, request_id, _m, body_len = decode_header(header)
    if magic != MAGIC or version != VERSION:
        raise ValueError(f"protocol mismatch: magic={magic:#x} version={version}")
    if body_len > K_MAX_BODY:
        raise ValueError(f"frame too large: {body_len}")
    body = recv_exact(sock, body_len) if body_len > 0 else b""
    return request_id, status, body


def main() -> int:
    p = argparse.ArgumentParser(description="MyRPC V2 协议 E2E 测试客户端")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=12345)
    p.add_argument("--body", default="test", help="请求体（单请求模式）")
    p.add_argument("--method", default=DEFAULT_METHOD, help="方法名，用于计算 method_id")
    p.add_argument("--concurrency", type=int, default=0,
                   help=">0 时启用单连接并发乱序验证，忽略 --body")
    p.add_argument("--timeout", type=float, default=15.0)
    args = p.parse_args()

    try:
        sock = socket.create_connection((args.host, args.port), timeout=args.timeout)
    except OSError as e:
        print(f"CONNECT_FAIL: {e}", file=sys.stderr)
        return 2

    try:
        sock.settimeout(args.timeout)
        method_id = fnv1a32(args.method)

        if args.concurrency > 0:
            # Fire all requests, then collect and match responses by request_id.
            n = args.concurrency
            sent = {}
            for i in range(n):
                body = f"req-{i}".encode("utf-8")
                sent[i + 1] = body  # request_id starts at 1
                sock.sendall(encode_frame(i + 1, method_id, body))

            received = {}
            for _ in range(n):
                rid, status, body = read_response(sock)
                if status != 0:
                    print(f"RESPONSE_ERROR: request_id={rid} status={status}", file=sys.stderr)
                    return 3
                received[rid] = body

            missing = [rid for rid in sent if rid not in received]
            duplicate = len(received) != n
            mismatch = [rid for rid, body in received.items()
                        if rid in sent and sent[rid] not in body]
            print(f"concurrency={n} total_sent={n} total_received={len(received)}")
            print(f"missing={missing} duplicates={duplicate} echo_mismatch={mismatch}")
            if missing or duplicate or mismatch:
                print("RESULT: UNEXPECTED (multiplex mismatch)", file=sys.stderr)
                return 1
            print("RESULT: OK", file=sys.stderr)
            return 0

        # Single request mode.
        body = args.body.encode("utf-8")
        sock.sendall(encode_frame(1, method_id, body))
        rid, status, resp_body = read_response(sock)
        text = resp_body.decode("utf-8", errors="replace")
        print(text)
        if status != 0:
            print(f"RESULT: UNEXPECTED (status={status})", file=sys.stderr)
            return 1
        if args.body not in text:
            print("RESULT: UNEXPECTED (body not echoed)", file=sys.stderr)
            return 1
        print("RESULT: OK", file=sys.stderr)
        return 0
    finally:
        sock.close()


if __name__ == "__main__":
    raise SystemExit(main())

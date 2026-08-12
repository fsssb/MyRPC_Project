#!/usr/bin/env python3
"""MyRPC V2 wire protocol E2E client: 24-byte header + serialized body.

Header layout (network byte order):
  magic(2) version(1) flags(1) msg_type(1) status(2) request_id(4)
  method_id(4) timeout_ms(4) body_len(4) reserved(1)

Request body is a Serializer struct with field 1 = prompt string. The echo
method replies with the same struct; the ai method replies with a string.

Modes:
  single echo/ai:  python3 scripts/rpc_e2e_client.py --method demo.echo --body test
  concurrency:     python3 scripts/rpc_e2e_client.py --concurrency 50
  timeout check:   python3 scripts/rpc_e2e_client.py --method demo.ai --timeout-ms 100
                   (ai handler takes ~250ms -> expect DEADLINE_EXCEEDED)
  heartbeat:       python3 scripts/rpc_e2e_client.py --heartbeat 5
"""
import argparse
import socket
import struct
import sys

MAGIC = 0x4D50
VERSION = 1
MSG_REQUEST = 0
MSG_RESPONSE = 1
MSG_HEARTBEAT = 3
MSG_HEARTBEAT_ACK = 4
K_HEADER = 24
K_MAX_BODY = 64 * 1024 * 1024

STATUS_OK = 0
STATUS_DEADLINE_EXCEEDED = 5

# Serializer value types (must match include/Serializer.h).
V_NULL, V_BOOL, V_INT64, V_UINT64, V_DOUBLE, V_STRING, V_ARRAY, V_MAP, V_STRUCT = range(9)


def fnv1a32(s: str) -> int:
    h = 2166136261
    for b in s.encode("utf-8"):
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def encode_varint(n: int) -> bytes:
    out = bytearray()
    while n >= 0x80:
        out.append((n & 0x7F) | 0x80)
        n >>= 7
    out.append(n)
    return bytes(out)


def decode_varint(buf: bytes, pos: int):
    """Returns (value, new_pos); raises ValueError on malformed input."""
    value = 0
    shift = 0
    while True:
        if pos >= len(buf):
            raise ValueError("truncated varint")
        b = buf[pos]
        pos += 1
        value |= (b & 0x7F) << shift
        if not (b & 0x80):
            return value, pos
        shift += 7
        if shift > 63:
            raise ValueError("varint too long")


def encode_struct_with_field1_string(value: str) -> bytes:
    """Encode struct {1: string(value)} as the server expects."""
    body = value.encode("utf-8")
    field = encode_varint((1 << 3) | 2) + bytes([V_STRING]) + encode_varint(len(body)) + body
    return field


def decode_field1_string(buf: bytes):
    """Parse a struct and return field 1 as string if present, else None."""
    pos = 0
    while pos < len(buf):
        tag, pos = decode_varint(buf, pos)
        field_id = tag >> 3
        if pos >= len(buf):
            raise ValueError("missing value type")
        vtype = buf[pos]
        pos += 1
        if field_id == 1 and vtype == V_STRING:
            length, pos = decode_varint(buf, pos)
            if pos + length > len(buf):
                raise ValueError("truncated string")
            return buf[pos:pos + length].decode("utf-8", errors="replace")
        # skip the value by type (only what the client can receive is needed)
        if vtype in (V_NULL,):
            continue
        if vtype in (V_BOOL,):
            pos += 1
        elif vtype in (V_INT64, V_UINT64):
            _, pos = decode_varint(buf, pos)
        elif vtype == V_DOUBLE:
            pos += 8
        elif vtype in (V_STRING, V_ARRAY, V_MAP, V_STRUCT):
            length, pos = decode_varint(buf, pos)
            pos += length
        else:
            raise ValueError(f"unknown value type {vtype}")
    return None


def encode_frame(request_id: int, method_id: int, body: bytes, timeout_ms: int = 0,
                 msg_type: int = MSG_REQUEST, status: int = 0) -> bytes:
    header = struct.pack("!HBBBHIIIIB", MAGIC, VERSION, 0, msg_type, status,
                         request_id, method_id, timeout_ms, len(body), 0)
    return header + body


def decode_header(raw: bytes):
    (magic, version, flags, msg_type, status, request_id, method_id,
     timeout_ms, body_len, _reserved) = struct.unpack("!HBBBHIIIIB", raw)
    return magic, version, flags, msg_type, status, request_id, method_id, body_len


def recv_exact(sock, n: int) -> bytes:
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise ConnectionError("connection closed mid-frame")
        data += chunk
    return data


def read_frame(sock):
    """Read one frame; returns (msg_type, status, request_id, body)."""
    header = recv_exact(sock, K_HEADER)
    magic, version, _f, msg_type, status, request_id, _m, body_len = decode_header(header)
    if magic != MAGIC or version != VERSION:
        raise ValueError(f"protocol mismatch: magic={magic:#x} version={version}")
    if body_len > K_MAX_BODY:
        raise ValueError(f"frame too large: {body_len}")
    body = recv_exact(sock, body_len) if body_len > 0 else b""
    return msg_type, status, request_id, body


def run_single(sock, args, method_id: int) -> int:
    body = encode_struct_with_field1_string(args.body)
    sock.sendall(encode_frame(1, method_id, body, timeout_ms=args.timeout_ms))
    msg_type, status, _rid, resp_body = read_frame(sock)
    if msg_type != MSG_RESPONSE:
        print(f"UNEXPECTED_FRAME: msg_type={msg_type}", file=sys.stderr)
        return 1
    text = decode_field1_string(resp_body)
    print(f"status={status} body={text!r}")
    if args.timeout_ms > 0:
        # The ai handler takes ~250ms; a short timeout must be enforced server-side.
        expect_deadline = status == STATUS_DEADLINE_EXCEEDED
        print(f"deadline_exceeded={expect_deadline}")
        if not expect_deadline:
            print("RESULT: UNEXPECTED (expected DEADLINE_EXCEEDED)", file=sys.stderr)
            return 1
    else:
        if status != STATUS_OK or text is None:
            print(f"RESULT: UNEXPECTED (status={status} body={text})", file=sys.stderr)
            return 1
        if args.body not in text:
            print("RESULT: UNEXPECTED (body not echoed)", file=sys.stderr)
            return 1
    print("RESULT: OK", file=sys.stderr)
    return 0


def run_concurrency(sock, n: int, method_id: int) -> int:
    sent = {}
    for i in range(n):
        body = f"req-{i}"
        payload = encode_struct_with_field1_string(body)
        sent[i + 1] = body
        sock.sendall(encode_frame(i + 1, method_id, payload))
    received = {}
    for _ in range(n):
        msg_type, status, rid, resp_body = read_frame(sock)
        if msg_type != MSG_RESPONSE or status != STATUS_OK:
            print(f"RESPONSE_ERROR: request_id={rid} status={status} msg_type={msg_type}",
                  file=sys.stderr)
            return 3
        received[rid] = resp_body
    missing = [rid for rid in sent if rid not in received]
    mismatch = [rid for rid, body in received.items()
                if rid in sent and decode_field1_string(body) != sent[rid]]
    print(f"concurrency={n} total_sent={n} total_received={len(received)}")
    print(f"missing={missing} duplicate={len(received) != n} echo_mismatch={mismatch}")
    if missing or len(received) != n or mismatch:
        print("RESULT: UNEXPECTED (multiplex mismatch)", file=sys.stderr)
        return 1
    print("RESULT: OK", file=sys.stderr)
    return 0


def run_heartbeat(sock, count: int) -> int:
    for i in range(count):
        sock.sendall(encode_frame(i + 1, 0, b"", msg_type=MSG_HEARTBEAT))
    for i in range(count):
        msg_type, status, rid, _body = read_frame(sock)
        if msg_type != MSG_HEARTBEAT_ACK or rid != i + 1:
            print(f"HEARTBEAT_FAIL: ack_msg_type={msg_type} rid={rid} expected={i + 1}",
                  file=sys.stderr)
            return 1
    print(f"heartbeat_ack={count}")
    print("RESULT: OK", file=sys.stderr)
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description="MyRPC V2 协议 E2E 测试客户端")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=12345)
    p.add_argument("--method", default="demo.echo", help="service.method")
    p.add_argument("--body", default="test", help="请求体（echo/ai 单请求模式）")
    p.add_argument("--timeout-ms", type=int, default=0, help="请求头 timeout_ms（0=无超时）")
    p.add_argument("--concurrency", type=int, default=0,
                   help=">0 时启用单连接并发乱序验证")
    p.add_argument("--heartbeat", type=int, default=0,
                   help=">0 时发送 N 个心跳并验证 ack")
    p.add_argument("--timeout", type=float, default=15.0, help="socket 超时（秒）")
    args = p.parse_args()

    try:
        sock = socket.create_connection((args.host, args.port), timeout=args.timeout)
    except OSError as e:
        print(f"CONNECT_FAIL: {e}", file=sys.stderr)
        return 2

    try:
        sock.settimeout(args.timeout)
        method_id = fnv1a32(args.method)
        if args.heartbeat > 0:
            return run_heartbeat(sock, args.heartbeat)
        if args.concurrency > 0:
            return run_concurrency(sock, args.concurrency, method_id)
        return run_single(sock, args, method_id)
    finally:
        sock.close()


if __name__ == "__main__":
    raise SystemExit(main())

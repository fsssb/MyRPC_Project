#!/usr/bin/env python3
"""单次长度头协议探测：避免 zsh 双引号里 ! 触发 history 展开。"""
import argparse
import socket
import struct
import sys


def encode_frame(payload: bytes) -> bytes:
    return struct.pack("!I", len(payload)) + payload


def main() -> int:
    p = argparse.ArgumentParser(description="MyRPC 长度头协议 E2E 测试客户端")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=12345)
    p.add_argument("--body", default="test", help="请求体（UTF-8）")
    p.add_argument("--timeout", type=float, default=15.0)
    args = p.parse_args()

    try:
        sock = socket.create_connection((args.host, args.port), timeout=args.timeout)
    except OSError as e:
        print(f"CONNECT_FAIL: {e}", file=sys.stderr)
        return 2

    try:
        payload = args.body.encode("utf-8")
        sock.settimeout(args.timeout)
        sock.sendall(encode_frame(payload))
        header = sock.recv(4)
        if len(header) < 4:
            print(f"RECV_FAIL: short header {len(header)}", file=sys.stderr)
            return 3
        (body_len,) = struct.unpack("!I", header)
        data = b""
        while len(data) < body_len:
            chunk = sock.recv(body_len - len(data))
            if not chunk:
                break
            data += chunk
        text = data.decode("utf-8", errors="replace")
        print(text)
        if "AI:" in text:
            print("RESULT: OK", file=sys.stderr)
            return 0
        print("RESULT: UNEXPECTED (no AI: prefix)", file=sys.stderr)
        return 1
    finally:
        sock.close()


if __name__ == "__main__":
    raise SystemExit(main())

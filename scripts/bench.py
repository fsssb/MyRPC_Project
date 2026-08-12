#!/usr/bin/env python3
"""MyRPC V2 benchmark: 24-byte header + Serializer body.

Each worker owns one connection and issues sequential calls (AI chain or echo,
see --method). The response is matched by request_id; status 0 and the expected
body prefix count as success.
"""
import argparse
import asyncio
import statistics
import struct
import time
from typing import List

MAGIC = 0x4D50
VERSION = 1
MSG_REQUEST = 0
MSG_RESPONSE = 1
K_HEADER = 24
K_MAX_BODY = 64 * 1024 * 1024
V_STRING = 5
STATUS_OK = 0


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


def encode_struct_field1_string(value: str) -> bytes:
    body = value.encode("utf-8")
    return encode_varint((1 << 3) | 2) + bytes([V_STRING]) + encode_varint(len(body)) + body


def decode_field1_string(buf: bytes):
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
        if vtype == V_STRING:
            length, pos = decode_varint(buf, pos)
            pos += length
        elif vtype in (0, 1):
            pos += 1
        elif vtype in (2, 3):
            _, pos = decode_varint(buf, pos)
        elif vtype == 4:
            pos += 8
        else:
            length, pos = decode_varint(buf, pos)
            pos += length
    return None


def encode_frame(request_id: int, method_id: int, body: bytes, timeout_ms: int) -> bytes:
    header = struct.pack("!HBBBHIIIIB", MAGIC, VERSION, 0, MSG_REQUEST, 0,
                         request_id, method_id, timeout_ms, len(body), 0)
    return header + body


async def recv_exactly(reader: asyncio.StreamReader, n: int) -> bytes:
    data = b""
    while len(data) < n:
        chunk = await reader.read(n - len(data))
        if not chunk:
            raise ConnectionError("connection closed unexpectedly")
        data += chunk
    return data


async def recv_response(reader: asyncio.StreamReader):
    header = await recv_exactly(reader, K_HEADER)
    (magic, version, _f, msg_type, status, request_id, _m, _t, body_len,
     _r) = struct.unpack("!HBBBHIIIIB", header)
    if magic != MAGIC or version != VERSION:
        raise ValueError("protocol mismatch")
    if body_len > K_MAX_BODY:
        raise ValueError("frame too large")
    body = await recv_exactly(reader, body_len) if body_len > 0 else b""
    return msg_type, status, request_id, body


async def worker(
    host: str,
    port: int,
    worker_id: int,
    deadline: float,
    timeout: float,
    method_id: int,
    expect_prefix: str,
    latencies: List[float],
    counter: List[int],
    failures: List[int],
) -> None:
    reader, writer = await asyncio.open_connection(host, port)
    try:
        seq = 0
        timeout_ms = int(timeout * 1000)
        while time.perf_counter() < deadline:
            payload = f"bench-{worker_id}-{seq}"
            frame = encode_frame(seq + 1, method_id,
                                 encode_struct_field1_string(payload), timeout_ms)

            start = time.perf_counter()
            writer.write(frame)
            await writer.drain()

            try:
                msg_type, status, _rid, response = await asyncio.wait_for(
                    recv_response(reader), timeout=timeout)
            except (asyncio.TimeoutError, ConnectionError):
                failures[0] += 1
                break
            elapsed_ms = (time.perf_counter() - start) * 1000.0

            if msg_type != MSG_RESPONSE or status != STATUS_OK:
                failures[0] += 1
                break
            text = decode_field1_string(response)
            if not text or not text.startswith(expect_prefix):
                failures[0] += 1
                break

            latencies.append(elapsed_ms)
            counter[0] += 1
            seq += 1
    finally:
        writer.close()
        await writer.wait_closed()


def percentile(values: List[float], p: float) -> float:
    if not values:
        return 0.0
    if len(values) == 1:
        return values[0]
    rank = (len(values) - 1) * p
    low = int(rank)
    high = min(low + 1, len(values) - 1)
    weight = rank - low
    return values[low] * (1 - weight) + values[high] * weight


async def run_bench(args: argparse.Namespace) -> None:
    latencies: List[float] = []
    counter = [0]
    failures = [0]
    method_id = fnv1a32(args.method)
    start = time.perf_counter()
    deadline = start + args.duration

    tasks = [
        asyncio.create_task(
            worker(
                args.host,
                args.port,
                idx,
                deadline,
                args.timeout,
                method_id,
                args.expect_prefix,
                latencies,
                counter,
                failures,
            )
        )
        for idx in range(args.concurrency)
    ]

    await asyncio.gather(*tasks, return_exceptions=True)
    total_time = time.perf_counter() - start
    latencies.sort()

    qps = counter[0] / total_time if total_time > 0 else 0.0
    avg = statistics.mean(latencies) if latencies else 0.0
    p99 = percentile(latencies, 0.99)

    print(f"method={args.method}")
    print(f"concurrency={args.concurrency}")
    print(f"duration_sec={total_time:.2f}")
    print(f"total_requests={counter[0]}")
    print(f"total_failures={failures[0]}")
    print(f"qps={qps:.2f}")
    print(f"avg_latency_ms={avg:.2f}")
    print(f"p99_latency_ms={p99:.2f}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="MyRPC V2 benchmark tool")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=12345)
    parser.add_argument("--method", default="demo.ai",
                        help="service.method to benchmark (demo.ai = AI chain, demo.echo = network)")
    parser.add_argument("--expect-prefix", default="AI:",
                        help="expected body prefix of a successful response")
    parser.add_argument("--concurrency", type=int, default=5)
    parser.add_argument("--duration", type=int, default=10)
    parser.add_argument("--timeout", type=float, default=5.0)
    return parser.parse_args()


if __name__ == "__main__":
    asyncio.run(run_bench(parse_args()))

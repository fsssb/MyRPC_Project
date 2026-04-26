#!/usr/bin/env python3
import argparse
import asyncio
import statistics
import struct
import time
from typing import List


def encode_frame(payload: bytes) -> bytes:
    return struct.pack("!I", len(payload)) + payload


async def recv_exactly(reader: asyncio.StreamReader, n: int) -> bytes:
    data = b""
    while len(data) < n:
        chunk = await reader.read(n - len(data))
        if not chunk:
            raise ConnectionError("connection closed unexpectedly")
        data += chunk
    return data


async def recv_frame(reader: asyncio.StreamReader) -> bytes:
    header = await recv_exactly(reader, 4)
    body_len = struct.unpack("!I", header)[0]
    return await recv_exactly(reader, body_len)


async def worker(
    host: str,
    port: int,
    worker_id: int,
    deadline: float,
    timeout: float,
    latencies: List[float],
    counter: List[int],
    failures: List[int],
) -> None:
    reader, writer = await asyncio.open_connection(host, port)
    try:
        seq = 0
        while time.perf_counter() < deadline:
            payload = f"bench-{worker_id}-{seq}".encode("utf-8")
            frame = encode_frame(payload)

            start = time.perf_counter()
            writer.write(frame)
            await writer.drain()

            try:
                response = await asyncio.wait_for(recv_frame(reader), timeout=timeout)
            except (asyncio.TimeoutError, ConnectionError):
                failures[0] += 1
                break
            elapsed_ms = (time.perf_counter() - start) * 1000.0

            if not response.startswith(b"AI: "):
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

    print(f"concurrency={args.concurrency}")
    print(f"duration_sec={total_time:.2f}")
    print(f"total_requests={counter[0]}")
    print(f"total_failures={failures[0]}")
    print(f"qps={qps:.2f}")
    print(f"avg_latency_ms={avg:.2f}")
    print(f"p99_latency_ms={p99:.2f}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="RPC benchmark tool")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=12345)
    parser.add_argument("--concurrency", type=int, default=1000)
    parser.add_argument("--duration", type=int, default=10)
    parser.add_argument("--timeout", type=float, default=5.0)
    return parser.parse_args()


if __name__ == "__main__":
    asyncio.run(run_bench(parse_args()))

# 验证说明

本文档记录 MyRPCProject 的可复现验证方式，只保留关键命令和结论，不保留完整构建日志。

## 本机构建

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

运行：

```bash
./build/rpc_demo 4
```

macOS 本机验证的是 `PollPoller` 路径。Linux `EpollPoller` 路径需要在 Linux 或 Docker 中验证。

## Docker Linux 路径

```bash
docker build --no-cache -t myrpc-epoll .
bash ./scripts/acceptance.sh myrpc-epoll myrpc-e2e 12345
```

关键预期输出：

```text
IN_CONTAINER: OK
HOST: OK
total_failures=0
RESULT: OK
```

宿主机侧检查要求命令运行在 Docker 端口映射所在机器上。远程沙箱中 `127.0.0.1` 可能不是 Docker 宿主机，所以宿主机侧失败可能是环境问题。

## E2E Client

```bash
python3 scripts/rpc_e2e_client.py --host 127.0.0.1 --port 12345 --body test --timeout 20
```

预期响应：

```text
AI: Intent(test) | Reasoning(test)
RESULT: OK
```

## 短时 Benchmark

当前 benchmark 脚本跑的是 AI 模拟链路：

```bash
python3 scripts/bench.py --host 127.0.0.1 --port 12345 --concurrency 5 --duration 2 --timeout 20
```

可接受结果形态：

```text
concurrency=5
total_failures=0
qps=...
avg_latency_ms=...
p99_latency_ms=...
```

这组数字不是网络栈极限性能，因为这条路径包含模拟业务耗时。

## 历史问题：epoll ET wakeup pipe

之前 Docker acceptance 曾在容器内 e2e 阶段超时。根因是 Linux epoll ET 模式下 wakeup pipe 处理不完整：如果 pipe fd 是阻塞的，或者 wakeup 读侧没有读到 `EAGAIN`，EventLoop 可能无法推进到 `doPendingFunctors()`。

当前实现中，Linux 下 wakeup pipe 两端设置为非阻塞，并且 wakeup 读侧循环读取到 `EAGAIN`。

相关代码：

- `src/EventLoop.cpp`
- `EventLoop::queueInLoop`
- `EventLoop::handleWakeupRead`

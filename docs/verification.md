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

## V2.0 验证记录（2026-08-12）

V2.0 把 V1 的 Length-Prefix 分帧升级为 24B RPC 头（`magic/version/flags/msg_type/status/request_id/method_id/timeout_ms/body_len/reserved`），新增自研序列化、`RpcServer` 路由与 deadline、C++ 客户端 stub（单连接多路复用）与应用层心跳。

本机（macOS，`PollPoller`）验证：

```text
单元验证（/tmp 独立测试程序）:
  Serializer 编解码往返（string/int/uint/double/bool/array/map/嵌套 struct）  PASS
  未知字段前向兼容 / 截断输入拒绝                                     PASS
  RpcFramer 粘包拆帧 / 半包保留 / 超大包拒收 / 协议魔数不符拒收          PASS

e2e（python3 scripts/rpc_e2e_client.py）:
  单请求 demo.echo / demo.ai                    RESULT: OK
  并发 50 乱序配对（missing=[] duplicates=False echo_mismatch=[]）  RESULT: OK
  demo.ai timeout-ms=100 → DEADLINE_EXCEEDED(5)   RESULT: OK
  心跳 5 个 → 全部 ack                          RESULT: OK
  未知方法 → METHOD_NOT_FOUND(2)

C++ 客户端 demo（./rpc_client_demo）:
  并发 100/300 多路复用全对、超时 status=5、超时后连接复用          PASS
  心跳保活（空闲 3s 后连接可用）                                      PASS
  对端消失判死（2 心跳周期内，status=UNKNOWN，无崩溃）               PASS

bench.py（V2 协议）:
  demo.echo 网络口径: concurrency=20, qps≈4.7w, p99≈0.7ms
  demo.ai  AI 链路口径: concurrency=3, qps≈12（单请求约 250ms）
```

Linux `EpollPoller` / ET 路径：本机 Docker Desktop 存在失效代理残留（`127.0.0.1:7897`，VPN 关闭后不可达）且 VM 网络异常（`no route to host`），本次**未完成**容器内 Linux epoll 验收。镜像构建与一键验收命令如下，Docker 环境正常时可复现：

```bash
docker build --no-cache -t myrpc-epoll .
bash ./scripts/acceptance.sh myrpc-epoll myrpc-e2e 12345
```

## V2.1 服务治理验证记录（2026-08-13）

本机（macOS，`PollPoller`）验证：

```text
阶段 A 服务端限流 + 优雅关闭:
  max_concurrency=5，30 并发 ai 请求 → 5 OK + 25 CONCURRENCY_LIMITED(9)   PASS
  优雅关闭：SIGINT 后新请求 SHUTTING_DOWN(8)、在途 2s 请求正常完成、进程正常退出  PASS
  （顺带修复 V1 遗留：TcpServer::stop 异步 forceClose 与 sub loop 销毁竞态）

阶段 B 多实例 LB:
  p2c 20000 次 pick 分布 10023/9977（≈1:1）                              PASS
  加权轮询 3:1 → 300/100                                              PASS
  一致性哈希同 key 稳定                                               PASS
  governance demo：2 实例 400 请求全 OK；kill 一实例后 100 请求全 OK      PASS

阶段 C 熔断 + 自动重连:
  错误注入实例：6 个错误后熔断，剩余请求全到健康实例（294 OK）            PASS
  恢复：替换为正常实例 → 自动重连(1s) + 半开探测 → 流量恢复（200 全 OK）   PASS
  （修复：连接失败与熔断解耦——连接类失败由 isHealthy/重连覆盖，不喂 breaker）

阶段 D 重试:
  非幂等 + 连接失败 → 快速失败（retries=0）                            PASS
  幂等 + 连接失败 → 重试 maxAttempts-1 次（retries=2）                 PASS
  服务端 INTERNAL_ERROR → 不重试（retries=0）                          PASS

阶段 E 注册中心:
  registry demo：发现 2 实例 → 100 请求全 OK                          PASS
  unregister → watch 收敛到 1 实例 → 100 请求全 OK                     PASS
  停止续租 → 租约 3s 过期 → 移除（0 实例）→ 快速失败                    PASS
  （连续 3 次运行稳定通过）

V2.0 回归：e2e 单请求/并发/心跳/超时 + rpc_client_demo 100 全绿。

## V2.1 补全验证记录（hedging + 恢复期限流，2026-08-13）

```text
hedging（2 实例：12345 快 echo + 12346 慢 echo 2s，maxAttempts=1 隔离重试）:
  hedgeAfterMs=300 → avg=135ms max=300ms 20/20 OK      PASS（治长尾：max 从 2003ms 降到 300ms）
  hedgeAfterMs=0   → max=2005ms（慢实例 2s 全量等待）    对照
  备份先到者胜：慢响应被 completed 标志丢弃               PASS

熔断恢复期限流（2 实例全错误注入 → 全 OPEN）:
  recoveryMinWorking=1 → 放行 33/60（≈50%），failfast 27  PASS
  recoveryMinWorking=0 → failfast 48（禁用，仅半开探测放行 12）PASS

回归：governance / registry / e2e / client demo 全绿。
```
```

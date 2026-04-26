# MyRPCProject

> **规范总览、目录结构、架构与维护约定**：见 [项目说明.md](项目说明.md)。下文侧重**怎么编译、怎么在 Docker/本机跑、怎么排错**。

一个基于 C++17 的高并发 Main-Sub Reactor RPC 框架示例，支持长度头协议、AI 任务编排模拟、心跳超时清理与优雅关闭。

在 **Linux** 上默认使用 `epoll`（**ET 边缘触发**），在 **macOS** 上回退为 `poll`；通过同一套 `Channel` / `EventLoop` 接口跨平台复用网络模型。

## 编译

```bash
cmake -B build
cmake --build build
```

## 用 Docker 在 Mac 上跑 Linux + Epoll（推荐）

在 macOS 本机没有 `epoll` 时，用容器在 **Linux 内核** 下编译与运行，即可完整验证 `EpollPoller`、ET 下非阻塞读写的「完全体」行为，并与本机 `PollPoller` 做对照压测。镜像内已 `cmake` 一键编译 `rpc_demo`，适合作为简历/面试中的**跨平台高并发网络验证**闭环。

在 **Mac 终端** 中（需已安装 Docker Desktop）。**请勿使用占位路径**；请 `cd` 到本机实际项目目录，例如：

```bash
cd ~/Desktop/MyRPCProject
ls Dockerfile
docker build -t myrpc-epoll .
docker run --rm -p 12345:12345 myrpc-epoll
```

若 `ls Dockerfile` 报不存在，说明当前目录不是项目根目录。构建成功后再 `docker run`；未构建时不要用 `docker pull myrpc-epoll`（那是 Hub 上的公共镜像名，与本地 `docker build -t` 不同）。

### Docker 拉取/构建失败

**默认** `Dockerfile` 使用 **`public.ecr.aws/ubuntu/ubuntu:22.04`**（与常见 `docker.io` 拉取问题错开，多数网络下更易成功）。

- 若 **ECR 超时**（`public.ecr.aws ... EOF`）：给 Docker 配**代理** / **换网络**后重试；或本机能拉 Docker Hub 时改回 Hub 再构建：  
  `docker build --build-arg BASE_IMAGE=ubuntu:22.04 -t myrpc-epoll .`  
- 若 **`auth.docker.io` 超时**（你手动改用 `ubuntu:22.04` 时）：同上，给 **Docker Desktop** 单独设代理（`Settings → Resources → Proxies`），**不要**只依赖系统 VPN；或配 `registry-mirrors`、能拉通的 `BASE_IMAGE`（见上一条）。

**不用 Docker 时**，仍可在本机 `cmake` + `./build/rpc_demo` 验收（本机为 `poll`；Epoll 需在 Linux/容器里）。

另开终端，可对容器内服务压测（本机 `python3` 连 `127.0.0.1:12345`）：

```bash
python3 scripts/bench.py --concurrency 100 --duration 8 --timeout 20
```

**zsh 注意**：在双引号里写 `python3 -c "....'` 若含 `!I` 等，会触发 `zsh: event not found`。请用下面的探测脚本，或给整条加**外层单引号**、Python 里用 `struct.pack("!I", ...)` 的双引号形式。

```bash
python3 scripts/rpc_e2e_client.py --host 127.0.0.1 --port 12345 --body test
```

### 自动化验收 & Codex/Agent 注意

- **先重建镜像**（含代码修复后再测）：`docker build --no-cache -t myrpc-epoll .`
- 一键脚本（**必须在本机已安装 Docker 的 shell 中执行**；**远程/沙箱里的 Agent 的 `127.0.0.1` 往往不是宿主机上的 Docker 端口，会导致全 0 与超时，属环境假阳性**）：

```bash
chmod +x scripts/acceptance.sh
./scripts/acceptance.sh myrpc-epoll myrpc-e2e 12345
```

- 若脚本第 3 步 **容器内** 通过、第 4 步 **宿主机** 失败：检查 Docker 代理/`host.docker.internal`；在 **本机「终端.app」** 中重试。
- 若第 3 步就失败：贴 `docker exec <容器> tail -100 /app/rpc.log` 与镜像构建时间，确认含最新代码。

使用 GDB 可在容器内交互调试（`docker run -it --cap-add=SYS_PTRACE ...` 等，按需加参数）。

## 运行

```bash
./build/rpc_demo 4
```

- 默认监听：`0.0.0.0:12345`
- 参数 `4` 表示 Sub Reactor 工作线程数

## 协议格式

- 帧格式：`[4-byte body length in network byte order][body bytes]`
- 服务端收到完整帧后触发业务回调，避免粘包/半包问题。

## 压测

压测脚本：`scripts/bench.py`

### 100 并发（实测）

```bash
python3 scripts/bench.py --concurrency 100 --duration 8 --timeout 20
```

实测输出（本机）：

- `qps=15.65`
- `avg_latency_ms=5022.76`
- `p99_latency_ms=6414.33`
- `total_requests=224`
- `total_failures=0`

### 1000 并发（压力验证）

```bash
python3 scripts/bench.py --concurrency 1000 --duration 3 --timeout 30
```

实测输出（本机）：

- `qps=14.36`
- `avg_latency_ms=15118.95`
- `p99_latency_ms=29719.73`
- `total_requests=472`
- `total_failures=572`

> 说明：当前业务层故意模拟“AI 编排 + 500ms 级耗时任务”，因此在高并发下吞吐受业务计算阶段限制，结果符合预期。

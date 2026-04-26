#!/usr/bin/env bash
# 一键验收：先「容器内」再「宿主机」，避免把自动化跑在联不到本机端口的沙箱里误判为失败
set -euo pipefail

IMAGE_NAME="${1:-myrpc-epoll}"
CNAME="${2:-myrpc-e2e}"
PORT="${3:-12345}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

cd "$ROOT"

echo "=== 0) 镜像: ${IMAGE_NAME} 端口: ${PORT} 容器名: ${CNAME} ==="
if ! docker image inspect "${IMAGE_NAME}:latest" >/dev/null 2>&1; then
  echo "未找到镜像 ${IMAGE_NAME}:latest，请先: docker build -t ${IMAGE_NAME} ."
  exit 1
fi

echo "=== 1) 清理旧容器 ==="
docker stop "${CNAME}" 2>/dev/null || true
docker rm "${CNAME}" 2>/dev/null || true

echo "=== 2) 启动 ==="
docker run --rm -d -p "${PORT}:${PORT}" --name "${CNAME}" "${IMAGE_NAME}"
sleep 2

echo "=== 3) 容器内自测 (不走宿主机网络，可验证服务本身) ==="
if ! docker exec "${CNAME}" test -f /app/scripts/rpc_e2e_client.py; then
  echo "FAIL: 镜像内无 /app/scripts/rpc_e2e_client.py，请用当前代码重新: docker build --no-cache -t ${IMAGE_NAME} ."
  docker stop "${CNAME}" 2>/dev/null || true
  exit 1
fi
if docker exec "${CNAME}" python3 /app/scripts/rpc_e2e_client.py --host 127.0.0.1 --port "${PORT}" --body test --timeout 20; then
  echo "IN_CONTAINER: OK"
else
  echo "IN_CONTAINER: FAIL (服务端或镜像有问题)"
  docker stop "${CNAME}" 2>/dev/null || true
  exit 1
fi

echo "=== 4) 宿主机自测 (须在本机有 Docker 的终端执行；沙箱/远程 CI 的 127.0.0.1 可能不是本机) ==="
if python3 "${ROOT}/scripts/rpc_e2e_client.py" --host 127.0.0.1 --port "${PORT}" --body test --timeout 20; then
  echo "HOST: OK"
else
  echo "HOST: FAIL (若上一步已 OK，则多为网络/环境：请在 Mac 本机「终端.app」中执行本脚本，勿用无/docker 网络的远程 Agent)"
  docker stop "${CNAME}" 2>/dev/null || true
  exit 1
fi

echo "=== 5) 轻量压测 (3s) ==="
python3 "${ROOT}/scripts/bench.py" --host 127.0.0.1 --port "${PORT}" --concurrency 5 --duration 2 --timeout 25 || true

echo "=== 6) 停止 ==="
docker stop "${CNAME}"
echo "=== 全部通过 ==="
exit 0

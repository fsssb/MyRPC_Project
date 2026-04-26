# Linux 环境下构建并运行（使用 EpollPoller + ET 模式）
# 默认使用 AWS Public ECR 上的 Ubuntu 22.04（国内/部分网络下比 docker.io 更易拉取）
# 若你环境更适合 Docker Hub，可: docker build --build-arg BASE_IMAGE=ubuntu:22.04 -t myrpc-epoll .
ARG BASE_IMAGE=public.ecr.aws/ubuntu/ubuntu:22.04
FROM ${BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    g++ \
    make \
    gdb \
    python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY . .

# 防止本机误传入的 build/ 或旧缓存导致 CMake 认为工程在 /Users/... 路径
RUN rm -rf build \
    && cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j

EXPOSE 12345

# Sub Reactor 工作线程数（可按需修改）
CMD ["./build/rpc_demo", "4"]

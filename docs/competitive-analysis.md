# RPC 竞品分析报告

更新时间：2026-08-12

本文档是 MyRPCProject V2 设计前的竞品调研，覆盖开源 RPC 框架、RPC 相关经典论文、以及工程实践技术博客。结论服务于 V2 设计方案草案（见 `docs/v2-design-draft.md`）：凡是「值得借鉴」的点都能在 V2 草案里找到落点，凡是「别人踩过的坑」都在 V2 草案里有规避设计。

## 1. 调研范围与方法

- **调研对象**：V1 是 C++17 单机 RPC 原型（Main-Sub Reactor、epoll ET / poll fallback、`[4-byte bodyLen][body]` 分帧、业务线程池 + weak_ptr 跨线程回写、outputBuffer 非阻塞写）。
- **三路调研**：
  - 开源框架：抓取各框架官方文档 / 官方 GitHub 仓库约 50 个页面。
  - 论文：一手原文（Birrell & Nelson、ZooKeeper、Raft 解析原文 PDF）+ 官方摘要 + The Morning Paper 权威解读交叉验证。
  - 工程实践：brpc 官方中文文档、美团技术、Sentinel/Dubbo/gRPC/Netty/Prometheus/AWS 官方文档、Linux man 手册等 30+ 来源。
- **说明**：第一轮调研时外网受限，个别细节未能从一手原文核实。2026-08-12 关闭 VPN 后已对全部不确定项二次核实（框架细节 8 项全部消除；论文/博客 8 项中 7 项消除）。**残留的不确定**：① Tail at Scale 英文原版 PDF（Google/CACM 直链本网络仍不可达），「对冲按 95 分位阈值触发」以 Morning Paper 原文引用与多份全文译文为准，「keep timeouts short」逐字表述未能核对；② 一篇知乎文章《IM心跳机制之 tcp 协议 KeepAlive 和应用层心跳》精确 URL（知乎 403），其核心结论已用 man7 手册与同主题公开文章替代核实。本报告只记录已检索验证的结论，不编造。

## 2. 开源框架对比总表

| 框架 | 语言 | 传输协议 | 请求关联键 | 序列化 | 线程模型 | 服务治理 | 特点 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| gRPC | 多语言 | HTTP/2 单连接多流 | stream id | protobuf（可扩展） | C++：同步 API 内置线程池 / 异步 CompletionQueue | resolver + LB + 重试/对冲 | 生态最全、流式、浏览器兼容差 |
| bRPC | C++ | TCP 单连接 / 连接池 / 短连三模式 | correlation_id | protobuf + 附件 | bthread M:N 协程 | 命名服务 + p2c/一致性哈希 + EMA 熔断 + 重试/backup | 内网高吞吐 C++ 标杆，协议可插拔 |
| Dubbo / Dubbo3 | Java 为主 | dubbo 协议单长连 / Triple（HTTP/2） | requestId / stream id | hessian2 / protobuf / kryo | NIO + 线程池复用 | 注册中心 + 六种集群容错 + 多 LB | Java 微服务治理生态最完整 |
| Thrift | 多语言 | TCP + Transport 层分帧 | seq id | TBinary / TCompact | 可插拔 | 无（靠上层框架） | IDL 代码生成鼻祖 |
| tRPC | C++/Go 等 | 自研 16B 定长头 + TCP | request_id / stream_id | protobuf（可扩展） | 连接池 / 多路复用 | 插件化命名服务/监控/流控 | 「不用 HTTP/2」的性能路线 |
| Kitex + Netpoll | Go | epoll Reactor + mux 多路复用 | sequence id | Thrift / Protobuf | goroutine 池 + Reactor | 熔断/限流/重试/追踪全套 | 高性能 Go RPC，零拷贝 LinkBuffer |
| SOFA RPC / Bolt | Java | TCP 22/20B 定长头 | requestId | Hessian（可扩展） | IO 线程 + 业务线程池分离 | 多注册中心/多 LB/预热 | 定长头自带 codec + timeout + FailFast |
| Cap'n Proto | 多语言 | 自定义编码 + 分帧 | - | 指针式零拷贝 | - | - | 序列化领域的零拷贝参考系 |

## 3. 重点框架深度分析

### 3.1 gRPC —— 生态最全，HTTP/2 多路复用范本

- **传输**：一次 RPC = 一个 HTTP/2 stream，单连接多路复用；GOAWAY 优雅摘流（server 关连接前告知最后一个已接受 stream，新请求客户端视为 UNAVAILABLE 可重试到别处，已接受的可完成）。
- **分帧**：每消息 5 字节前缀 = 1B 压缩标志 + 4B 大端长度（与 V1 的 Length-Prefix 同源，多携带了压缩位）。
- **状态与错误分离**：`grpc-status` 放在 Trailers 里，业务数据和错误码分离，错误可有结构化 detail。
- **超时**：默认无 deadline（官方明确要求生产必须显式设置）；deadline 走 header 传服务端；**deadline 传播用「已扣除耗时的剩余时间」而非绝对时间**；客户端 deadline 到期后服务端自动取消调用（业务逻辑需自行轮询检查）。
- **重试纪律（A6 提案）**：默认不重试；可配 retryPolicy / hedgingPolicy；**透明重试**只发生在「请求没离开客户端」或「stream 以 REFUSED_STREAM / GOAWAY(last-id < 本 id) 结束」（即服务端应用逻辑确定没见过该请求）时，不计入次数；**committed 判定**：收到 Response-Headers 或发送缓冲溢出后不可再重试 → 官方要求 server 尽量延迟发 Response-Headers。
- **重试防风暴**：`retryThrottling` 令牌桶（失败-1 / 成功按比例 +，maxTokens=10）；server 可用 `grpc-retry-pushback-ms` 显式要求推迟重试。
- **连接状态机**：channel 五态 IDLE / CONNECTING / READY / TRANSIENT_FAILURE / SHUTDOWN，重连指数退避，无活动时退回 IDLE 回收。
- **线程模型（C++，已核实）**：同步 API 由框架内置线程池调度，官方明确 handler 会被多线程并发调用（用户只需保证线程安全）；异步 API 以 **CompletionQueue** 为核心，`RequestXxx` 绑定 CQ + 唯一 tag，用户线程 `cq->Next()` 轮询事件按 tag 分派（官方示例为单 CQ 单线程循环，用户可开多线程各跑一个 CQ）。gRPC core 另有 polling engine（epoll 等）驱动底层 IO。
- **坑**：默认无超时无重试（避免库层擅自重试破坏幂等）；keepalive 间隔过频会被 LB 误判。

### 3.2 bRPC —— C++ 内网高吞吐标杆，与 V1 技术路线最近

- **线程模型（bthread）**：M:N 用户态线程（M bthread 映射到 N pthread），work-stealing 调度 + butex（bthread 与 pthread 可互相等待唤醒）；不是 N:1 协程（协程一个慢函数卡住全部）。server 端没有 IO 线程/处理线程之分，每请求起一个 bthread，天然按负载调节并发。
- **协议（baidu_std）**：12B 定长头（4B `PRPC` magic + 4B 包体长度 + 4B 元数据长度）；包体 = protobuf 元数据 + 数据 + **附件**（attachment 绕过序列化传大二进制）；元数据含 `correlation_id`（请求方唯一、响应原样带回）、`service_name/method_name/log_id`（64 位日志追踪 id）、`error_code/error_text`。元数据预留扩展字段号（100=Hulu、101=Sofa），跨实现协议演进的干净做法。
- **单连接多路复用**：默认单连接，连接上同时挂多个请求，响应可乱序；correlation_id + 进程级映射 O(1) 路由到 Controller（不需要全局哈希表）。三模式定量对比：短连接（qps×latency 个连接、1.5RTT+处理）< 连接池（qps×latency、1RTT）< 单连接（1 个连接、1RTT、可合并写出、CPU 最低）。
- **超时**：区分 `timeout_ms`（RPC 总超时，deadline 语义，到点即结束、超时后不重试）与 `connect_timeout_ms`（连接超时，默认 200ms，必须 < timeout_ms）。**默认值已核实为 500ms**（`src/brpc/channel.cpp` 构造函数 `timeout_ms(500)` 定案；中文文档正文写「默认值1秒」与 FAQ/源码矛盾，系文档错误）。
- **重试**：默认 max_retry=3，触发条件四条 AND（连接出错 / 没到超时 / 有剩余次数 / 错误值得重试），**默认只在连接出错时重试**；`backup_request`（默认关）在 `backup_request_ms` 内未返回就发第二个请求，实测 backup_request_ms=2ms 覆盖 95.5% 请求、10ms 覆盖 99.99%。
- **熔断**：默认连接级（连不上即熔断）一直开启；可选错误率熔断 = EMA 平滑 latency 计算错误代价 + 长短双窗口 + 熔断后隔离期 100ms 起指数翻倍（上限 30s）+ 恢复期健康检查。
- **雪崩治理**：集群全部不可用时的恢复期限流（接受请求概率 = q / min_working_instances），避免单台恢复后被瞬间打挂再熔断的死循环。
- **坑**：RPC 超时小于连接超时时熔断永不触发；同步调用持 pthread 锁会死锁；异步调用对象生命周期必须在 done 里释放；单连接 + VIP 会流量热点；激进重试对有限冗余集群造成 2-3 倍压力。

### 3.3 Dubbo / Dubbo3 —— Java 微服务治理生态最完整

- **协议**：2.x 默认 dubbo 协议（单一长连接 + NIO 异步 + hessian2，**不适合大包**，单连接经验上限约 7MB/s）；Dubbo3 默认 Triple（HTTP/2 + protobuf，**100% 与 gRPC 互通**，另有一套 HTTP 子协议让浏览器可直连）。**dubbo 协议头已核实（ExchangeCodec 源码，16 字节）**：`magic(0xdabb, 2B) + flag(1B) + status(1B，响应) + requestId(8B long) + data length(4B，仅 body)`，flag 高 3 位为 request/twoway/event，低 5 位为序列化 id。
- **集群容错（分场景族）**：`failover`（默认，重试其他机器，适合幂等读）、`failfast`（只调一次，非幂等写）、`failsafe`（异常忽略，审计类）、`failback`（后台定时重发，消息类）、`forking`（并行调 forks 个，一个成功即返回）、`broadcast`（广播）。
- **负载均衡**：random（默认加权随机）、roundrobin（**坑：慢提供者累积请求**）、leastactive（最少活跃）、consistenthash（160 虚拟节点）。
- **序列化兼容矩阵**：官方给出 hessian2 兼容规则表（属性增减正常、枚举多用新值抛异常、类型冲突抛异常），是序列化演进纪律的工程化清单。
- **注册中心**：ZooKeeper 树模型 + ephemeral 节点 + watch；阿里内部实际用自研数据库注册中心（ZK 只是桥接实现）。
- **坑**：roundrobin 慢机器累积；单连接大包瓶颈；hessian2 枚举/类型兼容陷阱。

### 3.4 Thrift —— IDL 与紧凑编码范本

- **四层栈**：Server → Processor（代码生成）→ Protocol → Transport；协议层流式不分帧，粘包问题在 Transport 层用 framed transport 解决。**长度前缀已核实为 4 字节（int32 大端）**（`TBufferTransports.{h,cpp}` 源码：`htonl` 写帧长、`ntohl` 读帧长）。
- **TCompactProtocol**：消息头 1B（protocol id 0x82 + 3 位消息类型 + 5 位版本）+ varint seq id；**字段头短格式 1B = 4 位字段号增量 + 4 位类型**，比 protobuf 的 tag 还省；整数 zigzag + varint；字段按 id 而非名字标识，未知字段可跳过。
- **Server 类型（已核实，Java 源码 javadoc）**：`TSimpleServer`（单线程阻塞，测试用）→ `TThreadPoolServer`（阻塞 + 线程池）→ `TNonblockingServer`（非阻塞单 selector 线程，**必须配 framed transport**）→ `THsHaServer`（半同步半异步：非阻塞 IO + 业务线程池，也必须配 framed）。
- **演进纪律**：struct 增减字段不影响旧客户端（整数 id）；IDL 编号不可变。
- **坑**：不支持继承/多态/异构容器；bool 字段编码历史上 1/2 两种值并存（兼容 bug）；字段类型冲突时行为未定义。

### 3.5 其余框架简述

- **tRPC**（腾讯）：16B 定长头（2B magic `0x930` + 帧类型 + 总大小 + 包头大小 + request_id + **第 15-16 字节已核实为保留字段**——trpc-cpp 实现 `char reversed[2]`，版本号实际在 16B 头之后的 protobuf 包头里；英文文档写「15 字节 = version」与实现不符）；包头为变长 protobuf，自带 `timeout(ms)`、caller/callee、func、`trans_info` 全链路透传 map（`trpc-`/`app-` 前缀分区）、content_type/encoding、attachment_size。官方明确「不用 HTTP/2 的原因是性能」。
- **Kitex + Netpoll**（字节）：epoll Main+Sub Reactor，选 **LT** 而非 ET（更及时、容错更高）；Nocopy LinkBuffer 链式缓冲；mux = 虚拟连接 + sequence id + 共享 map；序列化预计算长度 + 一次 malloc + SIMD 优化。坑：SubReactor 是 goroutine，Go 调度导致 p99 尖峰——「事件循环与业务调度耦合」的经典教训。
- **SOFA Bolt**（蚂蚁）：请求头 22B / 响应头 20B 定长二进制头，自带 `requestId`、`codec` 号、`timeout`、`classLen/headerLen/contentLen` 三段长度；服务端排队超过客户端超时时 FailFast 丢弃（oneway 除外）；默认 IO 线程与业务线程池分离。**SPI 机制已核实（ExtensionLoader 源码）**：可扩展接口标 `@Extensible`，实现类标 `@Extension(alias/code/order/override)`，按接口全名从 `META-INF/services/sofa-rpc/` 加载 `alias=className` 文本行，同别名按 `order()` + `override()/rejection()` 裁决覆盖，`singleton` 默认 true 走双检锁单例缓存。
- **Cap'n Proto / FlatBuffers**：Cap'n Proto 64 位 word 对齐 + 指针式布局（struct 指针 = 定长 data 区 + pointer 区分离），读写直接在缓冲上、无 parse 步骤；默认值 = 全零 → **新字段落在 padding 里时旧数据自动兼容**；惰性指针校验 + traversal limit（默认 64MiB）+ 深度限制防恶意输入。**FlatBuffers 机制已核实（官方 internals 文档）**：buffer 开头 `uoffset_t`(32bit) 指向 root table；table 首字段是 `soffset_t`(有符号 32bit) 指向 vtable（对象起始地址**减去**该值定位 vtable）；vtable 元素为 `voffset_t`(16bit)，首两元素为 vtable 大小与对象大小，其余为各字段偏移，偏移越界或为 0 即字段不存在返回默认值；buffer 从高地址向低地址反向构造。坑：零拷贝不是免费的（连续内存 resize 留洞不可回收）；只能 getter/setter 访问体验差。
- **JSON-RPC 2.0 / Motan**：轻量协议代表；`id` 关联 + Notification（无 id 不回包）+ Batch；Motan 是 Java 服务化时代轻量治理型代表（无 IDL 强约束）。

## 4. 论文要点提炼

### 4.1 Birrell & Nelson, *Implementing Remote Procedure Calls*（ACM TOCS 1984）

- 五层结构：user → user-stub → RPCRuntime → server-stub → server；stub 打包/解包，RPCRuntime 负责重传、ack、路由——**传输可靠性与业务隔离**。
- **调用标识**：机器 ID + 进程 ID + 序列号，用于调用方匹配结果（排除迟到旧响应）、被调方消除重传重复包。
- **失败模型**：调用方无法区分「请求未到达 / 到达已执行 / 执行完响应丢失」——结果是「执行了一次或一次都没执行，用户不知道是哪种」。超时后不能一律按失败处理，否则无法判断能否重试。
- **绑定（binding）**：接口名 = 抽象接口 + 具体实例，经 Grapevine 目录服务定位（注册中心原型）；绑定带唯一标识、每次调用校验，exporter 崩溃重启后绑定隐式失效。
- **启示**：V2 消息头必须加 request_id + 消息类型（同时解决响应匹配与服务端重传去重）；API 层区分三类结果 `OK / FAILED / UNKNOWN`；幂等性作为重试前提。

### 4.2 Dean & Barroso, *The Tail at Scale*（CACM 2013）

- **尾延迟随 fan-out 指数放大**：单机 p99=1s、100 台并行依赖时 63% 用户请求超 1s（`1−(1−p)^N`）；只看平均延迟完全失真，必须监控 p99/p99.9。
- 延迟可变性来源约 8 种，大多不可消除，只能「容忍」。
- **缓解手段**：hedged requests（首请求超过该类 **95 分位**预期延迟后发备份请求，额外负载约 5%）、tied requests（多台入队互通知、先处理者取消其余）、更多微分区/细粒度负载均衡、热点分区选择性提高复制因子、probation（慢机器临时踢出 + 影子请求测速以便回归）、good-enough responses（收集到足够比例响应即返回部分结果）、canary requests（高 fan-out 先发 1-2 个探路）。
- **启示**：超时基于剩余 deadline（整体 deadline 传播，每跳扣已耗时）；超时值定在延迟分布分位之上；监控含 p99 与超时率；hedged 前提是服务端幂等或去重；熔断踢出不是终点，要留影子探测。
- **核实注记**：① 此前误传的「coordinated omission（协调省略）」**不是这篇论文的内容**——那是压测延迟测量方法论的概念（Gil Tene 的 HdrHistogram 生态，2015），描述压测客户端闭环行为导致 p99 失真；② 「keep timeouts short」逐字表述在论文中未找到，对慢请求尽早放弃的机制是**对冲请求按 95 分位阈值触发**；③ 英文原版 PDF（Google/CACM 直链）本网络仍不可达，以上以 Morning Paper 原文引用 + 多份全文级中文译文交叉核实。

### 4.3 Google, *Dapper: a Large-Scale Distributed Systems Tracing Infrastructure*（2010）

- trace = 跨服务活动树，64-bit `trace_id`；span 带 `span_id` / `parent_id`；**上下文绑定线程、跨异步回调和 RPC 显式传播**。
- **结构信息必须由框架（RPC 层）自动产生**，不能依赖业务手工埋点（覆盖率必然不达标）。
- 采样按 trace 整树，**已核实原文（Dapper §4.4）**：首个生产版本统一采样概率为「每 1024 个候选取 1 个 trace」（one sampled trace for every 1024 candidates），当时正在部署自适应采样；Table 2 给出 1/1024 采样的实测开销为平均延迟 **−0.20%**、吞吐 **−0.06%**；高流量服务采样率可低至 **0.01%**。收集与请求路径异步解耦（本地日志 → daemon 拉取 → 汇聚）。
- **启示**：trace_id/span_id 内建在消息头、由框架自动生成透传；Reactor 回调链异步，trace 上下文要显式放进任务对象（不依赖 thread-local）；低采样率整树采样；错误日志带 trace_id。

### 4.4 ZooKeeper / Chubby —— 注册中心背景

- **设计哲学**：服务端只提供 wait-free 协调内核（文件系统式 znode），锁/选举/组员等原语由客户端构建。
- **两个关键原语**：**ephemeral 节点**（会话结束自动删除 → 故障感知）+ **watch**（一次性触发、与 session 绑定，只告知「变了」不带新值）。
- **故障判定基于会话超时**（超时未收到任何东西即判死），不依赖 TCP 断开事件。
- **条件更新**：setData/delete 带期望 version，版本不符失败（乐观并发控制）。
- **启示**：V2 服务发现只需 ephemeral + watch 两个原语（对接 etcd/zk 直接消费；自研注册中心也只实现 put/get/watch + 租约）；watch 一次性 → 必须「注册 → 事件 → 重新注册」循环并处理连接丢失事件；用版本 CAS 防并发覆盖。

### 4.5 Ongaro & Ousterhout, *Raft*（2014）—— 背景知识

- 三个独立子问题：leader 选举 / 日志复制 / 安全性；strong leader + 心跳；多数派提交；term 单调。
- **启示（背景性质）**：注册中心直接用现成 Raft/ZAB 系统（etcd/Consul/ZooKeeper），不要自研共识；「把协议拆成独立可验证子问题」的哲学适用于 RPC 框架——V2 应把连接管理、请求匹配、超时重试、服务治理拆成独立可单测模块。

## 5. 博客与工程实践要点

- **线程模型谱系**（brpc）：连接独占线程（C10K 根源）→ 单线程 reactor（libevent，单核）→ 多线程 reactor（多数 RPC 框架，但受 cache 一致性限制，粗糙实现 24 核可能不如精致单线程）→ M:N 协程（并发度是软限）。**线程数是硬限**：`最大 qps = 线程数 / 平均延时`，下游打满时退化为 `线程数 / 超时`，qps 断崖 3-4 倍。
- **协议头为什么要有这些字段**（brpc new_protocol）：magic（前 4B 即可判定协议，magic 在中间则无法快速失败）；correlation_id（多路复用配对依据）；status/error_text（业务错误走协议层）；log_id（64 位贯穿日志 = 链路追踪最简形态）。**协议 ≠ 序列化**（协议还含长度/校验/magic/多包组装）。
- **定长头 vs 变长头**：定长头 O(1) 解析、可流式增量；长度域语义必须写死（「长度=包体」还是「=整包」，Netty 用 lengthAdjustment 补偿两种口径）。
- **序列化**：protobuf 快而紧凑、有 schema、前后兼容，但不适合几 MB 以上大对象；msgpack 动态类型、无 schema（官方承认复杂模型支持弱，嵌套自定义类型反序列化麻烦）；自研二进制可控但兼容性/工具链全靠自己；大二进制走附件通道绕过序列化；压缩按需（snappy 对 128B 也有 ~0.75µs 开销，小包别压）。**性能排序已用权威基准核实**（alecthomas/go_serialization_benchmarks）：gogo/protobuf 与 msgp 最快档（~191ns/op）、encoding/json 1679~2612ns、gob 最慢（6683~33482ns）。
- **连接管理**：内网主推单连接多路复用（1RTT、可合并写、CPU 最低）；粘包半包三件套（定长帧 / 分隔符 / 长度域）；**TCP keepalive 默认参数已核实（man7 tcp(7)）**：`tcp_keepalive_time=7200s`（2 小时空闲才探测）+ `intvl=75s` + `probes=9`，即死连最长约 2h11min 才被发现——RPC 必须应用层心跳，且心跳周期 < 空闲清理阈值、空闲判定要排除心跳流量；连接池大小与并发不匹配会退化成长短连接抖动。
- **超时重试**：超时 = deadline 语义（brpc 点名批评「单次超时 × 次数」双层模型）；重试默认只对连接错误；退避 = 指数 + full jitter（AWS：`random × min(上限, base × 2^n)`）防雷鸣群；令牌桶配额防无限重试；hedged request 治长尾（用 latency_cdf 选 backup_request_ms）。
- **熔断限流**：Hystrix 状态机 CLOSED → OPEN → HALF-OPEN（睡眠窗后单请求试探）；Sentinel 滑动窗口 LeapArray + minRequestAmount 防低流量误熔断 + Warm Up 冷启动斜率放量 + 匀速排队（漏桶）；**限并发优于限 QPS**（并发 = 极限 QPS × 低负载延时，Little's law），超限立刻回错而不是排队（让客户端去重试别的节点）。
- **背压**：写路径非阻塞 + 排队 + 后台续写（brpc KeepWrite 线程）；高/低双水位避免抖动（Netty WRITE_BUFFER_WATER_MARK）；流式 max_buf_size 让发送速率被接收消费速率钳制；大消息会队头阻塞，需切割。
- **优雅关闭**：两阶段 Stop()/Join()——先拒绝新请求（回明确错误码 ELOGOFF，而非直接 close），等在途请求计数归零，超时强退（Dubbo 默认 10s）；「退不掉」通常是引用计数/在途计数被泄漏（正是 weak_ptr 场景），需要显式 SetFailed 主动失效。
- **可观测**：histogram 优于 summary（summary 分位数不可跨实例聚合）；桶边界要覆盖目标分布；最小指标集 = qps + 延时分布 + 错误率 + 熔断计数；**traceId 跨线程丢失是真实线上事故，已核实原文**（美团技术《一次「找回」TraceId 的问题分析与过程思考》，SegmentFault 官方转载全文：线上告警 → 定位 `@Async` 方法 → 异步线程 TraceId 为 null → 根因 ThreadLocal 不跨线程传播），异步切换点必须显式传。
- **高性能细节**：ET 读必须循环到 EAGAIN（漏事件是经典坑），ET 写 EAGAIN 必须重新挂 EPOLLOUT；批处理（一次读拆多帧、writev 合并写）；Disruptor 无锁环形队列（伪共享用 padding 解决，快 4 倍）；内存池 thread-local 等长池 + 版本号防 ABA（brpc ObjectPool，bthread 创建 <200ns），但官方反对滥用；io_uring SQPOLL 可免系统调用但「不是无脑 go faster 开关」，网络小包场景收益有限需压测。**唤醒通道阻塞已核实权威出处**：eventfd(2) 与 pipe(7) man 手册原文——写满时阻塞、非阻塞下返回 EAGAIN；muduo `EventLoop.cc` 建 eventfd 用 `EFD_NONBLOCK | EFD_CLOEXEC`（本项目 V1 也踩过同类坑，见 `docs/verification.md`）。**零拷贝数字已核实**：传统 IO 4 拷贝（2 DMA + 2 CPU）+ 4 次切换；mmap 省一次用户态拷贝 → 3 拷贝；sendfile Linux 2.4 起 2 拷贝（全 DMA）。

## 6. 值得借鉴清单

| # | 来源 | 借鉴点 | 落到 V2 阶段 |
| --- | --- | --- | --- |
| 1 | bRPC / tRPC / SOFA | 定长头 + 变长元数据 + 附件三段式协议 | V2.0 协议头 |
| 2 | gRPC / bRPC / Netpoll | 单连接多路复用：request_id 乱序配对、O(1) 响应路由 | V2.0 客户端 |
| 3 | gRPC / SOFA | deadline 传播用剩余时间；服务端排队超时 FailFast | V2.0 超时 |
| 4 | Birrell & Nelson | 三分结果码 OK/FAILED/UNKNOWN；幂等作为重试前提 | V2.0 客户端 + V2.1 重试 |
| 5 | gRPC A6 | 透明重试判定（请求未被处理过的证据）+ 重试令牌桶限流 | V2.1 重试 |
| 6 | bRPC | backup/hedged request 治长尾（>95 分位触发） | V2.1 重试 |
| 7 | bRPC / Sentinel | EMA 错误代价 + 长短双窗口熔断 + 半开探测 | V2.1 熔断 |
| 8 | bRPC | 集群恢复期限流（q/min_working_instances）防雪崩死循环 | V2.1 熔断 |
| 9 | ZooKeeper | ephemeral + 一次性 watch + 会话超时判死 + 版本 CAS | V2.1 注册发现 |
| 10 | Dubbo | 分场景容错族（failover/failfast/...）与 LB 策略集合 | V2.1 负载均衡 |
| 11 | Thrift compact / Cap'n Proto | 字段号增量编码；默认值 = 全零 → 新字段落 padding 自动兼容 | V2.0 序列化 |
| 12 | bRPC / Dubbo | 服务端 max_concurrency 限并发超限快速失败（Little's law） | V2.1 限流 |
| 13 | Netty / brpc | outputBuffer 高/低水位 + 慢消费者断开 | V2.2 背压 |
| 14 | brpc / Dubbo / K8s | 两阶段优雅关闭：先摘流量再等在途计数、超时强退 | V2.1 关闭 |
| 15 | Dapper / 美团 | trace_id/span_id 内建消息头、框架自动产生、显式跨线程传递、整树低采样 | V2.2 可观测 |
| 16 | Prometheus | histogram 桶聚合优于 summary；指标最小集 | V2.2 可观测 |
| 17 | bRPC | M:N 协程调度（work-stealing + butex）替代业务线程池 | V2.2 性能 |
| 18 | Netpoll / brpc | ET 读空到 EAGAIN、批量拆帧、writev 合并写、线程本地内存池 | V2.2 性能 |
| 19 | Cap'n Proto | 零拷贝只在减少拷贝路径（缓冲拼接），不做 sendfile 大改 | V2.2 性能 |
| 20 | bRPC / Thrift | 协议识别（magic 前置 4B 快速失败）+ 版本号 + 保留扩展字段号 | V2.0 协议头 |

## 7. 需要规避的坑

| # | 来源 | 坑 | V2 规避设计 |
| --- | --- | --- | --- |
| 1 | brpc | RPC 超时 < 连接超时 → 熔断永不触发 | 强制 `connect_timeout < timeout`，文档化 |
| 2 | brpc / 各家一致 | 重试放大 → 雪崩（有限冗余集群承受 2-3 倍压力） | 默认只重试连接错误 + 次数上限 + jitter 退避 + 令牌桶 |
| 3 | Dubbo | 单连接大包瓶颈（约 7MB/s 经验上限） | 大消息走附件/单连接模式按场景选择；max_body_size 拒收 |
| 4 | gRPC A6 | 收到响应头后再重试不安全 | committed 判定：响应头/发送溢出后禁重试 |
| 5 | brpc | 同步调用持 pthread 锁死锁；异步回调生命周期泄漏 | 文档纪律 + RAII done 回调；锁用非阻塞/协程友好锁 |
| 6 | Cap'n Proto / Kitex | 零拷贝 resize 留洞不可回收；无中间结构体验差 | 预计算长度 + 链式缓冲拼接，不全盘零拷贝 |
| 7 | Dubbo roundrobin | 慢提供者累积请求 | leastactive / p2c 替代 |
| 8 | ZooKeeper | watch 一次性，事件丢了不知道 | 重新注册循环 + 连接丢失事件触发全量刷新 |
| 9 | brpc | 集群恢复时单台瞬间吃满流量再熔断 | 恢复期限流 |
| 10 | Hystrix | 超时线程无法中断，池被「已超时还在跑」占满 | 服务端 deadline + 业务可中断；线程池隔离 bulkhead |
| 11 | Sentinel | 低流量误熔断（minRequestAmount 缺失） | 最小请求量阈值 |
| 12 | Netty / brpc | 无背压 → 慢消费者撑爆内存；ET 写 EAGAIN 忘挂 EPOLLOUT | 高/低水位 + 超限断连；写路径状态机 |
| 13 | brpc / K8s | 优雅关闭「退不掉」（引用计数不清零） | 显式 stopping 标志 + SetFailed + 在途计数 + 超时强退 |
| 14 | 美团真实事故 | traceId 跨线程丢失（ThreadLocal 不跨线程） | trace 上下文显式放进任务对象 |
| 15 | 心跳 vs 空闲清理 | 心跳计入流量 → 空闲清理失效；阈值 < 心跳周期 → 误清 | 心跳周期 < 空闲阈值；idle 判定排除心跳 |
| 16 | brpc / Linux | epoll ET 漏事件（缓冲没读空）；wakeup 通道阻塞 | 循环读到 EAGAIN；eventfd 非阻塞 + 写满容忍 |
| 17 | Thrift | 布尔值编码兼容 bug；字段类型冲突行为未定义 | 自研序列化：规范写死类型编码 + 未知字段跳过 |
| 18 | protobuf / hessian | 字段号删除重用破坏兼容 | 字段号 reserve 纪律 + 兼容矩阵文档化 |
| 19 | brpc | 连接池大小与并发不匹配 → 频繁建连退化 | 连接池容量按并发估算 + 空闲回收 + 延迟关闭 |
| 20 | SOFA | oneway 不保证送达，调用方无感知 | oneway 仅用于可丢场景，明确文档 |

## 8. 对 V2 的总体建议

综合三路调研，对 V2 的结论可以浓缩为一条主线：

> **V1 的技术选型（Main-Sub Reactor + ET + Length-Prefix + 业务线程池 + 非阻塞写）与工业级框架核心思路一致，方向正确。V2 不需要推倒重来，而是按「协议闭环 → 治理 → 性能/可观测」三层补全。**

三个最值得优先补强的方向（对应调研结论）：

1. **协议与语义闭环（V2.0）**：把 Length-Prefix 升级为带 `magic/version/msgType/request_id/method_id/status/body_len` 的定长头 + 自研 tag-based 序列化 + 单连接多路复用 + deadline 超时 + C++ 客户端 stub。这是从「分帧原型」到「可用 RPC」的分水岭，也是后续所有能力的地基（借鉴 #1/#2/#3/#4/#11/#20）。
2. **防雪崩与治理（V2.1）**：服务端 max_concurrency + 客户端节点级熔断（EMA 滑动窗口 + 半开）+ 保守重试（透明重试判定 + jitter + 令牌桶）+ 注册发现（ephemeral + watch）+ 分场景负载均衡。V1 当前业务线程池无上限保护，这是最直接的生产风险（借鉴 #5/#6/#7/#8/#9/#10/#12/#14）。
3. **性能与可观测（V2.2）**：M:N 协程调度、批量读写、内存池、outputBuffer 高水位背压、trace_id 链路追踪、histogram 指标、两阶段优雅关闭（借鉴 #13/#15/#16/#17/#18/#19）。

贯穿所有阶段的两条纪律（所有框架一致强调）：**重试必须考虑幂等**；**超时是 deadline 语义**。

详细设计见 `docs/v2-design-draft.md`。

# MsgBus 性能基准测试报告

[English](../en/BENCHMARK.md)

## CI 自动化压测

性能压测仅在 **tag 发布/预发布** 时运行（推送 `v*` tag 触发），普通提交和 PR 不执行压测以节省 CI 资源。

发布时会在以下环境自动运行压测：

| 平台 | 编译器 | Runner |
|------|--------|--------|
| Windows | MSVC (latest) | `windows-latest` |
| Linux | GCC 13 | `ubuntu-latest` |
| Linux | Clang 17 | `ubuntu-latest` |
| macOS | Apple Clang | `macos-latest` |

压测结果会作为 artifact 附在 [GitHub Release](../../releases) 页面（`benchmark-*.txt`）。

> **注意**：CI 环境为共享虚拟机，性能数据存在波动，仅用于回归检测和量级参考，不代表裸金属性能。

## 本地运行压测

```bash
cmake -B build -S . -DCMAKE_CXX_STANDARD=20
cmake --build build --config Release
./build/tests/Release/msgbus_bench      # Windows
./build/tests/msgbus_bench              # Linux / macOS
```

---

## 本地参考数据（Windows）

### 测试环境

- **OS**: Windows 10 (19045)
- **编译器**: MSVC 19.42 (Visual Studio 2022)
- **构建配置**: Release (`/O2`)
- **C++ 标准**: C++20
- **优化**: 对象池 + 侵入式引用计数 + TopicId 整数路由（替代 std::string topic）

---

## 基准测试项目

### 1. Raw Queue — 无锁队列裸吞吐

**场景**：1 生产者 / 1 消费者，纯 `LockFreeQueue<int>` 操作，无消息包装、无 topic 路由。

**目的**：测量队列本身的极限吞吐，作为系统上限参考。

| 指标 | 结果 |
|------|------|
| 消息数 | 1,000,000 |
| 耗时 | ~14 ms |
| 吞吐量 | **~71,000,000 ops/s** |

---

### 2. Throughput — 单发单收端到端吞吐

**场景**：1 个 publisher 线程，1 个 subscriber handler，完整 publish→queue→dispatch→handler 路径。

| 指标 | 结果 |
|------|------|
| 消息数 | 1,000,000 |
| 耗时 | ~210 ms |
| 吞吐量 | **~4,700,000 QPS** |

**分析**：相比 Raw Queue 的开销主要来自：
- 对象池 acquire/release（无锁，开销很小）
- `TypedMessage<T>` 构造或 reset
- TopicRegistry resolve（shared_lock + 哈希查找）
- topic 整数键哈希查找 + shared_mutex 读锁
- `std::function` 调用
- 侵入式引用计数原子操作

> 对比优化前（shared_ptr）：吞吐量基本持平，但延迟显著降低（见 Latency 测试）。

---

### 3. Multi-producer — 多生产者吞吐

**场景**：4 个 publisher 线程并发发布，1 个 subscriber。

| 指标 | 结果 |
|------|------|
| 线程数 | 4 |
| 每线程消息 | 250,000 |
| 总消息 | 1,000,000 |
| 耗时 | ~230 ms |
| 吞吐量 | **~4,400,000 QPS** |

**分析**：多线程 CAS 竞争导致约 20% 的吞吐下降，符合预期。Vyukov 算法在多生产者场景下仍保持良好性能。

---

### 4. Latency — 发布到处理延迟

**场景**：1 个 publisher，1 个 subscriber，逐条测量 publish 时刻到 handler 执行时刻的延迟。包含 1000 条预热消息。

| 百分位 | 当前 (μs) | 对象池优化后 (μs) | shared_ptr 原始 (μs) |
|--------|----------|------------------|---------------------|
| **min** | **0.6** | 0.3 | 36.2 |
| **p50** | **374** | 2,196 | 3,582.7 |
| **p90** | **480** | 4,977.8 | 4,977.8 |
| **p99** | **773** | 5,796.2 | 5,796.2 |
| **max** | **895** | 5,910.4 | 5,910.4 |

**分析**：

- **最小延迟 ~0.6μs**，得益于对象池复用消除堆分配 + 侵入式引用计数
- **p50 从 ~2.2ms 大幅降至 ~374μs**（~6x 改善），TopicId 整数路由消除 dispatch 路径上的字符串哈希/比较开销
- p90/p99 同步显著下降，表明整体排队时间缩短
- 在实际应用中（publish 频率较低），延迟会接近 min 值
- 三级退避策略在空闲时会增加延迟，但显著降低 CPU 占用

---

### 5. Multi-topic — 多 Topic 吞吐

**场景**：100 个不同 topic，每个 topic 发 10,000 条消息。

| 指标 | 当前（TopicId） | 对象池优化后 | shared_ptr 原始 |
|------|-----------------|-------------|----------------|
| Topic 数 | 100 | 100 | 100 |
| 总消息 | 1,000,000 | 1,000,000 | 1,000,000 |
| 耗时 | ~135 ms | ~172 ms | ~202 ms |
| 吞吐量 | **~7,300,000 QPS** | ~5,800,000 QPS | ~4,950,000 QPS |

**分析**：TopicId 优化在多 topic 场景下收益最大（**+26% QPS**），因为 dispatch 路径上 100 个 topic 的哈希查找从 `std::string` 键变为 `uint32_t` 整数键，显著降低了每次查找的开销。累计对比 shared_ptr 原始版本提升 **+47%**。

---

### 6. Fan-out — 广播扩散

**场景**：1 个 publisher，N 个 subscriber 订阅同一 topic。

#### 10 个订阅者

| 指标 | 结果 |
|------|------|
| 消息数 | 100,000 |
| 订阅者数 | 10 |
| 总投递数 | 1,000,000 |
| 耗时 | ~33 ms |
| 投递速率 | **~30,000,000 deliveries/s** |

#### 100 个订阅者

| 指标 | 结果 |
|------|------|
| 消息数 | 10,000 |
| 订阅者数 | 100 |
| 总投递数 | 1,000,000 |
| 耗时 | ~11 ms |
| 投递速率 | **~88,000,000 deliveries/s** |

**分析**：Fan-out 场景下投递速率反而更高，因为：
- 每条消息的 dequeue + topic 查找开销被多个 handler 调用摊薄
- 订阅者列表在内存中连续（vector），顺序遍历缓存友好
- `std::function<void(const T&)>` 调用开销很小
- 侵入式引用计数：Fan-out 场景下消息被多个 handler 共享，侵入式引用计数的原子操作比 shared_ptr 更轻量

---

## 性能汇总

| 测试场景 | QPS / 速率 | 说明 |
|----------|-----------|------|
| Raw Queue | ~71M ops/s | 队列极限吞吐 |
| 单发单收 | ~4.7M QPS | 端到端完整路径 |
| 4 生产者 | ~4.4M QPS | MPMC 并发 |
| 100 Topic | **~7.3M QPS** | TopicId 整数哈希路由（+26%） |
| Fan-out ×10 | ~30M del/s | 广播投递 |
| Fan-out ×100 | ~88M del/s | 高扇出投递 |
| 延迟 min | **~0.6 μs** | 对象池复用 |
| 延迟 p50 | **~374 μs** | TopicId 优化（优化前 ~2.2ms） |

> 以上为本地 Windows 环境参考数据。发布版本的各平台数据请在 [Releases](../../releases) 页面查看。

---

## Zero-copy 优化效果

### 优化内容

| 项目 | 优化前 | 优化后 |
|------|--------|--------|
| 消息指针 | `shared_ptr<IMessage>` | `MessagePtr`（侵入式引用计数） |
| 内存分配 | 每条 `new TypedMessage` + control block | 对象池 acquire/release，池命中时零分配 |
| 回收方式 | `shared_ptr` 析构 → `delete` | recycler 函数指针 → 对象池回收复用 |
| 引用计数 | 独立 control block 原子操作 | 嵌入 IMessage 的 `atomic<int>` |
| Topic 路由键 | `std::string` 哈希/比较 | `uint32_t` TopicId 整数哈希 |
| Topic 存储 | 每条消息携带 `std::string` | 每条消息仅携带 `uint32_t` |

### 效果

| 指标 | 改善 |
|------|------|
| 最小延迟 | 36μs → 0.6μs（**~60x**） |
| p50 延迟 | 3.6ms → 374μs（**~10x**） |
| 多 Topic QPS | 4.95M → 7.3M（**+47%**） |

---

## 性能瓶颈分析

### 当前瓶颈

1. **单 Router 线程**（多 dispatcher 模式）：Router 是串行瓶颈，高吞吐时可能成为限制
2. **通配符匹配**：每条消息需遍历全部 wildcard_entries_ 做字符串匹配
3. **三级退避**：低负载时 sleep 阶段增加尾部延迟
4. **publish 路径 TopicRegistry resolve**：每次 publish 需 shared_lock + 哈希查找注册表

### 已完成优化

| 优化 | 收益 | 状态 |
|------|------|------|
| 对象池替代 `shared_ptr` | 最小延迟 60x | ✅ 已实现 |
| 多 dispatcher（topic 分片） | 消费端水平扩展 | ✅ 已实现 |
| TopicId 整数路由 | 多 Topic +26%，p50 延迟 ~6x | ✅ 已实现 |

### 潜在优化方向

| 优化 | 预期收益 | 复杂度 |
|------|---------|--------|
| 通配符 trie 索引 | 通配符匹配 O(1) | 中 |
| Batch dequeue | QPS +20~30% | 中 |
| 条件变量替代 sleep 退避 | 尾部延迟 ↓90% | 低 |

---

## 与同类方案对比（参考量级）

| 方案 | 典型 QPS | 无锁 | 协程 | 对象池 | 通配符 |
|------|---------|------|------|--------|--------|
| **MsgBus (本项目)** | ~7.3M | ✓ | ✓ | ✓ | ✓ |
| Boost.Signals2 | ~1-2M | ✗ | ✗ | ✗ | ✗ |
| Qt Signals/Slots | ~0.5-1M | ✗ | ✗ | ✗ | ✗ |
| moodycamel + 手动路由 | ~8-15M | ✓ | ✗ | ✗ | ✗ |
| Redis Pub/Sub (本地) | ~0.1-0.5M | ✗ | ✗ | ✗ | ✓ |

> 注：对比数据为经验量级，实际结果取决于硬件和负载。

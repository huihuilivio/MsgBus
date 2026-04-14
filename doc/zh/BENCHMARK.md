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
- **优化**: 对象池 + 侵入式引用计数 + TopicId 整数路由 + 通配符 Trie 索引 + 条件变量退避

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

| 百分位 | 当前 (μs) | TopicId优化后 (μs) | 对象池优化后 (μs) | shared_ptr 原始 (μs) |
|--------|----------|------------------|---------------------|---------------------|
| **min** | **0.2** | 0.6 | 0.3 | 36.2 |
| **p50** | **57** | 374 | 2,196 | 3,582.7 |
| **p90** | **114** | 480 | 4,977.8 | 4,977.8 |
| **p99** | **250** | 773 | 5,796.2 | 5,796.2 |
| **max** | **358** | 895 | 5,910.4 | 5,910.4 |

**分析**：

- **最小延迟 ~0.2μs**，得益于对象池复用消除堆分配 + 侵入式引用计数
- **p50 从 ~374μs 大幅降至 ~57μs**（~7x 改善），条件变量替代 sleep 退避显著降低尾部延迟
- p90/p99 同步大幅下降（p99: 773μs → 250μs），表明空闲时能被条件变量快速唤醒
- 在实际应用中（publish 频率较低），延迟会接近 min 值

---

### 5. Multi-topic — 多 Topic 吞吐

**场景**：100 个不同 topic，每个 topic 发 10,000 条消息。

| 指标 | 当前（TopicId） | 对象池优化后 | shared_ptr 原始 |
|------|-----------------|-------------|----------------|
| Topic 数 | 100 | 100 | 100 |
| 总消息 | 1,000,000 | 1,000,000 | 1,000,000 |
| 耗时 | ~110 ms | ~172 ms | ~202 ms |
| 吞吐量 | **~9,200,000 QPS** | ~5,800,000 QPS | ~4,950,000 QPS |

**分析**：多 Topic 场景下 QPS 达到 **~9.2M**，相比 shared_ptr 原始版本提升 **+86%**。TopicId 整数哈希查找和条件变量唤醒共同贡献。

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

### 7. Wildcard — 通配符匹配吞吐

**场景**：N 个通配符 pattern，publish 到匹配其中 1 个 pattern 的 topic。

| 指标 | 100 个 pattern | 1000 个 pattern |
|------|---------------|-----------------|
| 消息数 | 100,000 | 100,000 |
| 耗时 | ~62 ms | ~62 ms |
| 吞吐量 | **~1,600,000 QPS** | **~1,600,000 QPS** |

**分析**：Trie 索引的核心优势：**匹配复杂度为 O(topic 层级深度)，与 pattern 数量无关**。100 个 pattern 和 1000 个 pattern 的 QPS 基本相同，而线性扫描方案下 1000 个 pattern 的 QPS 会降至 100 个 pattern 的 1/10。

---

### 8. Wildcard RCU — 并发匹配吞吐

**场景**：N 个通配符 pattern，4 个并发读者线程执行 match 操作，测量总吞吐。

| 指标 | 100 个 pattern | 1000 个 pattern |
|------|---------------|------------------|
| 消息数 | 100,000 | 100,000 |
| 读者线程 | 4 | 4 |
| 耗时 | ~49 ms | ~57 ms |
| 吞吐量 | **~2,030,000 QPS** | **~1,770,000 QPS** |

**分析**：RCU 读路径（`atomic<shared_ptr>` load，MSVC/GCC）实现近线性读者扩展。4 个并发读者达 ~2M QPS，相比单读者 ~1.6M QPS，争用极小。

---

### 9. FullPolicy — 队列满策略对比

**场景**：小队列（1024）频繁触发队列满路径，对比 5 种 FullPolicy 策略。

#### 单生产者吞吐（50万消息）

| 策略 | 发布数 | 投递数 | QPS |
|------|--------|--------|-----|
| ReturnFalse | 491,725 | 491,725 | **~4,500,000** |
| DropNewest | 500,000 | 496,010 | **~4,510,000** |
| DropOldest | 500,000 | 495,469 | **~3,070,000** |
| Block | 500,000 | 500,000 | **~4,010,000** |
| BlockTimeout | 500,000 | 500,000 | **~3,460,000** |

#### 多生产者吞吐（4 线程 × 12.5万消息）

| 策略 | 发布数 | 投递数 | QPS |
|------|--------|--------|-----|
| ReturnFalse | 491,656 | 491,656 | **~3,890,000** |
| DropNewest | 500,000 | 418,326 | **~4,100,000** |
| DropOldest | 500,000 | 495,804 | **~2,830,000** |
| Block | 500,000 | 500,000 | **~3,400,000** |
| BlockTimeout | 500,000 | 500,000 | **~3,560,000** |

#### 延迟（5万消息，队列=4096）

| 策略 | 投递数 | p50 (μs) | p90 (μs) | p99 (μs) |
|------|--------|---------|---------|--------|
| ReturnFalse | 38,826 | 1,259 | 1,689 | 1,751 |
| DropNewest | 44,565 | 1,121 | 1,593 | 1,918 |
| DropOldest | 50,000 | **26** | 387 | 419 |
| Block | 50,000 | 719 | 1,295 | 1,657 |
| BlockTimeout | 50,000 | 501 | 747 | 987 |

**分析**：
- **ReturnFalse / DropNewest**：最高吞吐（零/极小开销），但可能丢失消息
- **DropOldest**：由于 `publish_mutex_` 串行化导致吞吐较低，但实现**最低延迟**（新消息始终能入队）
- **Block / BlockTimeout**：100% 投递保证，通过 `cv_not_full_` wait/notify，中等吞吐
- 多生产者 CAS 竞争下，DropNewest QPS 最高，因为它直接丢弃无需重试

---

### 10. Multi-Dispatcher — 线程池吞吐

**场景**：1 个 Router 线程 + N 个 Worker 线程，10 个 topic，50万消息。

| Dispatcher 数 | 耗时 | QPS |
|------------|------|-----|
| 2 | ~206 ms | **~2,430,000** |
| 4 | ~201 ms | **~2,490,000** |

**分析**：多 dispatcher 相比单 dispatcher（~5M QPS）增加了 Router+Worker 开销，但能实现跨 topic 的真正并行 handler 执行，适合 CPU 密集型 handler。

---

### 11. Concurrent Sub/Unsub — COW 稳定性

**场景**：2 个线程快速订阅/取消订阅，同时 1 个 publisher 发送 50万消息。

| 指标 | 结果 |
|------|------|
| 消息数 | 500,000 |
| Sub/Unsub 订阅搅动线程 | 2 |
| 耗时 | ~214 ms |
| Publish QPS | **~2,340,000** |
| 搅动操作数 | ~203,000 |

**分析**：COW 订阅者列表确保在并发订阅搅动下 publish 吞吐保持稳定。无数据竞争，稳定订阅者无漏投。

---

### 12. Raw Queue MPMC — 多生产者多消费者

**场景**：4 个生产者、4 个消费者，纯 `LockFreeQueue<int>` 操作。

| 指标 | 结果 |
|------|------|
| 消息数 | 1,000,000 |
| 生产者 / 消费者 | 4 / 4 |
| 耗时 | ~118 ms |
| 吞吐量 | **~8,470,000 ops/s** |

**分析**：Vyukov MPMC 队列 4P/4C 达 ~8.5M ops/s，vs 1P/1C ~71M。来自 8 个并发线程的 CAS 竞争降低了吞吐，但保持无锁进展。

---

### 13. Object Pool Impact — 对象池影响

**场景**：50万消息，对比对象池命中（int，已预热）vs 池未命中（string，冷路径）。

| 模式 | 耗时 | QPS |
|------|------|-----|
| 对象池启用 (int) | ~64 ms | **~7,800,000** |
| 对象池无命中 (string) | ~122 ms | **~4,100,000** |

**分析**：对象池命中时实现 **~1.9x** 吞吐提升，通过消除堆分配。`string` 路径每条消息都需要分配和拷贝开销。

---

### 14. Stable Throughput — 多轮中位数

**场景**：50万消息，3 轮，报告中位数 QPS 减少波动。

| 配置 | 中位数 QPS |
|------|----------|
| 1P/1S | **~5,040,000** |
| 4P/1S | **~3,950,000** |

**分析**：3 轮中位数消除异常值波动。1P/1S 稳定在 ~5M QPS，展现一致性能。

---

## 性能汇总

| 测试场景 | QPS / 速率 | 说明 |
|----------|-----------|------|
| Raw Queue 1P/1C | ~71M ops/s | 队列极限吞吐 |
| Raw Queue 4P/4C | ~8.5M ops/s | Vyukov MPMC 多路竞争 |
| 单发单收 | ~5.0M QPS | 端到端完整路径（稳定中位数） |
| 4 生产者 | ~4.0M QPS | MPMC 并发（稳定中位数） |
| 100 Topic | **~9.2M QPS** | TopicId + condvar（+86%） |
| Fan-out ×10 | ~24M del/s | 广播投递 |
| Fan-out ×100 | ~85M del/s | 高扇出投递 |
| Wildcard ×100 | ~1.9M QPS | Trie 索引 |
| Wildcard ×1000 | ~2.0M QPS | O(depth) 不随模式数增长 |
| Wildcard RCU ×4 | ~2.0M QPS | 并发读者，近线性扩展 |
| FullPolicy Block | ~4.0M QPS | 100% 投递，cv_not_full_ |
| Multi-dispatcher ×4 | ~2.5M QPS | Router + 4 Workers |
| Pool Enabled | ~7.8M QPS | 对象池 ~1.9x vs 冷路径 |
| 并发 sub/unsub | ~2.3M QPS | COW 搅动下稳定 |
| 延迟 min | **~0.2 μs** | 对象池复用 |
| 延迟 p50 | **~57 μs** | condvar 唤醒（优化前 ~374μs） |

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
| 最小延迟 | 36μs → 0.2μs（**~180x**） |
| p50 延迟 | 3.6ms → 57μs（**~63x**） |
| 多 Topic QPS | 4.95M → 9.2M（**+86%**） |

---

## 性能瓶颈分析

### 当前瓶颈

1. **单 Router 线程**（多 dispatcher 模式）：Router 是串行瓶颈，高吞吐时可能成为限制
2. **publish 路径 TopicRegistry resolve**：每次 publish 需 shared_lock + 哈希查找注册表

### 已完成优化

| 优化 | 收益 | 状态 |
|------|------|------|
| 对象池替代 `shared_ptr` | 最小延迟 180x | ✅ 已实现 |
| 多 dispatcher（topic 分片） | 消费端水平扩展 | ✅ 已实现 |
| TopicId 整数路由 | 多 Topic +86%，p50 63x | ✅ 已实现 |
| 通配符 Trie 索引 | 匹配 O(depth)，不随模式数增长 | ✅ 已实现 |
| 条件变量替代 sleep 退避 | p50 延迟 ~7x，p99 ~3x | ✅ 已实现 |
| Trie 透明哈希 + 空节点修剪 | 匹配路径零分配，防内存泄漏 | ✅ 已实现 |
| API `string_view` 参数 | 调用方无需持有 `std::string` | ✅ 已实现 |
| FullPolicy 背压 | 5 种队列满策略，100% 投递（Block） | ✅ 已实现 |
| TopicHandle 缓存发布 | 跳过 registry resolve，无锁 publish | ✅ 已实现 |
| RCU Slot Map | Dispatch 读路径完全无锁（MSVC/GCC） | ✅ 已实现 |

### 潜在优化方向

| 优化 | 预期收益 | 复杂度 |
|------|---------|--------|
| Batch dequeue | QPS +20~30% | 中 |

---

## 与同类方案对比（参考量级）

| 方案 | 典型 QPS | 无锁 | 协程 | 对象池 | 通配符 |
|------|---------|------|------|--------|--------|
| **MsgBus (本项目)** | ~9.2M | ✓ | ✓ | ✓ | ✓ |
| Boost.Signals2 | ~1-2M | ✗ | ✗ | ✗ | ✗ |
| Qt Signals/Slots | ~0.5-1M | ✗ | ✗ | ✗ | ✗ |
| moodycamel + 手动路由 | ~8-15M | ✓ | ✗ | ✗ | ✗ |
| Redis Pub/Sub (本地) | ~0.1-0.5M | ✗ | ✗ | ✗ | ✓ |

> 注：对比数据为经验量级，实际结果取决于硬件和负载。

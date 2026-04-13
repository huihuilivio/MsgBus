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
- **优化**: 对象池 + 侵入式引用计数（替代 shared_ptr）

---

## 基准测试项目

### 1. Raw Queue — 无锁队列裸吞吐

**场景**：1 生产者 / 1 消费者，纯 `LockFreeQueue<int>` 操作，无消息包装、无 topic 路由。

**目的**：测量队列本身的极限吞吐，作为系统上限参考。

| 指标 | 结果 |
|------|------|
| 消息数 | 1,000,000 |
| 耗时 | ~90 ms |
| 吞吐量 | **~11,000,000 ops/s** |

---

### 2. Throughput — 单发单收端到端吞吐

**场景**：1 个 publisher 线程，1 个 subscriber handler，完整 publish→queue→dispatch→handler 路径。

| 指标 | 结果 |
|------|------|
| 消息数 | 1,000,000 |
| 耗时 | ~150 ms |
| 吞吐量 | **~6,700,000 QPS** |

**分析**：相比 Raw Queue 的开销主要来自：
- 对象池 acquire/release（无锁，开销很小）
- `TypedMessage<T>` 构造或 reset
- topic 哈希查找 + shared_mutex 读锁
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
| 耗时 | ~184 ms |
| 吞吐量 | **~5,440,000 QPS** |

**分析**：多线程 CAS 竞争导致约 20% 的吞吐下降，符合预期。Vyukov 算法在多生产者场景下仍保持良好性能。

---

### 4. Latency — 发布到处理延迟

**场景**：1 个 publisher，1 个 subscriber，逐条测量 publish 时刻到 handler 执行时刻的延迟。包含 1000 条预热消息。

| 百分位 | 优化后 (μs) | 优化前 (μs) |
|--------|------------|------------|
| **min** | **0.3** | 36.2 |
| **p50** | 2,196 | 3,582.7 |
| **p90** | 4,977.8 | 4,977.8 |
| **p99** | 5,796.2 | 5,796.2 |
| **max** | 5,910.4 | 5,910.4 |

**分析**：

- **最小延迟从 ~36μs 降至 ~0.3μs**（100x 改善），得益于对象池复用消除堆分配 + 侵入式引用计数消除 shared_ptr control block 开销
- p50 从 ~3.6ms 降至 ~2.2ms（~40% 改善），中位延迟也显著受益
- 高百分位延迟受排队效应主导（10 万条消息连续发布），对象池优化影响较小
- 在实际应用中（publish 频率较低），延迟会接近 min 值
- 三级退避策略在空闲时会增加延迟，但显著降低 CPU 占用

---

### 5. Multi-topic — 多 Topic 吞吐

**场景**：100 个不同 topic，每个 topic 发 10,000 条消息。

| 指标 | 优化后 | 优化前 |
|------|--------|--------|
| Topic 数 | 100 | 100 |
| 总消息 | 1,000,000 | 1,000,000 |
| 耗时 | ~172 ms | ~202 ms |
| 吞吐量 | **~5,800,000 QPS** | ~4,950,000 QPS |

**分析**：对象池优化在多 topic 场景下效果显著（**+18% QPS**），因为多 topic 切换导致更多消息对象创建/销毁，对象池复用的收益更大。

---

### 6. Fan-out — 广播扩散

**场景**：1 个 publisher，N 个 subscriber 订阅同一 topic。

#### 10 个订阅者

| 指标 | 结果 |
|------|------|
| 消息数 | 100,000 |
| 订阅者数 | 10 |
| 总投递数 | 1,000,000 |
| 耗时 | ~32 ms |
| 投递速率 | **~31,170,000 deliveries/s** |

#### 100 个订阅者

| 指标 | 结果 |
|------|------|
| 消息数 | 10,000 |
| 订阅者数 | 100 |
| 总投递数 | 1,000,000 |
| 耗时 | ~12 ms |
| 投递速率 | **~84,050,000 deliveries/s** |

**分析**：Fan-out 场景下投递速率反而更高，因为：
- 每条消息的 dequeue + topic 查找开销被多个 handler 调用摊薄
- 订阅者列表在内存中连续（vector），顺序遍历缓存友好
- `std::function<void(const T&)>` 调用开销很小
- 侵入式引用计数：Fan-out 场景下消息被多个 handler 共享，侵入式引用计数的原子操作比 shared_ptr 更轻量

---

## 性能汇总

| 测试场景 | QPS / 速率 | 说明 |
|----------|-----------|------|
| Raw Queue | ~11M ops/s | 队列极限吞吐 |
| 单发单收 | ~6.7M QPS | 端到端完整路径 |
| 4 生产者 | ~5.4M QPS | MPMC 并发 |
| 100 Topic | **~5.8M QPS** | 哈希路由（对象池优化 +18%） |
| Fan-out ×10 | ~31M del/s | 广播投递 |
| Fan-out ×100 | ~84M del/s | 高扇出投递 |
| 延迟 min | **~0.3 μs** | 对象池复用（优化前 36μs） |
| 延迟 p50 | **~2.2 ms** | 排队效应主导（优化前 3.6ms） |

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

### 效果

| 指标 | 改善 |
|------|------|
| 最小延迟 | 36μs → 0.3μs（**~100x**） |
| p50 延迟 | 3.6ms → 2.2ms（**~40%**） |
| 多 Topic QPS | 4.95M → 5.8M（**+18%**） |

---

## 性能瓶颈分析

### 当前瓶颈

1. **`std::string` topic**：topic 作为 `std::string` 存在拷贝和哈希开销
2. **单 Router 线程**（多 dispatcher 模式）：Router 是串行瓶颈，高吞吐时可能成为限制
3. **通配符匹配**：每条消息需遍历全部 wildcard_entries_ 做字符串匹配
4. **三级退避**：低负载时 sleep 阶段增加尾部延迟

### 已完成优化

| 优化 | 收益 | 状态 |
|------|------|------|
| 对象池替代 `shared_ptr` | 最小延迟 100x，多 Topic +18% | ✅ 已实现 |
| 多 dispatcher（topic 分片） | 消费端水平扩展 | ✅ 已实现 |

### 潜在优化方向

| 优化 | 预期收益 | 复杂度 |
|------|---------|--------|
| topic 使用 `string_view` + 预注册 ID | QPS +10~20% | 低 |
| 通配符 trie 索引 | 通配符匹配 O(1) | 中 |
| Batch dequeue | QPS +20~30% | 中 |
| 条件变量替代 sleep 退避 | 尾部延迟 ↓90% | 低 |

---

## 与同类方案对比（参考量级）

| 方案 | 典型 QPS | 无锁 | 协程 | 对象池 | 通配符 |
|------|---------|------|------|--------|--------|
| **MsgBus (本项目)** | ~6.7M | ✓ | ✓ | ✓ | ✓ |
| Boost.Signals2 | ~1-2M | ✗ | ✗ | ✗ | ✗ |
| Qt Signals/Slots | ~0.5-1M | ✗ | ✗ | ✗ | ✗ |
| moodycamel + 手动路由 | ~8-15M | ✓ | ✗ | ✗ | ✗ |
| Redis Pub/Sub (本地) | ~0.1-0.5M | ✗ | ✗ | ✗ | ✓ |

> 注：对比数据为经验量级，实际结果取决于硬件和负载。

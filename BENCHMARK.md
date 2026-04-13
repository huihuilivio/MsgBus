# MsgBus 性能基准测试报告

## CI 自动化压测

每次推送到 `main` 分支或提交 PR 时，CI 会自动在以下环境运行性能压测：

| 平台 | 编译器 | Runner |
|------|--------|--------|
| Windows | MSVC (latest) | `windows-latest` |
| Linux | GCC 13 | `ubuntu-latest` |
| Linux | Clang 17 | `ubuntu-latest` |
| macOS | Apple Clang | `macos-latest` |

压测结果以 artifact 形式保存，可在 [GitHub Actions](../../actions) 页面下载查看。

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
- `shared_ptr<IMessage>` 分配与引用计数
- `TypedMessage<T>` 构造
- topic 哈希查找 + shared_mutex 读锁
- `std::function` 调用

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

| 百分位 | 延迟 (μs) |
|--------|-----------|
| **min** | 36.2 |
| **p50** | 3,582.7 |
| **p90** | 4,977.8 |
| **p99** | 5,796.2 |
| **p99.9** | 5,866.5 |
| **max** | 5,910.4 |

**分析**：

- 最小延迟 ~36μs 表明在 dispatcher 空闲且自旋等待时能快速响应
- p50 较高是因为连续 publish 10 万条消息形成排队效应，dispatcher 消费速度跟不上生产速度
- 在实际应用中（publish 频率较低），延迟会接近 min 值
- 三级退避策略在空闲时会增加延迟，但显著降低 CPU 占用

---

### 5. Multi-topic — 多 Topic 吞吐

**场景**：100 个不同 topic，每个 topic 发 10,000 条消息。

| 指标 | 结果 |
|------|------|
| Topic 数 | 100 |
| 每 Topic 消息 | 10,000 |
| 总消息 | 1,000,000 |
| 耗时 | ~202 ms |
| 吞吐量 | **~4,950,000 QPS** |

**分析**：topic 路由通过 `unordered_map` 哈希查找，100 个 topic 相比单 topic 约有 25% 开销，主要来自哈希计算和缓存命中率下降。

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

---

## 性能汇总

| 测试场景 | QPS / 速率 | 说明 |
|----------|-----------|------|
| Raw Queue | ~11M ops/s | 队列极限吞吐 |
| 单发单收 | ~6.7M QPS | 端到端完整路径 |
| 4 生产者 | ~5.4M QPS | MPMC 并发 |
| 100 Topic | ~4.9M QPS | 哈希路由开销 |
| Fan-out ×10 | ~31M del/s | 广播投递 |
| Fan-out ×100 | ~84M del/s | 高扇出投递 |
| 延迟 min | ~36 μs | dispatcher 空闲时 |

> 以上为本地 Windows 环境参考数据。CI 各平台数据请在 [Actions artifacts](../../actions) 查看。

---

## 性能瓶颈分析

### 当前瓶颈

1. **`shared_ptr` 分配**：每条消息创建一个 `shared_ptr<TypedMessage<T>>`，涉及堆分配和引用计数原子操作
2. **`std::string` topic**：topic 作为 `std::string` 存在拷贝和哈希开销
3. **单 dispatcher 线程**：所有 topic 共享一个消费线程，高负载时成为瓶颈
4. **三级退避**：低负载时 sleep 阶段增加尾部延迟

### 优化方向

| 优化 | 预期收益 | 复杂度 |
|------|---------|--------|
| 对象池替代 `shared_ptr` | QPS +30~50% | 中 |
| topic 使用 `string_view` + 预注册 ID | QPS +10~20% | 低 |
| 多 dispatcher（topic 分片） | QPS 线性扩展 | 高 |
| Batch dequeue | QPS +20~30% | 中 |
| 条件变量替代 sleep 退避 | 尾部延迟 ↓90% | 低 |

---

## 与同类方案对比（参考量级）

| 方案 | 典型 QPS | 无锁 | 协程 |
|------|---------|------|------|
| **MsgBus (本项目)** | ~6.7M | ✓ | ✓ |
| Boost.Signals2 | ~1-2M | ✗ | ✗ |
| Qt Signals/Slots | ~0.5-1M | ✗ | ✗ |
| moodycamel + 手动路由 | ~8-15M | ✓ | ✗ |
| Redis Pub/Sub (本地) | ~0.1-0.5M | ✗ | ✗ |

> 注：对比数据为经验量级，实际结果取决于硬件和负载。

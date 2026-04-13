# MsgBus Benchmark Report

This document has moved to the `doc/` directory with i18n support:

- **English**: [doc/en/BENCHMARK.md](doc/en/BENCHMARK.md)
- **中文**: [doc/zh/BENCHMARK.md](doc/zh/BENCHMARK.md)

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

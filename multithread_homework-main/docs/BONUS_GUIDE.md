# 加分项实现指南

> 本文档详细介绍加分项的原理、实现步骤和常见陷阱。
> 建议在完成所有必做题（Phase 1–4）后再挑战加分项。

---

## 加分项1：带回调的异步接口

**测试**：`ctest -L bonus -R BonusCallback`

### 原理：回调模式 vs Future 模式

`async_query` 返回 `std::future`，调用方需要主动调用 `.get()` 等待结果：

```cpp
auto future = client.async_query("hero_001");
// ... 做其他事 ...
auto result = future.get();  // 阻塞直到结果就绪
```

`async_query_with_callback` 则是"推"模式——任务完成后自动调用回调，调用方无需等待：

```cpp
client.async_query_with_callback("hero_001", [](std::optional<Attribute> val) {
    // 在 worker 线程中被调用
    if (val) { /* 处理结果 */ }
});
// 立即返回，不阻塞
```

### std::function 类型擦除

`std::function<void(std::optional<Attribute>)>` 可以存储任意可调用对象（lambda、函数指针、仿函数），只要签名匹配。这是 C++ 的**类型擦除**技术。

### 实现提示

在 `pool_.dispatch` 的 lambda 中，先执行查询，再调用回调：

```cpp
void ExpeditionClient::async_query_with_callback(
    const std::string &key,
    std::function<void(std::optional<Attribute>)> callback) {
    pool_.dispatch([&reg = registry_, key, cb = std::move(callback)] {
        cb(reg.query(key));
    });
}
```

关键点：
- 回调在 **worker 线程**中执行，不阻塞调用线程
- `callback` 需要用 `std::move` 捕获（`std::function` 可能不可拷贝）
- `dispatch` 返回的 `future<void>` 可以忽略（fire-and-forget）

---

## 加分项2：事务细粒度锁（OCC）

**测试**：`ctest -L bonus -R BonusShardLock`

### 问题背景

基础实现的 `commit()` 逐个调用 `registry_.register_adventurer` / `dismiss`，每次调用都会独立加锁解锁。这有两个问题：

1. **非原子性**：多个写操作之间存在间隙，其他线程可能看到中间状态
2. **无并行性**：即使两个事务操作完全不同的 shard，也会因为逐个加锁而相互阻塞

加分项要求实现**真正的原子提交**：一次性锁住所有涉及的 shard，然后批量写入。

### 乐观并发控制（OCC）原理

OCC 分三个阶段：

```
读取阶段（不加锁）          校验阶段（加锁）           写入阶段（持锁）
─────────────────────    ─────────────────────    ─────────────────────
读取数据，记录版本号  →   检查版本号是否变化   →   版本未变则写入，否则中止
```

**为什么需要版本号？**

考虑以下场景（金币转账）：

```
时间线：
  T1 读取 A=100, B=200（版本 A:1, B:1）
  T2 读取 A=100, B=200（版本 A:1, B:1）
  T2 提交：A=50, B=250（版本变为 A:2, B:2）
  T1 提交：A=150, B=150（此时 A 的版本已是 2，不是 T1 读取时的 1）
           → OCC 检测到冲突，T1 中止，需要重试
```

没有版本号，T1 会直接覆盖 T2 的结果，导致金币凭空消失（lost update）。

### 为什么需要 put_unlocked？

`atomic_write` 需要先加锁，再写入。如果写入时调用 `put()`，而 `put()` 内部也会加锁，就会发生**同一线程对同一 mutex 重复加锁**——这是未定义行为（通常导致死锁）。

因此需要 `put_unlocked`：调用方已持有锁，直接操作 `data_` 而不再加锁。

### 为什么按 shard 索引排序加锁？

假设事务 T1 涉及 shard 0 和 shard 1，事务 T2 也涉及这两个 shard：

```
不排序（可能死锁）：          排序后（安全）：
T1: 锁 shard 0，等 shard 1   T1: 锁 shard 0，锁 shard 1
T2: 锁 shard 1，等 shard 0   T2: 等 shard 0，锁 shard 1
→ 循环等待，死锁！            → T1 先完成，T2 再执行，无死锁
```

排序后，所有事务都按相同顺序加锁，打破了循环等待条件。

### 各函数说明与实现步骤

**Step 1：实现 `Shard::get_versioned`**

为什么需要：普通 `get()` 只返回值，事务读取后无法知道"这条数据在我读完之后有没有被别人改过"。`get_versioned()` 额外返回版本号，事务把它存入 `read_set_`，提交时拿来和当前版本比对，就能检测出读写之间是否发生了冲突。

与 `get()` 的区别：逻辑完全相同，只是返回值从 `optional<Attribute>` 变成 `{value, version}` 对。不存在或已过期时返回 `{nullopt, 0}`。

**Step 2：实现 `Shard::put_unlocked`**

为什么需要：`atomic_write` 在提交时需要先把所有涉及的 shard 全部锁住，再统一写入。如果写入时调用普通的 `put()`，`put()` 内部会再次对同一个 mutex 加写锁——同一线程对 `std::shared_mutex` 重复加锁是未定义行为，通常直接死锁。所以需要一个"假设调用方已持锁"的版本，跳过加锁直接操作数据。

与 `put()` 的区别：去掉 `unique_lock`，其余逻辑相同。注意只更新 `value` 字段再 `version++`，不能用赋值覆盖整个 `Record`，否则 `version` 会被重置为 0，导致 OCC 永远检测不到冲突。

**Step 3：实现 `Shard::remove_unlocked`**

为什么需要：原因与 `put_unlocked` 相同——`atomic_write` 持锁期间不能再调用会加锁的 `remove()`。

与 `remove()` 的区别：去掉 `unique_lock`，直接 `data_.erase(key)` 返回结果。

**Step 4：实现 `GuildRegistry::query_versioned`**

为什么需要：`Transaction::get()` 需要在读取数据的同时拿到版本号，但 `GuildRegistry` 对外只暴露了 `query()`（不含版本号）。需要一个透传版本号的包装方法，让事务层不必直接操作 shard 内部。

与 `query()` 的区别：把内部调用从 `get()` 换成 `get_versioned()`，把版本号一并返回。统计计数器（hit/miss/query）的更新逻辑与 `query()` 完全相同。

**Step 5：实现 `GuildRegistry::atomic_write`**

为什么需要：这是整个加分项的核心。普通的逐个写入无法保证原子性（写到一半时其他线程可能读到中间状态），也无法检测并发冲突。`atomic_write` 通过"一次性锁住所有相关 shard + 提交前校验版本"解决这两个问题。

逻辑分四步：
1. 把 `write_set` 和 `read_set` 里所有 key 的 shard 索引收集起来，去重后从小到大排序（防止多个事务以不同顺序加锁导致死锁）
2. 按排好的顺序逐个对 shard 加 `unique_lock`，存入 vector（离开作用域自动释放）
3. 持锁后遍历 `read_set`，对每个 key 查当前版本号，与记录的版本号比较，不一致直接返回 false
4. 版本全部一致，遍历 `write_set`，有值调用 `put_unlocked`，nullopt 调用 `remove_unlocked`，返回 true

**Step 6：修改 `Transaction::get`**

为什么需要修改：原来调用 `registry_.query()` 拿不到版本号，`read_set_` 永远是空的，提交时 OCC 校验形同虚设。

改动：把 `registry_.query(key)` 换成 `registry_.query_versioned(key)`，把返回的版本号存入 `read_set_[key]`。read-your-writes 的逻辑（先查 `write_set_`）不变。

**Step 7：修改 `Transaction::commit`**

为什么需要修改：原来逐个调用 `register_adventurer` / `dismiss` 不是原子操作，也没有冲突检测。

改动：把逐个写入替换为一次调用 `registry_.atomic_write(write_set_, read_set_)`。返回 true 则设置 `committed_ = true`；返回 false 说明有版本冲突，不修改状态，让调用方决定是否重试。

### 常见陷阱

| 陷阱 | 症状 | 原因 | 修复 |
|------|------|------|------|
| `put_unlocked` 覆盖整个 Record | OCC 永远检测不到冲突，retry_count = 0 | `data_[key] = Record{value}` 重置了 version | 只更新 `value` 字段，再 `version++` |
| 忘记在 read_set 中记录版本 | 并发转账后金币总量不守恒 | `get()` 调用了 `query()` 而非 `query_versioned()` | 改用 `query_versioned`，存入 `read_set_` |
| 加锁时未包含 read_set 的 shard | 版本校验时 shard 未被锁住，存在 TOCTOU 竞争 | 只收集了 write_set 的 shard 索引 | 同时收集 write_set 和 read_set 的 shard 索引 |

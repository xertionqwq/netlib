# Timer 类 — 毫秒级定时器

> 日期：2026-06-16
> 阶段：Phase 2 协程核心 — 定时器基础设施，为 IOManager 提供时间驱动能力

## 基础介绍

Timer 提供毫秒级定时功能——在指定时间后执行回调。TimerManager 管理一组 Timer，通过 `std::set` 按绝对超时时间排序，支撑 IOManager 用 epoll_wait 的 timeout 参数替代 sleep 轮询。

## 两层结构

```
Timer                       TimerManager
─────                       ────────────
m_ms        超时间隔(ms)     timers_     std::set<Timer>, 按 next_ 升序
m_next      绝对到期时刻      addTimer()  创建并加入堆
m_cb        到期回调          getNextTimer()  最近到期还剩多少 ms
m_recurring 是否循环          listExpiredCb() 摘取所有到期回调
m_manager   反向指针          hasTimer()      堆是否非空
```

Timer 持有 `TimerManager*` 反向指针，支持 `cancel()` / `refresh()` / `reset()` 操作。TimerManager 是 IOManager 的基类之一。

## 完整时间线

### 1. 创建

```cpp
mgr.addTimer(3000, []() { timeout(); });
```

```
Timer 构造函数:
  now   = 14:30:00.000
  next_ = now + 3000ms = 14:30:03.000    ← 绝对到期时刻
  ms_   = 3000                            ← 记住间隔（循环时需要）

addTimer(timer):
  timers_.insert(timer)
    → timer 在 set 中按 next_ 排序
    → 如果它是新的"最早到期" timer
      → onTimerInsertAtFront()           ← IOManager 覆写为 tickle 唤醒 epoll
```

### 2. 等待到期（IOManager 视角）

```
IOManager::idle():
  timeout = getNextTimer()
    → (*timers_.begin())->next_ = 14:30:03.000
    → now             = 14:30:00.000
    → duration        = 3000ms
    → return 3000

  epoll_wait(epfd, events, 3000)         ← 阻塞最多 3 秒
    │
    ├─ IO 事件到达 → tickle 唤醒 → 立刻返回
    └─ 3 秒超时    → 定时器到期 → 返回 0 个事件
```

### 3. 到期提取

```
listExpiredCb(cbs):
  now = 14:30:03.001                     ← 略大于到期时间
  
  // 取第一个 timer
  (*timers_.begin())->next_ = 14:30:03.000
  now >= next_? 14:30:03.001 >= 14:30:03.000 → true → 提取!

  // 从堆删除 timer
  timers_.erase(timers_.begin())
  cbs.push_back(timer->cb_)

  // 若是循环 timer
  if (recurring_):
    next_ = now + ms_ = 14:30:06.001     ← 重新算
    timers_.insert(timer)                ← 重新入堆

  // 继续循环，检查下一个 timer 是否也到期
```

### 4. 回调执行

```
IOManager::idle():
  for cb in cbs:
    scheduleLock(cb)                      ← 把回调 schedule 为协程任务
```

后续 Scheduler 的 run() 循环中某个 worker 会取出执行。

## 关键设计

### Comparator — 按 next_ 排序

```cpp
struct Comparator {
    bool operator()(const Timer::ptr &a, const Timer::ptr &b) const {
        return a->next_ < b->next_;     // 升序：最早到期排最前
    }
};
```

`std::set` 保证 `*begin()` 永远是最近到期的 Timer。`getNextTimer()` 只需取 `*begin()` 即可。

### onTimerInsertAtFront — 留给 IOManager 的钩子

基类空实现。IOManager 覆写为 `tickle()`（写 pipe 唤醒 epoll_wait），确保新插入的更早定时器不会被 epoll 旧的 timeout 耽误。

### tickled_ — 防重复唤醒

插入一批定时器时，只有第一个"更早"的触发 `onTimerInsertAtFront`。`getNextTimer()` 被 IOManager 调用后重置 `tickled_`，允许下次通知。

### detectClockRollover — 系统时间回拨保护

检测到系统时间倒退超过 1 小时（NTP、手动调时）时，`listExpiredCb` 强制清空所有定时器，防止所有 `next_` 变成"遥远的未来"导致永久不触发。

### weak_ptr 条件定时器

`addConditionTimer` 用 `weak_ptr` 包装条件——定时器到期时检查对象是否还活着。用于"3 秒未收到数据关闭连接"这种场景，连接对象先于定时器销毁时自动跳过。

### 循环定时器

`recurring=true` 的 Timer 在 `listExpiredCb` 中被提取后，自动重新计算 `next_` 并重新插入堆。常规用途：心跳检测、定期清理。

## 与 IOManager 的关系

```
IOManager : public Scheduler, public TimerManager
                │                    │
                │                    ├─ getNextTimer()  → epoll_wait timeout
                │                    ├─ listExpiredCb() → schedule 到期回调
                │                    └─ onTimerInsertAtFront() → tickle 唤醒
                │
                ├─ idle()   → epoll_wait（带定时器 timeout）
                ├─ tickle() → pipe write
                └─ run()    → 调度循环（继承自 Scheduler）
```

TimerManager 纯数据管理，IOManager 把它和 epoll 粘起来。

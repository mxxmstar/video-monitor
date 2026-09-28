# PusherSession 设计文档

## 1. 概述

`PusherSession` 是媒体输出链路中的**会话编排层**，负责管理一次完整的推流会话生命周期和策略决策。它位于 `Publisher` 和 `FFmpegPusher` 之间，是"什么时候允许写"的策略层，不直接接触 FFmpeg API。

```text
Publisher -> PusherSession -> FFmpegPusher -> FFmpegMuxer -> 输出目标
```

### 1.1 核心职责

- 管理推流会话的状态机（Closed/WaitingForKeyframe/Running/Failed）
- 控制关键帧门控：首次启动时等待视频关键帧，丢弃非关键帧
- 时间戳策略：保留原始时间戳或从零开始重新计时
- 处理底层 Pusher 的异步错误事件
- 提供统一的 Open/Publish/Close 接口

### 1.2 设计原则

- **不直接调用 FFmpeg API**：通过 `IPusher` 接口与底层交互
- **策略与实现分离**：Session 决定"何时写"，Pusher 决定"怎么写"
- **线程安全**：支持多线程环境下的安全操作
- **异步事件处理**：独立事件线程处理底层错误通知

---

## 2. 架构设计

### 2.1 整体架构

```
┌─────────────────────────────────────────────────────────────────┐
│                        PusherSession                            │
│                                                                 │
│  ┌─────────────────┐         ┌──────────────────────────────┐  │
│  │   主线程接口     │         │        事件处理线程           │  │
│  │                 │         │                              │  │
│  │ Open()          │         │  eventLoop()                 │  │
│  │ Publish()       │◄───────►│  处理异步错误事件             │  │
│  │ Close()         │         │  更新 state_ 和 last_event_  │  │
│  │ RequestStop()   │         │                              │  │
│  └────────┬────────┘         └──────────────┬───────────────┘  │
│           │                                 │                  │
│           ▼                                 ▼                  │
│  ┌─────────────────┐         ┌──────────────────────────────┐  │
│  │ operation_mutex_│         │  event_queue_ (阻塞队列)     │  │
│  │ (recursive)     │         │  event_generation_ (代际)    │  │
│  │ 保护: state_    │         │  event_active_               │  │
│  │        last_    │         └──────────────────────────────┘  │
│  └─────────────────┘                                          │
│           │                                                   │
│           ▼                                                   │
│  ┌─────────────────┐                                          │
│  │   IPusher*      │  ← 底层推流实现（如 FFmpegPusher）        │
│  └─────────────────┘                                          │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 线程模型

`PusherSession` 使用**双线程模型**：
**后续会将事件线程放到publisher中**

| 线程 | 职责 | 阻塞点 |
|---|---|---|
| **主线程** | 调用 Open/Publish/Close/RequestStop | 等待 operation_mutex_ |
| **事件线程** | 处理底层 Pusher 的异步错误回调 | 等待 event_queue_ |

**线程交互流程：**

```
FFmpeg 内部线程
     │
     │ 网络超时/错误
     ▼
┌──────────────┐
│ EventCallback│  ← 回调函数（在 FFmpeg 线程中执行）
└──────┬───────┘
       │
       │ 入队
       ▼
┌──────────────┐         ┌──────────────┐
│ event_queue_ │────────►│  eventLoop() │
│ (阻塞队列)   │         │  (事件线程)   │
└──────────────┘         └──────┬───────┘
                                │
                                │ 获取 operation_mutex_
                                ▼
                         更新 state_ = Failed
```

---

## 3. 状态机

### 3.1 状态定义

```cpp
enum class PusherSessionState {
    Closed,              ///< 尚未打开，或已完成关闭
    WaitingForKeyframe,  ///< 输出已打开，等待第一个视频关键帧
    Running,             ///< 已写入首个关键帧，正常转发编码包
    Failed,              ///< 底层写入失败，必须先 Close 再重新 Open
};
```

### 3.2 状态转换图

```
                    Open() 成功
Closed ─────────────────────────> WaitingForKeyframe
  ▲                                  │
  │          Close()                 │ 收到关键帧
  │                                  │ 并成功写入
  │                                  ▼
  │                                Running
  │                                  │
  │        Close()                   │ 写入失败
  │                                  ▼
  └─────────────── Failed ◄──────────┘
                   │
                   │ Close()
                   ▼
                 Closed
```

### 3.3 各状态行为

| 状态 | Publish() 行为 | 说明 |
|---|---|---|
| **Closed** | 返回 Failed | 会话未打开或已关闭 |
| **WaitingForKeyframe** | 非关键帧返回 DroppedAwaitingKeyframe<br>关键帧尝试写入，成功后转为 Running | 确保输出从关键帧开始，可独立解码 |
| **Running** | 正常转发数据包 | 会话正常运行 |
| **Failed** | 返回 Failed，携带 last_event_ 错误 | 必须先 Close 再 Open 才能恢复 |

---

## 4. 核心功能

### 4.1 关键帧门控

首次打开会话后，Session 进入 `WaitingForKeyframe` 状态，此时：

- **非关键帧**：直接丢弃，返回 `DroppedAwaitingKeyframe`
- **关键帧**：尝试写入，成功后转为 `Running` 状态

```cpp
if (state_ == PusherSessionState::WaitingForKeyframe && !packet.keyframe) {
    return PusherPublishResult::DroppedAwaitingKeyframe();
}
```

**设计原因：**
- H.264/H.265 视频需要关键帧（IDR）作为解码起点
- 非关键帧（P/B 帧）依赖前面的帧，无法独立解码
- 从关键帧开始输出，确保播放器可以正常解码

### 4.2 时间戳策略

Session 支持两种时间戳模式：

```cpp
enum class PusherTimestampMode {
    Preserved,      ///< 保留原始时间戳
    StartAtZero,    ///< 从第一个关键帧开始从零计时
};

enum class PusherTimestampEpochScope {
    Session,        ///< 重连后沿用原 epoch，保持时间连续
    Connection,     ///< 重连后从零开始重新计时
};
```

**StartAtZero 模式工作流程：**

```
原始时间戳:     1000 → 1040 → 1080 → 1120 → ...
                (首包 pts=1000)

StartAtZero 后: 0    → 40   → 80   → 120  → ...
                (减去 epoch=1000)
```

**实现代码：**

```cpp
// 第一个成功写入的包建立 epoch
if (timestamp_policy_.mode == PusherTimestampMode::StartAtZero && 
    !timestamp_epoch_.has_value()) {
    timestamp_epoch_ = EarliestTimestamp(packet);
}

// 后续包减去 epoch
const std::int64_t offset = *timestamp_epoch_;
packet.pts -= offset;
packet.dts -= offset;
```

### 4.3 异步事件处理

#### 4.3.1 事件队列机制

```cpp
struct QueuedEvent {
    PusherEvent event;           ///< 事件数据
    std::uint64_t generation;    ///< 事件生成代际
};

BlockingMpmcQueue<std::optional<QueuedEvent>> event_queue_;
```

**工作流程：**

1. **底层触发事件**：FFmpeg 网络超时、写入失败等
2. **回调入队**：`event_callback_` 将事件包装为 `QueuedEvent` 推入队列
3. **事件线程处理**：`eventLoop()` 阻塞等待，取出事件后更新状态

#### 4.3.2 代际机制（Generation Pattern）

`event_generation_` 用于区分不同会话周期的事件，防止过期事件污染新会话。

**递增时机：**

```cpp
// Open 时递增
++event_generation_;
event_active_ = true;

// Close 时递增
++event_generation_;
event_active_ = false;
```

**代际检查：**

```cpp
// 事件线程处理时检查
if (!event_active_ || queued->generation != event_generation_) {
    continue;  // 过期事件，丢弃
}
```

**场景示例：**

```
时间线 ──────────────────────────────────────────────────────▶

Open()          Close()         Open()
gen=1           gen=2           gen=3
  │               │               │
  └─ 事件 A ──────┘               │
     (gen=1)                      │
                                  │
                     ┌─ 事件 B ───┘
                     │  (gen=2)
                     ▼
              eventLoop() 检查:
              gen=2 != gen=3 → 丢弃!
```

#### 4.3.3 两阶段停止机制

事件线程收到错误事件后，采用**两阶段加锁**策略快速停止底层写入：

```cpp
// 阶段 1：快速获取 event_mutex_，立即请求停止
{
    std::lock_guard<std::mutex> lock(event_mutex_);
    if (event_active_ && queued->generation == event_generation_ && pusher_) {
        pusher_->RequestStop();  // 快速中断
    }
}

// 阶段 2：等待 operation_mutex_，更新状态
std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
std::lock_guard<std::mutex> event_lock(event_mutex_);
// ... 更新 state_ = Failed
```

**设计原因：**

```
主线程持有 operation_mutex_，正在调用 pusher_->Push()
    ↓
FFmpeg 网络写入阻塞中（可能数秒）
    ↓
事件线程收到超时事件
    ↓
如果直接等待 operation_mutex_，无法及时停止 FFmpeg！
    ↓
解决方案：先获取轻量级 event_mutex_，立即调用 RequestStop()
    ↓
FFmpeg 收到中断信号，尽快退出阻塞
```

---

## 5. 核心接口

### 5.1 Open()

```cpp
PusherResult Open(const PusherSessionConfig& config);
```

**功能：**
1. 如果会话未关闭，先调用 Close() 结束旧会话
2. 重置 `stop_requested_` 和事件状态
3. 递增 `event_generation_`，激活事件处理
4. 调用 `pusher_->Open()` 打开底层输出
5. 设置状态为 `WaitingForKeyframe`
6. 初始化时间戳策略

**返回值：**
- `Success`：会话成功打开，等待关键帧
- `Failed`：打开失败，状态转为 Failed

### 5.2 Publish()

```cpp
PusherPublishResult Publish(const MediaPacket& packet);
```

**功能：**
1. 检查 `stop_requested_`，如果已请求停止则返回 Failed
2. 检查状态，Closed/Failed 状态拒绝发布
3. 验证 packet 类型（当前仅支持 H.264 视频）
4. 如果在 WaitingForKeyframe 状态且非关键帧，返回 DroppedAwaitingKeyframe
5. 应用时间戳策略（StartAtZero 模式）
6. 调用 `pusher_->Push()` 转发数据包
7. 如果是首个关键帧，建立 epoch 并转为 Running 状态

**返回值：**
- `Published`：数据包成功转发
- `DroppedAwaitingKeyframe`：等待关键帧期间丢弃
- `Failed`：发布失败，携带错误信息

### 5.3 Close()

```cpp
PusherResult Close();
```

**功能：**
1. 递增 `event_generation_`，停用事件处理
2. 调用 `pusher_->Close()` 关闭底层输出
3. 重置时间戳状态
4. 设置状态为 Closed（成功）或 Failed（失败）

**幂等性：** 可以安全地多次调用，不会重复关闭。

### 5.4 RequestStop()

```cpp
PusherResult RequestStop();
```

**功能：**
1. 设置 `stop_requested_ = true`
2. 调用 `pusher_->RequestStop()` 请求底层停止

**用途：** 优雅地中断当前推流操作，用于程序退出或用户取消。

---

## 6. 成员变量详解

### 6.1 线程同步

| 成员 | 类型 | 用途 |
|---|---|---|
| `operation_mutex_` | `std::recursive_mutex` | 串行化 Open/Publish/Close 操作 |
| `event_mutex_` | `std::mutex` | 保护事件队列和代际状态 |

### 6.2 事件处理

| 成员 | 类型 | 用途 |
|---|---|---|
| `event_queue_` | `BlockingMpmcQueue` | 多线程安全的事件队列 |
| `event_generation_` | `std::uint64_t` | 事件代际计数器 |
| `event_active_` | `bool` | 事件处理是否激活 |
| `event_thread_stopping_` | `bool` | 事件线程是否正在停止 |
| `event_thread_` | `std::thread` | 事件处理线程 |
| `last_event_` | `std::optional<PusherEvent>` | 最后处理的事件 |

### 6.3 状态管理

| 成员 | 类型 | 用途 |
|---|---|---|
| `state_` | `PusherSessionState` | 当前会话状态 |
| `stop_requested_` | `std::atomic<bool>` | 是否请求停止 |
| `pusher_` | `std::unique_ptr<IPusher>` | 底层推流实现 |

### 6.4 时间戳

| 成员 | 类型 | 用途 |
|---|---|---|
| `timestamp_policy_` | `PusherTimestampPolicy` | 时间戳策略配置 |
| `timestamp_epoch_` | `std::optional<std::int64_t>` | 时间戳基准点 |
| `timestamp_time_base_` | `std::optional<Rational>` | 时间戳时间基 |

---

## 7. 使用示例

### 7.1 基本使用

```cpp
// 创建 Session
PusherSession session;

// 配置
PusherSessionConfig config;
config.pusher.output_url = "output.mp4";
config.pusher.format_name = "mp4";
config.timestamp_policy.mode = PusherTimestampMode::StartAtZero;

// 打开会话
auto open_result = session.Open(config);
if (!open_result.Succeed()) {
    LOG_ERROR("Open failed: {}", open_result.error->message);
    return;
}

// 发布数据包
while (has_packets()) {
    MediaPacket packet = get_next_packet();
    auto publish_result = session.Publish(packet);
    
    if (publish_result.WasPublished()) {
        LOG_INFO("Packet published");
    } else if (publish_result.status == PusherPublishStatus::DroppedAwaitingKeyframe) {
        LOG_DEBUG("Dropped, waiting for keyframe");
    } else {
        LOG_ERROR("Publish failed: {}", publish_result.error->message);
        break;
    }
}

// 关闭会话
session.Close();
```

### 7.2 优雅停止

```cpp
// 主线程
session.RequestStop();

// 或者在信号处理中
std::signal(SIGINT, [](int) {
    session.RequestStop();
});
```

### 7.3 状态查询

```cpp
// 查询当前状态
auto state = session.State();
switch (state) {
    case PusherSessionState::Closed:
        LOG_INFO("Session is closed");
        break;
    case PusherSessionState::WaitingForKeyframe:
        LOG_INFO("Waiting for keyframe");
        break;
    case PusherSessionState::Running:
        LOG_INFO("Session is running");
        break;
    case PusherSessionState::Failed:
        auto event = session.LastEvent();
        if (event.has_value()) {
            LOG_ERROR("Session failed: {}", event->error.message);
        }
        break;
}
```

---

## 8. 错误处理

### 8.1 错误分类

```cpp
enum class PusherErrorCategory {
    InvalidConfiguration,  ///< 配置错误
    InvalidState,          ///< 状态错误（如未打开就 Publish）
    InvalidPacket,         ///< 数据包错误
    UnsupportedMedia,      ///< 不支持的媒体类型
    OpenFailed,            ///< 打开失败
    WriteFailed,           ///< 写入失败
    Timeout,               ///< 超时
    Cancelled,             ///< 用户取消
    Internal,              ///< 内部错误
};
```

### 8.2 错误传播链

```
FFmpeg 错误
    ↓
FFmpegMuxer 转换为 MuxerError
    ↓
FFmpegPusher 转换为 PusherError
    ↓
PusherSession 更新 state_ = Failed
    ↓
Publisher 查询 last_event_ 获取错误详情
```

### 8.3 错误恢复

当前版本**不自动重连**，失败后必须手动 Close 再 Open：

```cpp
auto result = session.Publish(packet);
if (!result.Succeed()) {
    // 关闭失败会话
    session.Close();
    
    // 重新打开
    auto open_result = session.Open(config);
    if (open_result.Succeed()) {
        // 重新等待关键帧
        LOG_INFO("Session reopened, waiting for keyframe");
    }
}
```

---

## 9. 测试策略

### 9.1 单元测试

使用 Mock Pusher 验证 Session 策略：

```cpp
class MockPusher : public IPusher {
public:
    PusherResult Open(const PusherConfig& config) override {
        opened_ = true;
        return PusherResult::Success();
    }
    
    PusherResult Push(const MediaPacket& packet) override {
        if (should_fail_) {
            return PusherResult::Failed(...);
        }
        pushed_packets_.push_back(packet);
        return PusherResult::Success();
    }
    
    void SetShouldFail(bool fail) { should_fail_ = fail; }
    
private:
    bool opened_ = false;
    bool should_fail_ = false;
    std::vector<MediaPacket> pushed_packets_;
};
```

**测试用例：**

1. **关键帧门控**：验证非关键帧被丢弃，关键帧被接受
2. **时间戳归零**：验证 StartAtZero 模式正确计算偏移
3. **状态转换**：验证 Closed → WaitingForKeyframe → Running → Failed
4. **错误处理**：验证 Push 失败后状态转为 Failed
5. **幂等 Close**：验证多次 Close 不会崩溃

### 9.2 集成测试

使用真实 FFmpegPusher 验证完整链路：

```cpp
TEST(PusherSession, PublishToMp4) {
    PusherSession session;
    
    PusherSessionConfig config;
    config.pusher.output_url = "test_output.mp4";
    config.pusher.format_name = "mp4";
    
    ASSERT_TRUE(session.Open(config).Succeed());
    
    // 发布固定数量的测试数据包
    for (int i = 0; i < 100; ++i) {
        auto packet = create_test_packet(i);
        auto result = session.Publish(packet);
        ASSERT_TRUE(result.Succeed() || 
                    result.status == PusherPublishStatus::DroppedAwaitingKeyframe);
    }
    
    ASSERT_TRUE(session.Close().Succeed());
    
    // 验证输出文件
    ASSERT_TRUE(file_exists("test_output.mp4"));
}
```

---

## 10. 未来扩展

### 10.1 自动重连

计划实现的重连逻辑：

```
Push 失败
    ↓
关闭旧 Pusher
    ↓
按配置等待（退避策略）
    ↓
创建并打开新 Pusher
    ↓
进入 WaitingForKeyframe
    ↓
等待新的关键帧
    ↓
恢复 Running
```

### 10.2 多轨支持

当前仅支持单路 H.264 视频，未来可扩展：

- 音频轨道
- 多视频轨道
- 轨道同步

### 10.3 统计信息

计划添加的统计：

- 已发布包数量
- 丢弃包数量（等待关键帧）
- 字节数
- 错误次数
- 重连次数

---

## 11. 与相关模块的对比

### 11.1 PusherSession vs FFmpegPusher

| 特性 | PusherSession | FFmpegPusher |
|---|---|---|
| **职责** | 策略层：何时写 | 实现层：怎么写 |
| **FFmpeg 依赖** | 无 | 直接调用 FFmpeg API |
| **状态管理** | 管理会话状态机 | 仅管理 opened 状态 |
| **关键帧策略** | 等待关键帧 | 不关心 |
| **时间戳策略** | 时间轴归零 | 不修改时间戳 |
| **线程模型** | 双线程（主线程 + 事件线程） | 单线程 |

### 11.2 PusherSession vs MediaStreamSession

| 特性 | PusherSession | MediaStreamSession |
|---|---|---|
| **方向** | 输出（推流） | 输入（拉流） |
| **管理对象** | Pusher | Puller |
| **数据流** | 写入数据包 | 读取数据包 |
| **重连策略** | 输出重连 | 输入重连 |

---

## 12. 常见问题

### Q1: 为什么需要独立的事件线程？

**A:** FFmpeg 的网络操作可能阻塞数秒，如果在主线程中同步处理错误，会导致 Publish() 长时间阻塞。独立事件线程可以：
- 异步接收底层错误通知
- 快速请求停止正在进行的写入操作
- 不阻塞主线程的 Open/Publish/Close 调用

### Q2: 为什么使用代际机制而不是简单的 bool 标志？

**A:** 代际机制可以区分**多次打开/关闭循环**中的事件。如果使用 bool 标志：
- 旧会话的事件可能在新会话打开后被错误处理
- 无法区分"当前会话的事件"和"已关闭会话的事件"

代际机制确保每个事件只影响其所属的会话周期。

### Q3: 为什么 operation_mutex_ 是 recursive_mutex？

**A:** 因为某些操作可能需要递归调用：
- Open() 内部可能调用 Close()
- Close() 内部可能再次调用 Close()（幂等）
- 事件线程处理事件时可能需要调用 RequestStop()

recursive_mutex 允许同一线程多次获取同一把锁。

### Q4: 失败后能否重试同一个 packet？

**A:** 当前**不能重试**。因为：
- `MediaPacket` 可能被底层 FFmpegPusher 消费
- 失败后 packet 状态不确定
- 重连后等待新的关键帧，而不是重试旧包

如果需要重试，必须改用非消费式 packet 契约或建立可重放队列。

---

## 13. 总结

`PusherSession` 是媒体输出链路中的核心编排层，通过以下设计实现了清晰的责任分离：

1. **状态机**：管理会话生命周期，控制数据流行为
2. **关键帧门控**：确保输出从关键帧开始，可独立解码
3. **时间戳策略**：支持保留原始时间戳或从零开始计时
4. **异步事件处理**：独立线程处理底层错误，快速响应
5. **代际机制**：防止过期事件污染新会话
6. **线程安全**：双锁设计，保护并发访问

这种设计使得 Session 层专注于"策略决策"，而将"具体实现"委托给底层 Pusher，符合单一职责原则和开闭原则。
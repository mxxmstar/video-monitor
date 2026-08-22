/// @file test_media_stream_session_scripted.cpp
/// @brief 使用测试内假拉流器验证 MediaStreamSession 的基础状态和统计。

#include "media/puller/i_puller.h"
#include "media/simple_buffer.h"
#include "media/stream/stream_session.h"

#include <boost/asio/io_context.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

/// @brief 按预先编排的结果模拟一个拉流器。
///
/// 这个类只用于单元测试，不连接真实的 RTSP 服务。测试者在构造时传入
/// 一组 PullReadResult，ReadPacket() 每调用一次就返回其中一项。这样可以
/// 稳定地制造 EOS、可重试错误、不可重试错误和主动停止等情况。
///
/// 它的价值在于把“网络是否可用”与“Session 如何处理拉流结果”分开：
/// FFmpegPuller 负责验证真实媒体输入，ScriptedPuller 负责验证 Session
/// 的状态机和统计逻辑。
class ScriptedPuller final : public IPuller {
public:
    /// @param results 按 ReadPacket() 调用顺序排列的模拟读取结果。
    explicit ScriptedPuller(std::vector<PullReadResult> results)
        : results_(std::move(results)) {}

    /// @brief 模拟打开输入端点。
    ///
    /// 假拉流器不真正解析 URI，只检查 URI 不为空。这个检查保留了
    /// IPuller::Open() 的基本配置校验职责。
    PullOpenResult Open(const InputEndpointConfig& endpoint) override {
        if (endpoint.uri.empty()) {
            return PullOpenResult::Failed({
                PullErrorCategory::InvalidConfiguration,
                0,
                "scripted endpoint URI is empty",
                false,
            });
        }

        // 一个 ScriptedPuller 理论上可以被关闭后再次打开，因此每次打开
        // 都从第一条脚本结果开始，并清除之前的关闭状态。
        position_ = 0;
        closed_.store(false);
        return PullOpenResult::Success();
    }

    /// @brief 模拟关闭输入。
    ///
    /// 使用原子变量是为了覆盖 Stop() 与 ReadPacket() 可能并发执行的场景。
    /// 真实 FFmpegPuller 也需要支持在读取阻塞时被 Close() 中断。
    void Close() override {
        closed_.store(true);
    }

    /// @brief 返回下一条预置的读取结果。
    ///
    /// 正常情况下先返回测试包，随后返回测试用例指定的终止或错误状态。
    /// 如果外部先调用 Close()，则优先返回 Stopped，模拟读取被主动取消。
    PullReadResult ReadPacket() override {
        if (closed_.load()) {
            return {
                PullError{
                    PullErrorCategory::Cancelled,
                    0,
                    "scripted puller was closed",
                    false,
                },
                nullptr,
                PullReadStatus::Stopped,
            };
        }

        // 理论上测试用例已经包含终止结果；这里仍保留 EOS 兜底，避免
        // 调用者在脚本耗尽后得到未定义行为。
        if (position_ >= results_.size()) {
            return {
                PullError{
                    PullErrorCategory::EndOfInput,
                    0,
                    "scripted input exhausted",
                    false,
                },
                nullptr,
                PullReadStatus::EOS,
            };
        }

        // position_ 指向当前尚未返回的脚本项。返回后自增，确保下一次
        // ReadPacket() 消费下一项，而不是重复返回当前项。
        return results_[position_++];
    }

    /// @brief 返回固定的模拟流信息。
    ///
    /// Session::Start() 会在 Open() 成功后立即读取这份信息并触发上层
    /// StreamInfoCallback，因此测试可以验证流信息回调是否正常工作。
    MultiStreamInfo GetStreamInfo() const override {
        return stream_info_;
    }

    /// @brief 保存事件回调以满足 IPuller 接口。
    ///
    /// 当前脚本测试不主动生成协议事件，所以不会调用该回调；后续测试
    /// 如果需要验证 puller 事件通知，可以在合适的脚本步骤中使用它。
    void SetEventCallback(EventCallback cb) override {
        event_callback_ = std::move(cb);
    }

private:
    /// 按时间顺序保存本次测试希望拉流器返回的结果。
    std::vector<PullReadResult> results_;

    /// 下一条待读取结果在 results_ 中的下标。
    ///
    /// 例如 results_ 有两项时：
    ///   - position_ == 0：下一次返回 results_[0]
    ///   - 返回 results_[0] 后变为 1
    ///   - position_ == 1：下一次返回 results_[1]
    ///
    /// 它只是“脚本播放进度”，不代表 MediaPacket 的 pts、stream_index
    /// 或字节偏移；Open() 会把它重置为 0。
    std::size_t position_{0};

    /// 是否已经关闭。Close() 可能从 Session 的控制线程调用，而
    /// ReadPacket() 在 Session 读取线程中执行，因此这里使用原子变量。
    std::atomic<bool> closed_{true};

    /// 保存上层设置的事件回调。当前测试不触发事件，但必须实现接口。
    EventCallback event_callback_;

    /// 测试用的固定视频流信息，模拟真实 puller 在 Open() 后探测到的结果。
    MultiStreamInfo stream_info_ = [] {
        MultiStreamInfo info;
        MediaStreamInfo video;
        video.media_type = MediaType::VIDEO;
        video.codec_type = CodecType::H264;
        video.stream_index = 0;
        video.detail = VideoStreamInfo{1920, 1080, 25.0F};
        info.stream_infos.push_back(std::move(video));
        info.video_stream_idx_ = 0;
        return info;
    }();
};

/// @brief 创建一个成功读取结果。
///
/// 为了让测试聚焦 Session，本函数构造最小但有效的 MediaPacket：它有
/// 视频类型、H.264 编码和一个 16 字节的 SimpleBuffer。Session 收到它后
/// 应该调用 PacketCallback，并把包数和字节数分别加一和加 16。
PullReadResult MakePacketResult(std::size_t size) {
    auto packet = std::make_shared<MediaPacket>();
    packet->type = MediaType::VIDEO;
    packet->codec = CodecType::H264;
    packet->stream_index = 0;
    packet->pts = 100;
    packet->dts = 100;
    packet->duration = 40;
    packet->time_base = {1, 1000};
    packet->keyframe = true;
    packet->buffer = std::make_shared<SimpleBuffer>(
        std::vector<std::uint8_t>(size, 0x5A));

    return {
        std::nullopt,
        std::move(packet),
        PullReadStatus::Packet,
    };
}

/// @brief 创建一个没有媒体包的状态结果。
///
/// EOS、Stopped 和错误状态都不携带有效 packet，但通过 PullError 提供
/// 分类、消息和是否可重试等信息，供 Session 或未来的重连策略使用。
PullReadResult MakeStatusResult(PullReadStatus status,
                                PullErrorCategory category,
                                bool retryable,
                                const char* message) {
    return {
        PullError{category, 0, message, retryable},
        nullptr,
        status,
    };
}

/// @brief 描述一个 Session 行为测试场景。
struct TestCase {
    const char* name;  ///< 测试名称，同时作为模拟错误消息。
    PullReadStatus final_read_status;  ///< 第一个包之后返回的状态。
    PullErrorCategory error_category;  ///< 错误状态对应的错误分类。
    bool retryable;  ///< 错误是否允许未来进行重连。
    MediaStreamSession::State expected_state;  ///< 当前 Session 预期状态。
};

/// @brief 执行一个完整的 Session 行为测试场景。
///
/// 每个场景的输入序列固定为：
///
///     有效媒体包 -> 场景指定的最终状态
///
/// 这样既能验证正常包处理，也能验证最终状态处理。状态回调到达后再
/// 调用 Stop()，可以在 Stop() 统一设置 KSTOPPED 之前捕获 FatalError
/// 对应的 KERROR。
bool RunCase(const TestCase& test_case) {
    std::vector<PullReadResult> results;
    results.push_back(MakePacketResult(16));

    // EOS 和 Stopped 使用固定的错误分类；RetryableError 和 FatalError
    // 使用测试用例传入的分类，以便明确验证不同读取结果的含义。
    if (test_case.final_read_status == PullReadStatus::EOS) {
        results.push_back(MakeStatusResult(
            PullReadStatus::EOS,
            PullErrorCategory::EndOfInput,
            false,
            "scripted end of input"));
    } else if (test_case.final_read_status == PullReadStatus::Stopped) {
        results.push_back(MakeStatusResult(
            PullReadStatus::Stopped,
            PullErrorCategory::Cancelled,
            false,
            "scripted stop"));
    } else {
        results.push_back(MakeStatusResult(
            test_case.final_read_status,
            test_case.error_category,
            test_case.retryable,
            test_case.name));
    }

    // MediaStreamSession 当前的读取循环使用独立线程，因此这里不需要
    // 调用 io.run()；io_context 只是满足 Session 的构造接口。
    boost::asio::io_context io;
    auto session = std::make_shared<MediaStreamSession>(io);
    session->SetPuller(std::make_unique<ScriptedPuller>(std::move(results)));

    // ScriptedPuller 不关心 puller_kind，但 Session 仍要求 endpoint 有
    // 非空 URI 才允许 Start()。
    InputEndpointConfig endpoint;
    endpoint.uri = "scripted://session-test";
    endpoint.puller_kind = PullerKind::FFmpeg;
    session->SetEndpoint(endpoint);

    // 本文件专门验证基础读取状态映射。关闭重连后，RetryableError 会
    // 直接进入 KERROR；真正的重连行为由独立测试覆盖。
    SessionConfig session_config;
    session_config.reconnect.enabled = false;
    session->SetSessionConfig(session_config);

    std::mutex mutex;
    std::condition_variable condition;
    int packet_count = 0;
    std::size_t packet_bytes = 0;
    bool stream_info_received = false;
    std::optional<MediaStreamSession::State> terminal_state;

    // Start() 成功后，Session 会先通过这个回调报告流信息，再启动读线程。
    // 只接受包含视频流的信息，避免空信息误判为成功。
    session->SetStreamInfoCallback(
        [&mutex, &condition, &stream_info_received](const MultiStreamInfo& info) {
            if (!info.HasVideoStream()) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                stream_info_received = true;
            }
            condition.notify_all();
        });

    // PacketCallback 由 Session 读取线程调用。回调内只记录数量和字节数，
    // 不做耗时操作，模拟上层接收包后进行基础统计的行为。
    session->SetPacketCallback(
        [&mutex, &condition, &packet_count, &packet_bytes](
            std::shared_ptr<MediaPacket> packet) {
            if (!packet || !packet->buffer || packet->buffer->Size() == 0) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++packet_count;
                packet_bytes += packet->buffer->Size();
            }
            condition.notify_all();
        });

    // 只记录本测试用例关心的最终状态。KCONNECTING 和 KCONNECTED 也会
    // 通过状态回调发出，但它们不是本测试的结束条件。
    session->SetStateCallback(
        [&mutex, &condition, &terminal_state, expected = test_case.expected_state](
            MediaStreamSession::State state) {
            if (state != expected) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                terminal_state = state;
            }
            condition.notify_all();
        });

    // Start() 会同步完成 Open()，随后由读取线程消费脚本结果。
    if (!session->Start()) {
        std::cerr << test_case.name << ": session start failed" << std::endl;
        return false;
    }

    // 读取线程通过状态回调通知测试线程已经到达终态。条件变量避免固定
    // sleep，也能在状态很快到达时立即继续。
    {
        std::unique_lock<std::mutex> lock(mutex);
        const bool finished = condition.wait_for(
            lock,
            std::chrono::seconds(2),
            [&] {
                return terminal_state.has_value();
            });
        if (!finished) {
            std::cerr << test_case.name << ": timed out waiting for state "
                      << MediaStreamSession::StateName(test_case.expected_state)
                      << std::endl;
            session->Stop();
            return false;
        }
    }

    // 终态已经被记录后再停止并等待读取线程退出，保证下面读取统计时
    // 不再有并发写入。对于 FatalError，terminal_state 已经在这里保存了
    // KERROR，之后 Stop() 把当前状态收尾为 KSTOPPED 不会影响断言。
    session->Stop();
    const MediaStreamSession::Stats stats = session->GetStats();

    // 每个场景都应收到一个 16 字节媒体包，并收到一次有效流信息。
    if (stats.packets_received != 1 || stats.bytes_received != 16 ||
        packet_count != 1 || packet_bytes != 16 || !stream_info_received) {
        std::cerr << test_case.name << ": packet or stream statistics mismatch"
                  << std::endl;
        return false;
    }

    std::cout << test_case.name << ": state="
              << MediaStreamSession::StateName(*terminal_state)
              << ", packets=" << stats.packets_received
              << ", bytes=" << stats.bytes_received << std::endl;
    return true;
}

}  // namespace

int main() {
    // 这里直接在代码中写测试参数，不通过 argc/argv 传递，符合当前阶段
    // 先学习 Session 行为、再接入真实 RTSP 流的目标。
    const TestCase test_cases[] = {
        // 正常输入结束：Session 应进入 STOPPED，而不是 ERROR。
        {"EOS", PullReadStatus::EOS, PullErrorCategory::EndOfInput, false,
         MediaStreamSession::State::KSTOPPED},
        // 基础状态测试关闭了重连，因此可重试错误直接进入 ERROR。
        {"RetryableError", PullReadStatus::RetryableError,
         PullErrorCategory::Network, true, MediaStreamSession::State::KERROR},
        // 不可恢复媒体错误应直接进入 ERROR。
        {"FatalError", PullReadStatus::FatalError,
         PullErrorCategory::InvalidMedia, false,
         MediaStreamSession::State::KERROR},
        // 外部主动停止不是错误，应进入 STOPPED。
        {"Stopped", PullReadStatus::Stopped, PullErrorCategory::Cancelled,
         false, MediaStreamSession::State::KSTOPPED},
    };

    for (const TestCase& test_case : test_cases) {
        if (!RunCase(test_case)) {
            return 1;
        }
    }

    std::cout << "MediaStreamSession scripted tests passed" << std::endl;
    return 0;
}

#pragma once

#include "media/puller/i_puller.h"

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

/// @brief 基于 FFmpeg 的拉流器实现
///
/// 实现 IPuller 纯虚接口，仅负责：
///   - Open()       — 创建 AVFormatContext、查找视频流、缓存 StreamInfo
///   - ReadPacket() — av_read_frame -> MediaPacket（零拷贝 FFmpegPacketBuffer）
///   - Close()      — avformat_close_input
///
/// 不负责重连 / watchdog / 状态机 / 统计（均由 StreamSession 管理）。
class FFmpegPuller : public IPuller {
public:
    explicit FFmpegPuller(FFmpegPullerConfig config);
    ~FFmpegPuller() override;

    // ==================== 生命周期 ====================

    /// @brief 打开流
    /// @param endpoint 输入端点配置
    /// @return 打开流结果结构体
    PullOpenResult Open(const InputEndpointConfig& endpoint) override;

    /// @brief 关闭流
    void Close() override;

    /// @brief 读取一个媒体包
    /// @return 读取媒体包结果结构体
    PullReadResult ReadPacket() override;

    /// @brief 获取流信息
    MultiStreamInfo GetStreamInfo() const override;

    /// @brief 设置事件回调
    void SetEventCallback(EventCallback cb) override;

private:
    /// @brief FFmpeg 中断回调上下文
    struct InterruptContext {
        std::atomic<bool> stop_requested{false};     ///< 是否请求停止读取
        std::atomic<bool> timed_out{false};            ///< 是否超时
        std::chrono::steady_clock::time_point deadline{};   ///< 超时时间点
    };

    /// @brief 视频流探针上下文
    struct VideoProbeContext {
        AVCodecParserContext* parser{nullptr};
        AVCodecContext* codec{nullptr};
        int packets{0};
        std::chrono::steady_clock::time_point started;
    };

    bool probeVideoStream(int stream_index, const AVPacket* packet);
    bool isVideoStreamInfoComplete(int stream_index) const;
    bool isVideoProbeExhausted(int stream_index) const;
    void resetVideoProbes();
    void updateVideoStreamInfo(int stream_index, int width, int height);

    // ── FFmpeg 资源 ──
    AVFormatContext*   fmt_ctx_{nullptr};     ///< 格式上下文
    // 指针由 AVFormatContext 持有，codecpars_ 本身不负责释放。
    std::map<int, AVCodecParameters*> codecpars_; ///< 已选择的流编码参数

    InterruptContext   interrupt_ctx_;         ///< 中断回调上下文
    MultiStreamInfo    cached_info_;           ///< 缓存的流信息
    std::map<int, std::unique_ptr<VideoProbeContext>> video_probes_; ///< 视频流探针上下文池
    mutable std::mutex info_mutex_; ///< 保护 cached_info_ 的互斥锁

    mutable std::mutex io_mutex_;              ///< 避免 Close 与 av_read_frame 并发关闭句柄

    // ── 配置 ──
    FFmpegPullerConfig config_;                ///< 拉流器配置，构造时注入，后续不再修改

    // ── 对象池 ──
    std::vector<AVPacket*> packet_pool_;       ///< AVPacket 对象池
    std::mutex             pool_mutex_;        ///< 保护对象池的互斥锁

    // ── 回调 ──
    EventCallback event_cb_;                   ///< 事件回调
    mutable std::mutex callback_mutex_; ///< 保护 event_cb_ 的互斥锁
};


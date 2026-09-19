#pragma once
#include <string>
#include <atomic>
#include <chrono>
#include <optional>
#include <utility>
#include <map>
#include "media/media_packet.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
}

struct MediaTrackConfig;

/// @brief Muxer I/O 选项
struct MuxerIoOptions {
    std::chrono::milliseconds open_timeout{5000};
    std::chrono::milliseconds write_timeout{10000};
};

enum MuxerProtocol {
    Rtmp,
    Rtsp,    
};

struct MuxerOptions {
    std::string protocol{};
    /// @brief 配置 AVFormatContext 的参数，如MP4 的 movflags、RTSP 的 rtsp_transport、FLV 的 flvflags 等
    std::map<std::string, std::string> extra_muxer_options;
};

/// @brief Muxer 选项
enum class MuxerOperation {
    AllocateContext, 
    CreateStream, 
    ConfigureStream, 
    OpenIo,
    WriteHeader, 
    WritePacket, 
    WriteTrailer, 
    CloseIo,
};


/// @brief Muxer 错误分类
/// @brief 不包含重试策略；Pusher 负责解释底层错误。
enum class MuxerErrorCategory {
    OpenFailed,              ///< 打开失败
    WriteFailed,             ///< 写入失败
    Timeout,                 ///< 超时
    Internal,                ///< 内部错误
    Cancelled,
    CloseFailed,
};

/// @brief Muxer 错误结构体
struct MuxerError {
    MuxerErrorCategory category;     ///< 错误分类
    int native_code{0};        // FFmpeg 的 AVERROR(...)
    std::string message;        ///< 错误消息
    MuxerOperation operation;

    MuxerError(MuxerErrorCategory category, int native_code, std::string message,
               MuxerOperation operation)
        : category(category), native_code(native_code), message(std::move(message)),
          operation(operation) {}
};

/// @brief Muxer 结果结构体
struct MuxerResult {
    std::optional<MuxerError> error;     ///< 错误信息

    bool Succeed() const noexcept {
        return !error.has_value();
    }

    static MuxerResult Success() {
        return {};
    }

    /// @brief 创建失败结果
    static MuxerResult Failed(MuxerError error) {
        return {std::move(error)};
    }
};

/// @brief FFmpeg Muxer
class FFmpegMuxer {
public:
    FFmpegMuxer();
    ~FFmpegMuxer();
    FFmpegMuxer(const FFmpegMuxer&) = delete;
    FFmpegMuxer& operator=(const FFmpegMuxer&) = delete;

    /// @brief 打开muxer
    /// @param output_url 输出URL
    /// @param video_info 视频流信息
    /// @return Muxer 结果结构体
    MuxerResult Open(const std::string& output_url, const MediaTrackConfig& config,
                     const MuxerIoOptions& io = {}, const MuxerOptions& muxer_options = {});

    /// @brief 参数需由调用方校验；进入 FFmpeg 写入后 AVPacket 被消费，不能重用。
    MuxerResult Write(const MediaPacket& packet);

    MuxerResult Close();

    /// @brief 请求停止写入
    void RequestStop();

    /// @brief 是否超时
    /// @return true 超时 false 未超时
    bool IsTimeOut() const { return interrupt_ctx_.timed_out.load(); }

    bool IsCanceled() const { return interrupt_ctx_.stop_requested.load(); }


private:
    void beginOperation(std::chrono::milliseconds timeout);
    MuxerResult failure(MuxerOperation operation, MuxerErrorCategory category, int code);
    MuxerIoOptions io_;
    /// @brief FFmpeg 中断回调上下文
    struct InterruptContext {
        std::atomic<bool> stop_requested{false};     ///< 是否请求停止写入
        std::atomic<bool> timed_out{false};            ///< 是否超时
        std::chrono::steady_clock::time_point deadline{
            std::chrono::steady_clock::time_point::max()};
    };


    InterruptContext interrupt_ctx_; ///< 中断回调上下文

    std::string output_url_{};     ///< 输出URL
    AVFormatContext* format_ctx_ = nullptr;
    AVStream* video_stream_ = nullptr;
    bool header_written_{false}; ///< 是否已写入头信息
    bool normalize_timestamps_{false}; ///< 是否需要归一化时间戳
    bool timestamp_offset_set_{false}; ///< 时间戳偏移量是否设置
    int64_t timestamp_offset_{0}; ///< 时间戳偏移量
};

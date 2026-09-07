#pragma once
#include <string>
#include "media/stream/stream_info.h"
#include "media/publisher/publisher_config.h"
#include "media/media_packet.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
}

class FFmpegMuxer {
public:
    FFmpegMuxer();
    ~FFmpegMuxer();

    /// @brief 打开muxer
    /// @param output_url 输出URL
    /// @param video_info 视频流信息
    /// @return true 成功 false 失败
    bool Open(const std::string& output_url, const MediaTrackConfig& config);

    /// @brief 将编码视频包写入输出容器。
    ///
    /// 当前实现采用消费式语义：packet 必须由 FFmpeg AVPacket 支持；
    /// 通过参数校验并进入 FFmpeg 写入调用后，该 AVPacket 不能再次重试或复用。
    bool Write(const MediaPacket& packet);

    void Close();

private:
    std::string output_url_{};    
    AVFormatContext* format_ctx_ = nullptr;
    AVStream* video_stream_ = nullptr;
    bool header_written_{false}; ///< 是否已写入头信息
    bool normalize_timestamps_{false}; ///< 是否需要归一化时间戳
    bool timestamp_offset_set_{false}; ///< 时间戳偏移量是否设置
    int64_t timestamp_offset_{0}; ///< 时间戳偏移量
};

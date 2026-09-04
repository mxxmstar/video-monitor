#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <variant>

#include "media/media_frame.h"
#include "media/media_packet.h"

using FramePtr = std::shared_ptr<MediaFrame>;
using PacketPtr = std::shared_ptr<MediaPacket>;

// 视频编码配置
struct VideoEncoderConfig {
    int width{0};                    // 视频宽度
    int height{0};                   // 视频高度
    int fps_num{25};                 // 帧率分子
    int fps_den{1};                  // 帧率分母
    PixelFormat pixel_format{PixelFormat::kI420}; // 输入像素格式
    int gop_size{50};                // GOP 大小（关键帧间隔）
    int max_b_frames{0};             // 最大 B 帧数
    int64_t max_bitrate{0};          // 最大码率(bps)
    int rc_buffer_size{0};           // VBV缓冲区大小（单位：字节）
    int sws_flags{2};             // 图像缩放标志位

    std::string preset{"ultrafast"}; // 编码器 preset（支持 preset 参数的编码器使用）
    std::string tune{"zerolatency"}; // 编码器 tune 参数（如 zerolatency 通过禁用 B 帧、关闭参考帧缓存来降低延迟）
    int crf{-1};                     // CRF 质量控制值（< 0 表示不启用）
    void Dump() const;
};

// 音频编码配置
struct AudioEncoderConfig {
    int sample_rate{0};              // 采样率
    int channels{0};                 // 通道数
    uint64_t channel_layout{0};      // 通道布局
    SampleFormat sample_format{SampleFormat::S16P}; // 输入采样格式
    void Dump() const;
};

// 编码器配置参数
struct EncoderConfig {
    MediaType   media_type{MediaType::VIDEO};    // 媒体类型（视频/音频）
    CodecType   codec_type{CodecType::H264};     // 编码格式（H264/H265/AAC/OPUS）

    std::variant<VideoEncoderConfig, AudioEncoderConfig> specific; // 独有配置

    int64_t bitrate{2'000'000};      // 目标码率(bps)

    // 时间基，如果 <= 0 则自动由 fps 或 sample_rate 推导
    int time_base_num{0};
    int time_base_den{0};

    std::string encoder_name;        // 指定的 FFmpeg 编码器名称（如 "libx264"），为空则自动选择
        
    bool global_header{false};       // 是否在 extradata 中存储全局头信息
    int thread_count{1};             // 编码线程数
    bool is_valid() const;
    
    bool is_video() const;
    bool is_audio() const;

    AudioEncoderConfig& audio();
    VideoEncoderConfig& video();

    const AudioEncoderConfig& audio() const;
    const VideoEncoderConfig& video() const;
};

/// @brief 编码后的轨道信息
struct EncodedTrackInfo {
    MediaType media_type{MediaType::UNKNOWN};    // 媒体类型（视频/音频）
    CodecType codec_type{CodecType::UNKNOWN};    // 编码格式（H264/H265/AAC/OPUS）
    Rational time_base{1, 1000000};             // 编码器使用的时间基（如 1/1000000）
    int width{0};                                // 视频宽度
    int height{0};                               // 视频高度
    float fps{0.0f};                             // 帧率
    int sample_rate{0};                          // 采样率
    int channels{0};                             // 通道数
    std::vector<std::uint8_t> extra_data;        // 当前编码会话的 codec extradata
};
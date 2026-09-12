#pragma once

#include <string>

#include "media/media_packet.h"
#include <variant>


/// @brief 视频轨道信息
struct VideoTrackConfig {
    int width{0};                                // 视频宽度
    int height{0};                               // 视频高度
    float fps{0.0f};                             // 帧率
};

/// @brief 音频轨道信息
struct AudioTrackConfig {
    int sample_rate{0};                          // 采样率
    int channels{0};                             // 通道数
};

struct MediaTrackConfig {
    int track_id{0};
    MediaType media_type{MediaType::VIDEO};
    CodecType codec_type{CodecType::H264};

    std::variant<VideoTrackConfig, AudioTrackConfig> track_config;

    int time_base_num{1};
    int time_base_den{1000000};
    std::vector<std::uint8_t> extra_data;



    bool is_valid() const;        
    
    bool is_video() const;
    bool is_audio() const;

    AudioTrackConfig& audio();
    VideoTrackConfig& video();

    const AudioTrackConfig& audio() const;
    const VideoTrackConfig& video() const;
};

/// @brief 单个输出目标的最小配置。
/// 初版只支持一条 H.264 视频轨道写入一个 FFmpeg 可识别的输出 URL。
struct PusherConfig {
    std::string output_url;           ///< 输出文件路径或输出协议 URL
    MediaTrackConfig video_track;     ///< 已编码视频轨道的参数与时间基
    
    bool is_valid() const;
};

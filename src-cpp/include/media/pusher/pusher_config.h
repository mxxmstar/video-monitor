#pragma once

#include <string>
#include <chrono>
#include <map>
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

/// @brief Pusher 通用 I/O 配置
struct PusherIoConfig {
    std::chrono::milliseconds connect_timeout{5000};     ///< 连接超时时间 , 默认 5s
    std::chrono::milliseconds write_timeout{10000};       ///< 写入超时时间 , 默认 10s
};



/// @brief rtsp 常用配置
struct RtspOutputOptions {
    std::string transport{"tcp"};
};

struct RtmpOutputOptions {
    std::optional<std::string> app;
    std::optional<std::string> playpath;
    std::optional<bool> tcp_nodelay;
};

struct FFmpegPusherConfig {
    /// @brief 输出格式
    std::optional<std::string> output_format;
    std::optional<RtspOutputOptions> rtsp;
    std::optional<RtmpOutputOptions> rtmp;
    /// @brief 配置 AVIOContext 的参数，tcp缓冲区大小，http头，代理设置等
    std::map<std::string, std::string> extra_io_options;
    /// @brief 配置 AVFormatContext 的参数，如MP4 的 movflags、FLV 的 flvflags 等
    std::map<std::string, std::string> extra_muxer_options;
};

/// @brief 单个输出目标的最小配置。
/// 初版只支持一条 H.264 视频轨道写入一个 FFmpeg 可识别的输出 URL。
struct PusherConfig {
    std::string output_url;           ///< 输出文件路径或输出协议 URL
    MediaTrackConfig video_track;     ///< 已编码视频轨道的参数与时间基
    PusherIoConfig io;                ///< 0 禁用超时，负数无效
    FFmpegPusherConfig ffmpeg;        ///< FFmpeg 输出配置
    std::optional<RtspOutputOptions> rtsp;
    std::optional<RtmpOutputOptions> rtmp;
    bool is_valid() const;
};

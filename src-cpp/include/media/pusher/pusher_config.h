#pragma once

#include <string>

#include "media/publisher/publisher_config.h"

/// @brief 单个输出目标的最小配置。
/// 初版只支持一条 H.264 视频轨道写入一个 FFmpeg 可识别的输出 URL。
struct PusherConfig {
    std::string output_url;           ///< 输出文件路径或输出协议 URL
    MediaTrackConfig video_track;     ///< 已编码视频轨道的参数与时间基
    
    bool is_valid() const;
};

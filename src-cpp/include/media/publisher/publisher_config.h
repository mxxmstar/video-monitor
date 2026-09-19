#pragma once

#include "media/pusher/pusher_session.h"

/// @brief 发布目标类型。
///
/// Publisher 只根据该类型选择发布路线，不处理对应路线的封装、协议和编码
/// 细节。当前仅实现 FFmpeg 文件输出；将来增加 RTSP Server 等路线时，在此
/// 扩展枚举和值对应的配置类型即可。
enum class PublisherKind {
    FFmpegFile,
    ZLMRTSP,
};

/// @brief 对上层暴露的一次发布任务配置。
///
/// PublisherConfig 的职责是选择发布路线，并持有该路线通用的会话配置。
/// 输出地址、容器格式和音视频轨道参数已属于具体输出目标，因此由
/// PusherSessionConfig::pusher（即 PusherConfig）持有，不能在这里重复保存。
struct PublisherConfig {
    PublisherKind kind{PublisherKind::FFmpegFile};
    PusherSessionConfig session;

    /// @brief 校验当前 Publisher 是否能创建所选路线及其会话。
    bool is_valid() const {
        return session.is_valid();
    }
};

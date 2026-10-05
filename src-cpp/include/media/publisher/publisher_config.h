#pragma once

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

#include "media/pusher/pusher_session.h"

/// @brief 发布目标类型。
///
/// Publisher 只根据该类型选择发布路线，不处理对应路线的封装、协议和编码
/// 细节。当前仅实现 FFmpeg 文件输出；将来增加 RTSP Server 等路线时，在此
/// 扩展枚举和值对应的配置类型即可。
enum class PublisherKind {
    Client,
    Server,
};

/// @brief 对上层暴露的一次发布任务配置。
///
/// PublisherConfig 的职责是选择发布路线，并配置各个独立的输出会话。
/// 输出地址、容器格式和音视频轨道参数已属于具体输出目标，因此由
/// PusherSessionConfig::pusher（即 PusherConfig）持有，不能在这里重复保存。
struct PublisherTargetConfig {
    std::string target_id;
    PusherSessionConfig session;
};

struct PublisherConfig {
    PublisherKind kind{PublisherKind::Client};
    std::vector<PublisherTargetConfig> targets;

    /// @brief 校验当前 Publisher 是否能创建所选路线及其会话。
    bool is_valid() const {
        if (kind != PublisherKind::Client || targets.empty()) {
            return false;
        }
        std::unordered_set<std::string> ids;
        const auto mode = targets.front().session.mode;
        return std::all_of(targets.begin(), targets.end(), [&](const PublisherTargetConfig& target) {
            return !target.target_id.empty() && ids.insert(target.target_id).second &&
                   target.session.mode == mode && target.session.is_valid();
        });
    }
};

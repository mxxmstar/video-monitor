#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "inference_cancellation.h"

/// @brief 一次推理请求在 pipeline、Session、provider 和结果之间透传的稳定上下文。
///
/// stream_id 和 frame_id 由视频 pipeline 分配；Session 不根据 PTS 猜测它们。
/// request_id 为本次提交的唯一标识，调用方未填写时由 Session 自动生成。model_name
/// 和 model_generation 都由已绑定 Session 的实际模型覆盖，防止调用方上下文与正在
/// 执行的模型或热更新代次不一致。
struct InferContext {
    uint64_t stream_id{0};    ///< 上游流标识；0 表示调用方尚未提供。
    uint64_t frame_id{0};     ///< 上游帧标识；0 表示调用方尚未提供。
    uint64_t request_id{0};   ///< 单次推理请求标识；0 表示由 Session 自动分配。
    std::string model_name;   ///< 实际执行模型的稳定名称。
    /// @brief Session 成功装配模型时分配的单调代次；0 表示当前请求尚未绑定模型。
    ///
    /// 同名模型重新加载也会得到新的代次。上层可用该值区分热更新前后的结果，后续
    /// M2-5 将以它作为抑制晚到旧结果的基础，调用方不应自行指定其业务含义。
    uint64_t model_generation{0};
    uint64_t pts{0};          ///< 时间戳，单位为微秒。
    /// @brief 调用方持有的单请求取消令牌；空值表示该请求不可单独取消。
    ///
    /// Session 和 scheduler 只读取令牌状态，不替调用方调用 Cancel()。支持原生
    /// 取消的 provider 会订阅它来中断运行中的 request；不支持的 provider 至少会
    /// 在完成路径返回 kCancelled 并跳过后处理。
    std::shared_ptr<InferenceCancellationToken> cancellation;
};

#pragma once

#include <memory>
#include <functional>

#include "media/media_packet.h"
#include "media/media_frame.h"
#include "media/stream/stream_info.h"


class IDecoder {
public:
    using FrameCallback = std::function<void(std::shared_ptr<MediaFrame>)>;

    virtual ~IDecoder() = default;

    /// @brief 初始化解码器
    /// @param info 流信息(编码格式，分辨率，extra_data等)    
    virtual bool Open(const MediaStreamInfo& info) = 0;

    /// @brief 解码 MediaPacket
    ///
    /// 解码产生的帧通过 SetFrameCallback 注册的回调逐帧返回。
    /// 返回 false 表示解码器内部错误，应关闭重建。
    virtual bool Decode(std::shared_ptr<MediaPacket> packet) = 0;

    /// @brief 显式刷新解码器内部缓存。
    ///
    /// Flush 的输出仍通过 FrameCallback 发送。调用方必须在 Graph 的停止屏障内
    /// 调用 Flush，确认输出完成后再调用 Close；Close 本身不再隐式产生媒体帧。
    virtual bool Flush() = 0;

    /// @brief 关闭解码器，释放所有资源。
    virtual void Close() = 0;

    /// @brief 设置解码帧回调
    /// @param cb 每次解码出一帧时被调用
    virtual void SetFrameCallback(FrameCallback cb) = 0;
};

class DecoderStats {
    uint64_t decoded_packet_count_{0}; ///< 已解码的包数量
    uint64_t decoded_frame_count_{0}; ///< 已解码的帧数量
    
    uint64_t total_decode_time_us_{0}; ///< 总解码时间（微秒）
    uint64_t max_decode_time_us_{0}; ///< 最大单帧解码时间（微秒）
    uint64_t min_decode_time_us_{0}; ///< 最小单帧解码时间（微秒）

    uint64_t avg_decode_time_us_{0}; ///< 平均单帧解码时间（微秒）    
};
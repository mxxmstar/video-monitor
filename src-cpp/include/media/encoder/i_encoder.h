#pragma once
#if 1

#include "media/encoder/encoder_config.h"

#define ENCODE_STATS_ENABLE 1


/// @brief 编码器抽象接口
class IEncoder {
public:
    /// @brief 编码器统计信息
    struct EncoderStats {            
        uint64_t encode_packets{0}; ///< 已编码的包数量
        uint64_t encode_frames{0}; ///< 已编码的帧数量
        uint64_t encode_calls{0}; ///< 编码调用数量
        uint64_t encode_errors{0}; ///< 编码错误调用数量
        
        uint64_t total_encode_time_us{0}; ///< 总编码时间（微秒）
        uint64_t max_encode_time_us{0}; ///< 最大单帧编码时间（微秒）
        uint64_t min_encode_time_us{UINT32_MAX}; ///< 最小单帧编码时间（微秒）

        uint64_t avg_encode_time_us{0}; ///< 平均单帧编码时间（微秒）
    };

    virtual ~IEncoder() = default;

    /// @brief 打开编码器并应用配置，返回是否成功
    virtual bool Open(const EncoderConfig& cfg) = 0;
    /// @brief 编码一帧数据，frame==nullptr 表示刷新（flush）编码器
    virtual bool Encode(FramePtr frame, std::vector<PacketPtr>& packets) = 0;
    /// @brief 显式刷新编码器并返回所有残留 packet；Close 不负责向外输出 packet。
    virtual bool Flush(std::vector<PacketPtr>& packets) = 0;
    /// @brief 查询当前编码会话的实际输出描述。
    virtual EncodedTrackInfo GetOutputInfo() const = 0;
    /// @brief 关闭编码器，释放资源
    virtual void Close() = 0;

#if ENCODE_STATS_ENABLE
    /// @brief 获取编码器统计信息
    virtual const EncoderStats& GetStats() const {
        return stats;
    }
    /// @brief 重置编码器统计信息
    virtual void ResetStats() {
        stats = EncoderStats{};
    }
    EncoderStats stats;
#endif
};
#endif
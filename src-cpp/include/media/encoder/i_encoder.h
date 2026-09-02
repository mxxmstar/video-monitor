#pragma once
#if 1

#include "media/encoder/encoder_config.h"

/// @brief 编码器抽象接口
class IEncoder {
public:
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
};
#endif
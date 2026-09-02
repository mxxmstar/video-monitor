#pragma once
#if 1
#include "media/encoder/i_encoder.h"
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
}
class MediaFrameConverter;
class FFmpegEncoder : public IEncoder {
public:
    FFmpegEncoder() = default;
    ~FFmpegEncoder() override;

    FFmpegEncoder(const FFmpegEncoder&) = delete;
    FFmpegEncoder& operator=(const FFmpegEncoder&) = delete;

    /// @brief 打开编码器并应用配置，返回是否成功
    bool Open(const EncoderConfig& cfg) override;
    /// @brief 编码一帧数据，frame==nullptr 表示刷新（flush）编码器
    bool Encode(FramePtr frame, std::vector<PacketPtr>& packets) override;
    /// @brief 显式刷新编码器并返回所有残留 packet；Close 不负责向外输出 packet。
    bool Flush(std::vector<PacketPtr>& packets) override;
    /// @brief 查询当前编码会话的实际输出描述。
    EncodedTrackInfo GetOutputInfo() const override;
    /// @brief 关闭编码器，释放资源
    void Close() override;

private:
    /// @brief 编码视频帧
    /// @param frame 输入视频帧
    /// @param packets 输出 packet 列表
    /// @return 是否成功
    bool encodeVideoFrame(FramePtr frame, std::vector<PacketPtr>& packets);
    
    /// @brief 编码音频帧
    /// @param frame 输入音频帧
    /// @param packets 输出 packet 列表
    /// @return 是否成功
    bool encodeAudioFrame(FramePtr frame, std::vector<PacketPtr>& packets);
    
    /// @brief 从编码器中接收所有已编码 packet
    /// @param packets 输出 packet 列表
    /// @return 是否成功
    bool receivePackets(std::vector<PacketPtr>& packets);

    /// @brief 向编码器发送一帧数据
    /// @param frame 输入帧
    /// @param packets 输出 packet 列表
    /// @return 是否成功
    /// @note 负责释放返回的指针
    bool sendFrameToEncoder(AVFrame* frame, std::vector<PacketPtr>& packets);


    /// @brief 检查编码器是否支持指定像素格式
    /// @param codec 编码器指针
    /// @param fmt 像素格式
    /// @return 是否支持
    /// @note 负责释放返回的指针
    bool isPixelFormatSupported(const AVCodec* codec, AVPixelFormat fmt) const;
    /// @brief 检查编码器是否支持指定采样格式
    /// @param codec 编码器指针
    /// @param fmt 采样格式
    /// @return 是否支持
    /// @note 负责释放返回的指针
    bool isSampleFormatSupported(const AVCodec* codec, AVSampleFormat fmt) const;

    /// @brief 查找视频编码器
    /// @param codec_id 编码器 ID
    /// @param input_fmt 输入像素格式
    /// @param encoder_name 编码器名称
    /// @param encoder_fmt 输出像素格式
    /// @return 找到的编码器指针，或 nullptr
    const AVCodec* findVideoEncoder(AVCodecID codec_id, AVPixelFormat input_fmt,
            const std::string& encoder_name, AVPixelFormat& encoder_fmt) const;
    /// @brief 查找音频编码器
    /// @param codec_id 编码器 ID
    /// @param input_fmt 输入采样格式
    /// @param encoder_name 编码器名称
    /// @param encoder_fmt 输出采样格式
    /// @return 找到的编码器指针，或 nullptr
    const AVCodec* findAudioEncoder(AVCodecID codec_id, AVSampleFormat input_fmt,
            const std::string& encoder_name, AVSampleFormat& encoder_fmt) const;    

    /// @brief 解析帧 pts，若帧的 pts 为 0，则自动分配递增的 pts
    int64_t resolveFramePts(const MediaFrame& frame);

    EncoderConfig config_;  ///< 编码器配置
    AVCodecContext* codec_ctx_ = nullptr;  ///< 编码器上下文
    int64_t next_pts_ = 0;  ///< pts 计数器，用于递增分配

    std::unique_ptr<MediaFrameConverter> converter_{};  ///< 帧转换器

};

#endif
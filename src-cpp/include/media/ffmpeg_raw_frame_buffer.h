#pragma once
/// @file ffmpeg_raw_frame_buffer.h
/// 直接持有 FFmpeg AVFrame 的媒体缓冲区。

#include "media/i_media_buffer.h"

#ifndef RAW_FRAME_BUFFER
#define RAW_FRAME_BUFFER    0
#endif


struct AVFrame;

/// @brief 直接接管（不复制） FFmpeg AVFrame 的媒体缓冲区，析构时调用 av_frame_free()。
/// AVFrame 通常是多平面的，多个平面可能位于不同的内存地址，因而不能
/// 直接用 IMediaBuffer 的 Data()/Size() 表示整帧连续数据。本类仍实现
/// IMediaBuffer，是为了放入 MediaFrame::buffer；真正访问 FFmpeg 数据时，
/// 应通过 GetFrame() 或 PlaneData() 访问。
class FFmpegRawFrameBuffer : public IMediaBuffer {
public:
    ///@brief 接管 frame 所有权，不复制 frame 中的任何平面数据。
    explicit FFmpegRawFrameBuffer(AVFrame* frame);
    ~FFmpegRawFrameBuffer() override;

    FFmpegRawFrameBuffer(const FFmpegRawFrameBuffer&) = delete;
    FFmpegRawFrameBuffer& operator=(const FFmpegRawFrameBuffer&) = delete;

    ///@brief Raw AVFrame 可能是多平面的，没有单一连续的 Data()，因此返回空指针。
    uint8_t* Data() override;
    const uint8_t* Data() const override;

    ///@brief Raw AVFrame 不提供单一连续字节区。
    bool IsContiguous() const override { return false; }

    ///@brief Raw AVFrame 不是连续 buffer，因此返回 0。
    size_t Size() const override;

    ///@brief 获取由本类持有的 AVFrame。
    AVFrame* GetFrame() const { return frame_; }

    ///@brief 判断 AVFrame 是否包含当前媒体模块可访问的有效平面。
    bool IsValid() const;

    ///@brief 获取指定平面数据。plane 从 0 开始。
    uint8_t* PlaneData(int plane);
    const uint8_t* PlaneData(int plane) const;

    ///@brief 获取 FFmpeg 根据格式计算出的平面数量。
    int PlaneCount() const;

    ///@brief 获取指定平面的实际行跨度；无效索引返回 0。
    int PlaneStride(int plane) const;

    ///@brief 获取指定平面的可读字节数；无法计算时返回 0。
    size_t PlaneSize(int plane) const;

private:
    AVFrame* frame_{nullptr};  ///< 本类独占并负责释放的 AVFrame
};

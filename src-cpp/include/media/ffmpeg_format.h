#pragma once
#include "media/media_frame.h"


extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}
namespace Media {
AVPixelFormat ToAVPixelFormat(PixelFormat format) {
    switch (format) {
        case PixelFormat::kNV12:  return AV_PIX_FMT_NV12;
        case PixelFormat::kNV21:  return AV_PIX_FMT_NV21;
        case PixelFormat::kI420:  return AV_PIX_FMT_YUV420P;
        case PixelFormat::kBGR24: return AV_PIX_FMT_BGR24;
        case PixelFormat::kRGB24: return AV_PIX_FMT_RGB24;
        case PixelFormat::kGRAY8: return AV_PIX_FMT_GRAY8;
        default:                 return AV_PIX_FMT_NONE;
    }
}

AVSampleFormat ToAVSampleFormat(SampleFormat format) {
    switch (format) {
        case SampleFormat::U8:   return AV_SAMPLE_FMT_U8;
        case SampleFormat::S16:  return AV_SAMPLE_FMT_S16;
        case SampleFormat::S32:  return AV_SAMPLE_FMT_S32;
        case SampleFormat::FLT:  return AV_SAMPLE_FMT_FLT;
        case SampleFormat::DBL:  return AV_SAMPLE_FMT_DBL;
        case SampleFormat::U8P:  return AV_SAMPLE_FMT_U8P;
        case SampleFormat::S16P: return AV_SAMPLE_FMT_S16P;
        case SampleFormat::S32P: return AV_SAMPLE_FMT_S32P;
        case SampleFormat::FLTP: return AV_SAMPLE_FMT_FLTP;
        case SampleFormat::DBLP: return AV_SAMPLE_FMT_DBLP;
        default:                 return AV_SAMPLE_FMT_NONE;
    }
}

PixelFormat FromAVPixelFormat(AVPixelFormat format) {
    switch (format) {
        case AV_PIX_FMT_NV12:    return PixelFormat::kNV12;
        case AV_PIX_FMT_NV21:    return PixelFormat::kNV21;
        case AV_PIX_FMT_YUV420P: return PixelFormat::kI420;
        case AV_PIX_FMT_YUVJ420P: return PixelFormat::kI420;
        case AV_PIX_FMT_BGR24:   return PixelFormat::kBGR24;
        case AV_PIX_FMT_RGB24:   return PixelFormat::kRGB24;
        case AV_PIX_FMT_GRAY8:   return PixelFormat::kGRAY8;
        default:                 return PixelFormat::kUnknown;
    }
}

SampleFormat FromAVSampleFormat(AVSampleFormat format) {
    switch (format) {
        case AV_SAMPLE_FMT_U8:   return SampleFormat::U8;
        case AV_SAMPLE_FMT_S16:  return SampleFormat::S16;
        case AV_SAMPLE_FMT_S32:  return SampleFormat::S32;
        case AV_SAMPLE_FMT_FLT:  return SampleFormat::FLT;
        case AV_SAMPLE_FMT_DBL:  return SampleFormat::DBL;
        case AV_SAMPLE_FMT_U8P:  return SampleFormat::U8P;
        case AV_SAMPLE_FMT_S16P: return SampleFormat::S16P;
        case AV_SAMPLE_FMT_S32P: return SampleFormat::S32P;
        case AV_SAMPLE_FMT_FLTP: return SampleFormat::FLTP;
        case AV_SAMPLE_FMT_DBLP: return SampleFormat::DBLP;
        default:                 return SampleFormat::Unknown;
    }
}
}
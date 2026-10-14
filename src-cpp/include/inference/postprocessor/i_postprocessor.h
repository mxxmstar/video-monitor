#pragma once
#include "tensordata/tensor_frame.h"
#include "media/media_frame.h"
#include "info/result.h"
class IPostprocessor {
public:
    /// @brief 允许通过接口指针安全释放具体后处理器
    ///
    /// 后处理器与预处理器一样由模型对象通过基类智能指针管理，因此必须保证
    /// 派生类资源能够在接口指针销毁时完整释放。
    virtual ~IPostprocessor() = default;

    virtual FrameResult Process(const TensorFrame& output) = 0;
};

#pragma once
#include "tensordata/tensor_frame.h"
#include "media/media_frame.h"
class IPreprocessor {
public:
    /// @brief 允许通过接口指针安全释放具体预处理器
    ///
    /// 预处理器由模型对象以 `unique_ptr<IPreprocessor>` 持有；如果基类析构函数
    /// 不是虚函数，释放派生实现时将不会执行派生析构逻辑，属于未定义行为。
    virtual ~IPreprocessor() = default;

    /// @brief 将MediaFrame转换为TensorFrame
    virtual TensorFrame Process(const MediaFrame&) = 0;
};

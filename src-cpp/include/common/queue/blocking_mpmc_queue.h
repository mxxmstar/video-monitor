#pragma once

#include "cameron/blockingconcurrentqueue.h"

#include <utility>

template<typename T>
class BlockingMpmcQueue {
public:
    bool push(const T& item) { return queue_.enqueue(item); }
    bool push(T&& item) { return queue_.enqueue(std::move(item)); }
    void wait_pop(T& item) { queue_.wait_dequeue(item); }

private:
    moodycamel::BlockingConcurrentQueue<T> queue_;
};

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

namespace hermes {

// EventBus<T> is a thread-safe bounded MPSC ring buffer (multiple producers, one consumer).
// It is designed to decouple the hot-path sampler thread from the policy/aggregator thread.
//
// When the buffer is full, push() drops the oldest item and returns false to signal overflow.
// This is intentional: the sampler must never block waiting for the policy thread.
//
// Usage:
//   EventBus<PressureSample> bus(128);
//   // sampler thread:
//   bus.push(sample);
//   // policy thread:
//   while (auto item = bus.pop()) { process(*item); }

template <typename T>
class EventBus {
public:
    explicit EventBus(std::size_t capacity = 256)
        : capacity_(capacity)
        , buffer_(capacity) {}

    // Push an item from any thread. Returns true if enqueued, false if overflowed
    // (oldest item dropped to make room).
    bool push(T item) {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool overflow = size_ >= capacity_;
        if (overflow) {
            // Drop oldest item
            read_ = (read_ + 1) % capacity_;
            --size_;
            ++drop_count_;
        }
        buffer_[write_] = std::move(item);
        write_ = (write_ + 1) % capacity_;
        ++size_;
        cv_.notify_one();
        return !overflow;
    }

    // Pop the next item (non-blocking). Returns nullopt if the buffer is empty.
    std::optional<T> pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (size_ == 0) {
            return std::nullopt;
        }
        T item = std::move(buffer_[read_]);
        read_ = (read_ + 1) % capacity_;
        --size_;
        return item;
    }

    // Pop with a timeout. Returns nullopt if no item arrives within the timeout.
    template <typename Duration>
    std::optional<T> pop_wait(Duration timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        const bool ready = cv_.wait_for(lock, timeout, [this] { return size_ > 0; });
        if (!ready || size_ == 0) {
            return std::nullopt;
        }
        T item = std::move(buffer_[read_]);
        read_ = (read_ + 1) % capacity_;
        --size_;
        return item;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return size_;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return size_ == 0;
    }

    uint64_t drop_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return drop_count_;
    }

    std::size_t capacity() const {
        return capacity_;
    }

private:
    const std::size_t capacity_;
    std::vector<T> buffer_;
    std::size_t read_{0};
    std::size_t write_{0};
    std::size_t size_{0};
    uint64_t drop_count_{0};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
};

} // namespace hermes

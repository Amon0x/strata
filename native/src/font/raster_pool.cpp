#include "font/raster_pool.hpp"

#include <algorithm>
#include <atomic>
#include <exception>

namespace strata::font {

RasterPool& RasterPool::shared() {
    static RasterPool* const pool = new RasterPool();
    return *pool;
}

RasterPool::RasterPool() {
    for (std::size_t index = 0U; index < helpers; ++index)
        threads_.emplace_back([this] { serve(); });
}

void RasterPool::run(const std::size_t count, const std::function<void()>& lane) {
    const std::scoped_lock batch(batch_mutex_);
    const std::size_t used = std::min(count, helpers);
    {
        const std::scoped_lock lock(mutex_);
        lane_ = &lane;
        waiting_ = used;
        running_ = used;
    }
    work_.notify_all();
    lane();
    std::unique_lock lock(mutex_);
    done_.wait(lock, [this] { return running_ == 0U; });
    lane_ = nullptr;
}

void RasterPool::serve() {
    for (;;) {
        const std::function<void()>* lane = nullptr;
        {
            std::unique_lock lock(mutex_);
            work_.wait(lock, [this] { return waiting_ != 0U; });
            --waiting_;
            lane = lane_;
        }
        (*lane)();
        {
            const std::scoped_lock lock(mutex_);
            --running_;
        }
        done_.notify_one();
    }
}

void parallel_for(const std::size_t count, const std::size_t parallel_threshold,
                  const std::function<void(std::size_t)>& work) {
    if (count < std::max<std::size_t>(parallel_threshold, 2U)) {
        for (std::size_t index = 0U; index < count; ++index)
            work(index);
        return;
    }
    std::atomic_size_t cursor{0U};
    std::atomic_bool failed{false};
    std::mutex failure_mutex;
    std::exception_ptr failure;
    const std::function<void()> lane = [&] {
        while (!failed.load(std::memory_order_acquire)) {
            const std::size_t index = cursor.fetch_add(1U, std::memory_order_relaxed);
            if (index >= count)
                return;
            try {
                work(index);
            } catch (...) {
                {
                    const std::scoped_lock lock(failure_mutex);
                    if (failure == nullptr)
                        failure = std::current_exception();
                }
                failed.store(true, std::memory_order_release);
                return;
            }
        }
    };
    const std::size_t available =
        std::max<std::size_t>(1U, static_cast<std::size_t>(std::thread::hardware_concurrency()));
    RasterPool::shared().run(std::min({RasterPool::helpers, available - 1U, count - 1U}), lane);
    if (failure != nullptr)
        std::rethrow_exception(failure);
}

} // namespace strata::font

#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace strata::font {

/**
 * The threads glyph work runs on, kept for the life of the process: atlas misses rasterized and
 * vector glyphs built. A lane keeps the FreeType faces it leased in thread-local caches, so a
 * persistent lane rasterizes the next batch without building them again, and no batch pays for
 * starting threads. One batch runs at a time; the caller works as a lane too.
 */
class RasterPool final {
  public:
    static constexpr std::size_t helpers = 3U;

    /** The pool, made on first use and never destroyed: its threads end with the process. */
    [[nodiscard]] static RasterPool& shared();

    /** Runs `lane` on the caller and on `count` helpers (at most `helpers`), and waits for all. */
    void run(std::size_t count, const std::function<void()>& lane);

  private:
    RasterPool();
    void serve();

    std::mutex batch_mutex_;
    std::mutex mutex_;
    std::condition_variable work_;
    std::condition_variable done_;
    const std::function<void()>* lane_ = nullptr;
    std::size_t waiting_ = 0U;
    std::size_t running_ = 0U;
    std::vector<std::thread> threads_;
};

/**
 * Calls `work` with every index below `count`, spread over the caller and the pool's helpers when
 * there are at least `parallel_threshold` of them, and waits for all. Each index is worked once,
 * on some lane, so `work` must touch only what its index owns. The first exception a lane throws
 * stops the others taking more and is rethrown here.
 */
void parallel_for(std::size_t count, std::size_t parallel_threshold,
                  const std::function<void(std::size_t)>& work);

} // namespace strata::font

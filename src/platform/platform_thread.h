/*!
 * @file platform_thread.h
 * @brief Cross-platform threading primitives (on top of C++11 std)
 */
#ifndef AIRPLAY2_PLATFORM_THREAD_H
#define AIRPLAY2_PLATFORM_THREAD_H

#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <string>
#include <memory>
#include <chrono>

namespace airplay2 {
namespace platform {

/*!
 * @brief Named thread wrapper with start/join/lifecycle control
 */
class Thread {
public:
    using Func = std::function<void()>;

    Thread() = default;
    ~Thread() { stop_and_join(); }

    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    /// Start the thread. Returns false if already running.
    bool start(Func func, const std::string& name = "");

    /// Request stop via atomic flag (cooperative; function must check it)
    void request_stop() { running_.store(false); }

    /// Join the thread (blocks until exit)
    void join();

    /// Combined stop + join
    void stop_and_join() { request_stop(); join(); }

    bool is_running() const { return running_.load(); }
    bool joinable() const { return impl_ && impl_->t.joinable(); }

    /// Thread-local: check if the current thread was asked to stop
    static bool current_should_stop();

private:
    struct Impl {
        std::thread t;
        Thread* owner = nullptr;
    };
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> running_{false};
};

/*!
 * @brief Lightweight manual-reset event
 */
class Event {
public:
    Event(bool auto_reset = false) : auto_reset_(auto_reset), signaled_(false) {}

    void set() {
        std::lock_guard<std::mutex> lk(mu_);
        signaled_ = true;
        cv_.notify_all();
    }

    void reset() {
        std::lock_guard<std::mutex> lk(mu_);
        signaled_ = false;
    }

    /// Wait indefinitely. Returns true if signaled.
    bool wait() {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return signaled_; });
        if (auto_reset_) signaled_ = false;
        return true;
    }

    /// Wait with timeout in milliseconds. Returns true if signaled.
    bool wait_for(uint32_t timeout_ms) {
        std::unique_lock<std::mutex> lk(mu_);
        bool ok = cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                               [this] { return signaled_; });
        if (ok && auto_reset_) signaled_ = false;
        return ok;
    }

private:
    bool auto_reset_;
    bool signaled_;
    std::mutex mu_;
    std::condition_variable cv_;
};

} // namespace platform
} // namespace airplay2

#endif // AIRPLAY2_PLATFORM_THREAD_H

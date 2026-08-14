/*!
 * @file platform_thread.cpp
 */
#include "platform_thread.h"
#include <thread>
#include <vector>

#if AP2_PLATFORM_MACOS || AP2_PLATFORM_IOS
    #include <pthread.h>
#elif AP2_PLATFORM_LINUX || AP2_PLATFORM_ANDROID
    #include <pthread.h>
#endif

namespace airplay2 {
namespace platform {

static thread_local Thread* g_current_thread = nullptr;

bool Thread::current_should_stop() {
    return g_current_thread && !g_current_thread->running_.load();
}

static void set_current_thread_name(const std::string& name) {
    if (name.empty()) return;
#if AP2_PLATFORM_MACOS || AP2_PLATFORM_IOS
    pthread_setname_np(name.c_str());
#elif AP2_PLATFORM_LINUX || AP2_PLATFORM_ANDROID
    // pthread_setname_np accepts max 15 chars on Linux
    char buf[16];
    size_t len = name.size();
    if (len > 15) len = 15;
    memcpy(buf, name.data(), len);
    buf[len] = 0;
    pthread_setname_np(pthread_self(), buf);
#elif AP2_PLATFORM_WINDOWS
    // SetThreadDescription requires Win10+; use the older SetThreadName via exception trick
    // We skip this to avoid exception complexity; names are for debugging only anyway.
    (void)buf;
#endif
}

bool Thread::start(Func func, const std::string& name) {
    if (impl_ && impl_->t.joinable()) return false;
    impl_ = std::make_unique<Impl>();
    impl_->owner = this;
    running_.store(true);
    impl_->t = std::thread([this, f = std::move(func), name]() {
        g_current_thread = this;
        set_current_thread_name(name);
        try {
            f();
        } catch (...) {
            // swallow; user function should handle
        }
        running_.store(false);
        g_current_thread = nullptr;
    });
    return true;
}

void Thread::join() {
    if (impl_ && impl_->t.joinable()) {
        impl_->t.join();
    }
}

} // namespace platform
} // namespace airplay2

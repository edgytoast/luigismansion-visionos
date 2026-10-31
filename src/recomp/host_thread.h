#pragma once
// Host threads that run recompiled guest code (the boot thread, sched's guest threads).
// Recompiled call chains are deep and every guest function is a host frame. Linux gives a
// thread 8 MB of stack and Windows builds run with 1 MB+, but Apple platforms give a secondary
// thread 512 KB (Apple Vision Pro port), so there these threads get a stack of their own size.
#include <functional>
#include <thread>
#if defined(__APPLE__)
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#endif

namespace recomp {
inline void startDetachedGuestHostThread(std::function<void()> body) {
#if defined(__APPLE__)
    constexpr size_t kStack = size_t(32) << 20;
    auto* heap = new std::function<void()>(std::move(body));
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kStack);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    const int err = pthread_create(&thread, &attr, [](void* p) -> void* {
        auto* fn = static_cast<std::function<void()>*>(p);
        (*fn)();
        delete fn;
        return nullptr;
    }, heap);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        std::fprintf(stderr, "[sched] cannot start a guest host thread (%d)\n", err);
        std::abort();
    }
#else
    std::thread(std::move(body)).detach();
#endif
}
}  // namespace recomp

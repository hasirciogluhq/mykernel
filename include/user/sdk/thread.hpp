#pragma once

#include <kernel/types.h>
#include <kernel/syscall.h>
#include <user/sdk/syscall.hpp>

namespace hsrc::sdk {

using tid_t = pid_t;

/* Forever timeout for Event / ConditionVariable waits. */
constexpr uint32_t kWaitForever = 0xFFFFFFFFu;

/*
 * Threading model
 * ---------------
 * - Kernel creates one **main thread** per process on spawn (tid == pid).
 * - Apps may create **N custom threads** via Thread::create (same flat AS).
 *   Cap: thread::max_per_process() (kernel PROC_THREADS_MAX); OOM → create fails.
 * - Idle / wait: prefer GxDevice::wait_input or Event/CV — not yield(0)
 *   or timed sleep polling.
 * - Blocking a thread only deschedules that thread; siblings keep running.
 * - Preemption: timer IRQ context-switches CPU hogs without voluntary yield.
 */
namespace this_thread {

inline tid_t get_id()
{
    return (tid_t)syscall0(SYS_GETTID);
}

inline void yield()
{
    hsrc::sdk::yield(0);
}

/* Block (PROC_BLOCKED) for up to `ticks` scheduler ticks — not a Ready spin. */
inline void sleep_for(uint32_t ticks)
{
    if (ticks == 0) {
        /* Optional cooperative hint only — fairness comes from timer preemption. */
        yield();
        return;
    }
    hsrc::sdk::yield(ticks);
}

[[noreturn]] inline void exit(int code = 0)
{
    (void)syscall1(SYS_THREAD_EXIT, (long)code);
    for (;;)
        (void)syscall1(SYS_EXIT, (long)code);
}

} // namespace this_thread

namespace thread {

inline tid_t main_id()
{
    return (tid_t)syscall0(SYS_GETPID);
}

inline constexpr int max_per_process()
{
    /* Mirrors kernel PROC_THREADS_MAX (1 main + N-1 custom). */
    return 256;
}

/*
 * Custom thread in the calling process (shared address space).
 * Entry: void fn(void *arg). Safe to return — SDK exits the thread.
 */
class Thread {
public:
    Thread() = default;
    Thread(const Thread &) = delete;
    Thread &operator=(const Thread &) = delete;

    Thread(Thread &&o) noexcept : tid_(o.tid_), joinable_(o.joinable_)
    {
        o.tid_ = 0;
        o.joinable_ = false;
    }

    Thread &operator=(Thread &&o) noexcept
    {
        if (this != &o) {
            if (joinable_)
                (void)detach();
            tid_ = o.tid_;
            joinable_ = o.joinable_;
            o.tid_ = 0;
            o.joinable_ = false;
        }
        return *this;
    }

    ~Thread()
    {
        if (joinable_)
            (void)detach();
    }

    static bool create_supported() { return true; }

    /* Spawn; on failure ok()==false — see last_error() (-EAGAIN/-ENOMEM/…). */
    static Thread create(void (*fn)(void *), void *arg = nullptr);
    static long last_error();

    bool ok() const { return tid_ > 0; }
    tid_t tid() const { return tid_; }
    bool joinable() const { return joinable_; }

    bool join(int *status_out = nullptr);
    bool detach();

private:
    tid_t tid_ = 0;
    bool joinable_ = false;
};

} // namespace thread

} // namespace hsrc::sdk

#pragma once

#include <kernel/types.h>
#include <kernel/syscall.h>
#include <user/sdk/syscall.hpp>
#include <user/sdk/thread.hpp>

namespace hsrc::sdk {

/*
 * Kernel auto-reset Event.
 * wait() leaves Ready until signal/broadcast or timeout — no yield(0) spin.
 */
class Event {
public:
    Event()
    {
        long id = syscall0(SYS_EVENT_CREATE);
        id_ = (id >= 0) ? (int)id : -1;
    }

    ~Event()
    {
        close();
    }

    Event(const Event &) = delete;
    Event &operator=(const Event &) = delete;

    Event(Event &&o) noexcept : id_(o.id_) { o.id_ = -1; }
    Event &operator=(Event &&o) noexcept
    {
        if (this != &o) {
            close();
            id_ = o.id_;
            o.id_ = -1;
        }
        return *this;
    }

    bool ok() const { return id_ >= 0; }
    int  id() const { return id_; }

    /* true = signaled; false = timeout / error. */
    bool wait(uint32_t timeout_ticks = kWaitForever)
    {
        if (id_ < 0)
            return false;
        long to = (timeout_ticks == kWaitForever) ? (long)-1 : (long)timeout_ticks;
        long r = syscall2(SYS_EVENT_WAIT, id_, to);
        return r == 0;
    }

    bool try_wait()
    {
        if (id_ < 0)
            return false;
        return syscall2(SYS_EVENT_WAIT, id_, 0) == 0;
    }

    void signal()
    {
        if (id_ >= 0)
            (void)syscall1(SYS_EVENT_SIGNAL, id_);
    }

    void broadcast()
    {
        if (id_ >= 0)
            (void)syscall1(SYS_EVENT_BROADCAST, id_);
    }

    void close()
    {
        if (id_ >= 0) {
            (void)syscall1(SYS_EVENT_DESTROY, id_);
            id_ = -1;
        }
    }

private:
    int id_ = -1;
};

/* Userspace mutex; blocks via Event when contended (ready for Phase B threads). */
class Mutex {
public:
    Mutex() = default;
    Mutex(const Mutex &) = delete;
    Mutex &operator=(const Mutex &) = delete;

    void lock()
    {
        for (;;) {
            if (__sync_bool_compare_and_swap(&locked_, 0, 1))
                return;
            (void)gate_.wait(kWaitForever);
        }
    }

    bool try_lock()
    {
        return __sync_bool_compare_and_swap(&locked_, 0, 1);
    }

    void unlock()
    {
        __sync_lock_release(&locked_);
        gate_.signal();
    }

private:
    volatile int locked_ = 0;
    Event gate_{};
};

class LockGuard {
public:
    explicit LockGuard(Mutex &m) : m_(m) { m_.lock(); }
    ~LockGuard() { m_.unlock(); }
    LockGuard(const LockGuard &) = delete;
    LockGuard &operator=(const LockGuard &) = delete;

private:
    Mutex &m_;
};

/*
 * Condition variable over Event + Mutex (Mesa-style).
 * wait() atomically unlocks, blocks on Event, then re-locks.
 */
class ConditionVariable {
public:
    ConditionVariable() = default;
    ConditionVariable(const ConditionVariable &) = delete;
    ConditionVariable &operator=(const ConditionVariable &) = delete;

    void wait(Mutex &m)
    {
        m.unlock();
        (void)ev_.wait(kWaitForever);
        m.lock();
    }

    /* true if notified (or spurious); false on timeout. */
    bool wait_for(Mutex &m, uint32_t timeout_ticks)
    {
        m.unlock();
        bool ok = ev_.wait(timeout_ticks);
        m.lock();
        return ok;
    }

    void notify_one() { ev_.signal(); }
    void notify_all() { ev_.broadcast(); }

private:
    Event ev_{};
};

} // namespace hsrc::sdk

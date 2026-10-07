#include "../include/Pthread.hpp"
#include "../include/Rwlock.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/Time/include/TimedWait.hpp"
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>

namespace {

PSRWLOCK Srw(void** storage) {
    static_assert(sizeof(SRWLOCK) == sizeof(void*));
    return reinterpret_cast<PSRWLOCK>(storage);
}

template <typename TryAcquire>
bool PollUntil(std::chrono::microseconds duration, TryAcquire tryAcquire) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    for (unsigned attempt = 0;; ++attempt) {
        if (tryAcquire()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        if (attempt < 64) std::this_thread::yield();
        else std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}

}

void GuestRwlock::lock_shared() { AcquireSRWLockShared(Srw(&_srw)); }
bool GuestRwlock::try_lock_shared() { return TryAcquireSRWLockShared(Srw(&_srw)) != 0; }
bool GuestRwlock::try_lock_shared_for(std::chrono::microseconds duration) { return PollUntil(duration, [&] { return try_lock_shared(); }); }
void GuestRwlock::unlock_shared() { ReleaseSRWLockShared(Srw(&_srw)); }
void GuestRwlock::lock() { AcquireSRWLockExclusive(Srw(&_srw)); }
bool GuestRwlock::try_lock() { return TryAcquireSRWLockExclusive(Srw(&_srw)) != 0; }
bool GuestRwlock::try_lock_for(std::chrono::microseconds duration) { return PollUntil(duration, [&] { return try_lock(); }); }
void GuestRwlock::unlock() { ReleaseSRWLockExclusive(Srw(&_srw)); }
#endif

static constexpr int SCE_OK = 0;
static constexpr int SCE_KERNEL_ERROR_ENOMEM = 0x8002000C;
static constexpr int SCE_KERNEL_ERROR_EDEADLK = 0x8002000B;
static constexpr int SCE_KERNEL_ERROR_EBUSY = 0x80020010;
static constexpr int SCE_KERNEL_ERROR_ETIMEDOUT = 0x8002003C;

static PthreadRwlockPrivate* RequireRwlock(PthreadRwlock* rwlock, const char* funcName) {
    if (!rwlock || !*rwlock) throw std::runtime_error(std::string(funcName) + ": null rwlock");
    return *rwlock;
}

static bool OwnsWrite(const PthreadRwlockPrivate* lock) {
    return lock->_writer.load(std::memory_order_acquire) == std::this_thread::get_id();
}

extern "C" {

int APS5_VABI scePthreadRwlockDestroy(PthreadRwlock* rwlock) {
    delete RequireRwlock(rwlock, __func__);
    *rwlock = nullptr;
    return SCE_OK;
}

int APS5_VABI scePthreadRwlockInit(PthreadRwlock* rwlock, const PthreadRwlockattr* attr, const char* name) {
    (void)attr;
    (void)name;
    if (!rwlock) throw std::runtime_error("scePthreadRwlockInit: null rwlock");
    auto* p = new (std::nothrow) PthreadRwlockPrivate();
    if (!p) return SCE_KERNEL_ERROR_ENOMEM;
    *rwlock = p;
    return SCE_OK;
}

int APS5_VABI scePthreadRwlockRdlock(PthreadRwlock* rwlock) {
    auto* lock = RequireRwlock(rwlock, __func__);
    if (OwnsWrite(lock)) return SCE_KERNEL_ERROR_EDEADLK;
    lock->_lock.lock_shared();
    return SCE_OK;
}

int APS5_VABI scePthreadRwlockTryrdlock(PthreadRwlock* rwlock) {
    auto* lock = RequireRwlock(rwlock, __func__);
    if (OwnsWrite(lock)) return SCE_KERNEL_ERROR_EBUSY;
    return lock->_lock.try_lock_shared() ? SCE_OK : SCE_KERNEL_ERROR_EBUSY;
}

int APS5_VABI scePthreadRwlockTimedrdlock(PthreadRwlock* rwlock, KernelUseconds usec) {
    auto* lock = RequireRwlock(rwlock, __func__);
    if (OwnsWrite(lock)) return SCE_KERNEL_ERROR_EDEADLK;
    const bool locked = TimedWait::AcquireUntil(TimedWait::DeadlineNanos(usec), [&] { return lock->_lock.try_lock_shared(); }, [&](std::uint64_t micros) { return lock->_lock.try_lock_shared_for(std::chrono::microseconds(micros)); });
    return locked ? SCE_OK : SCE_KERNEL_ERROR_ETIMEDOUT;
}

int APS5_VABI scePthreadRwlockWrlock(PthreadRwlock* rwlock) {
    auto* lock = RequireRwlock(rwlock, __func__);
    if (OwnsWrite(lock)) return SCE_KERNEL_ERROR_EDEADLK;
    lock->_lock.lock();
    lock->_writer.store(std::this_thread::get_id(), std::memory_order_release);
    return SCE_OK;
}

int APS5_VABI scePthreadRwlockTrywrlock(PthreadRwlock* rwlock) {
    auto* lock = RequireRwlock(rwlock, __func__);
    if (OwnsWrite(lock) || !lock->_lock.try_lock()) return SCE_KERNEL_ERROR_EBUSY;
    lock->_writer.store(std::this_thread::get_id(), std::memory_order_release);
    return SCE_OK;
}

int APS5_VABI scePthreadRwlockTimedwrlock(PthreadRwlock* rwlock, KernelUseconds usec) {
    auto* lock = RequireRwlock(rwlock, __func__);
    if (OwnsWrite(lock)) return SCE_KERNEL_ERROR_EDEADLK;
    const bool locked = TimedWait::AcquireUntil(TimedWait::DeadlineNanos(usec), [&] { return lock->_lock.try_lock(); }, [&](std::uint64_t micros) { return lock->_lock.try_lock_for(std::chrono::microseconds(micros)); });
    if (!locked) return SCE_KERNEL_ERROR_ETIMEDOUT;
    lock->_writer.store(std::this_thread::get_id(), std::memory_order_release);
    return SCE_OK;
}

int APS5_VABI scePthreadRwlockUnlock(PthreadRwlock* rwlock) {
    auto* lock = RequireRwlock(rwlock, __func__);
    if (OwnsWrite(lock)) {
        lock->_writer.store(std::thread::id{}, std::memory_order_release);
        lock->_lock.unlock();
    } else {
        lock->_lock.unlock_shared();
    }
    return SCE_OK;
}

int APS5_VABI scePthreadRwlockattrDestroy(PthreadRwlockattr* attr) {
    if (!attr || !*attr) throw std::runtime_error("scePthreadRwlockattrDestroy: null attr");
    delete *attr;
    *attr = nullptr;
    return SCE_OK;
}

int APS5_VABI scePthreadRwlockattrInit(PthreadRwlockattr* attr) {
    if (!attr) throw std::runtime_error("scePthreadRwlockattrInit: null attr");
    auto* p = new (std::nothrow) PthreadRwlockattrPrivate{0};
    if (!p) return SCE_KERNEL_ERROR_ENOMEM;
    *attr = p;
    return SCE_OK;
}

int APS5_VABI scePthreadRwlockattrSettype(PthreadRwlockattr* attr, int type) {
    if (!attr || !*attr) throw std::runtime_error("scePthreadRwlockattrSettype: null attr");
    (*attr)->type = type;
    return SCE_OK;
}

}

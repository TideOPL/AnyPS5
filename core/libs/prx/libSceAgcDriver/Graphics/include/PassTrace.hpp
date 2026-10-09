#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PASSTRACE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PASSTRACE_HPP

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <set>
#include <array>
#include <atomic>
#include <map>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

// DBG: DBG_PASS_TRACE_S=<seconds> logs every draw and dispatch with the textures and storage images
// it binds for DBG_PASS_TRACE_LEN seconds (default 0.5) from then, as [pass] lines.
inline bool PassTraceActive() {
    static const double start = std::getenv("DBG_PASS_TRACE_S") ? std::atof(std::getenv("DBG_PASS_TRACE_S")) : -1.0;
    static const double length = std::getenv("DBG_PASS_TRACE_LEN") ? std::atof(std::getenv("DBG_PASS_TRACE_LEN")) : 0.5;
    static const auto began = std::chrono::steady_clock::now();
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    // DBG_TRACE_TRIGGER=<file>: touching the file (a newer modification time) starts a DBG_PASS_TRACE_LEN window.
    static const char* trigger = std::getenv("DBG_TRACE_TRIGGER");
    if (trigger != nullptr) {
        static std::atomic<double> nextCheck{0.0};
        static std::atomic<double> windowStart{-1.0};
        static std::atomic<long long> lastStamp{-1};
        if (now >= nextCheck.load(std::memory_order_relaxed)) {
            nextCheck.store(now + 1.0, std::memory_order_relaxed);
            std::error_code error;
            const auto stamp = std::filesystem::last_write_time(trigger, error);
            if (!error) {
                const long long ticks = static_cast<long long>(stamp.time_since_epoch().count());
                const long long previous = lastStamp.exchange(ticks);
                if (previous != -1 && previous != ticks) windowStart.store(now);
            }
        }
        const double window = windowStart.load(std::memory_order_relaxed);
        if (window >= 0 && now < window + length) return true;
    }
    if (start < 0) return false;
    return now >= start && now < start + length;
}

// DBG_PEEK_SUBMIT=1: 32 words at each VS table pointer (SET_SH_REG 0x8c, words 8-9) as they were
// when the command buffer was submitted, by address.
inline std::mutex& SubmitPeekMutex() {
    static std::mutex mutex;
    return mutex;
}
inline std::map<std::uint64_t, std::array<std::uint32_t, 32>>& SubmitPeeks() {
    static std::map<std::uint64_t, std::array<std::uint32_t, 32>> peeks;
    return peeks;
}

// DBG: the queue of the packet this thread is executing (set at dispatches), for queue-selective debug switches.
inline std::uint32_t& DbgCurrentQueue() {
    thread_local std::uint32_t queue = 0;
    return queue;
}

// DBG_DUMP_GROUPS=<x>x<y>x<z> with DBG_DUMP_S=<s>: the buffers the first matching dispatch writes, dumped at the next dispatch.
inline bool& DbgCollectWrites() {
    thread_local bool collect = false;
    return collect;
}
inline std::vector<std::pair<std::uint64_t, std::uint64_t>>& DbgWrittenBuffers() {
    thread_local std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    return ranges;
}

// A census of the traced images at the next storage lookup, whatever DBG_STORAGE_CENSUS_S says.
inline std::atomic<bool>& DbgCensusRequested() {
    static std::atomic<bool> requested{false};
    return requested;
}

// The image addresses the trace saw, for the census (DBG_CENSUS_TRACED=1).
inline std::mutex& PassTraceMutex() {
    static std::mutex mutex;
    return mutex;
}
inline std::set<std::uint64_t>& PassTraceAddresses() {
    static std::set<std::uint64_t> addresses;
    return addresses;
}
inline void PassTraceNote(std::uint64_t address) {
    std::lock_guard lock(PassTraceMutex());
    PassTraceAddresses().insert(address);
}

// DBG: a ring of the latest recorded GPU writes (MarkGpuWrites), with the queue that recorded them and a sequence number.
struct DbgRecordedWrite { std::uint64_t begin, end; std::uint32_t queue; std::uint64_t sequence; };
inline std::mutex& DbgRecordedWritesMutex() { static std::mutex mutex; return mutex; }
inline std::vector<DbgRecordedWrite>& DbgRecordedWrites() { static std::vector<DbgRecordedWrite> writes; return writes; }
inline std::atomic<std::uint64_t>& DbgRecordSequence() { static std::atomic<std::uint64_t> sequence{0}; return sequence; }
inline void DbgNoteRecordedWrite(std::uint64_t begin, std::uint64_t end, std::uint32_t queue) {
    std::lock_guard lock(DbgRecordedWritesMutex());
    auto& writes = DbgRecordedWrites();
    if (writes.size() >= 4096) writes.erase(writes.begin(), writes.begin() + 1024);
    writes.push_back({begin, end, queue, ++DbgRecordSequence()});
}

// DBG_NAN_PROBE=<hex program hash>: the guest ranges such a dispatch wrote, scanned for non-finite values before the next dispatch.
inline std::vector<std::pair<std::uint64_t, std::uint64_t>>& DbgNanProbeRanges() { static thread_local std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges; return ranges; }

}

#endif

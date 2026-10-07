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

namespace AgcDriver::Graphics {

// DBG: DBG_PASS_TRACE_S=<seconds> logs every draw and dispatch with the textures and storage images
// it binds for DBG_PASS_TRACE_LEN seconds (default 0.5) from then, as [pass] lines.
inline bool PassTraceActive() {
    static const double start = std::getenv("DBG_PASS_TRACE_S") ? std::atof(std::getenv("DBG_PASS_TRACE_S")) : -1.0;
    static const double length = std::getenv("DBG_PASS_TRACE_LEN") ? std::atof(std::getenv("DBG_PASS_TRACE_LEN")) : 0.5;
    static const auto began = std::chrono::steady_clock::now();
    if (start < 0) return false;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
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

}

#endif

#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include "prx/libSceAgcDriver/Graphics/include/PassTrace.hpp"

namespace AgcDriver::DriverDetail {

void Driver::dispatch(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission, std::uint64_t indirectArguments) {
    const auto address = (static_cast<std::uint64_t>(readRegister(queue.shader, 0x20c)) << 8u) | (static_cast<std::uint64_t>(readRegister(queue.shader, 0x20d) & 0xffu) << 40u);
    auto it = submission.shaders->upper_bound(address);
    std::shared_ptr<const ShaderSnapshot> registeredShader;
    if (it != submission.shaders->begin()) {
        --it;
        if (address - it->second->codeAddress < it->second->code.size() * sizeof(std::uint32_t)) registeredShader = it->second;
    }
    if (!registeredShader) registeredShader = ReadRawComputeShader(address);
    const auto& snapshot = *registeredShader;
    require(snapshot.type == 0, "compute program refers to a non-compute shader");
    const auto userCount = (readRegister(queue.shader, 0x213) >> 1u) & 0x1fu;
    std::vector<std::uint32_t> userData;
    for (std::uint32_t i = 0; i < userCount; ++i) {
        userData.push_back(readUserData(queue.shader, 0x240 + i));
    }
    auto compute = Graphics::DecodeComputeStageInfo(queue.shader, snapshot.header);
    std::vector<ShaderRecompiler::MemoryRegion> memory{{snapshot.codeAddress, std::as_bytes(std::span(snapshot.code))}};
    if (!snapshot.header.empty()) memory.push_back({snapshot.headerAddress, snapshot.header});

    static const bool unlockedDevice = std::getenv("APS5_NO_UNLOCKED_DEVICE") == nullptr;
    std::shared_ptr<VulkanDevice> localDevice = unlockedDevice ? device.Load() : nullptr;
    if (localDevice == nullptr) {
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Dispatch);
        std::lock_guard gpuLock(GuestMemory::GpuMutex());
        if (device == nullptr) device = std::make_shared<VulkanDevice>();
        localDevice = device;
    }
    const auto codeOffset = static_cast<std::size_t>((address - snapshot.codeAddress) / sizeof(std::uint32_t));
    static std::atomic<bool> constCensusArmed{false};
    if (constCensusArmed.exchange(false)) Graphics::DbgCensusRequested() = true;
    // DBG_CONST_DUMP_CODE=<hex word>,...: for the first dispatch of the program starting with those words, save the
    // buffers of the V#s at user[8..11] and at offset 0x60 of the table user[12..13] points to (consts_<a|b>.bin).
    if (static const std::vector<std::uint32_t> constWords = [] {
            std::vector<std::uint32_t> words;
            if (const char* text = std::getenv("DBG_CONST_DUMP_CODE")) {
                for (char* end = nullptr; *text != '\0'; text = *end == ',' ? end + 1 : end) {
                    words.push_back(static_cast<std::uint32_t>(std::strtoul(text, &end, 16)));
                    if (end == text) break;
                }
            }
            return words;
        }(); !constWords.empty() && userData.size() >= 14 && snapshot.code.size() >= codeOffset + constWords.size() && std::equal(constWords.begin(), constWords.end(), snapshot.code.begin() + static_cast<std::ptrdiff_t>(codeOffset))) {
        static std::atomic<bool> dumped{false};
        static std::atomic<int> seen{0};
        static const int skip = std::getenv("DBG_CONST_DUMP_SKIP") ? std::atoi(std::getenv("DBG_CONST_DUMP_SKIP")) : 0;
        if (seen.fetch_add(1) >= skip && !dumped.exchange(true)) {
            const auto save = [](const char* name, const std::uint32_t* vsharp) {
                const auto base = vsharp[0] | (static_cast<std::uint64_t>(vsharp[1] & 0xffffu) << 32u);
                const auto stride = (vsharp[1] >> 16u) & 0x3fffu;
                const auto bytes = std::min<std::uint64_t>(static_cast<std::uint64_t>(vsharp[2]) * std::max(stride, 1u), 1u << 20u);
                if (std::FILE* file = std::fopen(name, "wb")) {
                    std::fwrite(reinterpret_cast<const void*>(base), 1, bytes, file);
                    std::fclose(file);
                }
                std::fprintf(stderr, "[dbg] const dump %s base 0x%llx bytes %llu\n", name, static_cast<unsigned long long>(base), static_cast<unsigned long long>(bytes));
            };
            static const bool noSave = std::getenv("DBG_CONST_DUMP_NOSAVE") != nullptr;
            static const bool onlyA = std::getenv("DBG_CONST_DUMP_ONLY_A") != nullptr;
            if (!noSave || onlyA) save("consts_a.bin", userData.data() + 8);
            const auto table = userData[12] | (static_cast<std::uint64_t>(userData[13] & 0xffffu) << 32u);
            if (!noSave && !onlyA) save("consts_b.bin", reinterpret_cast<const std::uint32_t*>(table + 0x60u));
            const auto* first = reinterpret_cast<const std::uint32_t*>(table);
            const auto depthAddress = (static_cast<std::uint64_t>(first[0]) | (static_cast<std::uint64_t>(first[1] & 0xffu) << 32u)) << 8u;
            if (auto* recorder = Graphics::Recorder::Active(); recorder != nullptr && recorder->Recording()) recorder->Sync();
            if (!noSave && !onlyA) Graphics::DbgDumpDepthSurface(depthAddress, "consts_depth.raw");
            std::fprintf(stderr, "[dbg] const dump depth 0x%llx\n", static_cast<unsigned long long>(depthAddress));
            constCensusArmed = true;
        }
    }
    // DBG_DISPATCH_CODE=<hex address>: print the program's first 48 words and its user data, once.
    if (static const std::uint64_t dbgCode = std::getenv("DBG_DISPATCH_CODE") ? std::strtoull(std::getenv("DBG_DISPATCH_CODE"), nullptr, 16) : 0; dbgCode == address) {
        static std::atomic<int> printed{0};
        if (printed.fetch_add(1) < 40) {
            std::string line = std::string("[dispatch-code]") + " threads " + std::to_string(compute.numThreads[0]) + "x" + std::to_string(compute.numThreads[1]) + "x" + std::to_string(compute.numThreads[2]) + " code:";
            char word[16];
            for (std::size_t i = codeOffset; i < snapshot.code.size() && i < codeOffset + 48; ++i) {
                std::snprintf(word, sizeof(word), " %08x", snapshot.code[i]);
                line += word;
            }
            line += " user:";
            for (const auto value : userData) {
                std::snprintf(word, sizeof(word), " %08x", value);
                line += word;
            }
            if (userData.size() >= 6) {
                const auto valueAddress = userData[4] | (static_cast<std::uint64_t>(userData[5] & 0xffffu) << 32u);
                std::snprintf(word, sizeof(word), " value %08x", *reinterpret_cast<const std::uint32_t*>(valueAddress));
                line += word;
                const auto base = userData[0] | (static_cast<std::uint64_t>(userData[1] & 0xffffu) << 32u);
                char tail[64];
                std::snprintf(tail, sizeof(tail), " base 0x%llx groups %u", static_cast<unsigned long long>(base), packet[1]);
                line += tail;
            }
            std::fprintf(stderr, "%s\n", line.c_str());
        }
    }
    std::array<std::uint32_t, 5> resolved{};
    if (indirectArguments != 0 && (matchesFillKernel(std::span(snapshot.code).subspan(codeOffset), userData, compute) || matchesLoadedFillKernel(std::span(snapshot.code).subspan(codeOffset), userData, compute))) {

        recordQueuedLabelsBeforeRead(submission.queue);
        const auto readStart = std::chrono::steady_clock::now();
        resolved = Pm4::ReadDispatchArguments(indirectArguments, packet[4]);
        countIndirect(IndirectFillKernel, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - readStart).count());
        packet = resolved;
        indirectArguments = 0;
    }
    if (fillBuffer(queue, submission.queue, packet, std::span(snapshot.code).subspan(codeOffset), userData, compute, localDevice)) {
        pendingDispatchPhases().outcome = DispatchOutcome::FillHle;
        return;
    }
    if (indirectArguments == 0 && copyBuffer(queue, submission.queue, packet, std::span(snapshot.code).subspan(codeOffset), userData, compute, localDevice, address)) {
        pendingDispatchPhases().outcome = DispatchOutcome::CopyHle;
        return;
    }
    if (indirectArguments == 0 && (packet[4] & 0x20u) != 0) {
        const std::array<std::uint32_t, 3> threads{packet[1], packet[2], packet[3]};
        for (std::uint32_t axis = 0; axis < 3; ++axis) {
            if (threads[axis] % compute.numThreads[axis] != 0) compute.partialThreads = threads;
        }
    }
    ShaderRecompiler::RecompileRequest request{
        {ShaderRecompiler::ShaderStage::Compute, address, std::span(snapshot.code).subspan(codeOffset), snapshot.headerAddress, snapshot.header},
        {(packet[4] & 0x8000u) != 0 ? 32u : 64u, 0, userData, compute, std::nullopt, std::nullopt, memory},
        localDevice->ComputeTarget((packet[4] & 0x8000u) != 0 ? 32u : 64u),
        {0, 0, 0, 128}
    };
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    static double captureMs = 0, keyMs = 0, recompileMs = 0, deviceMs = 0;
    static std::uint64_t cacheHits = 0;
    static std::uint64_t dispatches = 0;
    auto lap = std::chrono::steady_clock::now();

    std::array<double, DriverPhaseCount> phaseMs{};
    auto phaseLap = lap;
    DispatchPhaseTiming phaseTiming{profile, lap, phaseMs, phaseLap};
    if (profile && packetStartedAt() != std::chrono::steady_clock::time_point{}) phaseMs[PhasePrologue] = std::chrono::duration<double, std::milli>(lap - packetStartedAt()).count();

    static const bool noDispatchCacheEnv = std::getenv("APS5_NO_DISPATCH_CACHE") != nullptr;

    static const std::pair<std::uint64_t, std::uint64_t> probeDispatch = [] {
        const char* text = std::getenv("APS5_PROBE_DISPATCH");
        if (text == nullptr) return std::pair<std::uint64_t, std::uint64_t>{0, 0};
        char* end = nullptr;
        const auto probeAddress = std::strtoull(text, &end, 16);
        const auto index = end != nullptr && *end == ':' ? std::strtoull(end + 1, nullptr, 10) : 0ull;
        return std::pair<std::uint64_t, std::uint64_t>{probeAddress, index};
    }();
    bool probeThis = false;

    if (probeDispatch.first != 0 && (address & 0xfffffffffull) == (probeDispatch.first & 0xfffffffffull)) {
        static std::atomic<std::uint64_t> dispatchesSeen{0};
        probeThis = dispatchesSeen.fetch_add(1) == probeDispatch.second;
        if (probeThis) std::fprintf(stderr, "[gpu] probing dispatch %llu of 0x%llx\n", static_cast<unsigned long long>(probeDispatch.second), static_cast<unsigned long long>(address));
    }

    if (FailureMemo() && snapshot.handles->poisoned.load(std::memory_order_relaxed) != 0) {
        const std::string* poisoned = nullptr;
        if (SourceHandleFor(snapshot, codeOffset, localDevice->Serial(), request, probeThis, &poisoned) == nullptr && poisoned != nullptr) {
            pendingDispatchPhases().outcome = DispatchOutcome::SkippedMemo;
            return;
        }
    }
    const bool noDispatchCache = noDispatchCacheEnv || probeThis;
    std::uint64_t key = 0xcbf29ce484222325ull;
    const auto mix = [&](std::uint64_t value) {
        key ^= value;
        key *= 0x100000001b3ull;
    };
    mix(address);
    mix(packet[4] & 0x8000u);
    for (const auto threads : compute.partialThreads) mix(threads);
    for (const auto word : userData) mix(word);

    static const bool keyHygiene = std::getenv("APS5_NO_DISPATCH_KEY_HYGIENE") == nullptr;
    if (keyHygiene) {
        for (const auto offset : {0x207u, 0x208u, 0x209u, 0x212u, 0x213u}) {
            const auto found = queue.shader.find(offset);

            mix(found == queue.shader.end() ? (1ull << 32u) : found->second);
        }
    } else {
        for (const auto& [offset, value] : queue.shader) {
            mix(offset);
            mix(value);
        }
    }
    std::shared_ptr<const ShaderRecompiler::RecompileResult> compiledResult;

    std::shared_ptr<DispatchVariant> keepVariant;

    std::shared_ptr<DispatchVariant> attachVariant;

    std::shared_ptr<ShaderMemory> shaderMemory;
    std::vector<ShaderRecompiler::MemoryRegion> captured;

    std::vector<std::uint32_t> liveWords;
    bool dataHit = false;
    bool cached = false;
    bool validated = false;

    std::shared_ptr<DispatchEntry> missedEntry;
    bool missedDiffering = false;
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> capture;

    static const bool traceCache = std::getenv("APS5_TRACE_DISPATCH_CACHE") != nullptr;
    if (traceCache) {
        std::lock_guard traceLock(dispatchCacheMutex);
        struct Last { std::vector<std::uint32_t> userData; std::map<std::uint32_t, std::uint32_t> shader; std::uint64_t key; };
        static std::map<std::uint64_t, Last> last;
        static int reports = 0;
        auto& previous = last[address];
        if (previous.key != 0 && previous.key != key && reports < 200) {
            std::string what;
            for (std::size_t i = 0; i < userData.size(); ++i) {
                if (i >= previous.userData.size() || previous.userData[i] != userData[i]) {
                    char text[48];
                    std::snprintf(text, sizeof(text), " user[%zu] %08x->%08x", i, i < previous.userData.size() ? previous.userData[i] : 0u, userData[i]);
                    what += text;
                }
            }
            for (const auto& [offset, value] : queue.shader) {
                const auto old = previous.shader.find(offset);
                if (old == previous.shader.end() || old->second != value) {
                    char text[48];
                    std::snprintf(text, sizeof(text), " sh[%x] %08x->%08x", offset, old == previous.shader.end() ? 0u : old->second, value);
                    what += text;
                }
            }
            ++reports;
            std::fprintf(stderr, "[dispatch-cache] 0x%llx key changed:%s\n", static_cast<unsigned long long>(address), what.c_str());
        }
        previous.userData = userData;
        previous.shader = std::map<std::uint32_t, std::uint32_t>(queue.shader.begin(), queue.shader.end());
        previous.key = key;
    }

    if (!stampValidate()) mix(reinterpret_cast<std::uintptr_t>(registeredShader.get()));
    phaseTiming.Phase(PhaseKey);
    lookupDispatch(address, submission, key, noDispatchCache, traceCache, profile, memory, phaseTiming, phaseMs, compiledResult, keepVariant, captured, liveWords, dataHit, cached, validated, missedEntry, missedDiffering);
    if (cached) {
        captureMs += phaseTiming.Elapsed();
    } else {
        shaderMemory = std::make_shared<ShaderMemory>(memory, &queryPendingWrite, &observePendingWrite, hookWaitCounter());
        std::uint64_t forgetAtCapture = 0;

        static const bool dumpShaders = std::getenv("APS5_DUMP_SHADERS") != nullptr;
        try {

            struct ProbeScope {
                bool active;
                explicit ProbeScope(bool active) : active(active) { if (active) ShaderRecompiler::SetDebugProbeActive(true); }
                ~ProbeScope() { if (active) ShaderRecompiler::SetDebugProbeActive(false); }
            } probeScope{probeThis};
            const auto waitedBefore = traceCapSync() ? Graphics::Recorder::ThreadWaitedMs() : 0.0;
            forgetAtCapture = GuestMemory::ForgetSerial();
            const auto handle = SourceHandleFor(snapshot, codeOffset, localDevice->Serial(), request, probeThis);
            capture = [&] {
                const SampledReadScope sampling(evidenceReads);
                return shaderMemory->Capture(request, handle.get());
            }();
            captured = shaderMemory->Regions();
            request.context.memory = captured;
            if (traceCapSync()) traceCapture("dispatch-capture", address, submission.queue, captured, Graphics::Recorder::ThreadWaitedMs() - waitedBefore);
            captureMs += phaseTiming.Elapsed();
            phaseTiming.Phase(PhaseCapture);
            if (dumpShaders) static_cast<void>(dumpRequest(address, request));
            const auto started = std::chrono::steady_clock::now();

            static const bool reuseCapture = std::getenv("APS5_NO_CAPTURE_REUSE") == nullptr;
            bool memoHit = false;
            compiledResult = reuseCapture ? ShaderRecompiler::Recompile(request, *capture, &memoHit) : std::make_shared<const ShaderRecompiler::RecompileResult>(ShaderRecompiler::Recompile(request));
            if (compiledResult->cacheHit || memoHit) ++cacheHits;
            const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            static double totalMs = 0;
            totalMs += elapsed;
            if (profile && elapsed > 200) std::fprintf(stderr, "[gpu] compute shader 0x%llx recompile took %.0f ms (%zu SPIR-V words, %zu captured regions, total %.1f s)\n", static_cast<unsigned long long>(address), elapsed, compiledResult->spirv.size(), captured.size(), totalMs / 1000);
        } catch (const std::exception& error) {
            const auto dump = dumpShaders ? dumpRequest(address, request) : std::string{};

            std::string reason = error.what();
            if (const auto newline = reason.find('\n'); newline != std::string::npos) reason.resize(newline);
            char where[96];
            if (dump.empty()) std::snprintf(where, sizeof(where), "compute shader 0x%llx: ", static_cast<unsigned long long>(address));
            else std::snprintf(where, sizeof(where), "compute shader 0x%llx (%s): ", static_cast<unsigned long long>(address), dump.c_str());
            throw std::runtime_error(where + reason);
        }
        recompileMs += phaseTiming.Elapsed();
        phaseTiming.Phase(PhaseRecompile);
        insertDispatch(address, key, noDispatchCache, profile, registeredShader, forgetAtCapture, memory, shaderMemory, captured, capture, compiledResult, missedEntry, missedDiffering, attachVariant, phaseTiming);
    }
    if (verifyDataHits() && dataHit) verifyDataHit(snapshot, codeOffset, localDevice->Serial(), request, memory, address, *keepVariant, liveWords, *compiledResult);

    if (recordQueuedLabelsAfterCapture(submission.queue, captured)) {
        dispatch(queue, packet, submission, indirectArguments);
        return;
    }
    phaseTiming.Phase(PhaseQueuedLabels);
    const auto& compiled = *compiledResult;
    std::vector<Graphics::GuestMemorySnapshot> snapshots;
    for (const auto& region : captured) snapshots.push_back({region.guestAddress, region.bytes});
    std::array<std::uint32_t, 3> groups{packet[1], packet[2], packet[3]};
    if (indirectArguments == 0 && (packet[4] & 0x20u) != 0) {

        for (std::uint32_t axis = 0; axis < 3; ++axis) {
            const auto threads = std::max(readRegister(queue.shader, 0x207 + axis) & 0xffffu, 1u);
            groups[axis] = (groups[axis] + threads - 1) / threads;
        }
    }
    // DBG_CENSUS_AFTER_DISPATCH=<hex address>: a census of the traced images at the dispatch after
    // the first traced dispatch of that program (so its results are in).
    if (static const std::uint64_t censusAfter = std::getenv("DBG_CENSUS_AFTER_DISPATCH") ? std::strtoull(std::getenv("DBG_CENSUS_AFTER_DISPATCH"), nullptr, 16) : 0; censusAfter != 0) {
        static std::atomic<int> state{0};
        if (state.load() == 1) {
            state = 2;
            Graphics::DbgCensusRequested() = true;
        } else if (state.load() == 0 && address == censusAfter && Graphics::PassTraceActive()) state = 1;
    }
    // DBG_CENSUS_AFTER_CODE=<hex word>,<hex word>...: the same, for the program whose code starts with those words.
    if (static const std::vector<std::uint32_t> censusWords = [] {
            std::vector<std::uint32_t> words;
            if (const char* text = std::getenv("DBG_CENSUS_AFTER_CODE")) {
                for (char* end = nullptr; *text != '\0'; text = *end == ',' ? end + 1 : end) {
                    words.push_back(static_cast<std::uint32_t>(std::strtoul(text, &end, 16)));
                    if (end == text) break;
                }
            }
            return words;
        }(); !censusWords.empty()) {
        static std::atomic<int> state{0};
        if (state.load() == 1) {
            state = 2;
            Graphics::DbgCensusRequested() = true;
        } else if (state.load() == 0 && Graphics::PassTraceActive() && std::equal(censusWords.begin(), censusWords.end(), reinterpret_cast<const std::uint32_t*>(address))) {
            state = 1;
            std::fprintf(stderr, "[pass] census armed after code match 0x%llx\n", static_cast<unsigned long long>(address));
        }
    }
    // DBG_CENSUS_AFTER_GROUPS=<x>x<y> (DBG_CENSUS_AFTER_MAX=<n>, default 8): a census at the dispatch
    // after each traced dispatch of that group count.
    if (static const char* groupText = std::getenv("DBG_CENSUS_AFTER_GROUPS"); groupText != nullptr) {
        static const std::pair<std::uint32_t, std::uint32_t> wanted = [] { unsigned x = 0, y = 0; std::sscanf(std::getenv("DBG_CENSUS_AFTER_GROUPS"), "%ux%u", &x, &y); return std::pair<std::uint32_t, std::uint32_t>{x, y}; }();
        static const int limit = std::getenv("DBG_CENSUS_AFTER_MAX") ? std::atoi(std::getenv("DBG_CENSUS_AFTER_MAX")) : 8;
        static std::atomic<bool> armed{false};
        static std::atomic<int> taken{0};
        if (armed.exchange(false)) Graphics::DbgCensusRequested() = true;
        if (Graphics::PassTraceActive() && packet[1] == wanted.first && packet[2] == wanted.second && taken.fetch_add(1) < limit) {
            armed = true;
            std::fprintf(stderr, "[pass] census armed after dispatch 0x%llx (#%d)\n", static_cast<unsigned long long>(address), taken.load());
        }
    }
    if (Graphics::PassTraceActive()) std::fprintf(stderr, "[pass] %lu dispatch 0x%llx queue 0x%x groups %ux%ux%u\n", static_cast<unsigned long>(std::hash<std::thread::id>{}(std::this_thread::get_id()) % 1000u), static_cast<unsigned long long>(address), submission.queue, groups[0], groups[1], groups[2]);
    static const bool traceIo = std::getenv("APS5_TRACE_DISPATCH_IO") != nullptr;
    if (traceIo) {

        std::string words;
        if (std::getenv("APS5_TRACE_DISPATCH_IO")[0] == '2') {
            for (const auto word : userData) {
                char text[12];
                std::snprintf(text, sizeof(text), " %08x", word);
                words += text;
            }
        }
        std::fprintf(stderr, "[dispatch-io] shader 0x%llx%s\n", static_cast<unsigned long long>(address), words.c_str());
    }

    const auto rethrow = [&](const std::exception& error) {
        char where[64];
        std::snprintf(where, sizeof(where), "compute shader 0x%llx: ", static_cast<unsigned long long>(address));
        throw std::runtime_error(where + std::string(error.what()));
    };
    phaseTiming.Phase(PhaseSnapshots);

    std::shared_ptr<RecipeHit> recipeHit;
    if (cached && keepVariant != nullptr && !stampValidate()) {
        recipeHit = localDevice->PrepareRecipe(keepVariant->recipe.load(std::memory_order_acquire), indirectArguments != 0);
        phaseTiming.Phase(PhaseRecipePrecheck);
    }

    std::shared_ptr<const Recipe> builtRecipe;
    auto* const attachTo = stampValidate() ? nullptr : keepVariant != nullptr ? keepVariant.get() : attachVariant.get();
    bool writersNoted = false;
    const bool noteWrites = writeEvidenceEnabled() || traceCapSync();

    for (;;) {

        std::shared_ptr<PreparedDispatch> prepared;
        if (recipeHit == nullptr || VulkanDevice::VerifyRecipes()) {
            try {
                prepared = localDevice->PrepareDispatch(compiled, snapshots);
            } catch (const std::exception& error) {
                rethrow(error);
            }
            if (profile) {
                const auto now = std::chrono::steady_clock::now();
                const auto prepareMs = std::chrono::duration<double, std::milli>(now - phaseLap).count();
                phaseLap = now;
                double parts = 0;
                if (prepared != nullptr) {
                    const auto phases = VulkanDevice::PreparePhaseMs(*prepared);
                    phaseMs[PhasePrepareKey] += phases[0];
                    phaseMs[PhasePrepareFind] += phases[1];
                    phaseMs[PhasePreparePrecollect] += phases[2];
                    phaseMs[PhasePreparePresync] += phases[3];
                    phaseMs[PhasePrepareStageA] += phases[4];
                    for (const auto part : phases) parts += part;
                }
                phaseMs[PhasePrepareOther] += std::max(0.0, prepareMs - parts);
            }
        }
        GuestMemory::TagGpuLockSite(indirectArguments != 0 ? GuestMemory::GpuLockSite::Indirect : GuestMemory::GpuLockSite::Dispatch);
        std::lock_guard gpuLock(GuestMemory::GpuMutex());
        phaseTiming.Phase(PhaseLockWait);

        recordLabelsForPacket(localDevice.get(), submission.queue);
        phaseTiming.Phase(PhaseLabels);
        if (noteWrites && writerKeyedEvidence() && !writersNoted) {
            noteWrittenBuffers(address, submission.queue, compiled);
            writersNoted = true;
        }
        phaseTiming.Phase(PhaseNoteWriters);
        try {
            if (recipeHit != nullptr) {
                VulkanDevice::IndirectOutcome outcome{0, 0};
                const auto result = localDevice->DispatchRecipe(compiled, groups[0], groups[1], groups[2], indirectArguments, address, recipeHit, outcome, VulkanDevice::VerifyRecipes() ? prepared : nullptr, dataHit);
                if (result == RecipeOutcome::Rebuild) {

                    recipeHit = nullptr;
                    VulkanDevice::NoteRecipe(VulkanDevice::RecipeEvent::Restart, indirectArguments != 0);
                    continue;
                }
                if (indirectArguments != 0) countIndirect(outcome.cpuReason, outcome.argumentReadMs);
            } else if (indirectArguments != 0) {
                const auto outcome = localDevice->DispatchIndirect(compiled, indirectArguments, snapshots, address, std::move(prepared), attachTo != nullptr ? &builtRecipe : nullptr);
                countIndirect(outcome.cpuReason, outcome.argumentReadMs);
            } else {
                localDevice->Dispatch(compiled, groups[0], groups[1], groups[2], snapshots, address, std::move(prepared), attachTo != nullptr ? &builtRecipe : nullptr);
            }
        } catch (const std::exception& error) {
            rethrow(error);
        }
        if (builtRecipe != nullptr) {
            if (dataHit) {

                auto own = std::make_shared<Recipe>(*builtRecipe);
                own->dataWordsHash = Graphics::ShaderResources::DataWordsHash({ShaderRecompiler::ShaderStage::Compute, keepVariant->compiled.get(), 0});
                builtRecipe = std::move(own);
            }
            attachTo->recipe.store(std::move(builtRecipe), std::memory_order_release);
            VulkanDevice::NoteRecipe(VulkanDevice::RecipeEvent::Attach, indirectArguments != 0);
        }
        break;
    }
    phaseTiming.Phase(PhaseDevice);
    if (noteWrites && !writerKeyedEvidence()) noteWrittenBuffers(address, submission.queue, compiled);
    deviceMs += phaseTiming.Elapsed();
    phaseTiming.Phase(PhaseTail);
    if (profile) {
        auto& pending = pendingDispatchPhases();
        pending.outcome = DispatchOutcome::Real;
        pending.phases = true;
        pending.hit = cached;
        pending.validated = validated;
        pending.ms = phaseMs;
        pending.tailAt = phaseLap;
    }
    if (profile && ++dispatches % 100 == 0) AgcDriver::ProfilePrint_nid_no_patch( "[gpu] %llu dispatches (%llu dispatch cache hits, %llu evictions, %llu recompile cache hits): capture %.1f s, cache key %.1f s, recompile %.1f s, device %.1f s\n", static_cast<unsigned long long>(dispatches), static_cast<unsigned long long>(dispatchCacheHits), static_cast<unsigned long long>(dispatchCacheEvictions), static_cast<unsigned long long>(cacheHits), captureMs / 1000, keyMs / 1000, recompileMs / 1000, deviceMs / 1000);
}

}

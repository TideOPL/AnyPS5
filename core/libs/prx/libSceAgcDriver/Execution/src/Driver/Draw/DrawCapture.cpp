#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include <cstdlib>
#include <cstring>
#include <thread>
#include <algorithm>
#include <string>
#include <vector>
#include "prx/libSceAgcDriver/Graphics/include/PassTrace.hpp"

namespace AgcDriver::DriverDetail {

ShaderRecompiler::RecompileResult Driver::compileDrawStage(std::size_t i, std::uint32_t pushOffset, const QueueState& queue, const Submission& submission, const std::vector<DrawProgram>& programs, const Graphics::State& graphics, const ShaderRecompiler::ShaderPixelStageInfo& pixel, const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>>& vertexInfos, std::vector<ShaderRecompiler::MemoryRegion>& memory, const std::vector<ShaderRecompiler::LinkedProgram>& linked, const Pm4::DrawParameters& drawParameters, const std::shared_ptr<VulkanDevice>& localDevice, ShaderMemory& shaderMemory, std::vector<StageCapture>& stageCaptures, std::vector<bool>& recompiled, bool drawHit, const std::vector<std::shared_ptr<DispatchVariant>>& matched, const std::vector<std::vector<ShaderRecompiler::MemoryRegion>>& matchedRegions, bool profile, std::uint64_t dumpTarget, std::uint64_t dumpSlot1, std::uint64_t& captures, DrawPhaseTiming& phaseTiming, std::array<double, DrawDriverPhaseCount>& phaseMs, std::string& rejected) {
    using Stage = ShaderRecompiler::ShaderStage;
    phaseTiming.Phase(DrawRowVectors);
    const auto& program = programs[i];
    const auto waveSize = program.binary.stage == Stage::Fragment ? graphics.stages.fragmentWaveSize : graphics.stages.vertexWaveSize;
    ShaderRecompiler::RecompileRequest request{
        program.binary,
        {waveSize, program.firstUserSgpr, program.userData, std::nullopt, program.binary.stage == Stage::Fragment ? std::optional(pixel) : std::nullopt, vertexInfos[i], memory},
        localDevice->Target(),
        {0, 0, pushOffset, (graphics.stages.mesh ? ShaderRecompiler::MeshDrawPushOffsetBytes : Graphics::PipelinePushConstantBytes) - pushOffset},
        ShaderRecompiler::GraphicsCompileContext{program.firstUserSgpr, linked, graphics.stages.mesh, graphics.stages.tessellation, {drawParameters.indexAddress, drawParameters.indexCount, drawParameters.indexSize, drawParameters.instanceCount}}
    };
    const auto waitedBefore = traceCapSync() || profile ? Graphics::Recorder::ThreadWaitedMs() : 0.0;
    const std::string* poisoned = nullptr;
    const auto handle = SourceHandleFor(*program.snapshot, program.codeOffset, localDevice->Serial(), request, false, FailureMemo() ? &poisoned : nullptr);
    if (handle == nullptr && poisoned != nullptr) {
        rejected = *poisoned;
        return {};
    }
    auto& stageCapture = stageCaptures[i];
    stageCapture.forgetSerial = GuestMemory::ForgetSerial();
    stageCapture.pushOffset = pushOffset;
    const auto capture = [&] {
        const SampledReadScope sampling(evidenceReads);
        return shaderMemory.Capture(request, handle.get());
    }();

    stageCapture.regions = shaderMemory.TakeRecentRegions();
    recompiled[i] = true;
    memory = shaderMemory.Regions();

    if (drawHit) {
        for (std::size_t j = 0; j < programs.size(); ++j) {
            if (matched[j] != nullptr && !recompiled[j]) memory.insert(memory.end(), matchedRegions[j].begin(), matchedRegions[j].end());
        }
    }
    request.context.memory = memory;
    if (traceCapSync()) traceCapture("draw-capture", program.binary.codeAddress, submission.queue, memory, Graphics::Recorder::ThreadWaitedMs() - waitedBefore);
    if (profile) {
        ++captures;
        phaseTiming.Phase(DrawRowCapture);

        const auto waited = std::min(Graphics::Recorder::ThreadWaitedMs() - waitedBefore, phaseMs[DrawRowCapture]);
        phaseMs[DrawRowCapture] -= waited;
        phaseMs[DrawRowCaptureHookWaits] += waited;
    }
    if (Graphics::PassTraceActive()) {
        const auto name = dumpRequest(program.binary.codeAddress, request);
        std::fprintf(stderr, "[pass] %lu   program %s stage %d\n", static_cast<unsigned long>(std::hash<std::thread::id>{}(std::this_thread::get_id()) % 1000u), name.c_str(), static_cast<int>(program.binary.stage));
        // DBG_PEEK_CODE=<hex word>,...: for that program, the user data and 128 bytes at each user-data
        // pair that reads as a mapped guest address (the low 48 bits, as s_load and V# bases use it).
        static const std::vector<std::uint32_t> peekWords = [] {
            std::vector<std::uint32_t> words;
            if (const char* text = std::getenv("DBG_PEEK_CODE")) {
                for (char* end = nullptr; *text != '\0'; text = *end == ',' ? end + 1 : end) {
                    words.push_back(static_cast<std::uint32_t>(std::strtoul(text, &end, 16)));
                    if (end == text) break;
                }
            }
            return words;
        }();
        const auto& code = request.shader.code;
        if (!peekWords.empty() && code.size() >= peekWords.size() && std::equal(peekWords.begin(), peekWords.end(), code.begin())) {
            const auto& user = program.userData;
            std::string line;
            for (const auto word : user) {
                char text[12];
                std::snprintf(text, sizeof(text), " %08x", word);
                line += text;
            }
            std::fprintf(stderr, "[peek] program %s first sgpr %u user%s\n", name.c_str(), program.firstUserSgpr, line.c_str());
            // DBG_PEEK_ADDR=<hex>,...: 64 words at each of those addresses too.
            if (const char* text = std::getenv("DBG_PEEK_ADDR")) {
                for (char* end = nullptr; *text != '\0'; text = *end == ',' ? end + 1 : end) {
                    const auto address = std::strtoull(text, &end, 16);
                    if (end == text) break;
                    if (!GuestMemory::Accessible(reinterpret_cast<const void*>(address), 256)) continue;
                    std::string words;
                    for (std::size_t k = 0; k < 64; ++k) {
                        char word[12];
                        std::snprintf(word, sizeof(word), " %08x", reinterpret_cast<const std::uint32_t*>(address)[k]);
                        words += word;
                    }
                    std::fprintf(stderr, "[peek]   addr 0x%llx:%s\n", static_cast<unsigned long long>(address), words.c_str());
                }
            }
            for (std::size_t j = 0; j + 1 < user.size(); ++j) {
                const auto address = user[j] | (static_cast<std::uint64_t>(user[j + 1] & 0xffffu) << 32u);
                if (address < 0x100000000ull || !GuestMemory::Accessible(reinterpret_cast<const void*>(address), 128)) continue;
                std::string bytes;
                for (std::size_t k = 0; k < 32; ++k) {
                    char text[12];
                    std::snprintf(text, sizeof(text), " %08x", reinterpret_cast<const std::uint32_t*>(address)[k]);
                    bytes += text;
                }
                std::fprintf(stderr, "[peek]   user[%zu] 0x%llx:%s\n", j, static_cast<unsigned long long>(address), bytes.c_str());
                std::lock_guard peekLock(Graphics::SubmitPeekMutex());
                if (const auto found = Graphics::SubmitPeeks().find(address); found != Graphics::SubmitPeeks().end()) {
                    std::string then;
                    for (const auto word : found->second) {
                        char text[12];
                        std::snprintf(text, sizeof(text), " %08x", word);
                        then += text;
                    }
                    std::fprintf(stderr, "[peek]   at submit 0x%llx:%s\n", static_cast<unsigned long long>(address), then.c_str());
                }
            }
        }
    }
    if (dumpTarget != 0) {

        const auto slot0 = (static_cast<std::uint64_t>(readRegister(queue.context, 0x390)) << 40u) | (static_cast<std::uint64_t>(readRegister(queue.context, 0x318)) << 8u);
        if ((graphics.hasColorTarget && graphics.color.address == dumpTarget) || slot0 == dumpTarget) static_cast<void>(dumpRequest(program.binary.codeAddress, request));
    }
    if (dumpSlot1 != 0) {
        const auto value = [&](std::uint32_t offset) -> std::uint64_t { const auto it = queue.context.find(offset); return it == queue.context.end() ? 0u : it->second; };
        const auto slot1 = (value(0x391) << 40u) | (value(0x327) << 8u);
        if (slot1 == dumpSlot1) {
            static_cast<void>(dumpRequest(program.binary.codeAddress, request));
            if (std::FILE* file = std::fopen("draw_slot1.regs", "w")) {
                for (const auto& [offset, value] : queue.context) std::fprintf(file, "context %x %08x\n", offset, value);
                for (const auto& [offset, value] : queue.userConfig) std::fprintf(file, "uconfig %x %08x\n", offset, value);
                for (const auto& [offset, value] : queue.shader) std::fprintf(file, "shader %x %08x\n", offset, value);
                std::fclose(file);
            }
        }
    }

    static const bool reuseCapture = std::getenv("APS5_NO_CAPTURE_REUSE") == nullptr;
    phaseTiming.Phase(DrawRowCapture);

    stageCapture.compiled = reuseCapture ? ShaderRecompiler::Recompile(request, *capture) : std::make_shared<const ShaderRecompiler::RecompileResult>(ShaderRecompiler::Recompile(request));
    ShaderRecompiler::RecompileResult result = *stageCapture.compiled;
    phaseTiming.Phase(DrawRowRecompile);
    return result;
}

void Driver::cacheDrawStages(bool useDrawEntries, bool drawHit, const Pm4::DrawParameters& drawParameters, const std::optional<Graphics::IndirectDrawPath>& indirectCpu, const std::vector<DrawProgram>& programs, const std::vector<StageCapture>& stageCaptures, const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>>& vertexInfos, const std::vector<std::vector<Graphics::DecodeRead>>& decodeReads, bool verifyHit, const std::vector<std::shared_ptr<DispatchVariant>>& matched, std::vector<std::shared_ptr<DispatchVariant>>& fresh, std::uint64_t drawKey, bool registerKey, const std::shared_ptr<const DrawDecode>& decode, DrawPhaseTiming& phaseTiming) {
    if (useDrawEntries && !drawHit && !(drawParameters.indirect && indirectCpu)) {
        phaseTiming.Phase(DrawRowVectors);
        std::uint64_t unstable = 0, mismatches = 0;
        for (std::size_t i = 0; i < programs.size(); ++i) {
            const auto& stageCapture = stageCaptures[i];
            if (stageCapture.compiled == nullptr) continue;
            auto variant = std::make_shared<DispatchVariant>();
            variant->compiled = stageCapture.compiled;
            variant->shader = programs[i].snapshot;
            variant->forgetSerial = stageCapture.forgetSerial;
            variant->pushOffset = stageCapture.pushOffset;
            if (vertexInfos[i]) variant->vertexInfo = std::make_shared<const ShaderRecompiler::ShaderVertexStageInfo>(*vertexInfos[i]);

            std::vector<ShaderRecompiler::MemoryRegion> regions(stageCapture.regions.begin(), stageCapture.regions.end());
            for (const auto& read : decodeReads[i]) regions.push_back({read.address, std::as_bytes(std::span(read.bytes))});
            std::stable_sort(regions.begin(), regions.end(), [](const ShaderRecompiler::MemoryRegion& a, const ShaderRecompiler::MemoryRegion& b) { return a.guestAddress < b.guestAddress; });
            for (const auto& region : regions) {
                variant->runs.emplace_back(region.guestAddress, region.guestAddress + region.bytes.size());
                const auto count = region.bytes.size() / sizeof(std::uint32_t);
                const auto offset = variant->words.size();
                variant->words.resize(offset + count);
                std::memcpy(variant->words.data() + offset, region.bytes.data(), count * sizeof(std::uint32_t));
            }
            if (verifyHit && matched[i] != nullptr && (matched[i]->runs != variant->runs || matched[i]->words != variant->words)) {
                ++mismatches;
                static std::atomic<std::uint64_t> reports{0};
                if (reports.fetch_add(1) < 20) std::fprintf(stderr, "[draw-cache] verify: stage %zu (program 0x%llx) of a hit captured differently: %zu runs / %zu words matched, %zu / %zu fresh\n", i, static_cast<unsigned long long>(programs[i].binary.codeAddress), matched[i]->runs.size(), matched[i]->words.size(), variant->runs.size(), variant->words.size());
            }
            if (insertCompare()) {
                const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DrawCache);
                if (!captureStable(stageCapture.regions)) {
                    ++unstable;
                    continue;
                }
            }
            fresh[i] = std::move(variant);
        }
        insertDrawEntry(drawKey, fresh, registerKey ? decode : nullptr);
        if (unstable != 0 || mismatches != 0) {
            std::lock_guard cacheLock(drawCacheMutex);
            drawEntryCounters.unstable += unstable;
            drawEntryCounters.verifyMismatches += mismatches;
        }
        phaseTiming.Phase(DrawRowKeyLookupValidate);
    }
}

}

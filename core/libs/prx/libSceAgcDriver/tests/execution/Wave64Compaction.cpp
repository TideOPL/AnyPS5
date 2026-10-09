#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 64;
constexpr std::uint32_t Stride = 4;
alignas(256) std::array<std::uint32_t, Threads * Stride> Output{};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (4u << 16u), count, 0x01016facu};
}

std::vector<std::uint32_t> Program(std::uint32_t mask) {
    return {
        0x34280082u,
        0x36040080u + mask,
        0x7d8a0480u,
        0xd7650001u, 0x0001006au,
        0xd7660001u, 0x0002026bu,
        0xbe84106au,
        0x7e060204u,
        0x7e08026au,
        0x7e0a026bu,
        0xe0702000u, 0x80030114u,
        0xe0702004u, 0x80030314u,
        0xe0702008u, 0x80030414u,
        0xe070200cu, 0x80030514u,
        0xbf810000u, 0x00000000u, 0x00000000u,
    };
}

void Run(AgcDriver::VulkanDevice& device, const ShaderRecompiler::SpirvTarget& target, std::uint32_t mask) {
    Output.fill(0xdeadbeefu);
    std::vector<std::uint32_t> userData(16, 0u);
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size()));
    std::copy(output.begin(), output.end(), userData.begin() + 12);
    static std::vector<std::uint32_t> program;
    program = Program(mask);
    const std::span<const std::uint32_t> code(program);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {64, 0, userData, compute, std::nullopt, std::nullopt, memory},
        target,
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
}

void Check(const std::string& label, std::uint32_t mask) {
    std::uint64_t ballot = 0;
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        if ((tid & mask) != 0) ballot |= std::uint64_t{1} << tid;
    }
    const auto total = static_cast<std::uint32_t>(std::popcount(ballot));
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        const auto below = static_cast<std::uint32_t>(std::popcount(ballot & ((std::uint64_t{1} << tid) - 1u)));
        const auto* lane = &Output[tid * Stride];
        Require(lane[0] == below && lane[1] == total && lane[2] == static_cast<std::uint32_t>(ballot) && lane[3] == static_cast<std::uint32_t>(ballot >> 32u),
            label + " mask " + std::to_string(mask) + ": lane " + std::to_string(tid) + " slot " + std::to_string(lane[0]) + " (expected " + std::to_string(below) + "), total " + std::to_string(lane[1]) + " (expected " + std::to_string(total) + "), vcc 0x" + std::to_string(lane[3]) + ":" + std::to_string(lane[2]));
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        for (const std::uint32_t mask : {5u, 32u, 33u}) {
            Run(*device, device->ComputeTarget(32), mask);
            Check("32-lane host", mask);
            if (device->Target().subgroupSize >= 64) {
                Run(*device, device->Target(), mask);
                Check("64-lane host", mask);
            }
        }
        std::puts("wave64 compaction tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

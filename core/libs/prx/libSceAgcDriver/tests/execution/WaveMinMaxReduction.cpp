#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 64;
constexpr std::uint32_t Stride = 4;
alignas(256) std::array<float, Threads * Stride> Input{};
alignas(256) std::array<float, Threads * Stride> Output{};

alignas(256) constexpr std::array<std::uint32_t, 73> Code{
    0x34280082, 0xe0302000, 0x80000114, 0xbf8c3f70, 0x7e060301,
    0xbeea287e, 0x020a02ff, 0x7f800000, 0x020c06ff, 0x7f800000,
    0x1e0a0afa, 0xff011105, 0x1e0c0cfa, 0xff011106, 0x1e0a0afa, 0xff011205, 0x1e0c0cfa, 0xff011206,
    0x1e0a0afa, 0xff011405, 0x1e0c0cfa, 0xff011406, 0x1e0a0afa, 0xff011805, 0x1e0c0cfa, 0xff011806,
    0xd7781004, 0x03058305, 0xd7781007, 0x03058306, 0x1e0a0905, 0x1e0c0f06, 0xbefe046a,
    0xd7600002, 0x00013f05, 0xd7600003, 0x00017f05, 0xd7600004, 0x00013f06, 0xd7600005, 0x00017f06,
    0x1e0006f9, 0x86860602, 0x1e160af9, 0x86860604,
    0xbeea286a, 0x021002ff, 0xff800000, 0x021806ff, 0xff800000,
    0x201010fa, 0xff011108, 0x201818fa, 0xff01110c, 0x201010fa, 0xff011208, 0x201818fa, 0xff01120c,
    0x201010fa, 0xff011408, 0x201818fa, 0xff01140c, 0x201010fa, 0xff011808, 0x201818fa, 0xff01180c,
    0xd7781009, 0x03058308, 0xd7781003, 0x0305830c,
    0x20101308, 0x2018070c, 0xbefe046a,
};

alignas(256) constexpr std::array<std::uint32_t, 19> Tail{
    0xd7600006, 0x00013f08, 0xd7600007, 0x00017f08, 0xd7600008, 0x00013f0c, 0xd7600009, 0x00017f0c,
    0x20020ef9, 0x86860606, 0x201412f9, 0x86860608,
    0xe0702000, 0x80030014, 0xe0702004, 0x80030114, 0xbf810000, 0x00000000, 0x00000000,
};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (4u << 16u), count, 0x01016facu};
}

void Run(AgcDriver::VulkanDevice& device, const ShaderRecompiler::SpirvTarget& target) {
    for (std::uint32_t tid = 0; tid < Threads; ++tid) Input[tid * Stride] = tid < 40 ? -0.01f * static_cast<float>(tid + 1) : 0.000573f;
    Output.fill(-12345.0f);
    std::vector<std::uint32_t> userData(16, 0u);
    const auto input = BufferDescriptor(Input.data(), static_cast<std::uint32_t>(Input.size()));
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size()));
    std::copy(input.begin(), input.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 12);
    static std::vector<std::uint32_t> program;
    program.assign(Code.begin(), Code.end());
    program.insert(program.end(), Tail.begin(), Tail.end());
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

void Check(const char* label) {
    float expectedMin = Input[0], expectedMax = Input[0];
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        expectedMin = std::min(expectedMin, Input[tid * Stride]);
        expectedMax = std::max(expectedMax, Input[tid * Stride]);
    }
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        const auto minimum = Output[tid * Stride];
        const auto maximum = Output[tid * Stride + 1];
        Require(minimum == expectedMin && maximum == expectedMax, std::string(label) + ": lane " + std::to_string(tid) + " min " + std::to_string(minimum) + " max " + std::to_string(maximum) + ", expected -0.4 and 0.000573");
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(*device, device->ComputeTarget(32));
        Check("32-lane host");
        if (device->Target().subgroupSize >= 64) {
            Run(*device, device->Target());
            Check("64-lane host");
        }
        std::puts("wave min/max reduction tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

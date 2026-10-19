#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace gpu {

enum class ShaderStage {
    Compute,
};

// Outcome of one GLSL compile. On success `spirv` holds the module and `log` any warnings; on
// failure `spirv` is empty and `log` is glslang's info log, whose leading lines name the
// offending source line ("ERROR: 0:12: ...").
struct ShaderCompileResult {
    bool ok = false;
    std::vector<uint32_t> spirv;
    std::string log;

    // The first `maxLines` non-empty log lines, for a short user-facing message.
    std::string logHead(size_t maxLines = 3) const;
};

// Compiles Vulkan-flavoured GLSL to SPIR-V for the Vulkan 1.3 / SPIR-V 1.6 environment the device
// requests. `entry` names the entry point in the emitted module; the GLSL source still declares
// `void main()`.
//
// Thread-safe. Callers racing on the same (stage, entry, source) wait on the one compile in flight
// and share its result. Finished results, failures included, are memoised in a bounded LRU, so a
// shader evicted from it compiles again on its next request; callers that must not retry a rejected
// shader in a loop remember the failure themselves.
std::shared_ptr<const ShaderCompileResult> compileGlsl(std::string_view source, ShaderStage stage,
                                                        std::string_view entry = "main");

// Identifies the compiler build, so artifacts derived from its output can be invalidated when it changes.
std::string glslangVersionString();

// Number of finished results currently memoised, for tests asserting the bound.
size_t glslCompileCacheSizeForTesting();

// Number of compiles actually run by compileGlsl so far, for tests asserting de-duplication.
size_t glslCompileCountForTesting();

} // namespace gpu

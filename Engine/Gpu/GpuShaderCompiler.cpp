#include "GpuShaderCompiler.h"

#include <atomic>
#include <future>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

namespace gpu {

namespace {

    using ResultPtr = std::shared_ptr<const ShaderCompileResult>;

    std::mutex g_mutex;
    std::unordered_map<std::string, std::shared_future<ResultPtr>> g_compiles;
    std::atomic<size_t> g_compileCount { 0 };

    void initGlslang()
    {
        // The process-wide tables are never finalized: other threads may still be compiling at exit.
        static std::once_flag once;
        std::call_once(once, [] { glslang::InitializeProcess(); });
    }

    EShLanguage toGlslang(ShaderStage stage)
    {
        switch (stage) {
        case ShaderStage::Compute:
            return EShLangCompute;
        }
        return EShLangCompute;
    }

    ResultPtr fail(std::string log)
    {
        auto r = std::make_shared<ShaderCompileResult>();
        r->log = std::move(log);
        return r;
    }

    ResultPtr compile(const std::string& source, ShaderStage stage, const std::string& entry)
    {
        initGlslang();

        const EShLanguage lang = toGlslang(stage);
        glslang::TShader shader(lang);
        const char* text = source.c_str();
        shader.setStrings(&text, 1);
        shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
        shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
        shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
        shader.setSourceEntryPoint("main");
        shader.setEntryPoint(entry.c_str());

        const EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
        if (!shader.parse(GetDefaultResources(), 460, false, messages)) {
            return fail(std::string(shader.getInfoLog()) + shader.getInfoDebugLog());
        }

        glslang::TProgram program;
        program.addShader(&shader);
        if (!program.link(messages)) {
            return fail(std::string(program.getInfoLog()) + program.getInfoDebugLog());
        }
        glslang::TIntermediate* intermediate = program.getIntermediate(lang);
        if (!intermediate) {
            return fail("glslang produced no intermediate representation for the stage");
        }

        auto r = std::make_shared<ShaderCompileResult>();
        spv::SpvBuildLogger logger;
        glslang::SpvOptions options;
        options.disableOptimizer = true;
        options.validate = true;
        glslang::GlslangToSpv(*intermediate, r->spirv, &logger, &options);
        r->log = std::string(shader.getInfoLog()) + program.getInfoLog() + logger.getAllMessages();
        r->ok = !r->spirv.empty();
        if (!r->ok) {
            r->log += "SPIR-V generation produced an empty module";
        }
        return r;
    }

} // namespace

std::string ShaderCompileResult::logHead(size_t maxLines) const
{
    std::istringstream in(log);
    std::string line, out;
    size_t n = 0;
    while (n < maxLines && std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        if (n) {
            out += '\n';
        }
        out += line;
        ++n;
    }
    return out;
}

std::shared_ptr<const ShaderCompileResult> compileGlsl(std::string_view source, ShaderStage stage, std::string_view entry)
{
    std::string key;
    key.reserve(source.size() + entry.size() + 2);
    key += static_cast<char>('0' + static_cast<int>(stage));
    key += entry;
    key += '\0';
    key += source;

    std::shared_future<ResultPtr> future;
    std::promise<ResultPtr> promise;
    bool owner = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_compiles.find(key);
        if (it == g_compiles.end()) {
            future = promise.get_future().share();
            g_compiles.emplace(std::move(key), future);
            owner = true;
        } else {
            future = it->second;
        }
    }

    if (owner) {
        ResultPtr result;
        try {
            g_compileCount.fetch_add(1, std::memory_order_relaxed);
            result = compile(std::string(source), stage, std::string(entry));
        } catch (const std::exception& e) {
            result = fail(std::string("shader compiler failed: ") + e.what());
        } catch (...) {
            result = fail("shader compiler failed");
        }
        promise.set_value(std::move(result));
    }
    return future.get();
}

std::string glslangVersionString()
{
    initGlslang();
    const char* v = glslang::GetGlslVersionString();
    return v ? v : "";
}

size_t glslCompileCountForTesting()
{
    return g_compileCount.load(std::memory_order_relaxed);
}

} // namespace gpu

// GPU distribution: call the pinned official C ABI in-process, never a CLI.
#pragma once
#include <whisper.h>
#ifdef ASR_CUDA_RUNTIME
#include <ggml-backend.h>
#include <windows.h>
#include <filesystem>
#include <stdexcept>
#include <mutex>
#include <cstdio>
#include <string>
namespace asr_runtime {
inline thread_local bool cudaUsed = false;
inline thread_local bool cudaFailed = false;
inline void log(enum ggml_log_level, const char* text, void*) {
    if (!text) return;
    const std::string line(text);
    if (line.find("whisper_backend_init_gpu: using CUDA") != std::string::npos) cudaUsed = true;
    if (line.find("whisper_backend_init_gpu: failed to initialize") != std::string::npos) cudaFailed = true;
    std::fputs(text, stderr);
}
inline std::filesystem::path directory() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      reinterpret_cast<LPCWSTR>(&directory), &module);
    wchar_t name[32768];
    DWORD count = GetModuleFileNameW(module, name, 32768);
    if (!count || count >= 32768) throw std::runtime_error("cannot resolve ASR runtime directory");
    return std::filesystem::path(std::wstring(name, count)).parent_path();
}
inline HMODULE load(const std::filesystem::path& file) {
    HMODULE module = LoadLibraryExW(file.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) throw std::runtime_error("cannot load Whisper runtime DLL (Windows error " +
                                         std::to_string(GetLastError()) + ")");
    // ggml's global registry owns backend pointers for the process lifetime.
    // Keep modules resident, including after the last SDK engine is destroyed.
    return module;
}
template<class T> T symbol(HMODULE module, const char* name) {
    auto value = reinterpret_cast<T>(GetProcAddress(module, name));
    if (!value) throw std::runtime_error(std::string("missing pinned Whisper runtime export: ") + name);
    return value;
}
struct Api {
#define MEMBER(name) decltype(&::name) name = nullptr;
    MEMBER(whisper_free)
    MEMBER(whisper_lang_id)
    MEMBER(whisper_context_default_params)
    MEMBER(whisper_init_from_buffer_with_params)
    MEMBER(whisper_is_multilingual)
    MEMBER(whisper_full_default_params)
    MEMBER(whisper_full)
    MEMBER(whisper_lang_str)
    MEMBER(whisper_full_lang_id)
    MEMBER(whisper_full_n_segments)
    MEMBER(whisper_full_get_segment_text)
    MEMBER(whisper_full_get_segment_t0)
    MEMBER(whisper_full_get_segment_t1)
#undef MEMBER
    decltype(&::ggml_backend_register) registerBackend = nullptr;
    decltype(&::ggml_backend_dev_by_name) deviceByName = nullptr;
    std::once_flag cudaOnce;
    Api() {
        HMODULE self = nullptr;
        // Global ggml/Whisper callbacks must remain valid after a consumer closes its DLL.
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                          reinterpret_cast<LPCWSTR>(&directory), &self);
        const auto dir = directory();
        load(dir / L"ggml-base.dll");
        auto ggml = load(dir / L"ggml.dll");
        registerBackend = symbol<decltype(registerBackend)>(ggml, "ggml_backend_register");
        deviceByName = symbol<decltype(deviceByName)>(ggml, "ggml_backend_dev_by_name");
        // Use the official backend score to select a compatible CPU implementation.
        int bestScore = 0;
        HMODULE best = nullptr;
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            const auto name = entry.path().filename().wstring();
            if (name.rfind(L"ggml-cpu-", 0) != 0 || entry.path().extension() != L".dll") continue;
            auto candidate = load(entry.path());
            auto score = symbol<int (*)()>(candidate, "ggml_backend_score");
            int value = score();
            if (value > bestScore) { bestScore = value; best = candidate; }
        }
        if (!best) throw std::runtime_error("no compatible Whisper CPU backend");
        registerBackend(symbol<ggml_backend_reg_t (*)()>(best, "ggml_backend_init")());
        auto whisper = load(dir / L"whisper.dll");
#define BIND(name) name = symbol<decltype(name)>(whisper, #name);
        BIND(whisper_free)
        BIND(whisper_lang_id)
        BIND(whisper_context_default_params)
        BIND(whisper_init_from_buffer_with_params)
        BIND(whisper_is_multilingual)
        BIND(whisper_full_default_params)
        BIND(whisper_full)
        BIND(whisper_lang_str)
        BIND(whisper_full_lang_id)
        BIND(whisper_full_n_segments)
        BIND(whisper_full_get_segment_text)
        BIND(whisper_full_get_segment_t0)
        BIND(whisper_full_get_segment_t1)
#undef BIND
        symbol<decltype(&::whisper_log_set)>(whisper, "whisper_log_set")(log, nullptr);
    }
    void requireCuda() {
        std::call_once(cudaOnce, [&] {
            const auto dir = directory();
            load(dir / L"cudart64_12.dll");
            load(dir / L"cublasLt64_12.dll");
            load(dir / L"cublas64_12.dll");
            auto cuda = load(dir / L"ggml-cuda.dll");
            registerBackend(symbol<ggml_backend_reg_t (*)()>(cuda, "ggml_backend_init")());
        });
        if (!deviceByName("CUDA0")) throw std::runtime_error("no CUDA GPU available; CPU fallback rejected");
    }
};
inline Api& api() { static Api value; return value; }
}
#define whisper_free asr_runtime::api().whisper_free
#define whisper_lang_id asr_runtime::api().whisper_lang_id
#define whisper_context_default_params asr_runtime::api().whisper_context_default_params
#define whisper_init_from_buffer_with_params asr_runtime::api().whisper_init_from_buffer_with_params
#define whisper_is_multilingual asr_runtime::api().whisper_is_multilingual
#define whisper_full_default_params asr_runtime::api().whisper_full_default_params
#define whisper_full asr_runtime::api().whisper_full
#define whisper_lang_str asr_runtime::api().whisper_lang_str
#define whisper_full_lang_id asr_runtime::api().whisper_full_lang_id
#define whisper_full_n_segments asr_runtime::api().whisper_full_n_segments
#define whisper_full_get_segment_text asr_runtime::api().whisper_full_get_segment_text
#define whisper_full_get_segment_t0 asr_runtime::api().whisper_full_get_segment_t0
#define whisper_full_get_segment_t1 asr_runtime::api().whisper_full_get_segment_t1
#endif

// Single-header Windows C++ consumer. Load asr.dll at runtime; no import library.
#pragma once
#include <windows.h>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace asr {
struct Options { std::string model, language = "auto"; int threads = 0; int device = 0; };
struct Segment { int64_t startMs = 0, endMs = 0; std::string text; };
struct Transcript {
    std::string text, language;
    std::vector<Segment> segments;
    double audioMs = 0, decodeMs = 0, transcribeMs = 0;
    int utf8Replacements = 0;
};
inline std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, nullptr, 0);
    if (!count) throw std::runtime_error("invalid UTF-8 path");
    std::wstring result(size_t(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, result.data(), count);
    result.pop_back(); return result;
}
inline std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("invalid Unicode path");
    std::string result(size_t(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}
class Engine {
public:
    Engine() = default;
    ~Engine() { close(); }
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    bool loadDll(const std::string& path = "asr.dll", std::string* error = nullptr) {
        close();
        try {
            std::wstring resolved = wide(path);
            if (path == "asr.dll") {
                wchar_t exe[32768];
                DWORD count = GetModuleFileNameW(nullptr, exe, 32768);
                if (!count || count >= 32768) return fail(error, "cannot find executable directory");
                std::wstring name(exe, count);
                resolved = name.substr(0, name.find_last_of(L"\\/") + 1) + resolved;
            }
            wchar_t full[32768];
            DWORD count = GetFullPathNameW(resolved.c_str(), 32768, full, nullptr);
            if (!count || count >= 32768) return fail(error, "cannot resolve ASR DLL path");
            dll_ = LoadLibraryExW(full, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!dll_) return fail(error, "cannot load asr.dll: Windows error " + std::to_string(GetLastError()));
#define ASR_BIND(member, name) member = reinterpret_cast<decltype(member)>(GetProcAddress(dll_, name)); if (!member) { close(); return fail(error, std::string("missing SDK export: ") + name); }
            ASR_BIND(defaults_, "asr_default_options")
            ASR_BIND(create_, "asr_create")
            ASR_BIND(destroy_, "asr_destroy")
            ASR_BIND(run_, "asr_transcribe_file")
            progress_ = reinterpret_cast<decltype(progress_)>(GetProcAddress(dll_, "asr_transcribe_file_with_progress"));
            device_ = reinterpret_cast<decltype(device_)>(GetProcAddress(dll_, "asr_device"));
            ASR_BIND(free_, "asr_free_result")
            ASR_BIND(text_, "asr_result_text")
            ASR_BIND(language_, "asr_result_language")
            ASR_BIND(count_, "asr_result_segment_count")
            replacements_ = reinterpret_cast<decltype(replacements_)>(GetProcAddress(dll_, "asr_result_utf8_replacements"));
            ASR_BIND(segmentText_, "asr_result_segment_text")
            ASR_BIND(start_, "asr_result_segment_start_ms")
            ASR_BIND(end_, "asr_result_segment_end_ms")
            ASR_BIND(audio_, "asr_result_audio_ms")
            ASR_BIND(decode_, "asr_result_decode_ms")
            ASR_BIND(transcribe_, "asr_result_transcribe_ms")
#undef ASR_BIND
            if (error) error->clear();
            return true;
        } catch (const std::exception& e) { close(); return fail(error, e.what()); }
    }
    bool init(const Options& options = {}, std::string* error = nullptr) {
        if (!dll_ && !loadDll("asr.dll", error)) return false;
        if (handle_) destroy_(handle_);
        handle_ = nullptr;
        NativeOptions native{}; defaults_(&native);
        native.model = options.model.empty() ? nullptr : options.model.c_str();
        native.language = options.language.c_str(); native.threads = options.threads; native.device = options.device;
        char message[2048] = {};
        handle_ = create_(&native, message, sizeof(message));
        if (!handle_) return fail(error, message);
        if (error) error->clear();
        return true;
    }
    bool transcribeFile(const std::string& path, Transcript& out, std::string* error = nullptr,
                        void (*callback)(int, void*) = nullptr, void* user = nullptr) {
        out = {};
        if (!handle_) return fail(error, "ASR engine is not initialized");
        char message[2048] = {};
        if (callback && !progress_) return fail(error, "SDK does not support progress callbacks");
        void* result = callback ? progress_(handle_, path.c_str(), callback, user, message, sizeof(message))
                                : run_(handle_, path.c_str(), message, sizeof(message));
        if (!result) return fail(error, message);
        struct Guard { void* p; void (*release)(void*); ~Guard() { release(p); } } guard{result, free_};
        out.utf8Replacements = replacements_ ? replacements_(result) : 0;
        out.text = text_(result); out.language = language_(result);
        out.audioMs = audio_(result); out.decodeMs = decode_(result); out.transcribeMs = transcribe_(result);
        const int count = count_(result);
        for (int i = 0; i < count; ++i)
            out.segments.push_back({start_(result, i), end_(result, i), segmentText_(result, i)});
        if (error) error->clear();
        return true;
    }
    int device() const { return handle_ ? (device_ ? device_(handle_) : 0) : -1; }
    bool isLoaded() const { return handle_ != nullptr; }
    void close() {
        if (handle_ && destroy_) destroy_(handle_);
        handle_ = nullptr;
        if (dll_) FreeLibrary(dll_);
        dll_ = nullptr;
    }
private:
    struct NativeOptions {
        uint32_t struct_size; int32_t threads;
        const char* model; const char* language;
        int32_t device, reserved;
    };
    HMODULE dll_ = nullptr;
    void* handle_ = nullptr;
    void (*defaults_)(NativeOptions*) = nullptr;
    void* (*create_)(const NativeOptions*, char*, int) = nullptr;
    void (*destroy_)(void*) = nullptr;
    void* (*run_)(void*, const char*, char*, int) = nullptr;
    int (*device_)(void*) = nullptr;
    void* (*progress_)(void*, const char*, void (*)(int, void*), void*, char*, int) = nullptr;
    void (*free_)(void*) = nullptr;
    const char* (*text_)(void*) = nullptr;
    const char* (*language_)(void*) = nullptr;
    int (*count_)(void*) = nullptr;
    int (*replacements_)(void*) = nullptr;
    const char* (*segmentText_)(void*, int) = nullptr;
    int64_t (*start_)(void*, int) = nullptr;
    int64_t (*end_)(void*, int) = nullptr;
    double (*audio_)(void*) = nullptr;
    double (*decode_)(void*) = nullptr;
    double (*transcribe_)(void*) = nullptr;
    static bool fail(std::string* out, const std::string& text) { if (out) *out = text; return false; }
};
}

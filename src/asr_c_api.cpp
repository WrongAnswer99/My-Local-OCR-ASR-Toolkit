#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "asr_api.hpp"
#include "audio.hpp"
#include "asr_utf8.hpp"
#include "whisper_runtime.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
struct Engine {
    whisper_context* context = nullptr;
    int threads = 8;
    int device = 0;
    std::string language;
    std::string vadModel;
    std::mutex mutex;
    ~Engine() { if (context) whisper_free(context); }
};
struct Segment { int64_t start = 0, end = 0; std::string text; };
struct Result {
    std::string text, language;
    std::vector<Segment> segments;
    int utf8Replacements = 0;
    double audioMs = 0, decodeMs = 0, transcribeMs = 0;
};
void errorText(char* out, int capacity, const std::string& text) {
    if (out && capacity > 0) {
        const size_t count = std::min(text.size(), size_t(capacity - 1));
        std::memcpy(out, text.data(), count); out[count] = 0;
    }
}
std::filesystem::path defaultModel() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      reinterpret_cast<LPCWSTR>(&defaultModel), &module);
    wchar_t filename[32768];
    const DWORD count = GetModuleFileNameW(module, filename, 32768);
    if (!count || count >= 32768) throw std::runtime_error("cannot resolve ASR module directory");
    return std::filesystem::path(std::wstring(filename, count)).parent_path() / L"models/whisper/ggml-small-q5_1.bin";
}
std::string vadPathText(const std::filesystem::path& path) {
#ifdef ASR_CUDA_RUNTIME
    // The pinned official MSVC DLL opens UTF-8 paths with a wide ifstream.
    return path.u8string();
#else
    // The static MinGW VAD loader uses narrow ifstream. Use a short path when
    // available, then encode in the process's active Windows code page.
    std::wstring native = path.wstring();
    const DWORD capacity = GetShortPathNameW(path.c_str(), nullptr, 0);
    if (capacity) {
        std::wstring shortened(capacity, L'\0');
        const DWORD written = GetShortPathNameW(path.c_str(), shortened.data(), capacity);
        if (written && written < capacity) { shortened.resize(written); native = std::move(shortened); }
    }
    const UINT codepage = GetACP();
    BOOL replaced = FALSE;
    BOOL* check = codepage == CP_UTF8 ? nullptr : &replaced;
    const int count = WideCharToMultiByte(codepage, 0, native.data(), int(native.size()), nullptr, 0, nullptr, check);
    if (!count || replaced) throw std::runtime_error("VAD model path cannot be represented by the Windows code page");
    std::string result(size_t(count), '\0');
    if (!WideCharToMultiByte(codepage, 0, native.data(), int(native.size()), result.data(), count, nullptr, check) || replaced)
        throw std::runtime_error("cannot encode VAD model path");
    return result;
#endif
}
const Segment* segment(AsrResult handle, int index) {
    const auto* result = static_cast<const Result*>(handle);
    return result && index >= 0 && size_t(index) < result->segments.size() ? &result->segments[size_t(index)] : nullptr;
}
double elapsed(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}
}

extern "C" {
const char* asr_version() {
#ifdef ASR_CUDA_RUNTIME
    return "Whisper file ASR SDK 0.3 (whisper.cpp " ASR_WHISPER_VERSION ", CPU/CUDA)";
#else
    return "Whisper file ASR SDK 0.3 (whisper.cpp " ASR_WHISPER_VERSION ", CPU)";
#endif
}
void asr_default_options(AsrOptions* options) {
    if (!options) return;
    std::memset(options, 0, sizeof(*options));
    options->struct_size = sizeof(*options);
}
AsrHandle asr_create(const AsrOptions* given, char* error, int capacity) {
    errorText(error, capacity, "");
    try {
        AsrOptions defaults; asr_default_options(&defaults);
        const AsrOptions* options = given ? given : &defaults;
        if (options->struct_size < sizeof(AsrOptions)) throw std::runtime_error("AsrOptions.struct_size is too small");
        if (options->device < 0 || options->device > 1) throw std::runtime_error("device must be 0 (CPU) or 1 (CUDA)");
#ifndef ASR_CUDA_RUNTIME
        if (options->device != 0) throw std::runtime_error("this Whisper build supports CPU only (device=0)");
#endif
        if (options->threads < 0 || options->threads > 256) throw std::runtime_error("threads must be 0..256");
        auto engine = std::make_unique<Engine>();
        engine->threads = options->threads ? options->threads : int(std::min(8u, std::max(1u, std::thread::hardware_concurrency())));
        engine->language = options->language && *options->language ? options->language : "auto";
        if (engine->language != "auto" && whisper_lang_id(engine->language.c_str()) < 0)
            throw std::runtime_error("unsupported Whisper language: " + engine->language);
        const auto path = options->model && *options->model ? std::filesystem::path(asrWide(options->model)) : defaultModel();
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) throw std::runtime_error("cannot open Whisper model");
        const auto length = input.tellg();
        if (length < 1024 || length > std::streamoff(8LL * 1024 * 1024 * 1024))
            throw std::runtime_error("invalid Whisper model size");
        std::vector<char> data(static_cast<size_t>(length));
        input.seekg(0);
        if (!input.read(data.data(), length)) throw std::runtime_error("cannot read Whisper model");
        auto vadPath = path.parent_path() / L"ggml-silero-v6.2.0.bin";
        if (!std::filesystem::is_regular_file(vadPath))
            vadPath = defaultModel().parent_path() / L"ggml-silero-v6.2.0.bin";
        if (std::filesystem::is_regular_file(vadPath)) engine->vadModel = vadPathText(vadPath);
        auto params = whisper_context_default_params();
        params.use_gpu = options->device == 1;
#ifdef ASR_CUDA_RUNTIME
        if (params.use_gpu) asr_runtime::api().requireCuda();
        asr_runtime::cudaUsed = false;
        asr_runtime::cudaFailed = false;
#endif
        engine->context = whisper_init_from_buffer_with_params(data.data(), data.size(), params);
        if (!engine->context) throw std::runtime_error("Whisper model initialization failed; check model format");
#ifdef ASR_CUDA_RUNTIME
        if (params.use_gpu && (!asr_runtime::cudaUsed || asr_runtime::cudaFailed))
            throw std::runtime_error("CUDA backend initialization failed; CPU fallback rejected");
#endif
        engine->device = options->device;
        if (!whisper_is_multilingual(engine->context) && engine->language != "auto" && engine->language != "en")
            throw std::runtime_error("an English-only model cannot transcribe the requested language");
        return engine.release();
    } catch (const std::exception& exception) { errorText(error, capacity, exception.what()); return nullptr; }
    catch (...) { errorText(error, capacity, "unexpected Whisper initialization failure"); return nullptr; }
}
int asr_device(AsrHandle handle) { return handle ? static_cast<Engine*>(handle)->device : -1; }
void asr_destroy(AsrHandle handle) { delete static_cast<Engine*>(handle); }
AsrResult asr_transcribe_file(AsrHandle handle, const char* path, char* error, int capacity) {
    return asr_transcribe_file_with_progress(handle, path, nullptr, nullptr, error, capacity);
}
AsrResult asr_transcribe_file_with_progress(AsrHandle handle, const char* path, AsrProgressFn callback,
                                          void* user, char* error, int capacity) {
    errorText(error, capacity, "");
    try {
        if (!handle) throw std::runtime_error("ASR handle is null");
        if (!path || !*path) throw std::runtime_error("audio path is empty");
        auto* engine = static_cast<Engine*>(handle);
        std::lock_guard<std::mutex> lock(engine->mutex);
        auto result = std::make_unique<Result>();
        std::vector<float> samples;
        std::string decodeError;
        const auto start = std::chrono::steady_clock::now();
        if (!decodeAudioFile(path, samples, decodeError)) throw std::runtime_error(decodeError);
        const auto decoded = std::chrono::steady_clock::now();
        auto params = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
        params.beam_search.beam_size = 5;
        params.n_threads = engine->threads;
        params.language = engine->language.c_str();
        params.translate = false;
        params.print_progress = false; params.print_realtime = false; params.print_timestamps = false;
        params.print_special = false;
        // no_context clears earlier API calls, not the rolling prompt inside
        // this file. A bad segment must not condition every later window.
        params.no_context = true;
        params.n_max_text_ctx = 0;
        params.temperature = 0.0f;
        // Keep Whisper's quality-triggered fallback (entropy/log probability).
        // Disabling it traps difficult/noisy windows in greedy repetition.
        params.temperature_inc = 0.2f;
        params.suppress_nst = true;
        // Skip non-speech before decoding; Whisper maps times back to the source.
        if (!engine->vadModel.empty()) {
            params.vad = true;
            params.vad_model_path = engine->vadModel.c_str();
            params.vad_params.threshold = 0.25f;
            params.vad_params.min_speech_duration_ms = 100;
            params.vad_params.min_silence_duration_ms = 300;
            params.vad_params.speech_pad_ms = 300;
        }
        struct Progress { AsrProgressFn callback; void* user; int last = -1; } progress{callback, user};
        if (callback) {
            params.progress_callback_user_data = &progress;
            params.progress_callback = [](whisper_context*, whisper_state*, int percent, void* data) {
                auto* p = static_cast<Progress*>(data);
                percent = std::clamp(percent, 0, 99);
                if (percent > p->last) { p->last = percent; p->callback(percent, p->user); }
            };
            progress.last = 0; callback(0, user);
        }
        // duration_ms=0 processes the complete input, including subsequent
        // Whisper 30-second windows. Results from prior calls are not reused.
        const int status = whisper_full(engine->context, params, samples.data(), int(samples.size()));
        if (status != 0) throw std::runtime_error("Whisper transcription failed: " + std::to_string(status));
        const auto finished = std::chrono::steady_clock::now();
        result->audioMs = samples.size() * 1000.0 / WHISPER_SAMPLE_RATE;
        result->decodeMs = elapsed(start, decoded); result->transcribeMs = elapsed(decoded, finished);
        const char* language = whisper_lang_str(whisper_full_lang_id(engine->context));
        result->language = language ? language : "unknown";
        const int count = whisper_full_n_segments(engine->context);
        for (int i = 0; i < count; ++i) {
            const char* text = whisper_full_get_segment_text(engine->context, i);
            Segment item;
            item.start = std::clamp<int64_t>(whisper_full_get_segment_t0(engine->context, i) * 10, 0, int64_t(result->audioMs));
            item.end = std::clamp<int64_t>(whisper_full_get_segment_t1(engine->context, i) * 10, item.start, int64_t(result->audioMs));
            item.text = text ? text : "";
            result->segments.push_back(std::move(item));
        }
        std::vector<std::string> texts;
        for (const auto& item : result->segments) texts.push_back(item.text);
        result->utf8Replacements = asrNormalizeUtf8(texts);
        for (size_t i = 0; i < texts.size(); ++i) {
            result->segments[i].text = std::move(texts[i]);
            result->text += result->segments[i].text;
        }
        if (callback) callback(100, user);
        return result.release();
    } catch (const std::exception& exception) { errorText(error, capacity, exception.what()); return nullptr; }
    catch (...) { errorText(error, capacity, "unexpected transcription failure"); return nullptr; }
}
void asr_free_result(AsrResult handle) { delete static_cast<Result*>(handle); }
const char* asr_result_text(AsrResult handle) { return handle ? static_cast<Result*>(handle)->text.c_str() : ""; }
const char* asr_result_language(AsrResult handle) { return handle ? static_cast<Result*>(handle)->language.c_str() : ""; }
int asr_result_utf8_replacements(AsrResult handle) { return handle ? static_cast<Result*>(handle)->utf8Replacements : 0; }
int asr_result_segment_count(AsrResult handle) { return handle ? int(static_cast<Result*>(handle)->segments.size()) : 0; }
const char* asr_result_segment_text(AsrResult handle, int index) { const auto* s = segment(handle, index); return s ? s->text.c_str() : ""; }
int64_t asr_result_segment_start_ms(AsrResult handle, int index) { const auto* s = segment(handle, index); return s ? s->start : 0; }
int64_t asr_result_segment_end_ms(AsrResult handle, int index) { const auto* s = segment(handle, index); return s ? s->end : 0; }
double asr_result_audio_ms(AsrResult handle) { return handle ? static_cast<Result*>(handle)->audioMs : 0; }
double asr_result_decode_ms(AsrResult handle) { return handle ? static_cast<Result*>(handle)->decodeMs : 0; }
double asr_result_transcribe_ms(AsrResult handle) { return handle ? static_cast<Result*>(handle)->transcribeMs : 0; }
}

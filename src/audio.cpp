#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include "audio.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

std::wstring asrWide(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, nullptr, 0);
    if (!n) throw std::runtime_error("invalid UTF-8 path");
    std::wstring result(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, result.data(), n);
    result.pop_back();
    return result;
}

namespace {
template<class T> struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    T* operator->() const { return p; }
};
struct Platform {
    bool com = false, mf = false;
    ~Platform() { if (mf) MFShutdown(); if (com) CoUninitialize(); }
};
void checked(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        char code[16]; std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));
        throw std::runtime_error(std::string(operation) + " failed (" + code + ")");
    }
}

// Windowed-sinc resampling with a polyphase table. Low-pass before downsampling,
// retaining full duration without depending on an external ffmpeg executable.
std::vector<float> resample(const std::vector<float>& mono, unsigned rate) {
    if (rate == 16000) return mono;
    const uint64_t count = uint64_t(mono.size()) * 16000 / rate;
    if (count > uint64_t(std::numeric_limits<int>::max()))
        throw std::runtime_error("audio is too long for Whisper's sample-count API");
    const double cutoff = std::min(1.0, 16000.0 / rate);
    const int radius = int(std::ceil(16.0 / cutoff));
    const int taps = 2 * radius, phases = 256;
    constexpr double pi = 3.14159265358979323846;
    std::vector<double> weights(size_t(phases) * taps);
    for (int phase = 0; phase < phases; ++phase) {
        const double fraction = double(phase) / phases;
        for (int tap = 0; tap < taps; ++tap) {
            const double distance = (tap - radius + 1) - fraction;
            const double x = pi * cutoff * distance;
            const double sinc = std::abs(x) < 1e-12 ? 1.0 : std::sin(x) / x;
            const double window = 0.5 + 0.5 * std::cos(pi * distance / radius);
            weights[size_t(phase) * taps + tap] = cutoff * sinc * window;
        }
    }
    std::vector<float> output(static_cast<size_t>(count));
    for (size_t i = 0; i < output.size(); ++i) {
        const double position = double(i) * rate / 16000;
        const int64_t center = int64_t(position);
        const int phase = std::min(phases - 1, int((position - center) * phases));
        const double* kernel = weights.data() + size_t(phase) * taps;
        double value = 0, total = 0;
        for (int tap = 0; tap < taps; ++tap) {
            const int64_t index = center + tap - radius + 1;
            if (index >= 0 && uint64_t(index) < mono.size()) {
                value += mono[size_t(index)] * kernel[tap];
                total += kernel[tap];
            }
        }
        output[i] = total != 0 ? float(value / total) : 0;
    }
    return output;
}
}

bool decodeAudioFile(const std::string& path, std::vector<float>& samples, std::string& error) {
    samples.clear(); error.clear();
    try {
        const auto wide = asrWide(path);
        const DWORD attributes = GetFileAttributesW(wide.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
            throw std::runtime_error("audio file does not exist: " + path);
        Platform platform;
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (hr != RPC_E_CHANGED_MODE) checked(hr, "COM initialization");
        platform.com = SUCCEEDED(hr);
        checked(MFStartup(MF_VERSION, MFSTARTUP_FULL), "Media Foundation startup");
        platform.mf = true;
        Com<IMFSourceReader> reader;
        checked(MFCreateSourceReaderFromURL(wide.c_str(), nullptr, &reader.p), "open/decode audio file");
        checked(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE), "deselect streams");
        checked(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE), "select audio stream");
        Com<IMFMediaType> type;
        checked(MFCreateMediaType(&type.p), "create audio type");
        checked(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio), "set audio major type");
        checked(type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM), "set PCM type");
        checked(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16), "set 16-bit PCM");
        checked(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, type.p), "negotiate PCM decoder");
        Com<IMFMediaType> actual;
        checked(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actual.p), "get decoded format");
        UINT32 rate = 0, channels = 0, bits = 0, align = 0;
        checked(actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate), "get sample rate");
        checked(actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels), "get channels");
        checked(actual->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits), "get PCM bits");
        checked(actual->GetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, &align), "get PCM alignment");
        if (bits != 16 || channels == 0 || channels > 32 || align != channels * 2 || rate < 8000 || rate > 384000)
            throw std::runtime_error("unsupported decoded PCM format");
        std::vector<float> mono;
        std::vector<uint8_t> pending;
        while (true) {
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            Com<IMFSample> sample;
            checked(reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &timestamp, &sample.p), "read decoded audio");
            if (flags & MF_SOURCE_READERF_ERROR) throw std::runtime_error("audio decoder reported an error");
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
                throw std::runtime_error("audio format changed mid-file");
            if (sample.p) {
                Com<IMFMediaBuffer> buffer;
                checked(sample->ConvertToContiguousBuffer(&buffer.p), "get PCM buffer");
                BYTE* data = nullptr; DWORD length = 0;
                checked(buffer->Lock(&data, nullptr, &length), "lock PCM buffer");
                try { pending.insert(pending.end(), data, data + length); }
                catch (...) { buffer->Unlock(); throw; }
                checked(buffer->Unlock(), "unlock PCM buffer");
                const size_t frames = pending.size() / align;
                if ((uint64_t(mono.size()) + frames) * 16000 / rate > uint64_t(std::numeric_limits<int>::max()))
                    throw std::runtime_error("audio is too long for Whisper's sample-count API");
                const size_t begin = mono.size();
                mono.resize(begin + frames);
                for (size_t i = 0; i < frames; ++i) {
                    double sum = 0;
                    for (unsigned c = 0; c < channels; ++c) {
                        int16_t value;
                        std::memcpy(&value, pending.data() + i * align + c * 2, sizeof(value));
                        sum += value / 32768.0;
                    }
                    mono[begin + i] = float(sum / channels);
                }
                pending.erase(pending.begin(), pending.begin() + frames * align);
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        }
        if (!pending.empty()) throw std::runtime_error("incomplete PCM frame at end of audio");
        if (mono.empty()) throw std::runtime_error("audio contains no samples");
        samples = resample(mono, rate);
        if (samples.empty()) throw std::runtime_error("audio is too short");
        return true;
    } catch (const std::exception& exception) {
        error = exception.what(); samples.clear(); return false;
    }
}

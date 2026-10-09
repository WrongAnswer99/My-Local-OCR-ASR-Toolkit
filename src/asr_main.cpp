#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "asr.hpp"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
std::string json(const std::string& text) {
    std::ostringstream out; out << '"';
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c) << std::dec;
        else out << c;
    }
    out << '"'; return out.str();
}
void write(const std::string& path, const std::string& content) {
    std::ofstream file(std::filesystem::path(asr::wide(path)), std::ios::binary | std::ios::trunc);
    if (!file || !file.write(content.data(), std::streamsize(content.size())))
        throw std::runtime_error("cannot write output: " + path);
}
void help() {
    std::cout << "Usage: asr_cli <audio-file> [--model=path] [--language=auto|zh|en|...]\n"
                 "                [--threads=0..256] [--output=text.txt] [--json=result.json]\n"
                 "                [--device=cpu|gpu]\n"
                 "Offline OpenAI Whisper full-file transcription (CPU/CUDA SDK).\n"
                 "WAV/MP3/M4A and other installed Windows Media Foundation codecs.\n"
                 "Default model: models/whisper/ggml-small-q5_1.bin beside asr.dll.\n"
                 "Stdout: complete UTF-8 transcript. Stderr: diagnostics and timings.\n";
}
}
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    try {
        asr::Options options;
        std::string input, output, jsonPath;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = asr::utf8(argv[i]);
            if (argument == "--help" || argument == "-h") { help(); return 0; }
            if (argument.rfind("--model=", 0) == 0) { options.model = argument.substr(8); if (options.model.empty()) throw std::runtime_error("--model is empty"); }
            else if (argument.rfind("--language=", 0) == 0) { options.language = argument.substr(11); if (options.language.empty()) throw std::runtime_error("--language is empty"); }
            else if (argument.rfind("--device=", 0) == 0) {
                const auto value = argument.substr(9);
                if (value != "cpu" && value != "gpu") throw std::runtime_error("--device must be cpu or gpu");
                options.device = value == "gpu" ? 1 : 0;
            }
            else if (argument.rfind("--threads=", 0) == 0) {
                const std::string value = argument.substr(10);
                if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) throw std::runtime_error("invalid --threads");
                const unsigned long n = std::stoul(value);
                if (n > 256) throw std::runtime_error("--threads must be 0..256");
                options.threads = int(n);
            }
            else if (argument.rfind("--output=", 0) == 0) { output = argument.substr(9); if (output.empty()) throw std::runtime_error("--output is empty"); }
            else if (argument.rfind("--json=", 0) == 0) { jsonPath = argument.substr(7); if (jsonPath.empty()) throw std::runtime_error("--json is empty"); }
            else if (!argument.empty() && argument[0] == '-') throw std::runtime_error("unknown argument: " + argument);
            else if (input.empty()) input = argument;
            else throw std::runtime_error("only one audio file is accepted");
        }
        if (input.empty()) { help(); return 1; }
        const auto inputPath = std::filesystem::absolute(std::filesystem::path(asr::wide(input))).lexically_normal();
        if (!std::filesystem::is_regular_file(inputPath)) throw std::runtime_error("audio file does not exist: " + input);
        for (const auto& path : {output, jsonPath}) {
            if (path.empty()) continue;
            const auto destination = std::filesystem::absolute(std::filesystem::path(asr::wide(path))).lexically_normal();
            if (_wcsicmp(inputPath.c_str(), destination.c_str()) == 0 ||
                (std::filesystem::exists(destination) && std::filesystem::equivalent(inputPath, destination)))
                throw std::runtime_error("output must not overwrite the input audio");
        }
        if (!output.empty() && !jsonPath.empty() &&
            _wcsicmp(std::filesystem::absolute(std::filesystem::path(asr::wide(output))).lexically_normal().c_str(),
                     std::filesystem::absolute(std::filesystem::path(asr::wide(jsonPath))).lexically_normal().c_str()) == 0)
            throw std::runtime_error("text and JSON outputs must have different paths");
        asr::Engine engine;
        std::string error;
        const auto start = std::chrono::steady_clock::now();
        if (!engine.init(options, &error)) throw std::runtime_error(error);
        const double initMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        asr::Transcript transcript;
        if (!engine.transcribeFile(input, transcript, &error)) throw std::runtime_error(error);
        if (!output.empty()) write(output, transcript.text + "\n");
        if (!jsonPath.empty()) {
            std::ostringstream document;
            document << std::setprecision(12) << "{\n  \"backend\": \"whisper.cpp\", \"device\": " << json(options.device ? "gpu" : "cpu") << ",\n"
                     << "  \"language\": " << json(transcript.language) << ", \"text\": " << json(transcript.text)
                     << ",\n  \"utf8_replacements\": " << transcript.utf8Replacements
                     << ",\n  \"init_ms\": " << initMs << ", \"audio_ms\": " << transcript.audioMs
                     << ", \"decode_ms\": " << transcript.decodeMs << ", \"transcribe_ms\": " << transcript.transcribeMs << ",\n  \"segments\": [";
            for (size_t i = 0; i < transcript.segments.size(); ++i) {
                const auto& s = transcript.segments[i];
                document << (i ? "," : "") << "\n    {\"start_ms\": " << s.startMs << ", \"end_ms\": " << s.endMs << ", \"text\": " << json(s.text) << "}";
            }
            document << "\n  ]\n}\n"; write(jsonPath, document.str());
        }
        std::cout << transcript.text << '\n';
        std::fprintf(stderr, "[asr] device=%s language=%s audio=%.2f ms init=%.2f ms decode=%.2f ms transcribe=%.2f ms segments=%zu\n",
                     options.device ? "gpu" : "cpu", transcript.language.c_str(), transcript.audioMs, initMs, transcript.decodeMs, transcript.transcribeMs, transcript.segments.size());
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "[asr] %s\n", e.what()); return 1; }
}

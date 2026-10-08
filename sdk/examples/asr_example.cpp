#include "asr.hpp"
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc != 2) { std::cerr << "Usage: asr_example <audio-file>\n"; return 1; }
    asr::Engine engine;
    asr::Options options;
    options.language = "auto";
    options.threads = 8;
    std::string error;
    if (!engine.init(options, &error)) { std::cerr << error << '\n'; return 1; }
    asr::Transcript result;
    if (!engine.transcribeFile(asr::utf8(argv[1]), result, &error)) { std::cerr << error << '\n'; return 1; }
    std::cout << result.text << '\n';
    return 0;
}

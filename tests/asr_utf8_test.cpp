#include "asr_utf8.hpp"
#include <cassert>
#include <iostream>
#include <iterator>
#include <fstream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    const std::string replacement = "\xEF\xBF\xBD";
    const std::string character = "\xE5\x91\x83";
    std::vector<std::string> valid{"ASCII ", character, "\xF0\x9F\x98\x80"};
    auto original = valid;
    assert(asrNormalizeUtf8(valid) == 0 && valid == original);
    std::vector<std::string> split{"", "\xE5", "", "\x91", "\x83" "end"};
    assert(asrNormalizeUtf8(split) == 0);
    assert(split[1] == character && split[3].empty() && split[4] == "end");
    // The actual failed video: incomplete E5 91 followed by a new character E5 A0 B1.
    std::vector<std::string> broken{"prefix\xE5\x91", "\xE5\xA0\xB1" "suffix"};
    assert(asrNormalizeUtf8(broken) == 1);
    assert(broken[0] == "prefix" + replacement && broken[1] == "\xE5\xA0\xB1" "suffix");
    std::vector<std::string> invalid{"\x80", "\xC0\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xF0\x9F"};
    assert(asrNormalizeUtf8(invalid) == 11);
    std::vector<std::string> empty(3);
    assert(asrNormalizeUtf8(empty) == 0 && empty.size() == 3);
    if (argc == 2) {
        std::ifstream input(argv[1], std::ios::binary);
        assert(input);
        std::string raw((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        std::vector<std::string> fragments{raw};
        const int count = asrNormalizeUtf8(fragments);
        std::ofstream output(std::string(argv[1]) + ".normalized", std::ios::binary);
        output.write(fragments[0].data(), fragments[0].size());
        std::cout << "replacements=" << count << "\n";
    }
    std::cout << "UTF-8 malformed sequences, valid text and split segment tests passed\n";
}

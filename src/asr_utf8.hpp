// Normalize Whisper byte-token output at the SDK boundary.
// Decode across segment boundaries so a valid split code point is preserved.
#pragma once
#include <string>
#include <vector>
#include <cstddef>
inline int asrNormalizeUtf8(std::vector<std::string>& segments) {
    std::string bytes;
    std::vector<size_t> ends;
    for (const auto& text : segments) {
        bytes += text;
        ends.push_back(bytes.size());
    }
    for (auto& text : segments) text.clear();
    int replacements = 0;
    size_t owner = 0;
    for (size_t i = 0; i < bytes.size();) {
        while (owner + 1 < ends.size() && i >= ends[owner]) ++owner;
        const unsigned char lead = static_cast<unsigned char>(bytes[i]);
        size_t required = 0;
        if (lead < 0x80) required = 1;
        else if (lead >= 0xC2 && lead <= 0xDF) required = 2;
        else if (lead >= 0xE0 && lead <= 0xEF) required = 3;
        else if (lead >= 0xF0 && lead <= 0xF4) required = 4;
        size_t consumed = 1;
        if (required > 1) {
            while (consumed < required && i + consumed < bytes.size()) {
                const unsigned char next = static_cast<unsigned char>(bytes[i + consumed]);
                if (next < 0x80 || next > 0xBF) break;
                if (consumed == 1 &&
                    ((lead == 0xE0 && next < 0xA0) || (lead == 0xED && next > 0x9F) ||
                     (lead == 0xF0 && next < 0x90) || (lead == 0xF4 && next > 0x8F))) break;
                ++consumed;
            }
        }
        if (required && consumed == required) {
            segments[owner].append(bytes, i, consumed);
        } else {
            segments[owner].append("\xEF\xBF\xBD");
            ++replacements;
        }
        i += consumed;
    }
    return replacements;
}

#pragma once
#include <string>
#include <vector>

// UTF-8 Windows path, decoded to 16 kHz mono float PCM for Whisper.
bool decodeAudioFile(const std::string& path, std::vector<float>& samples, std::string& error);
std::wstring asrWide(const std::string& text);

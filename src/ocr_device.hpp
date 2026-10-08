// Shared execution backend configuration for the core and CLI tools.
#pragma once

#include <string>

enum class OCRDevice { CPU = 0, CUDA = 1 };

inline const char* ocrDeviceName(OCRDevice device) {
    return device == OCRDevice::CUDA ? "cuda" : "cpu";
}

inline bool parseOcrDeviceArgs(int argc, char* argv[], OCRDevice& device,
                               int& deviceId, std::string& err) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--device=", 0) == 0) {
            const auto value = arg.substr(9);
            if (value == "cpu") device = OCRDevice::CPU;
            else if (value == "cuda" || value == "gpu") device = OCRDevice::CUDA;
            else { err = "--device must be cpu, cuda or gpu"; return false; }
        } else if (arg.rfind("--gpu-device=", 0) == 0) {
            const auto value = arg.substr(13);
            try {
                size_t end = 0;
                deviceId = std::stoi(value, &end);
                if (end != value.size() || deviceId < 0) throw 0;
            } catch (...) {
                err = "--gpu-device must be a non-negative integer";
                return false;
            }
        } else if (arg == "--device" || arg == "--gpu-device") {
            err = "use --device=cpu|cuda and --gpu-device=<id>";
            return false;
        }
    }
    return true;
}

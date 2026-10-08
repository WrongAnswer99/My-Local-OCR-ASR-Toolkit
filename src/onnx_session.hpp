// ONNX Runtime 会话封装：运行时动态加载 onnxruntime.dll(避免链接期 ABI 问题)，
// 仅暴露本 OCR 项目所需的最小接口。
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "ocr_device.hpp"

// 前向声明 ORT 不透明类型(避免在头文件引入 onnxruntime_c_api.h)
struct OrtEnv;
struct OrtSession;
struct OrtMemoryInfo;
struct OrtAllocator;

class OnnxSession {
public:
    // 全进程唯一初始化：LoadLibrary + 取 OrtApi；重复调用为幂等。
    // 返回 false 表示失败，err 带说明。
    static bool ensureInitialized(std::string& err);

    OnnxSession();
    ~OnnxSession();
    // 不可拷贝，可移动
    OnnxSession(const OnnxSession&) = delete;
    OnnxSession& operator=(const OnnxSession&) = delete;
    OnnxSession(OnnxSession&& other) noexcept;
    OnnxSession& operator=(OnnxSession&& other) noexcept;

    bool load(const std::string& modelPath, std::string& err,
              int intraOpThreads = 0, OCRDevice device = OCRDevice::CPU,
              int gpuDeviceId = 0);

    bool loaded() const { return session_ != nullptr; }
    const std::string& inputName() const { return inputName_; }
    const std::string& outputName() const { return outputName_; }
    // 模型声明的输入维度(可能含 -1 动态维)
    bool inputShape(std::vector<int64_t>& dims, std::string& err) const;

    // 以给定形状喂入单个 float 输入，返回输出张量形状与数据
    bool run(const std::vector<int64_t>& inShape, const float* inputData,
             std::vector<int64_t>& outShape, std::vector<float>& outData,
             std::string& err);

private:
    bool loadImpl(const std::string& modelPath, std::string& err,
                  int intraOpThreads, OCRDevice device, int gpuDeviceId);
    void releaseAll();
    OrtEnv* env_ = nullptr;          // OrtEnv*
    OrtSession* session_ = nullptr;  // OrtSession*
    OrtMemoryInfo* memInfo_ = nullptr;  // OrtMemoryInfo*
    OrtAllocator* allocator_ = nullptr; // OrtAllocator*
    std::string inputName_;
    std::string outputName_;
    bool profiling_ = false;
};

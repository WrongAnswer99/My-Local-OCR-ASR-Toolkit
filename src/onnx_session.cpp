#include "onnx_session.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <onnxruntime_c_api.h>

#include <cstring>

namespace {

// 延迟加载 onnxruntime.dll，符号表仅有一个入口：OrtGetApiBase
struct RuntimeHandle {
    HMODULE dll = nullptr;
    const OrtApi* api = nullptr;
    bool ok = false;
    std::string err;

    bool init() {
        if (ok) return true;
        if (err.empty() && dll == nullptr) {
            dll = LoadLibraryA("onnxruntime.dll");
            if (!dll) {
                err = "LoadLibrary onnxruntime.dll failed: error code " +
                      std::to_string(GetLastError()) +
                      " (请确认 dll 与可执行文件同目录)";
                return false;
            }
        }
        auto getApiBase =
            (const OrtApiBase* (*)())GetProcAddress(dll, "OrtGetApiBase");
        if (!getApiBase) {
            err = "GetProcAddress(OrtGetApiBase) failed";
            return false;
        }
        api = getApiBase()->GetApi(ORT_API_VERSION);
        if (!api) {
            err = "OrtGetApiBase()->GetApi() returned null";
            return false;
        }
        ok = true;
        return true;
    }
};
RuntimeHandle g_runtime;

// 封装错误消息格式转换
std::string statusMessage(const OrtApi* api, OrtStatus* status) {
    if (!status) return "unknown error";
    const char* msg = api->GetErrorMessage(status);
    api->ReleaseStatus(status);
    return msg ? std::string(msg) : "unknown error";
}

size_t elementCount(const std::vector<int64_t>& dims) {
    size_t n = 1;
    for (int64_t d : dims) n *= size_t(d);
    return n;
}

// Windows 下 CreateSession 的模型路径参数是宽字符(ORTCHAR_T=wchar_t)
std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()),
                                        nullptr, 0);
    std::wstring w(size_t(len), 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), len);
    return w;
}

}  // namespace

bool OnnxSession::ensureInitialized(std::string& err) {
    if (g_runtime.init()) return true;
    err = g_runtime.err;
    return false;
}

OnnxSession::OnnxSession() = default;

OnnxSession::~OnnxSession() { releaseAll(); }

OnnxSession::OnnxSession(OnnxSession&& other) noexcept
    : env_(other.env_), session_(other.session_), memInfo_(other.memInfo_),
      allocator_(other.allocator_), inputName_(std::move(other.inputName_)),
      outputName_(std::move(other.outputName_)) {
    other.env_ = nullptr;
    other.session_ = nullptr;
    other.memInfo_ = nullptr;
    other.allocator_ = nullptr;
}

OnnxSession& OnnxSession::operator=(OnnxSession&& other) noexcept {
    if (this != &other) {
        releaseAll();
        env_ = other.env_;
        session_ = other.session_;
        memInfo_ = other.memInfo_;
        allocator_ = other.allocator_;
        inputName_ = std::move(other.inputName_);
        outputName_ = std::move(other.outputName_);
        other.env_ = nullptr;
        other.session_ = nullptr;
        other.memInfo_ = nullptr;
        other.allocator_ = nullptr;
    }
    return *this;
}

void OnnxSession::releaseAll() {
    const OrtApi* api = g_runtime.api;
    if (!api) return;
    if (session_) api->ReleaseSession(session_);
    if (env_) api->ReleaseEnv(env_);
    if (memInfo_) api->ReleaseMemoryInfo(memInfo_);
    // allocator_ 是运行时默认分配器，属 runtime 所有，不能 ReleaseAllocator
    session_ = nullptr;
    env_ = nullptr;
    memInfo_ = nullptr;
    allocator_ = nullptr;
}

bool OnnxSession::load(const std::string& modelPath, std::string& err,
                       int intraOpThreads) {
    const OrtApi* api = g_runtime.api;
    if (!api) {
        err = g_runtime.err;
        return false;
    }
    OrtEnv* env = nullptr;
    OrtStatus* st = api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "ppocr_onnx", &env);
    if (st) { err = statusMessage(api, st); return false; }
    env_ = env;

    OrtSessionOptions* opts = nullptr;
    st = api->CreateSessionOptions(&opts);
    if (st) { err = statusMessage(api, st); return false; }
    if (intraOpThreads > 0) {
        st = api->SetIntraOpNumThreads(opts, intraOpThreads);
        if (st) { err = statusMessage(api, st); api->ReleaseSessionOptions(opts); return false; }
    }
    OrtMemoryInfo* memInfo = nullptr;
    st = api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memInfo);
    if (st) { err = statusMessage(api, st); api->ReleaseSessionOptions(opts); return false; }
    memInfo_ = memInfo;
    OrtSession* session = nullptr;
    const std::wstring wpath = toWide(modelPath);
    st = api->CreateSession(env_, wpath.c_str(), opts, &session);
    api->ReleaseSessionOptions(opts);
    if (st) { err = "CreateSession(" + modelPath + ") failed: " + statusMessage(api, st); return false; }
    session_ = session;

    // 取输入/输出名(复制进 std::string 后立即释放)
    OrtAllocator* alloc = nullptr;
    st = api->GetAllocatorWithDefaultOptions(&alloc);
    if (st) { err = statusMessage(api, st); return false; }
    allocator_ = alloc;

    char* name = nullptr;
    size_t nameLen = 0;
    st = api->SessionGetInputName(session_, 0, alloc, &name);
    if (st) { err = statusMessage(api, st); return false; }
    inputName_.assign(name, strnlen(name, 512));
    api->AllocatorFree(alloc, name);

    st = api->SessionGetOutputName(session_, 0, alloc, &name);
    if (st) { err = statusMessage(api, st); return false; }
    outputName_.assign(name, strnlen(name, 512));
    api->AllocatorFree(alloc, name);
    return true;
}

bool OnnxSession::inputShape(std::vector<int64_t>& dims, std::string& err) const {
    const OrtApi* api = g_runtime.api;
    if (!session_) { err = "session not loaded"; return false; }
    OrtTypeInfo* typeInfo = nullptr;
    OrtStatus* st = api->SessionGetInputTypeInfo(session_, 0, &typeInfo);
    if (st) { err = statusMessage(api, st); return false; }
    const OrtTensorTypeAndShapeInfo* tensorInfo = nullptr;
    st = api->CastTypeInfoToTensorInfo(typeInfo, &tensorInfo);
    if (st) { err = statusMessage(api, st); api->ReleaseTypeInfo(typeInfo); return false; }
    size_t numDims = 0;
    st = api->GetDimensionsCount(tensorInfo, &numDims);
    if (st) { err = statusMessage(api, st); api->ReleaseTypeInfo(typeInfo); return false; }
    dims.resize(numDims);
    st = api->GetDimensions(tensorInfo, dims.data(), numDims);
    api->ReleaseTypeInfo(typeInfo);
    if (st) { err = statusMessage(api, st); return false; }
    return true;
}

bool OnnxSession::run(const std::vector<int64_t>& inShape, const float* inputData,
                      std::vector<int64_t>& outShape, std::vector<float>& outData,
                      std::string& err) {
    const OrtApi* api = g_runtime.api;
    if (!session_ || !memInfo_) { err = "session not loaded"; return false; }

    OrtValue* inVal = nullptr;
    OrtStatus* st = api->CreateTensorWithDataAsOrtValue(
        memInfo_, const_cast<float*>(inputData),
        elementCount(inShape) * sizeof(float), inShape.data(), inShape.size(),
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inVal);
    if (st) { err = statusMessage(api, st); return false; }

    const char* inputNames[] = {inputName_.c_str()};
    const char* outputNames[] = {outputName_.c_str()};
    OrtValue* outVal = nullptr;
    st = api->Run(session_, nullptr, inputNames, &inVal, 1, outputNames, 1, &outVal);
    api->ReleaseValue(inVal);
    if (st) { err = statusMessage(api, st); return false; }

    // 输出张量信息
    OrtTensorTypeAndShapeInfo* info = nullptr;
    st = api->GetTensorTypeAndShape(outVal, &info);
    if (st) { err = statusMessage(api, st); api->ReleaseValue(outVal); return false; }
    size_t numDims = 0;
    api->GetDimensionsCount(info, &numDims);
    outShape.resize(numDims);
    api->GetDimensions(info, outShape.data(), numDims);

    void* raw = nullptr;
    st = api->GetTensorMutableData(outVal, &raw);
    if (st) { err = statusMessage(api, st); api->ReleaseTensorTypeAndShapeInfo(info); api->ReleaseValue(outVal); return false; }
    const size_t count = elementCount(outShape);
    outData.assign(static_cast<float*>(raw), static_cast<float*>(raw) + count);

    api->ReleaseTensorTypeAndShapeInfo(info);
    api->ReleaseValue(outVal);
    return true;
}

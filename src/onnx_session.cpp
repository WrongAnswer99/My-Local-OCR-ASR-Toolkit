#include "onnx_session.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <onnxruntime_c_api.h>

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <mutex>

namespace {

// 延迟加载 onnxruntime.dll，符号表仅有一个入口：OrtGetApiBase
struct RuntimeHandle {
    HMODULE dll = nullptr;
    const OrtApi* api = nullptr;
    bool ok = false;
    std::string err;
    std::once_flag once;
    std::once_flag cudaOnce;
    std::wstring runtimeDir;
    std::string cudaError;
    std::vector<HMODULE> cudaLibraries;  // Pinned like the process-wide ORT DLL.

    bool init() {
        std::call_once(once, [this] { initialize(); });
        return ok;
    }

    bool initialize() {
        if (ok) return true;
        if (err.empty() && dll == nullptr) {
            // Prefer the runtime beside this module (ocr.dll for SDK users).
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&gModuleAnchor), &self);
            wchar_t modulePath[32768] = {};
            const DWORD n = GetModuleFileNameW(self, modulePath, 32768);
            std::wstring runtimePath(modulePath, n);
            const auto slash = runtimePath.find_last_of(L"\\/");
            runtimePath = runtimePath.substr(0, slash + 1) + L"onnxruntime.dll";
            if (GetFileAttributesW(runtimePath.c_str()) != INVALID_FILE_ATTRIBUTES)
                dll = LoadLibraryExW(runtimePath.c_str(), nullptr,
                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            else
                dll = LoadLibraryA("onnxruntime.dll");
            if (!dll) {
                err = "LoadLibrary onnxruntime.dll failed: error code " +
                      std::to_string(GetLastError()) +
                      " (请确认 dll 与可执行文件同目录)";
                return false;
            }
            const DWORD loadedLen = GetModuleFileNameW(dll, modulePath, 32768);
            runtimeDir.assign(modulePath, loadedLen);
            runtimeDir = runtimeDir.substr(0, runtimeDir.find_last_of(L"\\/") + 1);
        }
        auto getApiBase =
            (const OrtApiBase* (*)())GetProcAddress(dll, "OrtGetApiBase");
        if (!getApiBase) {
            err = "GetProcAddress(OrtGetApiBase) failed";
            return false;
        }
        // CUDA V2 needs API 12; only use this API prefix so newer headers can
        // also run with older released CPU/GPU DLLs (ORT >= 1.12).
        api = getApiBase()->GetApi(12);
        if (!api) {
            err = "ONNX Runtime >= 1.12 is required (C API 12 unavailable)";
            return false;
        }
        ok = true;
        return true;
    }
    bool preloadCudaLibraries(std::string& error) {
        std::call_once(cudaOnce, [this] {
            // ORT and cuDNN load some dependencies by name. Preload bundled
            // libraries by absolute path so SDK hosts in another directory do
            // not accidentally pick a different cuDNN/CUDA from PATH.
            const wchar_t* patterns[] = {L"cudart64_*.dll", L"cublasLt64_*.dll",
                L"cublas64_*.dll", L"cufft64_*.dll", L"nvrtc*.dll", L"cudnn*.dll"};
            for (const auto pattern : patterns) {
                WIN32_FIND_DATAW file = {};
                HANDLE search = FindFirstFileW((runtimeDir + pattern).c_str(), &file);
                if (search == INVALID_HANDLE_VALUE) continue;
                do {
                    if (file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    const auto fullPath = runtimeDir + file.cFileName;
                    HMODULE library = LoadLibraryExW(fullPath.c_str(), nullptr,
                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                    if (!library) {
                        const DWORD code = GetLastError();
                        const int len = WideCharToMultiByte(CP_UTF8, 0, file.cFileName,
                            -1, nullptr, 0, nullptr, nullptr);
                        std::string name(size_t(len), '\0');
                        WideCharToMultiByte(CP_UTF8, 0, file.cFileName, -1,
                            name.data(), len, nullptr, nullptr);
                        if (!name.empty()) name.pop_back();
                        cudaError = "Cannot load bundled CUDA dependency " + name +
                                    " (Windows error " + std::to_string(code) + ")";
                        break;
                    }
                    cudaLibraries.push_back(library);
                } while (FindNextFileW(search, &file));
                FindClose(search);
                if (!cudaError.empty()) break;
            }
        });
        error = cudaError;
        return error.empty();
    }
    static void gModuleAnchor() {}
};
RuntimeHandle g_runtime;

// 封装错误消息格式转换
std::string statusMessage(const OrtApi* api, OrtStatus* status) {
    if (!status) return "unknown error";
    const char* msg = api->GetErrorMessage(status);
    const std::string result = msg ? std::string(msg) : "unknown error";
    api->ReleaseStatus(status);
    return result;
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
      outputName_(std::move(other.outputName_)), profiling_(other.profiling_) {
    other.profiling_ = false;
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
        profiling_ = other.profiling_;
        other.profiling_ = false;
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
    if (session_ && profiling_ && allocator_) {
        char* filename = nullptr;
        OrtStatus* status = api->SessionEndProfiling(session_, allocator_, &filename);
        if (status) {
            std::fprintf(stderr, "[profile] %s\n", api->GetErrorMessage(status));
            api->ReleaseStatus(status);
        } else if (filename) {
            std::fprintf(stderr, "[profile] %s\n", filename);
            status = api->AllocatorFree(allocator_, filename);
            if (status) api->ReleaseStatus(status);
        }
    }
    profiling_ = false;
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
                       int intraOpThreads, OCRDevice device, int gpuDeviceId) {
    releaseAll();
    err.clear();
    if (device != OCRDevice::CPU && device != OCRDevice::CUDA) {
        err = "invalid OCR execution device";
        return false;
    }
    if (gpuDeviceId < 0) { err = "GPU device ID must be >= 0"; return false; }
    if (!ensureInitialized(err)) return false;
    if (loadImpl(modelPath, err, intraOpThreads, device, gpuDeviceId)) return true;
    releaseAll();
    return false;
}

bool OnnxSession::loadImpl(const std::string& modelPath, std::string& err,
                           int intraOpThreads, OCRDevice device, int gpuDeviceId) {
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
    if (const char* prefix = std::getenv("OCR_PROFILE_PREFIX")) {
        if (*prefix) {
            const auto slash = modelPath.find_last_of("\\/");
            const auto name = modelPath.substr(slash == std::string::npos ? 0 : slash + 1);
            const auto widePrefix = toWide(std::string(prefix) + "_" + name);
            st = api->EnableProfiling(opts, widePrefix.c_str());
            if (st) { err = statusMessage(api, st); api->ReleaseSessionOptions(opts); return false; }
            profiling_ = true;
        }
    }
    if (intraOpThreads > 0) {
        st = api->SetIntraOpNumThreads(opts, intraOpThreads);
        if (st) { err = statusMessage(api, st); api->ReleaseSessionOptions(opts); return false; }
    }
    if (device == OCRDevice::CUDA) {
        // Check the runtime package first: a CPU DLL must never silently
        // turn an explicit GPU request into a CPU-only session.
        char** providers = nullptr;
        int count = 0;
        st = api->GetAvailableProviders(&providers, &count);
        if (st) {
            err = statusMessage(api, st);
            api->ReleaseSessionOptions(opts);
            return false;
        }
        bool hasCuda = false;
        for (int i = 0; i < count; ++i)
            if (std::strcmp(providers[i], "CUDAExecutionProvider") == 0) hasCuda = true;
        st = api->ReleaseAvailableProviders(providers, count);
        if (st) {
            err = statusMessage(api, st);
            api->ReleaseSessionOptions(opts);
            return false;
        }
        if (!hasCuda) {
            err = "CUDAExecutionProvider unavailable: use the ONNX Runtime GPU package, "
                  "including onnxruntime_providers_cuda.dll and onnxruntime_providers_shared.dll";
            api->ReleaseSessionOptions(opts);
            return false;
        }
        if (!g_runtime.preloadCudaLibraries(err)) {
            api->ReleaseSessionOptions(opts);
            return false;
        }
        OrtCUDAProviderOptionsV2* cuda = nullptr;
        st = api->CreateCUDAProviderOptions(&cuda);
        const std::string id = std::to_string(gpuDeviceId);
        const char* keys[] = {"device_id"};
        const char* values[] = {id.c_str()};
        if (!st) st = api->UpdateCUDAProviderOptions(cuda, keys, values, 1);
        if (!st) st = api->SessionOptionsAppendExecutionProvider_CUDA_V2(opts, cuda);
        if (cuda) api->ReleaseCUDAProviderOptions(cuda);
        if (st) {
            err = "CUDA initialization failed (device " + id + "): " + statusMessage(api, st) +
                  "; check NVIDIA driver, matching CUDA/cuDNN DLLs and PATH";
            api->ReleaseSessionOptions(opts);
            return false;
        }
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

#pragma once
#include <stdint.h>

#ifdef _WIN32
#  ifdef ASR_BUILD_DLL
#    define ASR_API __declspec(dllexport)
#  else
#    define ASR_API __declspec(dllimport)
#  endif
#else
#  define ASR_API
#endif
#ifdef __cplusplus
extern "C" {
#endif

typedef void* AsrHandle;
typedef void* AsrResult;
typedef struct AsrOptions {
    uint32_t struct_size;
    int32_t threads;           /* 0 = auto, capped at 8; otherwise 1..256 */
    const char* model;         /* UTF-8; NULL = models/whisper/ggml-small-q5_1.bin beside DLL */
    const char* language;      /* auto (default), zh, en, ... */
    int32_t device;            /* 0 = CPU (default); 1 = CUDA, requires CUDA SDK, no fallback */
    int32_t reserved;
} AsrOptions;

ASR_API const char* asr_version(void);
ASR_API void asr_default_options(AsrOptions* options);
ASR_API AsrHandle asr_create(const AsrOptions* options, char* error, int error_capacity);
ASR_API void asr_destroy(AsrHandle handle);
/* Actual initialized device: 0 CPU, 1 CUDA, -1 invalid handle. */
ASR_API int asr_device(AsrHandle handle);
/* Synchronous callback on the calling thread; percent 0..100, monotonic.
   Callback must not throw or reenter/destroy this engine. 100 means success. */
typedef void (*AsrProgressFn)(int percent, void* user);
/* Blocking full-file transcription. Result is independent of handle lifetime.
   NULL on failure. All strings are UTF-8; no fixed transcript length limit. */
ASR_API AsrResult asr_transcribe_file(AsrHandle handle, const char* path, char* error, int error_capacity);
ASR_API AsrResult asr_transcribe_file_with_progress(AsrHandle handle, const char* path,
    AsrProgressFn callback, void* user, char* error, int error_capacity);
ASR_API void asr_free_result(AsrResult result);
/* Borrowed strings remain valid until asr_free_result. NULL result -> empty strings/zero. */
ASR_API const char* asr_result_text(AsrResult result);
ASR_API const char* asr_result_language(AsrResult result);
/* Malformed UTF-8 sequences replaced with U+FFFD; valid text is preserved. */
ASR_API int asr_result_utf8_replacements(AsrResult result);
ASR_API int asr_result_segment_count(AsrResult result);
ASR_API const char* asr_result_segment_text(AsrResult result, int index);
ASR_API int64_t asr_result_segment_start_ms(AsrResult result, int index);
ASR_API int64_t asr_result_segment_end_ms(AsrResult result, int index);
ASR_API double asr_result_audio_ms(AsrResult result);
ASR_API double asr_result_decode_ms(AsrResult result);
ASR_API double asr_result_transcribe_ms(AsrResult result);

#ifdef __cplusplus
}
#endif

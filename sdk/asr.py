"""Local Windows Whisper SDK. Only inference and result copying; no video/FFmpeg."""
import ctypes as C
from pathlib import Path
from time import perf_counter

class _Options(C.Structure):
    _fields_ = [("struct_size", C.c_uint32), ("threads", C.c_int32),
                ("model", C.c_char_p), ("language", C.c_char_p),
                ("device", C.c_int32), ("reserved", C.c_int32)]

_Progress = C.CFUNCTYPE(None, C.c_int, C.c_void_p)

class Engine:
    """Reusable full-file ASR engine. Use as a context manager; calls are blocking."""
    def __init__(self, directory=None, *, device="cpu", language="auto", threads=0, model=None):
        if device not in ("cpu", "gpu"):
            raise ValueError("device must be cpu or gpu")
        self.directory = Path(directory or Path(__file__).resolve().parent).resolve()
        self._handle = None
        self._dll = C.CDLL(str(self.directory / "asr.dll"))
        signatures = {
            "asr_version": ([], C.c_char_p),
            "asr_default_options": ([C.POINTER(_Options)], None),
            "asr_create": ([C.POINTER(_Options), C.c_char_p, C.c_int], C.c_void_p),
            "asr_destroy": ([C.c_void_p], None),
            "asr_device": ([C.c_void_p], C.c_int),
            "asr_transcribe_file_with_progress": ([C.c_void_p, C.c_char_p, _Progress, C.c_void_p, C.c_char_p, C.c_int], C.c_void_p),
            "asr_free_result": ([C.c_void_p], None),
        }
        for name in ("text", "language"):
            signatures["asr_result_" + name] = ([C.c_void_p], C.c_char_p)
        signatures["asr_result_utf8_replacements"] = ([C.c_void_p], C.c_int)
        signatures["asr_result_segment_count"] = ([C.c_void_p], C.c_int)
        signatures["asr_result_segment_text"] = ([C.c_void_p, C.c_int], C.c_char_p)
        for name in ("start_ms", "end_ms"):
            signatures["asr_result_segment_" + name] = ([C.c_void_p, C.c_int], C.c_int64)
        for name in ("audio_ms", "decode_ms", "transcribe_ms"):
            signatures["asr_result_" + name] = ([C.c_void_p], C.c_double)
        for name, (arguments, result) in signatures.items():
            function = getattr(self._dll, name)
            function.argtypes, function.restype = arguments, result
        options = _Options()
        self._dll.asr_default_options(C.byref(options))
        options.device = 1 if device == "gpu" else 0
        options.threads = threads
        options.language = language.encode("utf-8")
        options.model = str(Path(model).resolve()).encode("utf-8") if model else None
        error = C.create_string_buffer(4096)
        start = perf_counter()
        self._handle = self._dll.asr_create(C.byref(options), error, len(error))
        self.init_ms = (perf_counter() - start) * 1000
        if not self._handle:
            raise RuntimeError("ASR initialization failed: " + error.value.decode("utf-8", "replace"))
        self.device = "gpu" if self._dll.asr_device(self._handle) == 1 else "cpu"
        if self.device != device:
            self.close()
            raise RuntimeError("ASR device mismatch; CPU fallback rejected")

    @property
    def version(self):
        return self._dll.asr_version().decode("utf-8")

    def transcribe_file(self, audio, progress=None):
        if not self._handle:
            raise RuntimeError("ASR engine is closed")
        callback_errors = []
        def notify(percent, _):
            if progress and not callback_errors:
                try:
                    progress(percent)
                except BaseException as error:
                    callback_errors.append(error)
        callback = _Progress(notify)  # Keep alive throughout the blocking native call.
        error = C.create_string_buffer(4096)
        result = self._dll.asr_transcribe_file_with_progress(
            self._handle, str(Path(audio).resolve()).encode("utf-8"), callback, None, error, len(error))
        try:
            if callback_errors:
                raise callback_errors[0]
            if not result:
                raise RuntimeError("ASR transcription failed: " + error.value.decode("utf-8", "replace"))
            dll = self._dll
            return {
                "text": dll.asr_result_text(result).decode("utf-8"),
                "language": dll.asr_result_language(result).decode("utf-8"),
                "utf8_replacements": dll.asr_result_utf8_replacements(result),
                "device": self.device, "cuda_confirmed": self.device == "gpu",
                "init_ms": self.init_ms,
                **{name: getattr(dll, "asr_result_" + name)(result)
                   for name in ("audio_ms", "decode_ms", "transcribe_ms")},
                "segments": [{"start_ms": dll.asr_result_segment_start_ms(result, i),
                              "end_ms": dll.asr_result_segment_end_ms(result, i),
                              "text": dll.asr_result_segment_text(result, i).decode("utf-8")}
                             for i in range(dll.asr_result_segment_count(result))],
            }
        finally:
            if result:
                self._dll.asr_free_result(result)

    def close(self):
        if getattr(self, "_handle", None):
            self._dll.asr_destroy(self._handle)
            self._handle = None

    def __del__(self):
        self.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

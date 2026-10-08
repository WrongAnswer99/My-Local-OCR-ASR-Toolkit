"""Real Whisper C ABI lifecycle/error checks. Development-only; dist uses PowerShell."""
import argparse
import ctypes as C
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]


class Options(C.Structure):
    _fields_ = [("struct_size", C.c_uint32), ("threads", C.c_int32),
                ("model", C.c_char_p), ("language", C.c_char_p),
                ("device", C.c_int32), ("reserved", C.c_int32)]


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin-dir", type=Path, required=True)
    args = parser.parse_args()
    bindir = args.bin_dir.resolve()
    dll = C.CDLL(str(bindir / "asr.dll"))
    dll.asr_default_options.argtypes = [C.POINTER(Options)]
    dll.asr_create.argtypes = [C.POINTER(Options), C.c_char_p, C.c_int]
    dll.asr_create.restype = C.c_void_p
    dll.asr_destroy.argtypes = [C.c_void_p]
    dll.asr_transcribe_file.argtypes = [C.c_void_p, C.c_char_p, C.c_char_p, C.c_int]
    dll.asr_transcribe_file.restype = C.c_void_p
    dll.asr_free_result.argtypes = [C.c_void_p]
    dll.asr_result_text.argtypes = [C.c_void_p]
    dll.asr_result_text.restype = C.c_char_p
    dll.asr_result_segment_count.argtypes = [C.c_void_p]
    dll.asr_result_segment_count.restype = C.c_int
    error = C.create_string_buffer(2048)

    def options():
        value = Options()
        dll.asr_default_options(C.byref(value))
        check(value.struct_size == C.sizeof(Options), "Options ABI size mismatch")
        return value

    for field, value, expected in [("struct_size", 0, b"struct_size"),
                                   ("device", 1, b"CPU only"),
                                   ("threads", -1, b"threads"),
                                   ("language", b"invalid-language", b"language"),
                                   ("model", b"does-not-exist.bin", b"model")]:
        opt = options()
        setattr(opt, field, value)
        handle = dll.asr_create(C.byref(opt), error, len(error))
        if handle:
            dll.asr_destroy(handle)
            raise AssertionError(f"Invalid {field} unexpectedly initialized")
        check(expected in error.value, f"Wrong {field} error: {error.value!r}")
    check(not dll.asr_transcribe_file(None, b"file.wav", error, len(error)), "Null engine accepted")
    check(b"null" in error.value, "Null-engine error missing")

    model = ROOT / "models/whisper/ggml-small-q5_1.bin"
    opt = options()
    opt.model = str(model).encode("utf-8")
    opt.language = b"en"
    opt.threads = 8
    handle = dll.asr_create(C.byref(opt), error, len(error))
    check(handle, f"Create failed: {error.value!r}")
    results = []
    try:
        check(not dll.asr_transcribe_file(handle, b"missing.wav", error, len(error)), "Missing file accepted")
        fixture = str(ROOT / "tests/audio/jfk.wav").encode("utf-8")
        first = dll.asr_transcribe_file(handle, fixture, error, len(error))
        check(first, f"Transcription failed: {error.value!r}")
        results.append(first)
        first_text = dll.asr_result_text(first)
        check(b"country" in first_text.lower(), f"Expected JFK text missing: {first_text!r}")
        second = dll.asr_transcribe_file(handle, fixture, error, len(error))
        check(second, f"Repeated transcription failed: {error.value!r}")
        results.append(second)
        check(dll.asr_result_text(first) == first_text, "Prior result changed after engine reuse")
        check(dll.asr_result_text(second) == first_text, "Repeated transcript changed")
        dll.asr_destroy(handle)
        handle = None
        check(dll.asr_result_text(first) == first_text, "Result did not survive engine destruction")
        check(dll.asr_result_segment_count(first) > 0, "No segments")
    finally:
        for result in results:
            dll.asr_free_result(result)
        if handle:
            dll.asr_destroy(handle)

    # CLI must reject malformed options and unsafe output before loading a model.
    cli = str(bindir / "asr_cli.exe")
    fixture = str(ROOT / "tests/audio/jfk.wav")
    for option in ["--threads=-1", "--threads=4junk", "--threads=257", "--model=", "--language=", "--unknown"]:
        process = subprocess.run([cli, fixture, option], capture_output=True, timeout=10)
        check(process.returncode == 1, f"Bad CLI option accepted: {option}")
    process = subprocess.run([cli, fixture, "--output=" + fixture], capture_output=True, timeout=10)
    check(process.returncode == 1 and b"overwrite" in process.stderr, "CLI allowed overwriting input")
    process = subprocess.run([cli, "--help"], capture_output=True, timeout=10)
    check(process.returncode == 0 and b"--language" in process.stdout, "CLI help failed")
    print("Whisper C ABI lifecycle, repeatability and CLI error checks passed")


if __name__ == "__main__":
    main()

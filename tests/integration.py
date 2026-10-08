"""Real runtime/model integration checks; CUDA builds require a working GPU."""
import argparse
import ctypes as C
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]
IMAGE = ROOT / "images/ch_en_num.jpg"


class Options(C.Structure):
    _fields_ = [
        ("struct_size", C.c_uint32), ("device", C.c_int32), ("gpu_device_id", C.c_int32),
        ("det_model", C.c_char_p), ("rec_model", C.c_char_p), ("dict", C.c_char_p),
        ("norm", C.c_int32), ("threads", C.c_int32), ("det_limit", C.c_int32),
        ("text_score", C.c_double),
    ]


class Line(C.Structure):
    _fields_ = [
        ("text", C.c_char * 512), ("score", C.c_double),
        ("quad_x", C.c_double * 4), ("quad_y", C.c_double * 4),
        ("cx", C.c_int), ("cy", C.c_int), ("w", C.c_int), ("h", C.c_int),
    ]


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def run(bindir, name, args, code=0, contains=None):
    p = subprocess.run([str(bindir / (name + ".exe")), *args], cwd=ROOT,
                       capture_output=True, timeout=60)
    output = (p.stdout + p.stderr).decode("utf-8", errors="replace")
    check(p.returncode == code, f"{name} returned {p.returncode}, expected {code}: {output}")
    if contains:
        check(contains in output, f"{name}: expected {contains!r}: {output}")
    return output


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin-dir", type=Path, required=True)
    parser.add_argument("--runtime", choices=["cpu", "cuda"], required=True)
    args = parser.parse_args()
    bindir = args.bin_dir.resolve()
    check(IMAGE.is_file(), f"missing test fixture: {IMAGE}")
    for name in ["ppocr_onnx", "ocrwatch", "ocrmon", "api_demo"]:
        run(bindir, name, ["--help"], contains="--device")
    for name, prefix in [("ppocr_onnx", [str(IMAGE)]),
                         ("ocrwatch", ["--key=test"]), ("ocrmon", []),
                         ("api_demo", ["file", str(IMAGE)])]:
        for option in ["--device=bogus", "--gpu-device=-1", "--gpu-device=0junk", "--gpu-device"]:
            run(bindir, name, [*prefix, option], code=1, contains="[args]")
    run(bindir, "ppocr_onnx", [str(IMAGE), "--threads=4"], contains="backend: cpu")
    run(bindir, "api_demo", ["file", str(IMAGE)], contains="detected 13 line(s)")
    run(bindir, "ocrwatch", [f"--image={IMAGE}", "--key=正品促销", "--max-frames=1",
                             "--threads=4", "--device=cpu"], contains="backend: cpu")

    dll = C.CDLL(str(bindir / "ocr.dll"))
    dll.ocr_default_options.argtypes = [C.POINTER(Options)]
    dll.ocr_default_options.restype = None
    dll.ocr_create_ex.argtypes = [C.POINTER(Options), C.c_char_p, C.c_int]
    dll.ocr_create_ex.restype = C.c_void_p
    dll.ocr_create.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p, C.c_int, C.c_int,
                               C.c_int, C.c_double, C.c_char_p, C.c_int]
    dll.ocr_create.restype = C.c_void_p
    dll.ocr_destroy.argtypes = [C.c_void_p]
    dll.ocr_destroy.restype = None
    dll.ocr_run_file.argtypes = [C.c_void_p, C.c_char_p, C.POINTER(Line), C.c_int,
                                 C.c_char_p, C.c_int]
    dll.ocr_run_file.restype = C.c_int
    dll.ocr_run_bgr.argtypes = [C.c_void_p, C.POINTER(C.c_uint8), C.c_int, C.c_int,
                                C.POINTER(Line), C.c_int, C.c_char_p, C.c_int]
    dll.ocr_run_bgr.restype = C.c_int
    err = C.create_string_buffer(2048)

    def options(device=0):
        opt = Options()
        dll.ocr_default_options(C.byref(opt))
        check(opt.struct_size == C.sizeof(Options) and opt.norm == 2 and opt.device == 0,
              "option defaults/ABI layout mismatch")
        opt.threads = 4
        opt.device = device
        return opt

    def create(opt):
        return dll.ocr_create_ex(C.byref(opt), err, len(err))

    def recognize(handle):
        lines = (Line * 256)()
        n = dll.ocr_run_file(handle, str(IMAGE).encode("utf-8"), lines, len(lines), err, len(err))
        check(n == 13, f"unexpected OCR results: {n}: {err.value!r}")
        result = {lines[i].text.decode("utf-8"): lines[i].score for i in range(n)}
        check("正品促销" in result, "expected fixture text missing")
        return result

    check(not dll.ocr_create_ex(None, err, len(err)), "NULL options accepted")
    for attr, value in [("struct_size", 0), ("device", 99), ("gpu_device_id", -1)]:
        opt = options()
        setattr(opt, attr, value)
        check(not create(opt) and err.value, f"invalid {attr} accepted")
    opt = options()
    opt.det_model = b"missing-model.onnx"
    check(not create(opt) and b"CreateSession" in err.value, "missing model diagnostic lost")

    old = dll.ocr_create(None, None, None, 2, 4, 0, 0.5, err, len(err))
    check(old, f"legacy CPU API failed: {err.value!r}")
    cpu = create(options())
    check(cpu and not err.value, f"new CPU API failed or stale error: {err.value!r}")
    try:
        baseline = recognize(old)
        check(recognize(cpu) == baseline, "legacy and extended CPU APIs differ")
        # A successful BGR call validates the common memory-input path.
        pixels = (C.c_uint8 * (32 * 32 * 3))(*([255] * (32 * 32 * 3)))
        lines = (Line * 8)()
        check(dll.ocr_run_bgr(cpu, pixels, 32, 32, lines, 8, err, len(err)) == 0,
              f"blank BGR inference failed: {err.value!r}")
        if args.runtime == "cpu":
            check(not create(options(1)) and b"CUDAExecutionProvider unavailable" in err.value,
                  f"CPU runtime silently accepted CUDA: {err.value!r}")
            run(bindir, "ppocr_onnx", [str(IMAGE), "--device=cuda"], code=2,
                contains="CUDAExecutionProvider unavailable")
            run(bindir, "gpu_example", [str(IMAGE)], code=2,
                contains="CUDAExecutionProvider unavailable")
        else:
            gpu = create(options(1))
            check(gpu, f"CUDA initialization failed: {err.value!r}")
            try:
                # SDK host (Python) lives outside the DLL directory. Bundled
                # cuDNN/NVRTC must win over other software's PATH entries.
                kernel = C.WinDLL("kernel32", use_last_error=True)
                kernel.GetModuleHandleW.argtypes = [C.c_wchar_p]
                kernel.GetModuleHandleW.restype = C.c_void_p
                kernel.GetModuleFileNameW.argtypes = [C.c_void_p, C.c_wchar_p, C.c_uint32]
                kernel.GetModuleFileNameW.restype = C.c_uint32
                for name in ["cudnn64_9.dll", "nvrtc64_120_0.dll"]:
                    if (bindir / name).is_file():
                        module = kernel.GetModuleHandleW(name)
                        location = C.create_unicode_buffer(32768)
                        check(module and kernel.GetModuleFileNameW(module, location, len(location)),
                              f"bundled {name} was not loaded")
                        check(Path(location.value).resolve() == (bindir / name).resolve(),
                              f"wrong CUDA dependency loaded: {location.value}")
                result = recognize(gpu)
                check(result.keys() == baseline.keys(), "CPU/GPU fixture texts differ")
                check(all(abs(result[text] - baseline[text]) < 0.03 for text in result),
                      "CPU/GPU confidence difference exceeds tolerance")
                check(dll.ocr_run_bgr(gpu, pixels, 32, 32, lines, 8, err, len(err)) == 0,
                      f"CUDA BGR inference failed: {err.value!r}")
            finally:
                dll.ocr_destroy(gpu)
            bad = options(1)
            bad.gpu_device_id = 99999
            check(not create(bad) and err.value, "nonexistent GPU unexpectedly accepted")
            run(bindir, "ppocr_onnx", [str(IMAGE), "--device=gpu"], contains="backend: cuda")
            run(bindir, "api_demo", ["file", str(IMAGE), "--device=cuda"],
                contains="detected 13 line(s)")
            run(bindir, "gpu_example", [str(IMAGE)], contains="正品促销")
            run(bindir, "ocrwatch", [f"--image={IMAGE}", "--key=正品促销", "--max-frames=1",
                                     "--device=cuda"], contains="backend: cuda")
        check(recognize(cpu) == baseline, "GPU initialization/failure damaged CPU session")
    finally:
        dll.ocr_destroy(old)
        dll.ocr_destroy(cpu)
    print(f"PASS: {args.runtime} CLI, C ABI, C++ SDK, file/BGR inference and failure paths")


if __name__ == "__main__":
    main()

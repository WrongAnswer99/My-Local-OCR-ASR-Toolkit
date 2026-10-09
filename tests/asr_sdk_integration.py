"""Exercise the exported ASR SDK: actual device, reuse, progress, CPU/GPU equality."""
import argparse
import importlib.util
import json
from pathlib import Path
import re
from time import perf_counter

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dist-dir", type=Path, required=True)
    parser.add_argument("--audio", type=Path, required=True)
    parser.add_argument("--language", default="zh")
    parser.add_argument("--expected", type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    dist = args.dist_dir.resolve()
    spec = importlib.util.spec_from_file_location("exported_asr", dist / "asr.py")
    sdk = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sdk)
    records = {}
    for device in ("cpu", "gpu"):
        start = perf_counter()
        with sdk.Engine(dist, device=device, language=args.language, threads=8) as engine:
            assert engine.device == device
            progress = []
            result = engine.transcribe_file(args.audio, progress=progress.append)
            assert progress and progress[0] == 0 and progress[-1] == 100
            assert all(0 <= n <= 100 for n in progress)
            assert all(a < b for a, b in zip(progress, progress[1:])), progress
            result["wall_ms"] = (perf_counter() - start) * 1000
            assert result["audio_ms"] > 0 and result["segments"]
            assert all(0 <= s["start_ms"] <= s["end_ms"] <= result["audio_ms"] for s in result["segments"])
            if device == "gpu":
                # Reuse the same context, and copy results before freeing native buffers.
                repeated = engine.transcribe_file(args.audio)
                assert repeated["text"] == result["text"], "Reused engine changed text"
            records[device] = result
        assert result["text"], "Result must survive engine destruction"
        try:
            engine.transcribe_file(args.audio)
        except RuntimeError as error:
            assert "closed" in str(error)
        else:
            raise AssertionError("Closed engine accepted transcription")
    assert records["cpu"]["text"] == records["gpu"]["text"], "CPU/GPU text differs"
    if args.expected:
        normalized = lambda text: re.sub(r"[^\w]", "", text)
        assert normalized(records["gpu"]["text"]) == normalized(args.expected.read_text(encoding="utf-8-sig"))
    records["text_equal"] = True
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(records, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({device: {name: records[device][name] for name in
                               ("init_ms", "decode_ms", "transcribe_ms", "wall_ms", "device", "cuda_confirmed")}
                      for device in ("cpu", "gpu")}, ensure_ascii=False))
    print("CPU/GPU SDK equality, lifetime, progress, and GPU engine reuse passed")

if __name__ == "__main__":
    main()

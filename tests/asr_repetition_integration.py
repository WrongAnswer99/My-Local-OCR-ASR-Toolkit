"""Real offline ASR regression: speech/silence reuse, original timestamps and repetition."""
import argparse
import importlib.util
import json
from pathlib import Path
import tempfile
import shutil
import subprocess
import wave


def longest_run(segments):
    best = current = 0
    previous = None
    for segment in segments:
        text = segment["text"].strip()
        current = current + 1 if text == previous else 1
        best = max(best, current)
        previous = text
    return best


def check_result(result, duration):
    assert abs(result["audio_ms"] - duration) < 2, (result["audio_ms"], duration)
    assert result["text"] == "".join(item["text"] for item in result["segments"])
    assert result["utf8_replacements"] == 0
    assert all(0 <= item["start_ms"] <= item["end_ms"] <= duration for item in result["segments"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dist-dir", type=Path, required=True)
    parser.add_argument("--audio", type=Path, required=True, help="Regression WAV, supplied locally")
    parser.add_argument("--language", default="zh")
    parser.add_argument("--device", choices=("cpu", "gpu"), default="gpu")
    parser.add_argument("--max-run", type=int, default=3)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    dist = args.dist_dir.resolve()
    spec = importlib.util.spec_from_file_location("regression_asr", dist / "asr.py")
    sdk = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sdk)
    with wave.open(str(args.audio), "rb") as source:
        duration = source.getnframes() * 1000 / source.getframerate()
    assert (dist / "models/whisper/ggml-silero-v6.2.0.bin").is_file()
    records = {}
    with sdk.Engine(dist, device=args.device, language=args.language, threads=8) as engine:
        progress = []
        result = engine.transcribe_file(args.audio, progress=progress.append)
        check_result(result, duration)
        assert progress[0] == 0 and progress[-1] == 100
        assert all(a < b for a, b in zip(progress, progress[1:]))
        assert result["segments"] and longest_run(result["segments"]) <= args.max_run
        records["audio"] = result
        records["max_consecutive_equal_segments"] = longest_run(result["segments"])
        with tempfile.TemporaryDirectory() as folder:
            silence = Path(folder) / "silence.wav"
            with wave.open(str(silence), "wb") as out:
                out.setparams((1, 2, 16000, 0, "NONE", "not compressed"))
                out.writeframes(bytes(16000 * 2 * 10))
            empty = engine.transcribe_file(silence)
            check_result(empty, 10000)
            assert not empty["text"] and not empty["segments"], "VAD must not invent speech from silence"
            # After the empty result, reuse the same VAD/Whisper context with speech.
            jfk = Path(__file__).parent / "audio/jfk.wav"
            padded = Path(folder) / "padded.wav"
            with wave.open(str(jfk), "rb") as source:
                params = source.getparams()
                frames = source.readframes(source.getnframes())
            with wave.open(str(padded), "wb") as out:
                out.setparams(params)
                padding = bytes(params.framerate * params.nchannels * params.sampwidth * 10)
                out.writeframes(padding + frames + padding)
            # A separate English engine verifies content as well as source-relative timing.
            with sdk.Engine(dist, device=args.device, language="en", threads=8) as english:
                padded_result = english.transcribe_file(padded)
                assert "country" in padded_result["text"].lower(), padded_result["text"]
                check_result(padded_result, 20000 + params.nframes * 1000 / params.framerate)
                assert min(s["start_ms"] for s in padded_result["segments"]) >= 9500
                assert max(s["end_ms"] for s in padded_result["segments"]) > 15000
                silent_after = english.transcribe_file(silence)
                assert not silent_after["text"], "Previous result must not leak into a silent call"
                speech_again = english.transcribe_file(padded)
                assert "country" in speech_again["text"].lower(), "Silence must not prevent later speech recognition"
                records["padded_speech"] = padded_result
    # The static MinGW VAD loader uses narrow paths; a Unicode CLI directory
    # must still resolve its default speech/VAD models correctly.
    if not (dist / "whisper.dll").exists():
        with tempfile.TemporaryDirectory() as folder:
            unicode_dir = Path(folder) / "中文 SDK"
            models = unicode_dir / "models/whisper"
            models.mkdir(parents=True)
            for name in ("asr.dll", "asr_cli.exe"):
                shutil.copyfile(dist / name, unicode_dir / name)
            for name in ("ggml-small-q5_1.bin", "ggml-silero-v6.2.0.bin"):
                shutil.copyfile(dist / "models/whisper" / name, models / name)
            output = unicode_dir / "result.json"
            completed = subprocess.run([str(unicode_dir / "asr_cli.exe"), str(jfk.resolve()),
                                         "--language=en", "--json=" + str(output)],
                                        capture_output=True, timeout=60)
            assert completed.returncode == 0, completed.stderr.decode("utf-8", "replace")
            assert "country" in json.loads(output.read_text(encoding="utf-8"))["text"].lower()
            records["unicode_cli_directory"] = True
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(records, ensure_ascii=False, indent=2), encoding="utf-8")
    print("PASS: exported SDK repetition, VAD silence, engine reuse, full duration and original timestamps",
          args.device, "max run", records["max_consecutive_equal_segments"])


if __name__ == "__main__":
    main()

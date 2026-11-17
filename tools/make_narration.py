"""Regenerate original Russian comparison narration without recording user audio.

Requires: pip install gTTS; ffmpeg and ffprobe on PATH.
Uses Google Translate TTS through gTTS; only this public script is submitted.
Run: python tools/make_narration.py
Each synthesis process has a 45-second limit, one attempt, and no credentials.
Subtitles are sentence-length estimates fitted to each clip, not forced alignment.
"""
import concurrent.futures
import argparse
import importlib.metadata
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / "media" / "narration"
SCENES = [
    ("intro", .7, 7.3, "Это Zipshot. Скриншоты, аннотации и запись экрана. Сравним его с ShareX."),
    ("method", 8.4, 7.6, "Один компьютер. Одинаковая область. Без курсора. Только захват, без интерфейса и сохранения."),
    ("speed", 16.5, 12.5, "Результат замера виден на экране. Это ускорение захвата на данном компьютере, а не обещание для каждой операции."),
    ("workflow0", 29.4, 3.1, "Дальше — реальный редактор Zipshot."),
    ("workflow1", 33, 3.1, "Выделяем главное рамкой."),
    ("workflow2", 36.8, 3.1, "Добавляем стрелку."),
    ("workflow3", 40.6, 3.1, "Размываем фрагмент."),
    ("workflow4", 44.4, 3.1, "Подписываем. Готово к отправке."),
    ("audit", 48.4, 9.6, "Код проверен. Исправлены прозрачность, перезапись файлов и чтение повреждённых данных. Все тридцать пять проверок пройдены."),
    ("end", 58.4, 9.6, "Код, методика и результаты открыты на GitHub. Zipshot: меньше лишних действий, больше внимания к результату."),
]


def duration(path):
    return float(subprocess.check_output([
        "ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", str(path)
    ], text=True, timeout=15).strip())


def stamp(seconds):
    ms = round(seconds * 1000)
    return f"{ms // 3600000:02}:{ms // 60000 % 60:02}:{ms // 1000 % 60:02},{ms % 1000:03}"


def captions(text, seconds, offset=0):
    sentences = re.split(r"(?<=[.!?])\s+", text)
    total = sum(len(s) for s in sentences)
    elapsed = 0.08
    cues = []
    for sentence in sentences:
        length = (seconds - .20) * len(sentence) / total
        cues.append((offset + elapsed, offset + elapsed + length, sentence))
        elapsed += length
    return cues


def compose(cues):
    return "\n".join(f"{i}\n{stamp(start)} --> {stamp(end)}\n{text}\n" for i, (start, end, text) in enumerate(cues, 1))


def render(scene):
    name, offset, budget, text = scene
    target = DEST / (name + ".mp3")
    raw = ROOT / "build" / "narration" / (name + ".mp3")
    env = dict(os.environ)
    env["PYTHONPATH"] = os.pathsep.join(sys.path)
    code = "from gtts import gTTS; import sys; gTTS(sys.argv[1], lang='ru', lang_check=False, timeout=(8,20)).save(sys.argv[2])"
    subprocess.run([sys.executable, "-c", code, text, str(raw)], check=True, timeout=45, env=env)
    original = duration(raw)
    speed = max(1, original / (budget - .45))
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", str(raw), "-af", f"atempo={speed:.6f}",
                    "-codec:a", "libmp3lame", "-b:a", "128k", str(target)], check=True, timeout=30)
    seconds = duration(target)
    (DEST / (name + ".srt")).write_text(compose(captions(text, seconds)), encoding="utf-8")
    print(f"{name}: {seconds:.3f}s / {budget}s (tempo {speed:.3f})", flush=True)
    return {"name": name, "start_seconds": offset, "duration_seconds": seconds, "budget_seconds": budget,
            "original_seconds": original, "tempo": speed, "text": text}, captions(text, seconds, offset)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--reuse", action="store_true", help="rebuild caption timing/manifest from existing clips without network requests")
    args = parser.parse_args()
    DEST.mkdir(parents=True, exist_ok=True)
    (ROOT / "build" / "narration").mkdir(parents=True, exist_ok=True)
    if args.reuse:
        rendered = []
        for name, offset, budget, text in SCENES:
            seconds = duration(DEST / (name + ".mp3"))
            raw = ROOT / "build" / "narration" / (name + ".mp3")
            original = duration(raw) if raw.exists() else seconds
            (DEST / (name + ".srt")).write_text(compose(captions(text, seconds)), encoding="utf-8")
            rendered.append(({"name": name, "start_seconds": offset, "duration_seconds": seconds,
                "budget_seconds": budget, "original_seconds": original, "effective_tempo": original / seconds,
                "text": text}, captions(text, seconds, offset)))
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
            rendered = list(executor.map(render, SCENES))
    cues = [cue for item in rendered for cue in item[1]]
    (ROOT / "media" / "narration.srt").write_text(compose(cues), encoding="utf-8")
    (DEST / "manifest.json").write_text(json.dumps({"voice": "Google Translate Russian TTS", "engine": "gTTS " + importlib.metadata.version("gTTS"),
        "subtitles": "estimated sentence timing", "clips": [item[0] for item in rendered]}, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()

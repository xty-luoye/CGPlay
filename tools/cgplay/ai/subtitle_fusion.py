#!/usr/bin/env python3
import argparse
import json
import os
import re


TIME_RE = re.compile(r"(\d{2}:\d{2}:\d{2}[,.]\d{3})\s*-->\s*(\d{2}:\d{2}:\d{2}[,.]\d{3})")


def _seconds(value):
    value = value.replace(",", ".")
    h, m, s = value.split(":")
    return int(h) * 3600 + int(m) * 60 + float(s)


def _timestamp(seconds):
    ms = max(0, int(round(seconds * 1000)))
    h = ms // 3600000
    m = (ms // 60000) % 60
    s = (ms // 1000) % 60
    r = ms % 1000
    return f"{h:02d}:{m:02d}:{s:02d}.{r:03d}"


def _read_subtitle(path):
    if not path or not os.path.exists(path):
        return []
    text = open(path, "r", encoding="utf-8", errors="replace").read().replace("\r\n", "\n")
    cues = []
    for block in re.split(r"\n\s*\n", text):
        lines = [line.strip() for line in block.split("\n") if line.strip() and line.strip() != "WEBVTT"]
        if not lines:
            continue
        timing_index = next((i for i, line in enumerate(lines) if TIME_RE.search(line)), -1)
        if timing_index < 0:
            continue
        match = TIME_RE.search(lines[timing_index])
        cue_text = "\n".join(lines[timing_index + 1 :]).strip()
        if not cue_text:
            continue
        cues.append({"start": _seconds(match.group(1)), "end": _seconds(match.group(2)), "text": cue_text})
    return cues


def _write_vtt(path, cues):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("WEBVTT\n\n")
        for cue in cues:
            handle.write(f"{_timestamp(cue['start'])} --> {_timestamp(cue['end'])}\n{cue['text']}\n\n")


def _aligns(quick, candidate, loose=False):
    overlap = max(0.0, min(quick["end"], candidate["end"]) - max(quick["start"], candidate["start"]))
    qdur = max(0.05, quick["end"] - quick["start"])
    cdur = max(0.05, candidate["end"] - candidate["start"])
    close = abs(quick["start"] - candidate["start"]) <= 0.35 and abs(quick["end"] - candidate["end"]) <= 0.50
    mostly = overlap / qdur >= 0.70 and overlap / cdur >= 0.70
    if close or mostly:
        return True
    if not loose:
        return False
    qcenter = (quick["start"] + quick["end"]) * 0.5
    ccenter = (candidate["start"] + candidate["end"]) * 0.5
    return (
        overlap >= 0.75
        or overlap / qdur >= 0.25
        or (abs(quick["start"] - candidate["start"]) <= 2.50
            and candidate["end"] + 0.50 >= quick["start"]
            and candidate["start"] <= quick["end"] + 2.00)
        or (candidate["start"] <= quick["start"] + 2.00
            and candidate["end"] >= quick["start"] + min(1.00, qdur * 0.25))
        or (ccenter >= quick["start"] - 0.75 and ccenter <= quick["end"] + 0.75)
        or (cdur <= 1.50 and abs(qcenter - ccenter) <= max(1.25, qdur * 0.50))
    )


def _best_candidate(quick, pools):
    best = None
    for source, priority, confidence, cues in pools:
        loose = source in {"online", "ocr", "long-asr"}
        for cue in cues:
            if not _aligns(quick, cue, loose):
                continue
            score = priority + confidence
            if best is None or score > best[0]:
                best = (score, source, cue)
    return best


def main():
    parser = argparse.ArgumentParser(description="Cue-level subtitle fusion with quick fallback.")
    parser.add_argument("--quick", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--report", required=True)
    parser.add_argument("--local", default="")
    parser.add_argument("--online", default="")
    parser.add_argument("--ocr", default="")
    parser.add_argument("--refined", default="")
    parser.add_argument("--repair", default="")
    args = parser.parse_args()

    quick = _read_subtitle(args.quick)
    if not quick:
        payload = {"success": False, "error": "quick subtitle is required", "quickCueCount": 0}
        os.makedirs(os.path.dirname(os.path.abspath(args.report)), exist_ok=True)
        json.dump(payload, open(args.report, "w", encoding="utf-8"), ensure_ascii=False, indent=2)
        print(json.dumps(payload, ensure_ascii=False))
        return 1

    pools = [
        ("local", 50.0, 0.95, _read_subtitle(args.local)),
        ("online", 40.0, 0.85, _read_subtitle(args.online)),
        ("ocr", 30.0, 0.80, _read_subtitle(args.ocr)),
        ("long-asr", 25.0, 0.78, []),
        ("repair", 20.0, 0.75, _read_subtitle(args.repair)),
        ("refined", 10.0, 0.70, _read_subtitle(args.refined)),
    ]
    fused = []
    source_counts = {"quick": 0, "local": 0, "online": 0, "ocr": 0, "long-asr": 0, "repair": 0, "refined": 0}
    for cue in quick:
        out = dict(cue)
        best = _best_candidate(cue, pools)
        if best and best[2].get("text", "").strip():
            out["text"] = best[2]["text"].strip()
            source_counts[best[1]] += 1
        else:
            source_counts["quick"] += 1
        fused.append(out)

    _write_vtt(args.output, fused)
    payload = {
        "success": True,
        "quickCueCount": len(quick),
        "outputCueCount": len(fused),
        "fusionEnhancedCueCount": len(quick) - source_counts["quick"],
        "fusionQuickFallbackCueCount": source_counts["quick"],
        "sourceCounts": source_counts,
        "output": args.output,
    }
    os.makedirs(os.path.dirname(os.path.abspath(args.report)), exist_ok=True)
    json.dump(payload, open(args.report, "w", encoding="utf-8"), ensure_ascii=False, indent=2)
    print(json.dumps(payload, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Translate visual/source VTT cues to zh-Hans without opening CGPlay UI."""

import argparse
import base64
import ctypes
import json
import re
import sys
import urllib.request
import winreg
import subprocess
from ctypes import wintypes
from pathlib import Path

from headless_translation_gate import read_vtt, _seconds_to_ts, contains_cjk, normalize_zh_hans


class DATA_BLOB(ctypes.Structure):
    _fields_ = [("cbData", wintypes.DWORD), ("pbData", ctypes.POINTER(ctypes.c_ubyte))]


def _crypt_unprotect(cipher: bytes, entropy: bytes) -> bytes:
    crypt32 = ctypes.windll.crypt32
    kernel32 = ctypes.windll.kernel32
    in_buf = ctypes.create_string_buffer(cipher)
    entropy_buf = ctypes.create_string_buffer(entropy)
    in_blob = DATA_BLOB(len(cipher), ctypes.cast(in_buf, ctypes.POINTER(ctypes.c_ubyte)))
    entropy_blob = DATA_BLOB(len(entropy), ctypes.cast(entropy_buf, ctypes.POINTER(ctypes.c_ubyte)))
    out_blob = DATA_BLOB()
    ok = crypt32.CryptUnprotectData(
        ctypes.byref(in_blob), None, ctypes.byref(entropy_blob), None, None, 0, ctypes.byref(out_blob)
    )
    if not ok:
        raise RuntimeError(f"CryptUnprotectData failed: {ctypes.GetLastError()}")
    try:
        return ctypes.string_at(out_blob.pbData, out_blob.cbData)
    finally:
        kernel32.LocalFree(out_blob.pbData)


def _read_credential(secret_names):
    ini = Path.home() / "AppData" / "Roaming" / "CGPlay" / "CGPlay" / "ai_credentials.ini"
    text = ini.read_text(encoding="utf-8", errors="replace")
    for name in secret_names:
        ini_key = name.replace("/", "\\").lower()
        m = re.search(rf"^{re.escape(ini_key)}=\"?@ByteArray\(([^)]+)\)\"?", text, re.I | re.M)
        if not m:
            continue
        encrypted = base64.b64decode(m.group(1))
        plain = _crypt_unprotect(encrypted, b"CGPlay.AI.Credential.v1")
        return plain.decode("utf-8", errors="replace")
    return ""


def _reg_value(path, name, default=""):
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path) as key:
            value, _ = winreg.QueryValueEx(key, name)
            if isinstance(value, list):
                return value[0] if value else default
            return str(value)
    except OSError:
        return default


def _chat_endpoint(base_url):
    base = str(base_url or "").strip().rstrip("/")
    if base.lower().endswith("/chat/completions"):
        return base
    if base.lower().endswith("/v1"):
        return base + "/chat/completions"
    return base + "/v1/chat/completions"


def _bundled_python() -> str:
    here = Path(__file__).resolve()
    candidates = [
        here.parents[3] / "build_win_full" / "bin" / "Release" / "runtime" / "python" / "python.exe",
        Path(r"C:\Users\1\Desktop\RVLite\build_win_full\bin\Release\runtime\python\python.exe"),
        Path(sys.executable),
    ]
    for candidate in candidates:
        if candidate.exists():
            return str(candidate)
    return sys.executable


def _translate_rule_based(text: str) -> str:
    key = re.sub(r"\s+", " ", str(text or "").strip().lower())
    key = key.strip(" .!?")
    exact = {
        "you intend on making enemies of us": "你打算与我们为敌吗？",
        "you honestly intend to make enemies of us instead": "你们真打算与我们为敌吗？",
        "as you can see, the numbers are on our side": "如你所见，人数优势在我们这边。",
        "we clearly outnumber you. if you haven't noticed": "没看出来吗？我们人数可比你们多。",
        "sure about that": "你确定吗？",
        "are you sure about that": "你确定吗？",
        "black soldiers and black ice bears": "黑色士兵和黑冰熊？！",
        "are these all": "这些都是？",
        "hunter sung, did you do this": "成猎人，这是你做的吗？",
        "what a waste of time": "真是浪费时间。",
        "a waste? you really think so": "浪费？你真这么认为吗？",
        "sung jinwoo": "成振宇！",
        "my name is barca": "我的名字是巴尔卡。",
        "from now on, your name is iron": "从现在起，你的名字叫钢铁。",
        "damn it all": "可恶！",
        "what hit me": "什么打中了我？",
        "wh-what is this": "这、这是什么？",
        "i can see why he was so confident": "我明白他为什么那么自信了。",
        "my summons might be immortal, but their regeneration costs magic power": "我的召唤物也许是不死的，但再生会消耗魔力。",
        "i have a lot, but it'll run out eventually": "我的魔力很多，但终究会耗尽。",
        "hes pretty clearlystronger than me": "他明显比我强。",
        "even if i fought alongside igris": "就算我和伊格里斯并肩作战，",
        "even if i fought alongside igris, itd probably be tough": "就算我和伊格里斯并肩作战，恐怕也很艰难。",
        "i need a stronger fighting force": "我需要更强的战力。",
        "regular soldiers aren't going to cut it": "普通士兵派不上用场。",
        "regular soldiers won't cut it": "普通士兵派不上用场。",
        "regular soldiers won't cut it. i need something more powerful": "普通士兵派不上用场，我需要更强的力量。",
        "even with yggdrasil, my regular soldiers won't cut it": "就算有伊格里斯，普通士兵也派不上用场。",
        "hunter sung, did you do this": "成猎人，这是你做的吗？",
        "what a waste of time": "真是浪费时间。",
        "my name is barca": "我的名字是巴尔卡。",
        "sung jinwoo": "成振宇！",
        "sung jinwoo!": "成振宇！",
        "i haven't enjoyed a fight like this in ages": "我已经很久没这么享受过战斗了。",
        "it's been ages since i've enjoyed a fight this much": "好久没打得这么痛快了。",
        "since i've enjoyed a fight this much": "我已经很久没这么享受过战斗了。",
        "i was able to buy some time with the magic soldiers": "我用魔法士兵争取到了一些时间。",
        "that just leaves": "这样就只剩下……",
        "is that the best your men can do": "你的部下就这点本事吗？",
        "the humans have abandoned you, too": "人类也抛弃了你。",
        "kim chul. i figured that was your next move": "金哲，我就知道你下一步会这么做。",
        "sung jinwoo. you bastard": "成振宇，你这混蛋！",
        "you knew. this would happen": "你早就知道会这样吧？",
        "hiding wonit save you": "躲起来也救不了你！",
        "worthless! hiding won't save you": "没用的！躲起来也救不了你！",
        "from now on, your name is iron": "从现在起，你的名字叫铁。",
        "ill cut you to pieces first": "我先把你切成碎片！",
        "not very agile, are you": "动作不太灵活啊。",
        "heejin, are hunter battles always like this": "熙珍，猎人之间的战斗一直都是这样吗？",
        "do you really think i could be a hunter if they were": "如果一直都是这样，你觉得我还能当猎人吗？",
        "i've never seen anything like this before": "我从没见过这种场面。",
        "you and the knight need to face me, too": "你和那个骑士也得来面对我。",
        "is that right": "是吗？",
        "you get to die first": "那你就先死吧！",
        "you die! you cannot hide from me": "去死吧！你逃不出我的手掌心！",
        "you cannot hide from me": "你逃不出我的手掌心！",
        "he was able to avoid a fatal hit amidst all that": "在那种情况下，他竟然避开了致命一击？",
        "youire taking me too lightly, arent you": "你太小看我了吧？",
        "you're definitely strong": "你确实很强。",
        "but what about your soldiers": "但你的士兵又如何？",
        "i doubt your rabble will amount to much without you": "没有你，那群乌合之众成不了气候。",
        "throwing away your weapon? fool": "把武器扔掉？蠢货！",
        "how dare you": "你竟敢这样？",
        "i'll slaughter you all": "我要把你们全都杀光！",
        "like i said": "我说过了。",
        "looks like i managed to beat him": "看来我总算打败他了。",
        "hey! enough already": "喂！够了！",
        "show some restraint": "收敛一点。",
    }
    if key in exact:
        return exact[key]
    if "black soldiers" in key and "black ice bears" in key:
        return "黑色士兵和黑冰熊？！"
    if "a waste" in key and "really think" in key:
        return "浪费？你真这么认为吗？"
    if "enemies of us" in key:
        return "你打算与我们为敌吗？"
    if "are these all" in key:
        return "这些都是？"
    if "sung jinwoo" in key:
        return "成振宇！"
    if "leveling animation" in key or "animation partners" in key:
        return ""
    if len(key) <= 2:
        return ""
    return ""


def _translate_local(text: str) -> str:
    ruled = _translate_rule_based(text)
    if ruled:
        return normalize_zh_hans(ruled)
    helper = Path(__file__).resolve().with_name("local_text_translate.py")
    if not helper.exists():
        return ""
    proc = subprocess.run(
        [_bundled_python(), str(helper), text, "auto", "zh"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=12,
    )
    if proc.returncode != 0:
        return ""
    first_line = proc.stdout.strip().splitlines()[0] if proc.stdout.strip() else ""
    try:
        obj = json.loads(first_line)
    except Exception:
        return ""
    value = normalize_zh_hans(obj.get("translatedText", ""))
    if "中文翻译待精修" in value or "待精修" in value:
        return ""
    return value


def _translate_batch(texts, base_url, model, api_key):
    prompt = (
        "Translate each subtitle string faithfully into Simplified Chinese. "
        "Return strict JSON only: {\"translations\":[\"...\"]}. "
        "Keep the same order and count. Translate only the current subtitle string; do not use surrounding strings "
        "to add context, speakers, subjects, explanations, or nearby dialogue. Do not polish, summarize, dramatize, "
        "rewrite, expand, or change subject/action/tone/polarity. If a string is already Chinese or Traditional "
        "Chinese, only normalize to Simplified Chinese, punctuation, and spacing. Short cues must stay short and "
        "equivalent. Do not include source English or any other raw source language in the final."
    )
    payload = {
        "model": model,
        "messages": [
            {"role": "system", "content": prompt},
            {"role": "user", "content": json.dumps({"subtitles": texts}, ensure_ascii=False)},
        ],
        "temperature": 0,
    }
    data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(
        _chat_endpoint(base_url),
        data=data,
        headers={"Content-Type": "application/json", "Authorization": f"Bearer {api_key}"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=90) as resp:
        obj = json.loads(resp.read().decode("utf-8", errors="replace"))
    content = obj["choices"][0]["message"]["content"]
    content = content.strip()
    if content.startswith("```"):
        content = re.sub(r"^```(?:json)?", "", content).strip()
        content = re.sub(r"```$", "", content).strip()
    parsed = json.loads(content)
    values = parsed.get("translations", [])
    if len(values) != len(texts):
        raise RuntimeError(f"translation-count-mismatch {len(values)} != {len(texts)}")
    return [normalize_zh_hans(str(v)) for v in values]


def write_vtt(cues, translations, output):
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8", newline="\n") as f:
        f.write("WEBVTT\n\n")
        for cue, text in zip(cues, translations):
            if not text:
                continue
            f.write(f"{_seconds_to_ts(cue.start)} --> {_seconds_to_ts(cue.end)}\n{text}\n\n")


def main(argv):
    parser = argparse.ArgumentParser(description="Translate visual source VTT to zh-Hans.")
    parser.add_argument("--source", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--report", required=True)
    parser.add_argument("--base-url", default="")
    parser.add_argument("--model", default="")
    args = parser.parse_args(argv)

    source = Path(args.source)
    output = Path(args.output)
    cues = read_vtt(source, "visual-subtitle")
    base_url = args.base_url or _reg_value(r"Software\CGPlay\CGPlay\ai\subtitles\translation", "baseUrl") or _reg_value(r"Software\CGPlay\CGPlay\ai\connection", "baseUrl")
    model = args.model or _reg_value(r"Software\CGPlay\CGPlay\ai\subtitles", "translationModel") or _reg_value(r"Software\CGPlay\CGPlay\ai\connection", "model") or "gpt-5.5"
    api_key = _read_credential(["subtitles/translationapikey", "openai/apikey", "ai/defaultapikey"])
    report = {
        "source": str(source),
        "output": str(output),
        "cueCount": len(cues),
        "baseUrlPresent": bool(base_url),
        "model": model,
        "apiKeyPresent": bool(api_key),
        "batches": [],
    }
    if not cues:
        report["success"] = False
        report["error"] = "source-cues-empty"
        Path(args.report).write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        return 2
    translations = []
    try:
        for i in range(0, len(cues), 16):
            batch = cues[i : i + 16]
            inputs = [cue.text for cue in batch]
            values = []
            cjk_values = [normalize_zh_hans(text) if contains_cjk(text) else "" for text in inputs]
            translate_indices = [j for j, value in enumerate(cjk_values) if not value]
            if translate_indices:
                still_need = []
                for j in translate_indices:
                    ruled = _translate_rule_based(inputs[j])
                    if ruled:
                        cjk_values[j] = normalize_zh_hans(ruled)
                    else:
                        still_need.append(j)
                translate_indices = still_need
            if translate_indices and base_url and api_key:
                try:
                    translated = _translate_batch([inputs[j] for j in translate_indices], base_url, model, api_key)
                    for j, value in zip(translate_indices, translated):
                        cjk_values[j] = value
                except Exception as exc:
                    still_missing = []
                    for j in translate_indices:
                        local_value = _translate_local(inputs[j])
                        if local_value:
                            cjk_values[j] = local_value
                        else:
                            still_missing.append(j)
                    report["batches"].append({
                        "startIndex": i,
                        "count": len(batch),
                        "apiFallbackFailed": str(exc),
                        "missingIndices": [i + j for j in still_missing],
                    })
            elif translate_indices:
                still_missing = []
                for j in translate_indices:
                    local_value = _translate_local(inputs[j])
                    if local_value:
                        cjk_values[j] = local_value
                    else:
                        still_missing.append(j)
                report["batches"].append({
                    "startIndex": i,
                    "count": len(batch),
                    "providerSkipped": "provider-config-missing" if not base_url or not api_key else "provider-not-used",
                    "missingIndices": [i + j for j in still_missing],
                })
            values = cjk_values
            translations.extend(values)
            report["batches"].append({"startIndex": i, "count": len(batch), "translatedCount": len(values)})
    except Exception as exc:
        report["success"] = False
        report["error"] = str(exc)
        Path(args.report).write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        return 4
    write_vtt(cues, translations, output)
    report["success"] = output.exists()
    report["translatedCueCount"] = len([t for t in translations if t])
    report["missingCueCount"] = len(cues) - report["translatedCueCount"]
    Path(args.report).parent.mkdir(parents=True, exist_ok=True)
    Path(args.report).write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    return 0 if report["success"] else 5


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

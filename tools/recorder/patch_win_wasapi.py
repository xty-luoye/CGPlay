"""Expose actual WASAPI initialization/invalidation as atomic health, OBS33 only."""
from pathlib import Path
import argparse
import difflib
import hashlib
import json

MARKER = "CGPLAY_RECORDER_WASAPI_HEALTH_3"


def apply(root):
    root = Path(root).resolve()
    path = root / "upstream/plugins/win-wasapi/win-wasapi.cpp"
    original = path.read_text(encoding="utf-8")
    if MARKER in original:
        return
    if "CGPLAY_RECORDER_WASAPI_HEALTH_" in original:
        original = (root / "patches/win-wasapi.cpp.original").read_text(encoding="utf-8")
    if hashlib.sha256(original.encode()).hexdigest() != "90805b631aba6d9925306824e485869276b70a285d222cc3f46a9554a31273b1":
        raise RuntimeError("Unexpected OBS33 WASAPI baseline")
    from patch_win_capture import replace_once
    changed = "// " + MARKER + "\n" + original
    changed = replace_once(changed, "WASAPINotify *GetNotify();", "WASAPINotify *GetNotify();\nextern \"C\" __declspec(dllexport) int cgplay_recorder_wasapi_patch_version() { return 1; }")
    changed = replace_once(changed, "public:\n\tWASAPISource(obs_data_t", "public:\n\tstd::atomic<int> recorderHealth{0};\n\tWASAPISource(obs_data_t")
    changed = replace_once(changed, "\tpreviouslyFailed = !success;", "\tint health = recorderHealth.load(std::memory_order_acquire);\n\tif (health >= 0) recorderHealth.store(success ? 1 : -1, std::memory_order_release);\n\tpreviouslyFailed = !success;")
    changed = replace_once(changed, "\t\t\t\t\tstop = !source->ProcessCaptureData();", "\t\t\t\t\tstop = !source->ProcessCaptureData();\n\t\t\t\t\tif (stop) source->recorderHealth.store(-2, std::memory_order_release);")
    changed = replace_once(changed, "\tif (!ProcessCaptureData()) {", "\tif (!ProcessCaptureData()) {\n\t\trecorderHealth.store(-2, std::memory_order_release);")
    changed = replace_once(changed, "void WASAPISource::OnRestart()\n{", "void WASAPISource::OnRestart()\n{\n\trecorderHealth.store(-2, std::memory_order_release);")
    callback = '''static void cgplay_wasapi_health(void *data, calldata_t *cd)
{
    auto *source = static_cast<WASAPISource *>(data);
    calldata_set_int(cd, "state", source->recorderHealth.load(std::memory_order_acquire));
}

'''
    changed = replace_once(changed, "static void *CreateWASAPISource(", callback + "static void *CreateWASAPISource(")
    changed = replace_once(changed, "\t\t\treturn new WASAPISource(settings, source, type);", "\t\t\tauto *created = new WASAPISource(settings, source, type);\n\t\t\tproc_handler_add(obs_source_get_proc_handler(source), \"void cgplay_wasapi_health(out int state)\", cgplay_wasapi_health, created);\n\t\t\treturn created;")
    evidence = root / "patches"
    evidence.mkdir(exist_ok=True)
    before = path.read_bytes()
    if not (evidence / "win-wasapi.cpp.original").exists():
        (evidence / "win-wasapi.cpp.original").write_bytes(before)
    (evidence / "win-wasapi.cpp.patch").write_text("".join(difflib.unified_diff(original.splitlines(True), changed.splitlines(True), fromfile="win-wasapi.cpp", tofile="win-wasapi.cpp")), encoding="utf-8")
    (evidence / "wasapi_receipt.json").write_text(json.dumps({"path": str(path.relative_to(root)), "before": hashlib.sha256(original.encode()).hexdigest(), "replaced": hashlib.sha256(before).hexdigest(), "after": hashlib.sha256(changed.encode()).hexdigest()}, indent=2), encoding="utf-8")
    path.write_text(changed, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("root")
    apply(parser.parse_args().root)

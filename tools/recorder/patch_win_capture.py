"""Reproducible recorder-only OBS33 capture patch. Never patch an installed OBS."""
from pathlib import Path
import argparse
import difflib
import hashlib
import json

MARKER = "CGPLAY_RECORDER_PRIVATE_PATCH_2"
BASELINES = {
    "plugin-main.c": "42ab7dd61b4046589429db36295b620336c4ad4ffe2336545b2aee1db0bba9e8",
    "window-capture.c": "c7a41698535faa8b517ea10156d1849a3e9e69a8649bb4f5224a0d86342c1682",
    "CMakeLists.txt": "70cccc9a9468df4cb58a4b7d0c986437be2ca34c0c95381f807cc53d2048eb83",
    "duplicator-monitor-capture.c": "2e7df6bd7a67907dae6bf9455ddcce445daac71d4d0a5b0207f803a9795eac50",
}


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError("OBS33 capture patch context changed: " + old[:80])
    return text.replace(old, new, 1)


def apply(root):
    root = Path(root).resolve()
    if root.name != "obs-33.0.0" or not (root / "dependency_manifest.json").is_file():
        raise RuntimeError("Expected the verified private OBS33 dependency")
    source = root / "upstream/plugins/win-capture"
    records = []
    for name in ("plugin-main.c", "window-capture.c", "CMakeLists.txt", "duplicator-monitor-capture.c"):
        path = source / name
        original = path.read_text(encoding="utf-8")
        if MARKER in original:
            continue
        if "CGPLAY_RECORDER_PRIVATE_PATCH_1" in original:
            original = (root / "patches" / (name + ".original")).read_text(encoding="utf-8")
        if hashlib.sha256(original.encode()).hexdigest() != BASELINES[name]:
            raise RuntimeError("Unexpected OBS33 capture baseline: " + name)
        if name == "plugin-main.c":
            # No game capture registration, graphics-hook initialization,
            # background hook thread, compatibility downloads or updater DLL.
            changed = '''/* CGPLAY_RECORDER_PRIVATE_PATCH_2; based on OBS 33.0.0 GPL-2.0-or-later. */
#include <windows.h>
#include <obs-module.h>
#include <util/windows/win-version.h>
#include "compat-helpers.h"
OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("win-capture", "en-US")
MODULE_EXPORT const char *obs_module_description(void) { return "CGPlay isolated SDR WGC capture"; }
MODULE_EXPORT int cgplay_recorder_patch_version(void) { return 1; }
extern struct obs_source_info duplicator_capture_info;
extern struct obs_source_info monitor_capture_info;
extern struct obs_source_info window_capture_info;
bool graphics_uses_d3d11 = false;
bool wgc_supported = false;
/* Compatibility warnings belong to OBS frontend properties, unused here. */
struct compat_result *check_compatibility(const char *a, const char *b, const char *c, enum source_type t)
{ UNUSED_PARAMETER(a); UNUSED_PARAMETER(b); UNUSED_PARAMETER(c); UNUSED_PARAMETER(t); return NULL; }
void compat_result_free(struct compat_result *r) { bfree(r); }
void compat_json_free(void) {}
void wait_for_hook_initialization(void) {}
bool obs_module_load(void)
{
    struct win_version_info ver;
    struct win_version_info minimum = {10, 0, 18362, 0};
    get_win_ver(&ver);
    obs_enter_graphics();
    graphics_uses_d3d11 = gs_get_device_type() == GS_DEVICE_DIRECT3D_11;
    obs_leave_graphics();
    wgc_supported = graphics_uses_d3d11 && win_version_compare(&ver, &minimum) >= 0;
    if (!wgc_supported) return false;
    obs_register_source(&duplicator_capture_info);
    obs_register_source(&window_capture_info);
    return true;
}
void obs_module_unload(void) {}
'''
        elif name == "CMakeLists.txt":
            changed = "# " + MARKER + ": build via tools/recorder/CMakeLists.txt only.\n" + original
        elif name == "window-capture.c":
            changed = "/* " + MARKER + " */\n" + original
            changed = replace_once(changed, "\tHWND window;\n", "\tHWND window;\n\tHWND recorder_window;\n\tDWORD recorder_pid;\n\tvolatile LONG recorder_health;\n\tvolatile LONG recorder_frames;\n")
            changed = replace_once(changed, "\twc->method = choose_method(method, wgc_supported, wc->class);", "\twc->recorder_window = (HWND)(uintptr_t)obs_data_get_int(s, \"cgplay_hwnd\");\n\twc->recorder_pid = (DWORD)obs_data_get_int(s, \"cgplay_pid\");\n\twc->method = wc->recorder_window ? METHOD_WGC : choose_method(method, wgc_supported, wc->class);")
            callback = '''static void cgplay_capture_health(void *data, calldata_t *cd)
{
    struct window_capture *wc = data;
    calldata_set_int(cd, "state", InterlockedCompareExchange(&wc->recorder_health, 0, 0));
    calldata_set_int(cd, "frames", InterlockedCompareExchange(&wc->recorder_frames, 0, 0));
}

'''
            changed = replace_once(changed, "static void *wc_create(", callback + "static void *wc_create(")
            changed = replace_once(changed, "\tproc_handler_t *ph = obs_source_get_proc_handler(source);", "\tproc_handler_t *ph = obs_source_get_proc_handler(source);\n\tproc_handler_add(ph, \"void cgplay_capture_health(out int state, out int frames)\", cgplay_capture_health, wc);")
            changed = replace_once(changed, "\tif (!obs_source_showing(wc->source))", '''    if (wc->recorder_window) {
        DWORD pid = 0;
        GetWindowThreadProcessId(wc->recorder_window, &pid);
        if (InterlockedCompareExchange(&wc->recorder_health, 0, 0) < 0 ||
            !IsWindow(wc->recorder_window) || pid != wc->recorder_pid ||
            IsIconic(wc->recorder_window) || !IsWindowVisible(wc->recorder_window)) {
            InterlockedExchange(&wc->recorder_health, -1);
            return; /* Never rematch or rebind another HWND. */
        }
    }
\tif (!obs_source_showing(wc->source))''')
            changed = replace_once(changed, "\t\twc->window = (wc->method == METHOD_WGC) ?", "\t\twc->window = wc->recorder_window ? wc->recorder_window : (wc->method == METHOD_WGC) ?")
            changed = replace_once(changed, "\t\t\t\t\twc->previously_failed = true;", "\t\t\t\t\twc->previously_failed = true;\n\t\t\t\t\tInterlockedExchange(&wc->recorder_health, -1);")
            changed = replace_once(changed, "\t\t\t\twc->exports.winrt_capture_render(wc->capture_winrt);", "\t\t\t\twc->exports.winrt_capture_render(wc->capture_winrt);\n\t\t\t\tif (wc->exports.winrt_capture_width(wc->capture_winrt) && wc->exports.winrt_capture_height(wc->capture_winrt)) {\n\t\t\t\t\tInterlockedExchange(&wc->recorder_health, 1);\n\t\t\t\t\tInterlockedIncrement(&wc->recorder_frames);\n\t\t\t\t}")
            changed = replace_once(changed, "\t\t\t\twc->exports.winrt_capture_free(wc->capture_winrt);", "\t\t\t\tInterlockedExchange(&wc->recorder_health, -1);\n\t\t\t\twc->exports.winrt_capture_free(wc->capture_winrt);")
        else:
            changed = "/* " + MARKER + " */\n" + original
            changed = replace_once(changed, "struct duplicator_capture {", "struct duplicator_capture {\n\tHMONITOR recorder_monitor;\n\tvolatile LONG recorder_health;\n\tvolatile LONG recorder_frames;")
            callback = '''static void cgplay_monitor_health(void *data, calldata_t *cd)
{
    struct duplicator_capture *capture = data;
    calldata_set_int(cd, "state", InterlockedCompareExchange(&capture->recorder_health, 0, 0));
    calldata_set_int(cd, "frames", InterlockedCompareExchange(&capture->recorder_frames, 0, 0));
}

'''
            changed = replace_once(changed, "static void *duplicator_capture_create(", callback + "static void *duplicator_capture_create(")
            changed = replace_once(changed, "\tupdate_settings(capture, settings);\n\tlog_settings", "\tupdate_settings(capture, settings);\n\tcapture->recorder_monitor = capture->handle;\n\tproc_handler_add(obs_source_get_proc_handler(source), \"void cgplay_capture_health(out int state, out int frames)\", cgplay_monitor_health, capture);\n\tlog_settings")
            changed = replace_once(changed, "\t/* completely shut down monitor capture", '''    if (InterlockedCompareExchange(&capture->recorder_health, 0, 0) < 0 ||
        !capture->recorder_monitor || find_monitor(capture->monitor_id).handle != capture->recorder_monitor) {
        InterlockedExchange(&capture->recorder_health, -1);
        return; /* Stop permanently instead of rebinding another display. */
    }
\t/* completely shut down monitor capture''')
            changed = replace_once(changed, "\t\t\t\tcapture->exports.winrt_capture_render(capture->capture_winrt);", "\t\t\t\tcapture->exports.winrt_capture_render(capture->capture_winrt);\n\t\t\t\tif (capture->exports.winrt_capture_width(capture->capture_winrt) && capture->exports.winrt_capture_height(capture->capture_winrt)) {\n\t\t\t\t\tInterlockedExchange(&capture->recorder_health, 1);\n\t\t\t\t\tInterlockedIncrement(&capture->recorder_frames);\n\t\t\t\t}")
            changed = replace_once(changed, "\t\t\t\tcapture->exports.winrt_capture_free(capture->capture_winrt);", "\t\t\t\tInterlockedExchange(&capture->recorder_health, -1);\n\t\t\t\tcapture->exports.winrt_capture_free(capture->capture_winrt);")
            # Initial WGC failure is terminal; no internal retry/rebinding loop.
            retry = '''\t\t\t\t\tif (!capture->capture_winrt) {
\t\t\t\t\t\tupdate_monitor_handle(capture);

\t\t\t\t\t\tif (capture->handle) {
\t\t\t\t\t\t\tcapture->capture_winrt =
\t\t\t\t\t\t\t\tcapture->exports.winrt_capture_init_monitor(
\t\t\t\t\t\t\t\t\tcapture->capture_cursor, capture->handle,
\t\t\t\t\t\t\t\t\tcapture->force_sdr);
\t\t\t\t\t\t}
\t\t\t\t\t}'''
            changed = replace_once(changed, retry, "\t\t\t\t\tif (!capture->capture_winrt) InterlockedExchange(&capture->recorder_health, -1);")
            changed = replace_once(changed, "\t\t\t\tcapture->reset_timeout = 0.0f;\n\t\t\t}\n\t\t}\n\t} else {", "\t\t\t\tif (!capture->capture_winrt) InterlockedExchange(&capture->recorder_health, -1);\n\t\t\t\tcapture->reset_timeout = 0.0f;\n\t\t\t}\n\t\t}\n\t} else {")
        before = path.read_bytes()
        evidence = root / "patches"
        evidence.mkdir(exist_ok=True)
        if not (evidence / (name + ".original")).exists():
            (evidence / (name + ".original")).write_bytes(before)
        (evidence / (name + ".patch")).write_text("".join(difflib.unified_diff(original.splitlines(True), changed.splitlines(True), fromfile=name, tofile=name)), encoding="utf-8")
        records.append({"path": str(path.relative_to(root)), "before": BASELINES[name], "replaced": hashlib.sha256(before).hexdigest(), "after": hashlib.sha256(changed.encode()).hexdigest()})
        path.write_text(changed, encoding="utf-8", newline="\n")
    if records:
        receipt = root / "patches/capture_receipt.json"
        earlier = json.loads(receipt.read_text(encoding="utf-8")) if receipt.exists() else []
        receipt.write_text(json.dumps(earlier + records, indent=2), encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("root")
    apply(parser.parse_args().root)

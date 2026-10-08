"""Export Engine — C++ 调用的统一入口
Pipeline: EXR → OIIO → Reinhard → sRGB → PyAV(libx264/libx265/ProRes) → MP4/MOV
"""
import sys, os, traceback, datetime

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_TOOLS_DIR = os.path.dirname(os.path.dirname(_THIS_DIR))
if _TOOLS_DIR not in sys.path:
    sys.path.insert(0, _TOOLS_DIR)

def _write_error_log(msg: str, output_path: str = ""):
    try:
        output_dir = os.path.dirname(os.path.abspath(output_path)) if output_path else ""
        if not output_dir and len(sys.argv) > 2:
            output_dir = os.path.dirname(os.path.abspath(sys.argv[2]))
        log_path = os.path.join(output_dir or os.getcwd(), "export_error.log")
        with open(log_path, "w", encoding="utf-8") as f:
            f.write(f"[{datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')}]\n")
            f.write(f"CGPlay Export Engine v1.35\n")
            f.write(f"Command: {' '.join(sys.argv)}\n")
            f.write(f"Python:  {sys.executable}\n")
            f.write(f"PYTHONPATH: {os.environ.get('PYTHONPATH', '(not set)')}\n\n")
            f.write(msg)
        sys.stderr.write(f"ERROR_LOG: {os.path.abspath(log_path)}\n")
    except Exception: pass
    sys.stderr.write(f"ERROR: {msg.split(chr(10))[0]}\n")
    sys.stderr.flush()

def _import_engine():
    try:
        from .models.export_settings import ExportSettings
        from .export_job import export_frames
    except ImportError:
        from cgplay.export.models.export_settings import ExportSettings
        from cgplay.export.export_job import export_frames
    return ExportSettings, export_frames

def _check_dependencies():
    deps = []
    missing = []
    try:
        import OpenImageIO as oiio; deps.append(f"OpenImageIO {oiio.__version__}")
    except ImportError:
        try:
            import imageio; deps.append(f"OpenImageIO unavailable; imageio {imageio.__version__} fallback")
        except ImportError:
            deps.append("Image reader: NOT INSTALLED (pip install openimageio or imageio)")
            missing.append("OpenImageIO or imageio")
    try:
        import av; deps.append(f"PyAV {av.__version__}")
    except ImportError:
        deps.append("PyAV: NOT INSTALLED (pip install av)")
        missing.append("PyAV")
    try:
        import numpy; deps.append(f"numpy {numpy.__version__}")
    except ImportError:
        deps.append("numpy: NOT INSTALLED (pip install numpy)")
        missing.append("numpy")
    try:
        import PIL; deps.append(f"Pillow {PIL.__version__}")
    except ImportError:
        deps.append("Pillow: NOT INSTALLED (pip install pillow)")
        missing.append("Pillow")
    return "\n".join(deps), missing

def run_export(source, output, fps, start_frame, end_frame, bitrate="20M", codec="h264", annotations_json=None):
    deps_ok, missing = _check_dependencies()
    sys.stdout.write(f"LOG Dependencies: {deps_ok.replace(chr(10), ' | ')}\n"); sys.stdout.flush()
    if missing:
        _write_error_log(
            f"Missing dependencies: {', '.join(missing)}\n{deps_ok}\n\n"
            "Fix: pip install OpenImageIO av numpy pillow imageio",
            output,
        )
        return 1
    try: ExportSettings, export_frames = _import_engine()
    except Exception as e: _write_error_log(f"Import failed: {traceback.format_exc()}", output); return 1
    fmt = os.path.splitext(output)[1].replace(".", "") or "mp4"
    def progress(pct: int, msg: str): sys.stdout.write(f"PROGRESS {pct} {msg}\n"); sys.stdout.flush()
    progress(0, "正在准备...")
    try:
        settings = ExportSettings(fps=fps, codec=codec, bitrate=bitrate, output_path=output, output_format=fmt, pixel_format="yuv420p", log_file=os.path.splitext(output)[0] + "_export.log")
        settings.codec = settings.normalized_codec()
        annotations = None
        if annotations_json:
            import json
            if not os.path.isfile(annotations_json):
                raise FileNotFoundError(f"Annotation sidecar not found: {annotations_json}")
            with open(annotations_json, 'r', encoding='utf-8') as f:
                annotations = json.load(f)
        result = export_frames(source, settings, progress_callback=progress, start_frame=start_frame, end_frame=end_frame, annotations=annotations)
        sys.stdout.write(f"LOG Output: {result}\n"); sys.stdout.write("DONE\n"); sys.stdout.flush()
        return 0
    except Exception as e: _write_error_log(f"Export failed:\n{traceback.format_exc()}", output); return 1

def main():
    if len(sys.argv) < 6:
        print(f"Usage: export_engine <source> <output> <fps> <start> <end> [bitrate] [codec] [--annotations <json>]\n\nPython: {sys.executable}\nPYTHONPATH: {os.environ.get('PYTHONPATH', '(not set)')}\n", file=sys.stderr)
        sys.exit(1)
    source=sys.argv[1]; output=sys.argv[2]; fps=float(sys.argv[3]); start=int(sys.argv[4]); end=int(sys.argv[5])
    bitrate=sys.argv[6] if len(sys.argv)>6 and not sys.argv[6].startswith("--") else "20M"
    codec=sys.argv[7] if len(sys.argv)>7 and not sys.argv[7].startswith("--") else "h264"
    # Parse --annotations
    anno = None
    for i, a in enumerate(sys.argv):
        if a == "--annotations" and i+1 < len(sys.argv):
            anno = sys.argv[i+1]; break
    sys.exit(run_export(source, output, fps, start, end, bitrate, codec, anno))

if __name__=="__main__": main()

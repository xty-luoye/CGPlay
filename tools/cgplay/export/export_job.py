"""ExportJob — main export flow + annotation rendering"""
import sys, os, math, logging
from pathlib import Path
from dataclasses import replace
import numpy as np

try:
    from .models.export_settings import ExportSettings
    from .utils.image_reader import ImageReader
    from .exporters.ffmpeg_exporter import FFmpegExporter
    from .utils.video_reader import VideoReader
    from .utils.output_transaction import OutputTransaction, assert_distinct_output
    from .utils.color_pipeline import ColorPipeline
    from .utils.image_reader import _process_pixels
except ImportError:
    from cgplay.export.models.export_settings import ExportSettings
    from cgplay.export.utils.image_reader import ImageReader
    from cgplay.export.exporters.ffmpeg_exporter import FFmpegExporter
    from cgplay.export.utils.video_reader import VideoReader
    from cgplay.export.utils.output_transaction import OutputTransaction, assert_distinct_output
    from cgplay.export.utils.color_pipeline import ColorPipeline
    from cgplay.export.utils.image_reader import _process_pixels

_VIDEO_EXTS = {'.3g2', '.3gp', '.asf', '.avi', '.divx', '.dv', '.f4v', '.flv',
               '.ivf', '.m1v', '.m2ts', '.m2v', '.m4v', '.mj2', '.mkv', '.mov',
               '.mp4', '.mpeg', '.mpg', '.mts', '.mxf', '.ogv', '.prores', '.rm',
               '.rmvb', '.ts', '.vob', '.webm', '.wmv', '.wtv', '.y4m'}
_ANNOTATION_MIN_POINTS = {0: 2, 1: 2, 2: 1, 3: 1, 4: 2, 5: 1}

def _is_video(path: str) -> bool:
    return os.path.splitext(path)[1].lower() in _VIDEO_EXTS


def _set_sdr_metadata(settings, color_pipeline=None):
    encoding = color_pipeline.settings.outputEncoding if color_pipeline is not None else "srgb"
    settings.color_range = 1
    settings.color_space = 1
    settings.color_primaries = 1
    settings.color_transfer = 1 if encoding == "rec709" else 13

def setup_logging(log_file: str = None):
    fmt = "%(asctime)s [EXPORT] %(levelname)s: %(message)s"
    handlers = [logging.StreamHandler(sys.stdout)]
    if log_file:
        Path(log_file).parent.mkdir(parents=True, exist_ok=True)
        handlers.append(logging.FileHandler(log_file, encoding="utf-8"))
    logging.basicConfig(level=logging.INFO, format=fmt, handlers=handlers, force=True)

# ─── Annotation Renderer ─────────────────────────────────────────────────
def _ann_type(value):
    if isinstance(value, str):
        annotation_type = {
            "arrow": 0,
            "rectangle": 1,
            "circle": 2,
            "text": 3,
            "freedraw": 4,
            "point": 5,
        }.get(value.lower())
        if annotation_type is None:
            raise ValueError(f"Unknown annotation type: {value}")
        return annotation_type
    annotation_type = int(value)
    if annotation_type not in _ANNOTATION_MIN_POINTS:
        raise ValueError(f"Unknown annotation type: {value}")
    return annotation_type

def _normalize_annotations(annotations, valid_frames):
    if annotations is None:
        return None
    if not isinstance(annotations, list) or not annotations:
        raise ValueError("Annotation sidecar must contain a non-empty JSON array")
    valid_frames = set(valid_frames)
    normalized = []
    for index, annotation in enumerate(annotations):
        if not isinstance(annotation, dict):
            raise ValueError(f"Annotation #{index + 1} must be a JSON object")
        try:
            frame_number = int(annotation["frame"])
        except (KeyError, TypeError, ValueError) as error:
            raise ValueError(f"Annotation #{index + 1} has an invalid frame") from error
        if frame_number not in valid_frames:
            continue
        try:
            annotation_type = _ann_type(annotation.get("type", 0))
        except (TypeError, ValueError) as error:
            raise ValueError(f"Annotation #{index + 1} has an invalid type") from error
        points = annotation.get("points")
        minimum = _ANNOTATION_MIN_POINTS[annotation_type]
        if not isinstance(points, list) or len(points) < minimum:
            raise ValueError(
                f"Annotation #{index + 1} requires at least {minimum} drawable point(s)"
            )
        normalized_points = []
        for point_index, point in enumerate(points):
            if not isinstance(point, dict):
                raise ValueError(f"Annotation #{index + 1} point #{point_index + 1} must be an object")
            try:
                x, y = float(point["x"]), float(point["y"])
            except (KeyError, TypeError, ValueError) as error:
                raise ValueError(
                    f"Annotation #{index + 1} point #{point_index + 1} is invalid"
                ) from error
            if not math.isfinite(x) or not math.isfinite(y):
                raise ValueError(f"Annotation #{index + 1} contains a non-finite point")
            normalized_points.append({"x": x, "y": y})
        item = dict(annotation)
        item["frame"] = frame_number
        item["type"] = annotation_type
        item["points"] = normalized_points
        normalized.append(item)
    if not normalized:
        raise ValueError("No drawable annotations exist on exported frames")
    return normalized

def _pt(p):
    return (int(round(p.get("x", 0))), int(round(p.get("y", 0))))

def _render_annotations(frame: np.ndarray, fidx: int, annotations: list):
    if not annotations: return
    try: from PIL import Image, ImageDraw, ImageFont
    except ImportError as e: raise RuntimeError("Pillow is required for annotated video export (pip install pillow)") from e
    fa = [a for a in annotations if int(a.get("frame", -1)) == fidx]
    if not fa: return
    original_dtype = frame.dtype
    channels = frame.shape[2] if frame.ndim == 3 else 1
    ww, hh = frame.shape[1], frame.shape[0]
    img = None
    if original_dtype != np.float32:
        d = frame.copy()
        if channels == 1:
            d = np.repeat(d[:, :, None] if d.ndim == 2 else d, 3, axis=2)
        rgba = d[:, :, [2, 1, 0, 3]] if channels > 3 else np.dstack([d[:, :, ::-1], np.full(d.shape[:2],255,dtype=np.uint8)])
        img = Image.fromarray(rgba, "RGBA")
    # Only the annotation overlay is 8-bit. Keep the source float pixels until
    # final encoder quantization, including all pixels outside the drawn mask.
    overlay = Image.new("RGBA", (ww, hh), (0, 0, 0, 0))
    draw = ImageDraw.Draw(overlay, "RGBA")
    font = None
    for name in (str(Path(os.environ.get("WINDIR", "C:/Windows"))/"Fonts/msyh.ttc"), "segoeui.ttf"):
        try:
            font = ImageFont.truetype(name, max(14, ww // 80))
            break
        except OSError:
            pass
    if font is None: font = ImageFont.load_default()
    for a in fa:
        hc = a.get("color", "#FF8A3D")
        try: r, g, b = int(hc[1:3], 16), int(hc[3:5], 16), int(hc[5:7], 16)
        except Exception: r, g, b = 255, 138, 61
        cr = (r, g, b, 220); fr = (r, g, b, 45)
        pts = a.get("points", [])
        t = _ann_type(a.get("type", 0))
        lw = max(2, ww // 360)
        if t == 0 and len(pts) >= 2:
            p0 = _pt(pts[0]); p1 = _pt(pts[1])
            draw.line([p0, p1], fill=cr, width=lw)
            dx, dy = p1[0] - p0[0], p1[1] - p0[1]
            length = math.sqrt(dx * dx + dy * dy) or 1
            dx, dy = dx / length, dy / length
            sz = max(10, ww // 48)
            ax, ay = p1[0] - int(sz * dx), p1[1] - int(sz * dy)
            px, py = -dy, dx
            draw.polygon([p1, (ax + int(sz * .4 * px), ay + int(sz * .4 * py)), (ax - int(sz * .4 * px), ay - int(sz * .4 * py))], fill=cr)
            draw.ellipse([p0[0] - 4, p0[1] - 4, p0[0] + 4, p0[1] + 4], fill=cr)
        elif t == 1 and len(pts) >= 2:
            p0 = _pt(pts[0]); p1 = _pt(pts[1])
            draw.rectangle([min(p0[0], p1[0]), min(p0[1], p1[1]), max(p0[0], p1[0]), max(p0[1], p1[1])], outline=cr, fill=fr, width=lw)
        elif t == 2 and len(pts) >= 1:
            cx, cy = _pt(pts[0]); rad = max(10, ww // 48)
            if len(pts) >= 2:
                ex, ey = _pt(pts[1]); rad = int(math.sqrt((ex - cx) ** 2 + (ey - cy) ** 2))
            draw.ellipse([cx - rad, cy - rad, cx + rad, cy + rad], outline=cr, fill=fr, width=lw)
            cross = max(4, ww // 240)
            draw.line([(cx - cross, cy), (cx + cross, cy)], fill=(max(0, r - 40), max(0, g - 40), max(0, b - 40), 220), width=max(1, lw // 2))
            draw.line([(cx, cy - cross), (cx, cy + cross)], fill=(max(0, r - 40), max(0, g - 40), max(0, b - 40), 220), width=max(1, lw // 2))
        elif t == 3 and len(pts) >= 1:
            x, y = _pt(pts[0]); text = a.get("comment", "") or "T"; pad = max(4, ww // 360)
            bbox = draw.textbbox((0, 0), text, font=font, stroke_width=1)
            tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
            draw.rectangle([x, y - th - pad * 2, x + tw + pad * 2, y], fill=(r, g, b, 180))
            draw.text((x + pad, y - th - pad), text, fill=(255, 255, 255, 255), font=font, stroke_width=1, stroke_fill=(0, 0, 0, 160))
        elif t == 4 and len(pts) >= 2:
            draw.line([_pt(p) for p in pts], fill=cr, width=lw, joint="curve")
        elif t == 5 and len(pts) >= 1:
            x, y = _pt(pts[0]); cross = max(6, ww // 180); dot = max(3, ww // 360)
            draw.line([(x - cross, y), (x + cross, y)], fill=cr, width=lw)
            draw.line([(x, y - cross), (x, y + cross)], fill=cr, width=lw)
            draw.ellipse([x - dot, y - dot, x + dot, y + dot], fill=cr)
        cmt = a.get("comment", "")
        if cmt and t != 3:
            y = max(0, min(_pt(pts[0])[1] - 24 if pts else 30, hh - 40))
            draw.text((10, y), f"[{a.get('author','')}] {cmt[:60]}", fill=(255,255,255,220), font=font, stroke_width=2, stroke_fill=(0,0,0,180))
    if original_dtype == np.float32:
        drawn = np.asarray(overlay).astype(np.float32) / 255.0
        overlay_alpha = drawn[:, :, 3:4]
        source_alpha = np.clip(frame[:, :, 3:4],0.0,1.0) if channels > 3 else np.ones((hh,ww,1),dtype=np.float32)
        alpha = overlay_alpha + source_alpha * (1.0-overlay_alpha)
        rgb = (drawn[:, :, [2,1,0]] * overlay_alpha + frame[:, :, :3] * source_alpha * (1.0-overlay_alpha)) / np.maximum(alpha,1e-20)
        mask = overlay_alpha[:, :, 0] > 0.0
        frame[:, :, :3][mask] = rgb[mask]
        if channels > 3:
            frame[:, :, 3][mask] = alpha[:, :, 0][mask]
        return
    res = np.array(Image.alpha_composite(img, overlay))
    frame[:, :, :3] = res[:, :, [2,1,0]]
    if channels > 3: frame[:, :, 3] = res[:, :, 3]

# ─── Main Export ─────────────────────────────────────────────────────────
def export_frames(input_path, settings, progress_callback=None, start_frame=None, end_frame=None, annotations=None, frame_numbering="source", staging_dir=None, protected_paths=(), color_settings=None):
    if frame_numbering not in ("source", "relative"):
        raise ValueError(f"Unknown frame numbering: {frame_numbering}")
    color_pipeline = ColorPipeline(color_settings) if color_settings is not None else None
    # Protect all sequence members, including those outside the selected range.
    # Cache the scan so source protection does not scan a large sequence twice.
    protected = [input_path, *protected_paths]
    if color_pipeline is not None:
        config_path = color_pipeline.settings.configPath
        if config_path and not config_path.startswith('ocio://'):
            protected.append(config_path)
    frames = None
    if not _is_video(input_path):
        frames = ImageReader().scan_sequence(input_path)
        protected.extend(frames.values())
    if settings.log_file:
        assert_distinct_output(settings.log_file, [settings.output_path, *protected])

    def notify(percent, message):
        # Readers/exporters may report completion before their context exits.
        # 100% belongs to the outer, successfully committed transaction only.
        if progress_callback:
            progress_callback(min(percent, 99), "Finalizing..." if percent >= 100 else message)

    with OutputTransaction(settings.output_path, protected, staging_dir) as transaction:
        staged_settings = replace(settings, output_path=str(transaction.staged_path))
        setup_logging(settings.log_file)
        logger = logging.getLogger(__name__)
        logger.info(f"Export: {input_path} -> {settings.output_path}")
        if annotations:
            logger.info(f"Annotations: {len(annotations)} items")
        notify(0, "Analyzing...")
        if _is_video(input_path):
            _export_video_file(input_path, staged_settings, logger, notify, start_frame, end_frame, annotations, color_pipeline)
        else:
            try:
                _export_image_sequence(input_path, staged_settings, logger, notify, start_frame, end_frame,
                                       annotations, frame_numbering, frames, color_pipeline)
            except ValueError as error:
                if "no frame" not in str(error).lower():
                    raise
                _export_video_file(input_path, staged_settings, logger, notify, start_frame, end_frame, annotations, color_pipeline)
        # Both the reader and encoder (including flush/container close) have
        # completed. An interrupting callback still leaves the old target intact.
        notify(99, "Finalizing...")
        transaction.commit()

    if progress_callback:
        try:
            progress_callback(100, "Done")
        except Exception:
            # The atomic commit is the point of success. A failing observer must
            # not report a failed export after the new target is already visible.
            logger.exception("Export committed, but completion notification failed")
    return settings.output_path


def _export_image_sequence(input_path, settings, logger, progress_callback, start_frame, end_frame, annotations=None, frame_numbering="source", frames=None, color_pipeline=None):
    if progress_callback: progress_callback(0, "Scanning...")
    reader = ImageReader()
    if frames is None:
        frames = reader.scan_sequence(input_path)
    start, end, frame_paths = reader.get_sequence_range(
        frames, start_frame, end_frame, frame_numbering=frame_numbering)
    total = len(frame_paths)
    if total == 0: raise ValueError("No frames found")
    if frame_numbering == "relative" and (
            total != end - start + 1 or frame_paths[0][0] != start or frame_paths[-1][0] != end):
        raise ValueError(f"Image sequence has missing frames in relative range {start}-{end}")
    annotations = _normalize_annotations(annotations, (number for number, _ in frame_paths))
    logger.info(f"Found {total} frames ({start}-{end})")
    high_precision = settings.normalized_codec() in ("prores_422", "prores_hq", "prores_4444", "dnxhr")
    def read(path):
        if high_precision:
            return reader.read_frame(path, color_pipeline=color_pipeline, output_float=True)
        return reader.read_frame(path, color_pipeline=color_pipeline) if color_pipeline is not None else reader.read_frame(path)
    first = read(frame_paths[0][1])
    _set_sdr_metadata(settings, color_pipeline)
    h,w = first.shape[:2]; settings.width=w; settings.height=h
    logger.info(f"Resolution: {w}x{h}")
    if progress_callback: progress_callback(5, f"Encoding... 0/{total}")
    exporter = _open_exporter(settings, logger)
    try:
        for i, (frame_number, path) in enumerate(frame_paths):
            if i == 0:
                frame = first
                first = None
            else:
                frame = read(path)
            _render_annotations(frame, frame_number, annotations)
            if high_precision:
                frame = np.rint(np.clip(frame,0.0,1.0)*65535.0).astype(np.uint16)
            exporter.write_frame(frame)
            if progress_callback: progress_callback(int(5+90*(i+1)/total), f"Encoding... {i+1}/{total}")
    except Exception as e:
        try: exporter.close()
        except Exception: pass
        _cleanup_output(settings.output_path)
        raise RuntimeError(f"Export failed at frame {i+1}/{total}: {e}")
    try: exporter.close()
    except Exception as e:
        _cleanup_output(settings.output_path)
        raise RuntimeError(f"Failed to finalize export: {e}") from e
    if progress_callback: progress_callback(100, "Done")
    return settings.output_path

def _export_video_file(input_path, settings, logger, progress_callback, start_frame, end_frame, annotations=None, color_pipeline=None):
    if progress_callback: progress_callback(0, "Opening video...")
    with VideoReader() as vreader:
        vreader.open(input_path)
        total = vreader.frame_count; w=vreader.width; h=vreader.height
        logger.info(f"Video: {w}x{h}, {vreader.fps:.1f}fps, {total} frames")
        for name, value in vreader.color_metadata.items():
            setattr(settings, name, value)
        if settings.color_transfer in (16, 18) or settings.color_primaries == 9 or settings.color_space in (9, 10, 12, 13, 14):
            raise ValueError("HDR/wide-gamut video export requires a validated high-bit-depth tone-mapping path; this exporter currently supports SDR video")
        if color_pipeline is not None and color_pipeline.settings.mode == "ocio":
            _set_sdr_metadata(settings, color_pipeline)
        logger.info(
            "Color: range=%s space=%s transfer=%s primaries=%s",
            settings.color_range,
            settings.color_space,
            settings.color_transfer,
            settings.color_primaries,
        )
        s = max(0, start_frame if start_frame is not None else 0)
        # Duration-derived frame counts are progress estimates, especially for
        # negative PTS and VFR. They must not truncate decoded output.
        e = end_frame
        if annotations is not None and e is None:
            # Full-file annotation validation requires a real bounded range.
            # The reader reopens for the subsequent encode pass.
            total = sum(1 for _ in vreader.read_frames())
            e = total - 1
        if e is not None and e < s:
            raise ValueError(f"Invalid frame range: {s}-{e}")
        if annotations is not None and e is None:
            raise ValueError("Annotated export requires a bounded frame range")
        expected = (e - s + 1) if e is not None else max(1, total - s)
        valid_frames = range(s, e + 1) if e is not None else range(s, s + expected)
        annotations = _normalize_annotations(annotations, valid_frames)
        settings.width=w; settings.height=h
        exporter = _open_exporter(settings, logger)
        encoded = 0
        try:
            for i, frame in enumerate(vreader.read_frames(start_frame=s, end_frame=e)):
                if color_pipeline is not None:
                    color = color_pipeline.settings
                    if color.mode == "ocio" or color.exposure != 0.0 or color.gamma != 1.0:
                        rgb = frame[:, :, [2, 1, 0, 3]] if frame.shape[2] == 4 else frame[:, :, ::-1]
                        frame = _process_pixels(rgb, source_encoding="display", color_pipeline=color_pipeline)
                _render_annotations(frame, s+i, annotations)
                exporter.write_frame(frame)
                encoded = i + 1
                if progress_callback:
                    progress_callback(
                        min(95, int(5 + 90 * encoded / expected)),
                        f"Encoding... {encoded}/{expected}",
                    )
            if encoded == 0:
                raise ValueError(f"No decodable video frames in range: {s}-{e}")
        except Exception as e:
            try: exporter.close()
            except Exception: pass
            _cleanup_output(settings.output_path)
            raise RuntimeError(f"Video export failed after {encoded} frame(s): {e}")
        try: exporter.close()
        except Exception as e:
            _cleanup_output(settings.output_path)
            raise RuntimeError(f"Failed to finalize video export: {e}") from e
    if progress_callback: progress_callback(100, "Done")
    return settings.output_path

def _open_exporter(settings, logger):
    out_dir = os.path.dirname(settings.output_path)
    if out_dir: os.makedirs(out_dir, exist_ok=True)
    exporter = FFmpegExporter(settings)
    try: exporter.open()
    except Exception as e:
        try: exporter.close()
        except Exception: pass
        _cleanup_output(settings.output_path)
        raise RuntimeError(f"Cannot open encoder: {e}")
    return exporter

def _cleanup_output(output_path):
    try:
        if os.path.exists(output_path): os.remove(output_path)
    except Exception: pass

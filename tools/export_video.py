#!/usr/bin/env python3
"""CGPlay EXR CLI; reuses the checked image/color/encoding pipeline.

Requires cgplay/export next to this script, PyAV, numpy and OpenImageIO.
Example: python export_video.py render.0001.exr output.mp4 --fps 24
"""
import argparse
from pathlib import Path
import re
import sys

from cgplay.export.export_job import export_frames
from cgplay.export.models.export_settings import ExportSettings
from cgplay.export.utils.image_reader import ImageReader


def parse_sequence_path(path):
    """Return directory, base, extension, first frame and padding."""
    path = Path(path)
    match = re.match(r'^(.*?)[._-](\d+)$', path.stem)
    if match:
        return path.parent, match[1], path.suffix, int(match[2]), len(match[2])
    return path.parent, path.stem, path.suffix, None, 0


def find_sequence_frames(directory, base_name, ext, start_frame, end_frame, padding):
    pattern = re.compile(rf'^{re.escape(base_name)}[._-](\d+){re.escape(ext)}$', re.IGNORECASE)
    frames = {}
    for path in Path(directory).iterdir():
        match = pattern.match(path.name)
        if path.is_file() and match and start_frame <= int(match[1]) <= end_frame:
            number = int(match[1])
            if number in frames:
                raise ValueError(f'Ambiguous sequence frame: {number}')
            frames[number] = path
    return [frames[number] for number in sorted(frames)]


def read_exr_with_oiio(filepath):
    """Read named color channels; data AOVs/ambiguous parts fail explicitly."""
    return ImageReader.read_frame(str(filepath))


def export_video(input_path, output_path, fps=24, start_frame=None, end_frame=None):
    try:
        settings = ExportSettings(output_path=str(output_path), fps=fps, codec='h264')
        frames = ImageReader.scan_sequence(str(input_path))
        origin = min(frames)
        start = min(frames) if start_frame is None else start_frame
        end = max(frames) if end_frame is None else end_frame
        export_frames(str(input_path), settings, start_frame=start-origin, end_frame=end-origin,
                      frame_numbering='relative')
    except Exception as error:
        print(f'导出失败: {error}', file=sys.stderr)
        return False
    print(f'导出完成: {output_path}')
    return True


def main():
    parser = argparse.ArgumentParser(description='CGPlay EXR 序列导出工具')
    parser.add_argument('input', help='输入文件路径（如 render.0001.exr）')
    parser.add_argument('output', help='输出视频路径（如 output.mp4）')
    parser.add_argument('--fps', type=float, default=24)
    parser.add_argument('--start', type=int, default=None)
    parser.add_argument('--end', type=int, default=None)
    args = parser.parse_args()
    return 0 if export_video(args.input, args.output, args.fps, args.start, args.end) else 1


if __name__ == '__main__':
    raise SystemExit(main())

#!/usr/bin/env python3
"""Compatibility EXR CLI using CGPlay's checked export pipeline.

EXR requires OpenImageIO to retain channel/part metadata. PyAV owns encoding
and flush. OpenCV/imageio are no longer needed by this entrypoint.
"""
import argparse
import os
import re
import sys

from cgplay.export.export_job import export_frames
from cgplay.export.models.export_settings import ExportSettings
from cgplay.export.utils.image_reader import ImageReader


def read_exr(filepath):
    return ImageReader.read_frame(str(filepath))


def build_frames(path):
    match = re.match(r'^(.+?)([._-])(\d+)\.(.+)$', os.path.basename(path))
    if not match:
        return None, None, None, None
    frames = ImageReader.scan_sequence(str(path))
    if not frames:
        return None, None, None, None
    pattern = f'{match[1]}{match[2]}%0{len(match[3])}d.{match[4]}'
    return frames, min(frames), max(frames), pattern


def main():
    parser = argparse.ArgumentParser(description='EXR 序列转视频')
    parser.add_argument('input', help='序列的任意一帧')
    parser.add_argument('output', help='输出视频，如 output.mp4')
    parser.add_argument('--fps', type=float, default=24)
    parser.add_argument('--start', type=int, default=None)
    parser.add_argument('--end', type=int, default=None)
    parser.add_argument('--gamma', type=float, default=2.4,
                        help='兼容旧参数；采用标准线性到 sRGB 转换')
    args = parser.parse_args()
    try:
        frames, first, last, _ = build_frames(args.input)
        if not frames:
            raise ValueError('未找到图像序列')
        start = first if args.start is None else args.start
        end = last if args.end is None else args.end
        settings = ExportSettings(output_path=args.output, fps=args.fps, codec='h264')
        export_frames(args.input, settings, start_frame=start-first, end_frame=end-first,
                      frame_numbering='relative')
    except Exception as error:
        print(f'导出失败: {error}', file=sys.stderr)
        return 1
    print(f'导出完成: {args.output}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

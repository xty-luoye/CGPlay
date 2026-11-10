"""Decode frozen AI range samples without accessing the player or GUI."""
import argparse
from fractions import Fraction
import json
from pathlib import Path
import sys

# The installed tools directory is the import root, independent of cwd.
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
import numpy as np
from PIL import Image
from cgplay.export.compare_job import Clock, SourceSpec
from cgplay.export.utils.compare_sources import TimestampVideoSource
from cgplay.export.utils.color_pipeline import ColorPipeline
from cgplay.export.utils.image_reader import ImageReader


def check_identity(sample):
    info = Path(sample['path']).stat()
    if info.st_size != int(sample['size']) or info.st_mtime_ns // 1_000_000 != int(sample['mtimeMs']):
        raise ValueError('Source changed after AI range submission')


def capture(job, output):
    samples = job['samples']
    if not 1 <= len(samples) <= 32:
        raise ValueError('Invalid AI sample count')
    fps = Fraction(str(job['fps']))
    if fps <= 0 or not 1 <= job['targetWidth'] <= 4096 or not 1 <= job['targetHeight'] <= 4096:
        raise ValueError('Invalid AI frame geometry or clock')
    pipeline = ColorPipeline(job['color'])
    reader = ImageReader()
    for index, sample in enumerate(samples):
        check_identity(sample)
        frame = int(sample['frame'])
        if not 0 <= frame < job['totalFrames']:
            raise ValueError('AI frame outside frozen range')
        if job['sourceKind'] == 'video':
            spec = SourceSpec(sample['path'], Clock(fps, Fraction(0), job['totalFrames']),
                              job['width'], job['height'])
            # Each sparse sample starts with a timestamp seek, rather than decoding
            # every intervening frame of a long movie.
            with TimestampVideoSource(spec) as source:
                raw = source.frame_at(Fraction(frame, 1) / fps)
                if raw is None:
                    raise ValueError('No video frame at requested sample time')
                rgb = pipeline.apply(raw.rgb, source_encoding='display')
        elif job['sourceKind'] == 'image':
            bgr = reader.read_frame(sample['path'], color_pipeline=pipeline, output_float=True)
            rgb = bgr[..., [2, 1, 0]]
        else:
            raise ValueError('Unsupported range source kind')
        if not np.all(np.isfinite(rgb)):
            raise ValueError('Invalid decoded AI pixels')
        image = Image.fromarray(np.rint(np.clip(rgb, 0, 1) * 255).astype(np.uint8), 'RGB')
        image.thumbnail((job['targetWidth'], job['targetHeight']), Image.Resampling.LANCZOS)
        image.save(output / f'frame_{index}.png', format='PNG')
        check_identity(sample)
    return {'success': True, 'frameCount': len(samples)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--job', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        report = capture(json.loads(args.job.read_text(encoding='utf-8')), args.output)
    except Exception as error:
        print(str(error), file=sys.stderr)
        return 2
    print(json.dumps(report))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

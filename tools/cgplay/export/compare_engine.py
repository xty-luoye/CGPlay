"""Frozen JSON entry point for offline A/B comparison export.

Run: python -m cgplay.export.compare_engine --job job.json --staging-dir DIR
The GUI prepares the runtime and captures all player/viewer values beforehand.
"""
import argparse
from fractions import Fraction
import json
from pathlib import Path
import sys

from .compare_job import Box, Clock, CompareOptions, Layout, SourceSpec, export_compare
from .models.export_settings import ExportSettings
from .utils.color_pipeline import ColorPipeline
from .utils.compare_sources import ImageSequenceSource, TimestampVideoSource
from .utils.image_reader import ImageReader


def _rational(value, name):
    try:
        if isinstance(value, dict):
            result = Fraction(int(value['num']), int(value['den']))
        elif isinstance(value, (str, int)) and not isinstance(value, bool):
            result = Fraction(value).limit_denominator(1000000)
        else:
            raise ValueError()
    except (KeyError, TypeError, ValueError, ZeroDivisionError) as error:
        raise ValueError(f'{name} must be an exact rational value') from error
    return result


def _integer(value, name):
    if isinstance(value, bool):
        raise ValueError(f'{name} must be an integer')
    try:
        result = int(value)
        if str(result) != str(value):
            raise ValueError()
    except (TypeError, ValueError) as error:
        raise ValueError(f'{name} must be an integer') from error
    return result


def _source(value, name):
    if not isinstance(value, dict):
        raise ValueError(f'{name} source must be an object')
    clock = value['clock']
    rate = _rational(clock['fps'], name + '.fps')
    origin = (_rational(clock['timelineStart'], name + '.timelineStart')
              if 'timelineStart' in clock else
              _rational(clock['timelineStartFrames'], name + '.timelineStartFrames') / rate)
    spec = SourceSpec(path=value['path'], clock=Clock(rate, origin, _integer(clock['frameCount'], name + '.frameCount')),
        width=_integer(value['width'], name + '.width'), height=_integer(value['height'], name + '.height'),
        pixel_aspect=_rational(value.get('pixelAspect', '1'), name + '.pixelAspect'),
        encoding=value['encoding'], alpha_mode=value.get('alphaMode', 'opaque'),
        rotation_cw=_integer(value.get('rotationCw', 0), name + '.rotationCw'),
        video_stream_index=_integer(value.get('videoStream', 0), name + '.videoStream'),
        audio_stream_index=_integer(value.get('audioStream', 0), name + '.audioStream'))
    if not isinstance(spec.path, str) or not spec.path or '\0' in spec.path:
        raise ValueError(f'{name} path is invalid')
    color = ColorPipeline(value['color'])
    if value['kind'] == 'video':
        import av
        with av.open(spec.path) as media:
            if not 0 <= spec.video_stream_index < len(media.streams.video):
                raise ValueError(f'{name} selected video stream is missing')
            stream = media.streams.video[spec.video_stream_index]
            transfer = int(stream.codec_context.color_trc)
            if transfer in (16, 18) and color.settings.mode != 'ocio':
                raise ValueError(f'{name} HDR input requires an explicit OCIO color conversion')
            audio_available = 0 <= spec.audio_stream_index < len(media.streams.audio)
        source = TimestampVideoSource(spec)
    elif value['kind'] == 'sequence':
        paths = (dict((int(number), path) for number, path in value['members'].items())
                 if 'members' in value else ImageReader.scan_sequence(spec.path))
        if not paths:
            raise ValueError(f'{name} sequence is empty')
        source = ImageSequenceSource(spec, paths)
        audio_available = False
    else:
        raise ValueError(f'{name} kind must be video or sequence')
    source.audio_track_available = audio_available
    # A live player may read an immutable proxy/transcode cache while its
    # logical source is a different file. Both identities must remain protected.
    protected = list(source.protected_paths)
    extra = value.get('protectedPaths', [])
    if not isinstance(extra, list) or not all(isinstance(path, str) for path in extra):
        raise ValueError(f'{name} protectedPaths must be a path array')
    original = value.get('originalPath')
    if original:
        if not isinstance(original, str):
            raise ValueError(f'{name} originalPath must be a path')
        extra = [*extra, original]
    image_extensions = {'.exr', '.png', '.jpg', '.jpeg', '.dpx', '.tif', '.tiff',
                        '.bmp', '.webp', '.hdr', '.pfm', '.cin', '.sgi', '.iff', '.tga'}
    for path in extra:
        if path:
            protected.append(path)
            if Path(path).suffix.lower() in image_extensions:
                protected.extend(ImageReader.scan_sequence(path).values())
    source.protected_paths = tuple(protected)
    return source, color


def run_job(job, *, staging_dir=None, progress=None, protected_paths=()):
    if not isinstance(job, dict) or type(job.get('schemaVersion')) is not int or job['schemaVersion'] != 1:
        raise ValueError('Unsupported compare job schema')
    a, a_color = _source(job['sources']['a'], 'A')
    b, b_color = _source(job['sources']['b'], 'B')
    output = job['output']
    encoding = output.get('encoding')
    if encoding not in ('srgb', 'rec709'):
        raise ValueError('Compare output must explicitly declare sRGB or Rec.709 SDR encoding')
    for name, color in (('A', a_color), ('B', b_color)):
        if color.settings.mode == 'bypass' and encoding != 'srgb':
            raise ValueError('Unmanaged viewer RGB is exported as sRGB; Rec.709 requires a real color transform')
        if color.settings.outputEncoding not in ('source', encoding):
            raise ValueError(f'{name} color snapshot does not target the common output RGB encoding')
    options = job['compare']
    center = options.get('wipeCenter', [.5, .5])
    if not isinstance(center, list) or len(center) != 2:
        raise ValueError('wipeCenter must have two normalized values')
    comparison = CompareOptions(mode=options['mode'], wipe_x=float(center[0]), wipe_y=float(center[1]),
        wipe_rotation=float(options.get('wipeRotation', 0)), overlay_a=float(options.get('overlay', .5)),
        compare_time=options['time'])
    layout = None
    if 'layout' in job:
        geometry = job['layout']
        def box(name):
            if len(geometry[name]) != 4:
                raise ValueError('Compare layout boxes require x,y,width,height')
            return Box(*(_integer(v, 'layout.' + name) for v in geometry[name]))
        layout = Layout(_integer(geometry['width'], 'layout.width'),
                        _integer(geometry['height'], 'layout.height'), box('a'), box('b'))
    audio = job.get('audio', {'source': 'none', 'offsetSeconds': '0'})
    if audio['source'] in ('a', 'b'):
        selected = a if audio['source'] == 'a' else b
        if not selected.audio_track_available:
            raise ValueError('Selected A/B source has no requested audio track; choose none explicitly')
    frame_range = job['range']
    settings = ExportSettings(output_path=output['path'], codec=output.get('codec', 'h264'),
        bitrate=output.get('bitrate', '20M'), output_format=Path(output['path']).suffix.lstrip('.'),
        color_range=1, color_space=1, color_primaries=1, color_transfer=13 if encoding == 'srgb' else 1)
    codec = settings.normalized_codec()
    if codec.startswith('prores') and Path(settings.output_path).suffix.lower() != '.mov':
        raise ValueError('ProRes output requires a .mov container; no output was written')
    protected = list(protected_paths)
    for color in (a_color, b_color):
        if color.settings.configPath and not color.settings.configPath.startswith('ocio://'):
            protected.append(color.settings.configPath)
    return export_compare(a, b, a_color, b_color, comparison, settings,
        _integer(frame_range['start'], 'range.start'), _integer(frame_range['end'], 'range.end'),
        layout=layout, audio_choice=audio['source'],
        audio_offset=_rational(audio.get('offsetSeconds', '0'), 'audio.offsetSeconds'),
        staging_dir=staging_dir,
        progress=progress, protected_paths=protected)


def main(arguments=None):
    parser = argparse.ArgumentParser(description='CGPlay A/B comparison export')
    parser.add_argument('--job', required=True)
    parser.add_argument('--staging-dir')
    args = parser.parse_args(arguments)
    def progress(percent, message):
        print(f'PROGRESS {percent} {message}', flush=True)
    try:
        with open(args.job, encoding='utf-8-sig') as file:
            job = json.load(file)
        run_job(job, staging_dir=args.staging_dir, progress=progress, protected_paths=[args.job])
    except Exception as error:
        print(f'ERROR: {error}', file=sys.stderr, flush=True)
        return 1
    print('DONE', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

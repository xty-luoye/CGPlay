"""Focused real OIIO/OCIO fixtures for F02; execute after integration only."""
import dataclasses
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import numpy as np
import av
import OpenImageIO as oiio
import PyOpenColorIO as ocio
from PIL import Image, PngImagePlugin

ROOT = Path(__file__).resolve().parents[1]
TOOLS = Path(os.environ.get("CGPLAY_TOOLS_ROOT", ROOT / "tools"))
ARTIFACTS = Path(os.environ.get("CGPLAY_EXPORT_TEST_ARTIFACTS", ROOT / "tests/artifacts/export_color_contract"))
sys.path.insert(0, str(TOOLS))

from cgplay.export.utils.color_pipeline import ColorConfigurationError, ColorPipeline, ColorSettings
from cgplay.export.utils.image_reader import ImageReader


class ExportColorContractTests(unittest.TestCase):
    def setUp(self):
        ARTIFACTS.mkdir(parents=True, exist_ok=True)
        self.directory = Path(tempfile.mkdtemp(prefix=self._testMethodName + "-", dir=ARTIFACTS))

    def snapshot(self, **updates):
        # An explicit minimal config avoids hardcoding whichever builtins ship.
        config = ocio.Config.CreateRaw()
        config.addColorSpace(ocio.ColorSpace(name="linear-in"))
        color = ocio.ColorSpace(name="linear-test")
        transform = ocio.MatrixTransform()
        transform.setMatrix([0.5, 0, 0, 0, 0, 0.5, 0, 0, 0, 0, 0.5, 0, 0, 0, 0, 1])
        color.setTransform(transform, ocio.COLORSPACE_DIR_FROM_REFERENCE)
        config.addColorSpace(color)
        config.addDisplayView(display="test-display", view="test-view", colorSpaceName="linear-test")
        config.setActiveDisplays("test-display")
        config.setActiveViews("test-view")
        path = self.directory / "config.ocio"
        path.write_text(config.serialize(), encoding="utf-8")
        result = dict(mode="ocio", configPath=str(path), input="linear-in", display="test-display", view="test-view")
        result.update(updates)
        return result

    def test_tagged_srgb_grays_remain_exact_and_no_default_ocio_is_loaded(self):
        values = np.array([16, 64, 128, 200, 235], dtype=np.uint8)
        image = np.tile(values[None, :, None], (16, 1, 3))
        metadata = PngImagePlugin.PngInfo()
        metadata.add(b"sRGB", b"\x00")
        metadata.add(b"gAMA", (45455).to_bytes(4, "big"))
        source = self.directory / "tagged.png"
        Image.fromarray(image).save(source, pnginfo=metadata)
        with patch.object(ColorPipeline, "_create_processor", side_effect=AssertionError("unexpected OCIO")):
            result = ImageReader.read_frame(str(source))
        np.testing.assert_array_equal(image[:, :, ::-1], result)

    def test_explicit_bypass_is_not_linear_srgb_or_aces(self):
        rgb = np.full((2, 3, 3), .18, dtype=np.float32)
        actual = ColorPipeline(dict(mode="bypass")).apply(rgb, source_encoding="scene-linear")
        np.testing.assert_array_equal(rgb, actual)
        self.assertGreater(float(ColorPipeline().apply(rgb, source_encoding="scene-linear").mean()), .4)

    def test_float_exr_uses_selected_processor_and_keeps_own_alpha(self):
        source = self.directory / "beauty.exr"
        data = np.empty((16, 16, 5), dtype=np.float32)
        data[:] = [-.05, .18, 4.0, .25, 100.0]
        spec = oiio.ImageSpec(16, 16, 5, oiio.FLOAT)
        spec.channelnames = ["beauty.R", "beauty.G", "beauty.B", "beauty.A", "Z"]
        writer = oiio.ImageOutput.create(str(source))
        self.assertTrue(writer.open(str(source), spec), writer.geterror())
        try:
            self.assertTrue(writer.write_image(data), writer.geterror())
        finally:
            writer.close()
        pipeline = ColorPipeline(self.snapshot(exposure=1.0, gamma=1.2))
        actual = ImageReader.read_frame(str(source), color_pipeline=pipeline)
        expected_rgb = pipeline.apply(data[:, :, :3], source_encoding="scene-linear")
        np.testing.assert_array_equal(np.rint(expected_rgb[:, :, ::-1] * 255).astype(np.uint8), actual[:, :, :3])
        self.assertTrue(np.all(actual[:, :, 3] == 63))

    def test_exposure_gamma_order_matches_renderer_before_ocio(self):
        source = np.array([[[.01, .1, .2], [-.05, .4, .6]]], dtype=np.float32)
        settings = self.snapshot(exposure=1.0, gamma=2.0)
        actual = ColorPipeline(settings).apply(source, source_encoding="scene-linear")
        expected = source * 2
        expected = np.where(expected >= 0, np.sqrt(np.maximum(expected, 0)), expected)
        config = ocio.Config.CreateFromFile(settings["configPath"])
        display = ocio.DisplayViewTransform(src="linear-in", display="test-display", view="test-view")
        pipeline = ocio.LegacyViewingPipeline()
        pipeline.setDisplayViewTransform(display)
        pipeline.setLooksOverrideEnabled(True)
        pipeline.setLooksOverride("")
        pipeline.getProcessor(config, config.getCurrentContext()).getDefaultCPUProcessor().applyRGB(expected)
        np.testing.assert_allclose(np.clip(expected, 0, 1), actual, atol=2e-6)

    def test_invalid_configuration_input_display_view_or_runtime_fails_explicitly(self):
        baseline = self.snapshot()
        for update in (dict(configPath=str(self.directory / "missing.ocio")), dict(input="missing"),
                       dict(display="missing"), dict(view="missing"), dict(configCacheId="changed")):
            with self.subTest(update=update), self.assertRaises(ColorConfigurationError):
                ColorPipeline(dict(baseline, **update))
        with patch.dict(sys.modules, {"PyOpenColorIO": None}), self.assertRaises(ColorConfigurationError):
            ColorPipeline(baseline)

    def test_snapshot_is_immutable_and_rejects_nonfinite_or_data_role(self):
        original = dict(mode="bypass", exposure=1.0)
        pipeline = ColorPipeline(original)
        original["exposure"] = 8.0
        self.assertEqual(1.0, pipeline.settings.exposure)
        with self.assertRaises(dataclasses.FrozenInstanceError):
            pipeline.settings.gamma = 2.0
        for invalid in (dict(exposure=float("nan")), dict(gamma=0), dict(gamma=True),
                        dict(sourceRole="data"), dict(sourceRole="unknown"), dict(schemaVersion=True)):
            with self.subTest(invalid=invalid), self.assertRaises(ColorConfigurationError):
                ColorPipeline(invalid)

    def test_known_rgb_data_aov_is_rejected_without_touching_beauty_or_alpha(self):
        from cgplay.export.utils.image_reader import _exr_color_channels
        for prefix in ("normal", "N", "velocity", "depth", "position", "crypto_object"):
            with self.subTest(prefix=prefix), self.assertRaises(ColorConfigurationError):
                _exr_color_channels([prefix + "." + name for name in "RGB"])
        self.assertEqual([0, 1, 2, 3], _exr_color_channels(["beauty.R", "beauty.G", "beauty.B", "beauty.A", "Z"]))
        self.assertEqual([0, 1, 2], _exr_color_channels(["R", "G", "B", "Z"]))

    def test_engine_honors_color_sidecar_and_writes_matching_sdr_bitstream_tags(self):
        from cgplay.export.export_engine import run_export
        source=self.directory/'source.png'
        Image.fromarray(np.full((32,32,3),64,dtype=np.uint8)).save(source)
        for encoding,transfer in [('srgb',13),('rec709',1)]:
            with self.subTest(encoding=encoding):
                snapshot=self.snapshot(outputEncoding=encoding)
                sidecar=self.directory/(encoding+'.json')
                sidecar.write_text(json.dumps(snapshot),encoding='utf-8')
                output=self.directory/(encoding+'.mp4')
                with contextlib.redirect_stdout(io.StringIO()):
                    result=run_export(str(source),str(output),24,0,0,color_settings_json=str(sidecar))
                self.assertEqual(0,result)
                with av.open(str(output)) as container:
                    stream=container.streams.video[0]
                    self.assertEqual(transfer,stream.codec_context.color_trc)
                    self.assertEqual(1,stream.codec_context.color_primaries)
                    self.assertEqual(1,stream.codec_context.colorspace)
                    frames=list(container.decode(video=0))
                    self.assertEqual(1,len(frames))
                    # The test OCIO view halves RGB. An ignored sidecar would
                    # keep 64; a double application would yield approximately16.
                    self.assertAlmostEqual(32,float(frames[0].to_ndarray(format='rgb24').mean()),delta=3)

    def test_float_composition_entry_preserves_headroom_negative_values_and_alpha(self):
        values=np.array([[[-.05,.18,4.0]]],dtype=np.float32)
        pipeline=ColorPipeline(dict(mode='bypass'))
        np.testing.assert_array_equal(values,pipeline.apply_float(values))
        path=self.directory/'float.exr'
        data=np.tile(np.array([-.05,.18,4.0,.25],dtype=np.float32),(4,4,1))
        spec=oiio.ImageSpec(4,4,4,oiio.FLOAT);spec.channelnames=['R','G','B','A']
        writer=oiio.ImageOutput.create(str(path))
        self.assertTrue(writer.open(str(path),spec))
        try:self.assertTrue(writer.write_image(data))
        finally:writer.close()
        frame=ImageReader.read_frame(str(path),color_pipeline=pipeline,output_float=True)
        self.assertEqual(np.float32,frame.dtype)
        np.testing.assert_allclose(frame,data[:,:,[2,1,0,3]],atol=1e-7)

    def test_normal_prores_export_keeps_more_than_eight_effective_bits(self):
        from cgplay.export.export_job import export_frames
        from cgplay.export.models.export_settings import ExportSettings
        source=self.directory/'gradient.exr'
        ramp=np.linspace(.05,.95,1024,dtype=np.float32)
        data=np.tile(ramp[None,:,None],(16,1,3))
        spec=oiio.ImageSpec(1024,16,3,oiio.FLOAT);spec.channelnames=['R','G','B']
        writer=oiio.ImageOutput.create(str(source))
        self.assertTrue(writer.open(str(source),spec))
        try:self.assertTrue(writer.write_image(data))
        finally:writer.close()
        target=self.directory/'gradient.mov'
        export_frames(str(source),ExportSettings(codec='prores_hq',output_path=str(target),output_format='mov'),
            start_frame=0,end_frame=0,color_settings=dict(mode='bypass'))
        with av.open(str(target)) as container:
            frame=next(container.decode(video=0))
            self.assertEqual('yuv422p10le',frame.format.name)
            luma=np.frombuffer(frame.planes[0],dtype='<u2').reshape(frame.height,frame.planes[0].line_size//2)[:,:frame.width]
            self.assertGreater(len(np.unique(luma[8])),400)

    def test_float_annotation_preserves_untouched_precision_and_real_alpha(self):
        from cgplay.export.export_job import _render_annotations
        frame=np.empty((96,256,4),dtype=np.float32)
        frame[:,:,:3]=np.linspace(.123456,.765432,256,dtype=np.float32)[None,:,None]
        frame[:,:,3]=.25
        original=frame.copy()
        note=dict(frame=0,type=1,color='#ff4000',author='CGPlay',comment='测试批注',
            points=[dict(x=8,y=32),dict(x=90,y=50)])
        _render_annotations(frame,0,[note])
        np.testing.assert_array_equal(original[70:,:,:],frame[70:,:,:])
        reference=np.zeros(frame.shape,dtype=np.uint8)
        _render_annotations(reference,0,[note])
        expected_alpha=.25+.75*reference[:,:,3].astype(np.float32)/255.0
        np.testing.assert_allclose(expected_alpha,frame[:,:,3],atol=1e-6)
        self.assertGreater(float(frame[:,:,3].max()),.8)
        self.assertFalse(np.array_equal(original[:60,:,:],frame[:60,:,:]))
        Image.fromarray(np.rint(np.clip(frame[:,:,[2,1,0,3]],0,1)*255).astype(np.uint8),'RGBA').save(self.directory/'float_chinese_annotation.png')

    def test_hdr_video_and_unsupported_display_fail_without_replacing_output(self):
        from cgplay.export.export_job import export_frames
        from cgplay.export.models.export_settings import ExportSettings
        source=self.directory/'tagged_hdr.mp4'
        with av.open(str(source),'w') as container:
            stream=container.add_stream('libx264',rate=24)
            stream.width=32;stream.height=32;stream.pix_fmt='yuv420p'
            stream.codec_context.color_primaries=9
            stream.codec_context.color_trc=16
            stream.codec_context.colorspace=9
            for i in range(3):
                frame=av.VideoFrame.from_ndarray(np.full((32,32,3),64+i,dtype=np.uint8),format='rgb24')
                for packet in stream.encode(frame):container.mux(packet)
            for packet in stream.encode():container.mux(packet)
        with av.open(str(source)) as container:
            self.assertEqual(16,container.streams.video[0].codec_context.color_trc)
        target=self.directory/'preserved.mp4';target.write_bytes(b'previous-export-preserved')
        before=hashlib.sha256(target.read_bytes()).hexdigest()
        settings=ExportSettings(output_path=str(target))
        with self.assertRaisesRegex(ValueError,'HDR/wide-gamut'):
            export_frames(str(source),settings,start_frame=0,end_frame=1)
        self.assertEqual(before,hashlib.sha256(target.read_bytes()).hexdigest())
        with self.assertRaisesRegex(ColorConfigurationError,'SDR'):
            export_frames(str(source),settings,color_settings=self.snapshot(outputEncoding='unsupported'))
        self.assertEqual(before,hashlib.sha256(target.read_bytes()).hexdigest())


if __name__ == "__main__":
    unittest.main()

"""Per-export immutable color settings; no global/default OCIO processor.

GUI jobs send mode=ocio or bypass explicitly. The legacy CLI's mode=auto keeps
ordinary sRGB images encoded and uses only a declared linear-to-sRGB fallback
for scene-linear images. It never silently selects an ACES view.
"""
from dataclasses import dataclass, fields
import math
import re

import numpy as np


class ColorConfigurationError(ValueError):
    """A requested color contract could not be honored; do not reader-fallback."""


def assert_color_layer(name):
    """Reject known data semantics even when a producer names components RGB.

    Unknown names cannot prove semantics. Callers with richer part/AOV metadata
    must send sourceRole=data/unknown rather than declaring those frames color.
    """
    data_names = {"n", "p", "z", "normal", "normals", "position", "depth",
                  "velocity", "motion", "motionvector", "motionvectors", "uv",
                  "id", "objectid", "materialid", "mask", "disparity"}
    tokens = re.split(r"[._/\s-]+", name.casefold())
    if any(token in data_names or token.startswith("crypto") for token in tokens):
        raise ColorConfigurationError(f"Selected EXR RGB layer is a data AOV: {name}")


@dataclass(frozen=True)
class ColorSettings:
    schemaVersion: int = 1
    mode: str = "auto"
    configPath: str = ""
    input: str = ""
    display: str = ""
    view: str = ""
    look: str = ""
    exposure: float = 0.0
    gamma: float = 1.0
    sourceRole: str = "color"
    configCacheId: str = ""
    outputEncoding: str = "srgb"

    @classmethod
    def from_mapping(cls, value=None):
        if isinstance(value, cls):
            result = value
        else:
            if value is None:
                value = {}
            if not isinstance(value, dict):
                raise ColorConfigurationError("Color settings must be a JSON object")
            unknown = set(value).difference(field.name for field in fields(cls))
            if unknown:
                raise ColorConfigurationError(f"Unknown color settings: {sorted(unknown)}")
            result = cls(**value)
        if type(result.schemaVersion) is not int or result.schemaVersion != 1:
            raise ColorConfigurationError("Unsupported color settings schema")
        for name in ("mode", "configPath", "input", "display", "view", "look", "sourceRole", "configCacheId", "outputEncoding"):
            if not isinstance(getattr(result, name), str):
                raise ColorConfigurationError(f"Color setting {name} must be a string")
        if result.mode not in ("auto", "bypass", "ocio"):
            raise ColorConfigurationError(f"Unknown color mode: {result.mode}")
        if result.outputEncoding not in ("srgb", "rec709", "source") or (result.mode == "ocio" and result.outputEncoding == "source"):
            raise ColorConfigurationError("Video export supports only the selected sRGB/Rec.709 SDR display targets; HDR/P3 output is not implemented")
        if result.sourceRole not in ("color", "data", "unknown"):
            raise ColorConfigurationError(f"Unknown source role: {result.sourceRole}")
        for name, low, high in (("exposure", -10.0, 10.0), ("gamma", 0.01, 4.0)):
            value = getattr(result, name)
            if type(value) not in (int, float) or not math.isfinite(value) or not low <= value <= high:
                raise ColorConfigurationError(f"Color setting {name} must be finite in [{low}, {high}]")
        if result.mode == "ocio" and not all((result.configPath, result.input, result.display, result.view)):
            raise ColorConfigurationError("OCIO mode requires configPath, input, display and view")
        return result


class ColorPipeline:
    """Construct one processor per job and never mutate its settings per frame."""

    def __init__(self, settings=None):
        self.settings = ColorSettings.from_mapping(settings)
        self._cpu = None
        if self.settings.sourceRole != "color":
            raise ColorConfigurationError("Data/unknown AOV cannot be exported through a color transform; select RGB color")
        if self.settings.mode == "ocio":
            self._cpu = self._create_processor()

    def _create_processor(self):
        settings = self.settings
        try:
            import PyOpenColorIO as ocio
        except ImportError as error:
            raise ColorConfigurationError("PyOpenColorIO is required by the selected export color settings") from error
        try:
            # Matches tlRender OCIOConfig::File for both files and ocio:// URIs.
            config = ocio.Config.CreateFromFile(settings.configPath)
            if settings.display not in tuple(config.getDisplays()):
                raise ColorConfigurationError(f"OCIO display does not exist: {settings.display}")
            if settings.view not in tuple(config.getViews(settings.display)):
                raise ColorConfigurationError(f"OCIO view does not exist: {settings.view}")
            if settings.configCacheId and settings.configCacheId != config.getCacheID():
                raise ColorConfigurationError("OCIO configuration changed or runtime config identity differs from the captured viewer state")
            transform = ocio.DisplayViewTransform(src=settings.input, display=settings.display, view=settings.view)
            pipeline = ocio.LegacyViewingPipeline()
            pipeline.setDisplayViewTransform(transform)
            # tlRender always enables the override, including an empty look.
            pipeline.setLooksOverrideEnabled(True)
            pipeline.setLooksOverride(settings.look)
            return pipeline.getProcessor(config, config.getCurrentContext()).getDefaultCPUProcessor()
        except ColorConfigurationError:
            raise
        except Exception as error:
            raise ColorConfigurationError(f"Cannot build requested OCIO export transform: {error}") from error

    def apply(self, rgb, *, source_encoding="display"):
        return np.clip(self.apply_float(rgb, source_encoding=source_encoding), 0.0, 1.0)

    def apply_float(self, rgb, *, source_encoding="display"):
        """Transform without clipping/quantizing, for composition before output."""
        if source_encoding not in ("display", "scene-linear"):
            raise ColorConfigurationError(f"Unknown source encoding: {source_encoding}")
        work = np.array(rgb, dtype=np.float32, order="C", copy=True)
        if work.ndim != 3 or work.shape[2] != 3 or not np.all(np.isfinite(work)):
            raise ColorConfigurationError("Color input must be finite HxWx3 RGB")
        # Matches tlRender's color brightness and Levels shader order. The
        # renderer passes 1 / levels.gamma to pow; do not invert it twice.
        work *= 2.0 ** self.settings.exposure
        if self.settings.gamma != 1.0:
            work = np.where(work >= 0.0,
                            np.power(np.maximum(work, 0.0), 1.0 / self.settings.gamma), work)
        if self._cpu is not None:
            self._cpu.applyRGB(work)
        elif self.settings.mode == "auto" and source_encoding == "scene-linear":
            positive = np.maximum(work, 0.0)
            work = np.where(positive <= 0.0031308, positive * 12.92,
                            1.055 * np.power(positive, 1.0 / 2.4) - 0.055)
        # Explicit bypass mirrors the viewer with OCIO disabled; no hidden ACES
        # or sRGB transform is inserted. Alpha never enters this RGB processor.
        if not np.all(np.isfinite(work)):
            raise ColorConfigurationError("Color transform produced non-finite pixels")
        return work

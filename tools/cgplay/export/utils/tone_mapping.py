"""Compatibility helpers for explicit, per-export color processing.

No OCIO configuration is selected or loaded at module import time.
"""
import numpy as np

from .color_pipeline import ColorPipeline


def apply_pipeline(img, *, color_pipeline=None, source_encoding="scene-linear"):
    return (color_pipeline or ColorPipeline()).apply(img, source_encoding=source_encoding)


def aces_filmic(x):
    """Optional analytic helper; never selected as an implicit export fallback."""
    x = np.maximum(x.astype(np.float32), 0.0)
    a, b, c, d, e = 2.51, 0.03, 2.43, 0.59, 0.14
    return (x * (a * x + b)) / (x * (c * x + d) + e)


def linear_to_srgb(img):
    return ColorPipeline().apply(img, source_encoding="scene-linear")

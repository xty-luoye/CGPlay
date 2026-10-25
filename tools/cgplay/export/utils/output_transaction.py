"""Protect inputs and replace an export target only after full finalization.

The caller owns the commit point. Closing an encoder must never commit a file,
because the same close operation is also used to release a failed encoder.
"""
import logging
import os
from pathlib import Path
import shutil
import tempfile


def _canonical(path):
    return os.path.normcase(os.path.realpath(os.path.abspath(os.fspath(path))))


def _identity(path):
    try:
        info = os.stat(path)
    except FileNotFoundError:
        return None
    return info.st_dev, info.st_ino


def _protected_files(paths):
    # Retain the original identity as well as the name. A source may be renamed
    # while encoding; its original hard link must remain protected at commit.
    return tuple((_canonical(path), _identity(path)) for path in paths if path)


def _check_distinct(path, protected):
    name = _canonical(path)
    identity = _identity(path)
    for source_name, original_identity in protected:
        if name == source_name or (
            identity is not None
            and identity in (original_identity, _identity(source_name))
        ):
            raise ValueError(f"Output would overwrite an input file: {path}")


def assert_distinct_output(path, protected_paths):
    """Reject path aliases, symlinks and existing hard links to protected files."""
    _check_distinct(path, _protected_files(protected_paths))


class OutputTransaction:
    """Write in an owned temporary directory, then atomically replace the target.

    An optional staging_dir is owned by the process supervisor, which may clean
    it after forcibly terminating Python. This class only removes its own child.
    staging_dir must be on the target volume so os.replace cannot become a copy.
    """

    def __init__(self, output_path, protected_paths, staging_dir=None):
        if not output_path or not os.fspath(output_path).strip():
            raise ValueError("Output path must not be empty")
        self.requested_path = os.fspath(output_path)
        self.destination = Path(_canonical(output_path))
        self.protected = _protected_files(protected_paths)
        self.staging_dir = Path(staging_dir) if staging_dir is not None else None
        self.directory = None
        self.staged_path = None
        self.committed = False

    def _validate_destination(self):
        # Also inspect the requested path again in case a symlink was retargeted
        # after construction. Only the resolved destination is ever replaced.
        _check_distinct(self.requested_path, self.protected)
        _check_distinct(self.destination, self.protected)
        if self.destination.exists() and not self.destination.is_file():
            raise ValueError(f"Output is not a regular file: {self.requested_path}")

    def __enter__(self):
        self._validate_destination()
        self.destination.parent.mkdir(parents=True, exist_ok=True)
        parent = self.staging_dir or self.destination.parent
        if not parent.is_dir():
            raise ValueError(f"Staging directory does not exist: {parent}")
        if os.stat(parent).st_dev != os.stat(self.destination.parent).st_dev:
            raise ValueError("Staging directory must be on the output volume")
        self.directory = Path(tempfile.mkdtemp(prefix=".cgplay-export-", dir=parent))
        # Preserve the extension for PyAV's muxer inference.
        self.staged_path = self.directory / Path(self.requested_path).name
        return self

    def commit(self):
        if self.committed:
            raise RuntimeError("Export output was already committed")
        if self.staged_path is None or not self.staged_path.is_file():
            raise RuntimeError("Export did not produce a staged output")
        if self.staged_path.stat().st_size == 0:
            raise RuntimeError("Export produced an empty staged output")
        self._validate_destination()
        # The reader and encoder must already be closed by the outer export job.
        with self.staged_path.open("r+b") as staged:
            os.fsync(staged.fileno())
        os.replace(self.staged_path, self.destination)
        self.committed = True

    def __exit__(self, exc_type, exc_value, traceback):
        if self.directory is not None:
            try:
                shutil.rmtree(self.directory)
            except OSError:
                # Preserve the real export error (or successful commit). A forced
                # process stop is handled by the supervisor's staging directory.
                logging.getLogger(__name__).warning(
                    "Could not remove export staging directory: %s",
                    self.directory,
                    exc_info=True,
                )
        return False

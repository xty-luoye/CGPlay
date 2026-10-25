from pathlib import Path
import importlib.util
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'tests/artifacts/runtime_overlay_1_0_8'
spec=importlib.util.spec_from_file_location('runtime_overlay_gate',ROOT/'tools/validate_runtime_overlay.py')
gate=importlib.util.module_from_spec(spec);spec.loader.exec_module(gate)


class RuntimeOverlayContracts(unittest.TestCase):
    def setUp(self):
        OUT.mkdir(parents=True,exist_ok=True)
        temporary=tempfile.TemporaryDirectory(dir=OUT);self.addCleanup(temporary.cleanup)
        self.base=Path(temporary.name);self.source=self.base/'source';self.source.mkdir()
        (self.source/'python.exe').write_bytes(b'fixture not executable');(self.source/'Lib').mkdir()
        self.destination=self.base/'runtime/python'

    def test_fresh_and_same_version(self):
        self.assertEqual(0,gate.validate(self.source,self.destination)['destinationFiles'])
        self.destination.mkdir(parents=True);(self.destination/'python.exe').write_bytes(b'old contents')
        self.assertEqual(0,gate.validate(self.source,self.destination)['obsoleteFiles'])

    def test_obsolete_native_metadata_or_extension_refused_without_mutation(self):
        for relative in ['Lib/site-packages/av/codec.cp311-win_amd64.pyd',
                         'Lib/site-packages/av-17.1.0.dist-info/METADATA','Lib/custom.py']:
            target=self.destination/relative;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(b'old runtime')
            with self.assertRaisesRegex(ValueError,'Refusing mixed runtime'):gate.validate(self.source,self.destination)
            self.assertEqual(b'old runtime',target.read_bytes());target.unlink()

    def test_bytecode_does_not_block(self):
        target=self.destination/'Lib/__pycache__/example.pyc';target.parent.mkdir(parents=True);target.write_bytes(b'generated')
        self.assertEqual(0,gate.validate(self.source,self.destination)['destinationFiles'])

    def test_sourceless_legacy_bytecode_is_not_treated_as_cache(self):
        for name in ['Lib/legacy.pyc','Lib/site-packages/legacy.pyo']:
            target=self.destination/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(b'legacy module')
            with self.assertRaisesRegex(ValueError,'Refusing mixed runtime'):gate.validate(self.source,self.destination)
            self.assertEqual(b'legacy module',target.read_bytes());target.unlink()

    def test_overlap_and_incomplete_sources(self):
        with self.assertRaisesRegex(ValueError,'overlap'):gate.validate(self.source,self.source)
        with self.assertRaisesRegex(ValueError,'overlap'):gate.validate(self.source,self.source/'nested')
        with self.assertRaisesRegex(ValueError,'incomplete'):gate.validate(self.base/'missing',self.destination)


if __name__=='__main__':unittest.main()

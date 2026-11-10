"""R12/R13 current Release objects, local SRT and fake provider; no network."""
import unittest
from tests.test_review_metadata_repairs import ROOT
from tests.product_object_probe import build_object_probe
class SubtitleRuntimeRepairs(unittest.TestCase):
 def test_cancel_and_real_scheduler_dialogue(self):
    main=(ROOT/'tests/review_repair_probes/subtitle_cancel.cpp').read_text(encoding='utf-8')
    build_object_probe(ROOT/'tests/artifacts/fix_review_20261101/stage29/subtitle',main)
if __name__=='__main__':unittest.main()

#!/usr/bin/env python3
"""Checks temporal alignment and local quality masks without a GPU."""
import sys
import unittest
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
from unittest.mock import patch

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools import eval_quality
from tools.eval_quality import local_masks, masked_psnr, target_counter, targeted_masks


class EvaluationTests(unittest.TestCase):
    def test_report_rejects_incomplete_alignment(self):
        black = np.zeros((8, 8, 3), dtype=np.float32)
        edged = black.copy()
        edged[:, 4:] = 255
        gt = [{"gen": False, "img": edged if i % 2 else black} for i in range(5)]
        real = [{"gen": False, "img": img, "idx": i}
                for i, img in zip((0, 2, 4), (black, edged, black))]
        generated = [{"gen": True, "img": edged, "idx": i, "t": 0.5} for i in (1, 3, 5)]
        test = real + generated

        def run(extra_args):
            out = StringIO()
            with patch.object(eval_quality, "load_dir", side_effect=[gt, test]), \
                 patch.object(eval_quality, "decode_counter", side_effect=[0, 1, 2, 3, 4, 0, 1, 2]), \
                 patch.object(sys, "argv", ["eval_quality.py", "gt", "test", "2", *extra_args]), \
                 redirect_stdout(out):
                status = eval_quality.main()
            return status, out.getvalue()

        status, report = run([])
        self.assertEqual(status, 1)
        self.assertIn("comparison coverage: 2/3 expected generated frames (3 dumped; minimum 3)", report)
        status, report = run(["--min-comparable", "2"])
        self.assertEqual(status, 0)
        self.assertIn("comparison coverage: 2/3 expected generated frames (3 dumped; minimum 2)", report)
        status, report = run(["--expected-generated", "11", "--min-comparable", "8"])
        self.assertEqual(status, 1)
        self.assertIn("comparison coverage: 2/11 expected generated frames (3 dumped; minimum 8)", report)

    def test_interpolation_and_extrapolation_alignment(self):
        for multiplier in (2, 3, 4):
            for step in range(1, multiplier):
                fraction = round(step / multiplier, 2)  # dump filenames keep two decimals
                expected = 7 * multiplier + step
                self.assertEqual(target_counter(7, fraction, multiplier), expected)
                self.assertEqual(target_counter(7, 1 + fraction, multiplier), expected)

    def test_masks_focus_on_changing_edges(self):
        prev = np.zeros((8, 8, 3), dtype=np.float32)
        nxt = prev.copy()
        nxt[:, 4:, :] = 255
        ref = nxt.copy()
        changing, edges = local_masks(prev, nxt, ref)
        self.assertFalse(changing[:, :4].any())
        self.assertTrue(changing[:, 4:].all())
        self.assertTrue(edges.any())
        self.assertFalse(edges[:, :3].any())
        self.assertEqual(masked_psnr(ref, ref, edges), 99.0)
        damaged = ref.copy()
        damaged[edges] = 0
        self.assertLess(masked_psnr(damaged, ref, edges), 20.0)

    def test_counter_is_not_counted_as_scene_motion(self):
        prev = np.zeros((180, 200, 3), dtype=np.float32)
        nxt = prev.copy()
        nxt[134:164, 16:160] = 255  # six-digit counter rectangle
        nxt[60:70, 175:185] = 255   # genuine scene motion
        changing, edges = local_masks(prev, nxt, nxt)
        self.assertFalse(changing[134:164, 16:160].any())
        self.assertFalse(edges[134:164, 16:160].any())
        self.assertTrue(changing[60:70, 175:185].all())
        self.assertTrue(edges[60:70, 175:185].any())

    def test_revealed_bar_and_rotation_masks(self):
        prev = np.zeros((200, 200, 3), dtype=np.float32)
        ref = prev.copy()
        nxt = prev.copy()
        prev[80:120, 100:110] = (242, 217, 51)
        ref[80:120, 110:120] = (242, 217, 51)
        nxt[80:120, 120:130] = (242, 217, 51)
        edges = np.zeros((200, 200), dtype=bool)
        edges[135, 48] = True  # rotating square center in top-down image
        edges[15, 180] = True
        revealed, rotating = targeted_masks(prev, nxt, ref, edges)
        self.assertTrue(revealed[90, 105])
        self.assertFalse(revealed[90, 115])
        self.assertTrue(rotating[135, 48])
        self.assertFalse(rotating[15, 180])


if __name__ == "__main__":
    unittest.main()

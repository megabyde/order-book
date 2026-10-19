#!/usr/bin/env python3

from __future__ import annotations

import contextlib
import io
import json
import math
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import hillclimb as h


class HarnessTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.build = self.root / "build"
        (self.build / "generators").mkdir(parents=True)
        (self.build / "generators/conan_toolchain.cmake").write_text("set(flags unchanged)\n")
        (self.root / "conan.lock").write_text("{}\n")
        self.cache = self.build / "CMakeCache.txt"
        self.cache.write_text(
            "CMAKE_BUILD_TYPE:STRING=Release\nENABLE_HARDENING:BOOL=ON\n"
            "ENABLE_LTO:BOOL=ON\nWARNINGS_AS_ERRORS:BOOL=ON\n"
            f"CMAKE_CXX_COMPILER:FILEPATH={sys.executable}\nCMAKE_CXX_FLAGS:STRING=-O3\n"
        )
        self.addCleanup(patch.stopall)
        patch.object(h, "ROOT", self.root).start()
        self.feed = self.root / "feed.csv"
        self.feed.write_text("header\ninput\n")
        self.baseline = self.executable("baseline.py", "print('first\\nsecond')\n")
        self.manifest = self.root / "manifest.json"
        self.assertEqual(
            self.invoke(
                "freeze",
                "--baseline",
                self.baseline,
                "--build-dir",
                self.build,
                "--manifest",
                self.manifest,
                "--timeout",
                ".3",
                "--case",
                f"target:target:csv:{self.feed}",
            ),
            0,
        )

    def executable(self, name: str, body: str) -> Path:
        path = self.root / name
        path.write_text(f"#!{sys.executable}\n{body}")
        path.chmod(0o755)
        return path

    def invoke(self, *arguments: str | Path) -> int:
        with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
            return h.main([str(argument) for argument in arguments])

    def run_candidate(self, body: str, name: str = "run") -> tuple[int, h.Record]:
        candidate = self.executable(f"{name}.py", body)
        out = self.root / name
        status = self.invoke(
            "run", "--manifest", self.manifest, "--candidate", candidate, "--out", out
        )
        return status, json.loads((out / "result.json").read_text())

    def assert_failure(self, status: int, result: h.Record, message: str) -> None:
        self.assertEqual(status, 1)
        self.assertFalse(result["success"])
        self.assertGreater(result["error_count"], 0)
        self.assertIn(message, " ".join(result["errors"]))
        self.assertNotIn("summary", result)

    def test_complete_pairs_and_observed_counts(self) -> None:
        status, result = self.run_candidate("print('first\\nsecond')\n")
        self.assertEqual(status, 0)
        manifest = json.loads(self.manifest.read_text())
        self.assertEqual(manifest["cases"][0]["input_counts"]["records"], 1)
        self.assertEqual(manifest["cases"][0]["expected_output"]["lines"], 2)
        samples = [item for item in result["observations"] if item["phase"] == "timing"]
        self.assertEqual(len(samples), 16)
        for pair in range(8):
            expected = ["reference", "candidate"] if pair % 2 == 0 else ["candidate", "reference"]
            self.assertEqual(
                [item["binary"] for item in samples[2 * pair : 2 * pair + 2]], expected
            )
        self.assertTrue(all(item["exit_code"] == 0 for item in samples))
        self.assertTrue(all(item["user_seconds"] >= 0 for item in samples))
        self.assertTrue(all("voluntary_context_switches" in item for item in samples))
        self.assertTrue(all("output" not in item for item in samples))

    def test_wrong_output_and_same_length_reordering(self) -> None:
        for index, output in enumerate(("wrong", "second\\nfirst")):
            with self.subTest(output=output):
                status, result = self.run_candidate(f"print('{output}')\n", f"wrong{index}")
                self.assert_failure(status, result, "wrong output")
                self.assertFalse(any(item["phase"] == "timing" for item in result["observations"]))

    def test_nonzero_exit(self) -> None:
        status, result = self.run_candidate(
            "import sys\nprint('failure', file=sys.stderr)\nsys.exit(7)\n"
        )
        self.assert_failure(status, result, "process failed")
        sample = result["observations"][-1]
        self.assertEqual(sample["exit_code"], 7)
        self.assertIn("failure", sample["stderr"])

    def test_spawn_failure(self) -> None:
        candidate = self.root / "invalid"
        candidate.write_text("not executable format\n")
        candidate.chmod(0o755)
        out = self.root / "spawn"
        status = self.invoke(
            "run", "--manifest", self.manifest, "--candidate", candidate, "--out", out
        )
        result = json.loads((out / "result.json").read_text())
        self.assert_failure(status, result, "Exec format error")
        self.assertIsNone(result["observations"][-1]["exit_code"])

    def test_timeout_kills_child(self) -> None:
        status, result = self.run_candidate("import time\ntime.sleep(2)\n")
        self.assert_failure(status, result, "process failed")
        sample = result["observations"][-1]
        self.assertTrue(sample["timeout"])
        self.assertEqual(sample["exit_code"], -9)
        self.assertLess(sample["elapsed_seconds"], 1.5)

    def test_timing_failure_keeps_raw_samples_without_summary(self) -> None:
        counter = self.root / "calls"
        body = (
            "from pathlib import Path\n"
            f"p = Path({str(counter)!r})\n"
            "n = int(p.read_text()) if p.exists() else 0\np.write_text(str(n + 1))\n"
            "print('first\\nsecond')\nraise SystemExit(7 if n == 2 else 0)\n"
        )
        status, result = self.run_candidate(body)
        self.assert_failure(status, result, "process failed")
        samples = [item for item in result["observations"] if item["phase"] == "timing"]
        self.assertGreater(len(samples), 0)
        self.assertLess(len(samples), 16)
        self.assertEqual(samples[-1]["exit_code"], 7)

    def test_input_mutation_after_validation_is_rejected(self) -> None:
        status, result = self.run_candidate(
            f"from pathlib import Path\nPath({str(self.feed)!r}).write_text('mutated')\n"
            "print('first\\nsecond')\n"
        )
        self.assert_failure(status, result, "frozen file changed")

    def test_binary_source_mutation_is_rejected(self) -> None:
        source = self.root / "run.py"
        status, result = self.run_candidate(
            "from pathlib import Path\n"
            f"with Path({str(source)!r}).open('a') as out: out.write('# changed\\n')\n"
            "print('first\\nsecond')\n"
        )
        self.assert_failure(status, result, "binary identity changed")

    def test_build_policy_change_is_rejected(self) -> None:
        self.cache.write_text(self.cache.read_text().replace("-O3", "-O2"))
        status, result = self.run_candidate("print('first\\nsecond')\n")
        self.assert_failure(status, result, "build policy changed")
        self.assertEqual(result["observations"], [])

    def test_input_mutation_before_run_is_rejected(self) -> None:
        self.feed.write_text("header\nchanged\n")
        status, result = self.run_candidate("print('first\\nsecond')\n")
        self.assert_failure(status, result, "frozen file changed")

    def test_existing_output_is_preserved(self) -> None:
        self.run_candidate("print('first\\nsecond')\n")
        saved = (self.root / "run/result.json").read_bytes()
        status = self.invoke(
            "run",
            "--manifest",
            self.manifest,
            "--candidate",
            self.baseline,
            "--out",
            self.root / "run",
        )
        self.assertEqual(status, 1)
        self.assertEqual((self.root / "run/result.json").read_bytes(), saved)

    def test_experiment_lock_rejects_overlap(self) -> None:
        with h.experiment_lock(self.manifest):
            status, result = self.run_candidate("print('first\\nsecond')\n")
        self.assert_failure(status, result, "BlockingIOError")
        self.assertEqual(result["observations"], [])

    def test_against_keeps_original_baseline(self) -> None:
        original = self.manifest.read_bytes()
        incumbent = self.executable("incumbent", "print('first\\nsecond')\n")
        out = self.root / "against"
        status = self.invoke(
            "run",
            "--manifest",
            self.manifest,
            "--candidate",
            self.baseline,
            "--against",
            incumbent,
            "--out",
            out,
        )
        self.assertEqual(status, 0)
        result = json.loads((out / "result.json").read_text())
        self.assertEqual(result["binaries"]["reference"]["path"], str(incumbent))
        self.assertEqual(self.manifest.read_bytes(), original)


class StatisticsTest(unittest.TestCase):
    def test_exact_one_sided_sign_test_and_ties(self) -> None:
        self.assertEqual(h.sign_test([0.8] * 8, "faster")["p"], 1 / 256)
        self.assertEqual(h.sign_test([0.8] * 7 + [1.2], "faster")["p"], 9 / 256)
        self.assertEqual(h.sign_test([0.8] * 6 + [1.2] * 2, "faster")["p"], 37 / 256)
        self.assertEqual(h.sign_test([0.8] * 3 + [1.0] * 5, "faster")["p"], 1 / 8)
        self.assertEqual(h.sign_test([1.0] * 8, "faster")["p"], 1)
        self.assertEqual(h.sign_test([1.2] * 8, "slower")["p"], 1 / 256)

    def test_ratio_of_medians_differs_from_median_of_ratios(self) -> None:
        reference = [1.0, 1.0, 1.0, 1.0, 10.0, 10.0, 10.0, 10.0]
        candidate = [1.0, 1.0, 1.0, 10.0, 1.0, 1.0, 1.0, 10.0]
        samples = [
            {
                "phase": "timing",
                "case": "target",
                "pair": pair,
                "binary": label,
                "elapsed_seconds": times[pair],
            }
            for pair in range(8)
            for label, times in (("reference", reference), ("candidate", candidate))
        ]
        summary = h.summarize([{"name": "target", "role": "target"}], samples)
        case = summary["cases"]["target"]
        self.assertEqual(case["candidate_median_seconds"], 1.0)
        self.assertEqual(case["reference_median_seconds"], 5.5)
        self.assertEqual(case["paired_median_ratio"], 1.0)
        self.assertAlmostEqual(case["median_ratio"], 1.0 / 5.5)
        self.assertAlmostEqual(summary["target_score"], 1.0 / 5.5)

    def test_guard_excluded_from_metric_but_blocks_acceptance(self) -> None:
        cases = [
            {"name": "csv", "role": "target"},
            {"name": "itch", "role": "target"},
            {"name": "guard", "role": "guard"},
        ]
        samples = [
            {
                "phase": "timing",
                "case": case["name"],
                "pair": pair,
                "binary": label,
                "elapsed_seconds": ratio if label == "candidate" else 1.0,
            }
            for case, ratio in zip(cases, [0.5, 0.8, 2.0], strict=True)
            for pair in range(8)
            for label in ("reference", "candidate")
        ]
        result = h.summarize(cases, samples)
        self.assertAlmostEqual(result["target_score"], math.sqrt(0.4))
        self.assertTrue(result["metric_win"])
        self.assertFalse(result["eligible"])
        self.assertEqual(result["requires_investigation"], ["guard"])
        self.assertTrue(result["cases"]["guard"]["confirmed_regression"])
        with self.assertRaisesRegex(ValueError, "incomplete"):
            h.summarize(cases, samples[:-1])


if __name__ == "__main__":
    unittest.main()

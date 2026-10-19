#!/usr/bin/env python3
"""Freeze replay inputs, compare eight paired CLI runs, or collect separate Linux perf data."""

from __future__ import annotations

import argparse
import contextlib
import fcntl
import hashlib
import json
import math
import os
import platform
import shutil
import signal
import statistics
import struct
import subprocess
import sys
import tempfile
import threading
import time
from collections.abc import Iterator
from datetime import UTC, datetime
from pathlib import Path
from typing import Any, BinaryIO

ROOT = Path(__file__).resolve().parents[1]
HARNESS = Path(__file__).resolve()
PAIRS = 8
Record = dict[str, Any]


def digest(path: Path | str) -> str:
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def identity(path: Path | str) -> Record:
    path = Path(path).resolve(strict=True)
    return {"path": str(path), "sha256": digest(path)}


def write_json(path: Path, value: Record) -> None:
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def build_policy(build_dir: Path) -> Record:
    cache = {}
    for line in (build_dir / "CMakeCache.txt").read_text().splitlines():
        if "=" not in line or line.startswith(("#", "//")):
            continue
        key, value = line.split("=", 1)
        key = key.split(":", 1)[0]
        if (
            key in {"ENABLE_HARDENING", "ENABLE_LTO", "WARNINGS_AS_ERRORS", "CMAKE_BUILD_TYPE"}
            or key.startswith(("CMAKE_CXX_COMPILER", "CMAKE_CXX_FLAGS", "CMAKE_EXE_LINKER_FLAGS"))
        ) and not key.endswith("-ADVANCED"):
            cache[key] = value
    required = {"ENABLE_HARDENING": "ON", "ENABLE_LTO": "ON", "WARNINGS_AS_ERRORS": "ON"}
    if any(cache.get(key) != value for key, value in required.items()):
        raise ValueError("build must retain hardening, LTO and warnings-as-errors")
    if cache.get("CMAKE_BUILD_TYPE") != "Release":
        raise ValueError("build must use Release")
    compiler = identity(cache["CMAKE_CXX_COMPILER"])
    compiler["version"] = subprocess.check_output(
        [compiler["path"], "--version"], text=True, timeout=10
    )
    files = [ROOT / "conan.lock", build_dir / "generators/conan_toolchain.cmake"]
    return {
        "build_dir": str(build_dir),
        "cache": cache,
        "compiler": compiler,
        "files": [identity(path) for path in files],
    }


def input_counts(path: Path, format_name: str) -> Record:
    count = 0
    with path.open("rb") as stream:
        if format_name == "csv":
            next(stream, None)
            count = sum(bool(line.strip()) for line in stream)
        else:
            while prefix := stream.read(2):
                if len(prefix) != 2:
                    raise ValueError(f"truncated ITCH prefix in {path}")
                size = struct.unpack(">H", prefix)[0]
                if size == 0 or len(stream.read(size)) != size:
                    raise ValueError(f"truncated or empty ITCH frame in {path}")
                count += 1
    return {
        "bytes": path.stat().st_size,
        "records": count,
        "record_unit": "CSV rows excluding header" if format_name == "csv" else "ITCH frames",
    }


def command(binary: Path, case: Record) -> list[str]:
    return [str(binary), f"--format={case['format']}", case["input"]["path"]]


def execute(
    argv: list[str],
    timeout: float,
    observations: list[Record],
    *,
    stdout: int | BinaryIO = subprocess.DEVNULL,
    **labels: object,
) -> Record:
    observation = {**labels, "command": argv, "timeout": False, "exit_code": None}
    observations.append(observation)
    with tempfile.TemporaryFile() as errors:
        started = time.perf_counter()
        with subprocess.Popen(
            argv, stdout=stdout, stderr=errors, start_new_session=True
        ) as process:

            def expire() -> None:
                observation["timeout"] = True
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(process.pid, signal.SIGKILL)

            timer = threading.Timer(max(0, timeout - (time.perf_counter() - started)), expire)
            timer.daemon = True
            timer.start()
            try:
                _, status, usage = os.wait4(process.pid, 0)
                process.returncode = os.waitstatus_to_exitcode(status)
            finally:
                timer.cancel()
                timer.join()
            observation.update(
                elapsed_seconds=time.perf_counter() - started,
                exit_code=process.returncode,
                user_seconds=usage.ru_utime,
                system_seconds=usage.ru_stime,
                voluntary_context_switches=usage.ru_nvcsw,
                involuntary_context_switches=usage.ru_nivcsw,
            )
        errors.seek(0)
        observation["stderr"] = errors.read(4096).decode(errors="replace")
    if observation["timeout"] or observation["exit_code"] != 0:
        raise RuntimeError(f"process failed: {observation}")
    return observation


def validate(
    binary: Path,
    case: Record,
    timeout: float,
    observations: list[Record],
    label: str,
) -> Record:
    with tempfile.TemporaryFile() as output:
        execute(
            command(binary, case),
            timeout,
            observations,
            phase="validation",
            binary=label,
            case=case["name"],
            stdout=output,
        )
        output.seek(0)
        hashed, size, lines, last = hashlib.sha256(), 0, 0, b""
        while chunk := output.read(1024 * 1024):
            hashed.update(chunk)
            size += len(chunk)
            lines += chunk.count(b"\n")
            last = chunk[-1:]
        actual = {
            "sha256": hashed.hexdigest(),
            "bytes": size,
            "lines": lines + int(bool(last) and last != b"\n"),
        }
    observations[-1]["output"] = actual
    if "expected_output" in case and actual != case["expected_output"]:
        raise ValueError(f"wrong output for {label}/{case['name']}: {actual}")
    return actual


def sign_test(ratios: list[float], direction: str) -> Record:
    values = [ratio for ratio in ratios if ratio != 1]
    wins = sum(ratio < 1 if direction == "faster" else ratio > 1 for ratio in values)
    n = len(values)
    return {
        "wins": wins,
        "non_ties": n,
        "p": sum(math.comb(n, k) for k in range(wins, n + 1)) / 2**n,
    }


def summarize(cases: list[Record], observations: list[Record]) -> Record:
    summaries, target_pairs = {}, [[] for _ in range(PAIRS)]
    for case in cases:
        ratios, reference_times, candidate_times = [], [], []
        for pair in range(PAIRS):
            samples = {
                sample["binary"]: sample["elapsed_seconds"]
                for sample in observations
                if sample.get("phase") == "timing"
                and sample["case"] == case["name"]
                and sample["pair"] == pair
            }
            if samples.keys() != {"reference", "candidate"}:
                raise ValueError("incomplete paired observations; no survivor summary")
            ratios.append(samples["candidate"] / samples["reference"])
            reference_times.append(samples["reference"])
            candidate_times.append(samples["candidate"])
            if case["role"] == "target":
                target_pairs[pair].append(ratios[-1])
        reference_median = statistics.median(reference_times)
        candidate_median = statistics.median(candidate_times)
        median = candidate_median / reference_median
        regression = sign_test(ratios, "slower")
        summaries[case["name"]] = {
            "role": case["role"],
            "ratios": ratios,
            "median_ratio": median,
            "reference_median_seconds": reference_median,
            "candidate_median_seconds": candidate_median,
            "paired_median_ratio": statistics.median(ratios),
            "regression_sign_test": regression,
            "confirmed_regression": median > 1.05 and regression["p"] <= 0.05,
        }
    score = statistics.geometric_mean(
        item["median_ratio"] for item in summaries.values() if item["role"] == "target"
    )
    paired = [statistics.geometric_mean(values) for values in target_pairs]
    test = sign_test(paired, "faster")
    investigations = [name for name, item in summaries.items() if item["median_ratio"] > 1.05]
    return {
        "cases": summaries,
        "target_score": score,
        "paired_target_ratios": paired,
        "target_sign_test": test,
        "metric_win": score < 1 and test["p"] <= 0.05,
        "requires_investigation": investigations,
        "eligible": score < 1 and test["p"] <= 0.05 and not investigations,
        "target_at_most_075": score <= 0.75,
    }


@contextlib.contextmanager
def experiment_lock(manifest: Path) -> Iterator[None]:
    with manifest.with_suffix(manifest.suffix + ".lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield


def verify_manifest(manifest: Record) -> None:
    if manifest["harness"] != identity(HARNESS):
        raise ValueError("harness identity changed; freeze a new experiment")
    if manifest["build_policy"] != build_policy(Path(manifest["build_policy"]["build_dir"])):
        raise ValueError("build policy changed")
    for item in [manifest["baseline"], *(case["input"] for case in manifest["cases"])]:
        if identity(item["path"]) != item:
            raise ValueError(f"frozen file changed: {item['path']}")


def snapshot(source: Path, destination: Path) -> Record:
    before = identity(source)
    shutil.copy2(before["path"], destination)
    destination.chmod(0o555)
    if digest(destination) != before["sha256"]:
        raise ValueError(f"binary changed while copying: {source}")
    return before


def freeze(args: argparse.Namespace, result: Record) -> None:
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    with experiment_lock(args.manifest):
        if args.manifest.exists():
            raise ValueError("manifest already exists")
        cases = []
        for specification in args.case:
            role, name, format_name, filename = specification.split(":", 3)
            if role not in {"target", "guard"} or format_name not in {"csv", "itch"}:
                raise ValueError(f"invalid case: {specification}")
            if not name or name in {case["name"] for case in cases}:
                raise ValueError("case names must be nonempty and unique")
            path = Path(filename).resolve(strict=True)
            cases.append(
                {
                    "name": name,
                    "role": role,
                    "format": format_name,
                    "input": identity(path),
                    "input_counts": input_counts(path, format_name),
                }
            )
        if not any(case["role"] == "target" for case in cases):
            raise ValueError("at least one target case required")
        baseline = args.manifest.with_suffix(".baseline")
        with baseline.open("xb"):
            pass
        source = snapshot(args.baseline, baseline)
        manifest = {
            "schema": 1,
            "pairs": PAIRS,
            "timeout_seconds": args.timeout,
            "harness": identity(HARNESS),
            "baseline": identity(baseline),
            "baseline_source": source,
            "cases": cases,
            "build_policy": build_policy(args.build_dir.resolve()),
        }
        for case in cases:
            case["expected_output"] = validate(
                baseline, case, args.timeout, result["observations"], "baseline"
            )
        verify_manifest(manifest)
        if identity(source["path"]) != source:
            raise ValueError("baseline source changed during freeze")
        with args.manifest.open("x") as output:
            json.dump(manifest, output, indent=2, sort_keys=True)
            output.write("\n")
        result["manifest"] = str(args.manifest)
        result["cases"] = cases


def profile(
    args: argparse.Namespace,
    manifest: Record,
    binaries: dict[str, Path],
    result: Record,
) -> None:
    result["perf_version"] = subprocess.check_output(["perf", "--version"], text=True, timeout=10)
    for case in manifest["cases"]:
        for mode in ("record", "stat"):
            destination = args.out / f"{case['name']}.{mode}"
            flags = (
                ["-e", "cpu-clock", "-g"]
                if mode == "record"
                else [
                    "-e",
                    "task-clock,cycles,instructions,branches,branch-misses,cache-references,"
                    "cache-misses,context-switches",
                    "-x,",
                ]
            )
            argv = [
                "perf",
                mode,
                *flags,
                "-o",
                str(destination),
                "--",
                *command(binaries["candidate"], case),
            ]
            execute(
                argv,
                manifest["timeout_seconds"],
                result["observations"],
                phase="profile",
                case=case["name"],
                mode=mode,
            )


def measure(args: argparse.Namespace, result: Record) -> None:
    with experiment_lock(args.manifest):
        manifest_identity = identity(args.manifest)
        manifest = json.loads(args.manifest.read_text())
        result["manifest"] = manifest_identity
        result["cases"] = manifest["cases"]
        verify_manifest(manifest)
        sources = {
            "candidate": args.candidate,
            "reference": getattr(args, "against", None) or Path(manifest["baseline"]["path"]),
        }
        binaries = {label: args.out / label for label in sources}
        originals = {label: snapshot(source, binaries[label]) for label, source in sources.items()}
        result["binaries"] = originals
        try:
            for label, binary in binaries.items():
                for case in manifest["cases"]:
                    validate(
                        binary, case, manifest["timeout_seconds"], result["observations"], label
                    )
            if args.action == "profile":
                profile(args, manifest, binaries, result)
            else:
                for pair in range(PAIRS):
                    order = (
                        ["reference", "candidate"] if pair % 2 == 0 else ["candidate", "reference"]
                    )
                    for case in manifest["cases"]:
                        for label in order:
                            execute(
                                command(binaries[label], case),
                                manifest["timeout_seconds"],
                                result["observations"],
                                phase="timing",
                                binary=label,
                                case=case["name"],
                                pair=pair,
                                order=order,
                            )
        finally:
            verify_manifest(manifest)
            for label, original in originals.items():
                if (
                    identity(original["path"]) != original
                    or digest(binaries[label]) != original["sha256"]
                ):
                    raise ValueError(f"binary identity changed during measurement: {label}")
            if identity(args.manifest) != manifest_identity:
                raise ValueError("manifest changed during measurement")
        if args.action == "run":
            result["summary"] = summarize(manifest["cases"], result["observations"])


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest="action", required=True)
    frozen = actions.add_parser("freeze")
    frozen.add_argument("--baseline", type=Path, required=True)
    frozen.add_argument("--build-dir", type=Path, default=ROOT / "build/release")
    frozen.add_argument(
        "--case", action="append", required=True, help="target|guard:NAME:csv|itch:PATH"
    )
    frozen.add_argument("--pairs", type=int, choices=[PAIRS], default=PAIRS)
    frozen.add_argument("--timeout", type=float, default=300)
    for action in (frozen, actions.add_parser("run"), actions.add_parser("profile")):
        action.add_argument("--manifest", type=Path, required=True)
        if action is not frozen:
            action.add_argument("--candidate", type=Path, required=True)
            action.add_argument("--out", type=Path, required=True)
            if action.prog.endswith("run"):
                action.add_argument("--against", type=Path)
    args = parser.parse_args(argv)
    args.manifest = args.manifest.resolve()
    if args.action == "freeze" and (not math.isfinite(args.timeout) or args.timeout <= 0):
        parser.error("--timeout must be finite and positive")
    result = {
        "action": args.action,
        "started_utc": datetime.now(UTC).isoformat(),
        "success": False,
        "error_count": 0,
        "errors": [],
        "observations": [],
        "host": {
            "platform": platform.platform(),
            "python": sys.version,
            "affinity": sorted(os.sched_getaffinity(0)),
        },
    }
    destination = args.manifest.with_suffix(".freeze-result.json")
    try:
        if args.action != "freeze":
            args.out = args.out.resolve()
            args.out.mkdir(parents=True, exist_ok=False)
            destination = args.out / "result.json"
        else:
            destination.parent.mkdir(parents=True, exist_ok=True)
        with destination.open("x"):
            pass
    except OSError as error:
        print(f"cannot reserve result: {error}", file=sys.stderr)
        return 1
    try:
        if args.action == "freeze":
            freeze(args, result)
        else:
            measure(args, result)
        result["success"] = True
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.SubprocessError) as error:
        result["error_count"] += 1
        result["errors"].append(f"{type(error).__name__}: {error}")
        print(result["errors"][-1], file=sys.stderr)
    result["finished_utc"] = datetime.now(UTC).isoformat()
    write_json(destination, result)
    report = {
        "result": str(destination),
        "success": result["success"],
        "error_count": result["error_count"],
    }
    if "summary" in result:
        report.update(
            target_score=result["summary"]["target_score"],
            target_sign_p=result["summary"]["target_sign_test"]["p"],
        )
    report["validation_counts"] = [
        {
            "case": case["name"],
            "input_records": case["input_counts"]["records"],
            "observed_outputs": [
                {"binary": item["binary"], "lines": item["output"]["lines"]}
                for item in result["observations"]
                if item.get("phase") == "validation"
                and item.get("case") == case["name"]
                and "output" in item
            ],
        }
        for case in result.get("cases", [])
    ]
    print(json.dumps(report))
    return int(not result["success"])


if __name__ == "__main__":
    sys.exit(main())

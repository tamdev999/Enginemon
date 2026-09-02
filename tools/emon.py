#!/usr/bin/env python3
"""
emon.py  —  Enginemon canonical build + test CLI

Delegates all compilation to CMake (via CMakePresets.json) and all test
execution to CTest (via test presets in CMakePresets.json).  Does not
implement process supervision, timeout handling, or test scheduling.

Usage:
    python tools/emon.py build [--preset NAME] [--clean] [--configure]
    python tools/emon.py test  [--preset NAME]
    python tools/emon.py verify [--rom PATH] [--no-build]
    python tools/emon.py status

Default toolchain: clang-cl + Ninja (build_clang/).
Secondary toolchain: MSVC + MSBuild (build/).  Use --preset msvc-all.

Build presets:   all (default), engine, compiler, corpus, oracle, smoke, save, msvc-all
Test presets:    all (default), engine, compiler, corpus, oracle, standalone, save, msvc-all

Environment:
    ENGINEMON_ROM          Path to Crystal ROM (used by verify and test ROM presets)
    ENGINEMON_SCCACHE=1    Enable sccache compiler cache (clang-cl only)
"""

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
REPO_ROOT  = Path(__file__).resolve().parent.parent
BUILD_DIR  = REPO_ROOT / "build_clang"   # default: clang-cl build
BUILD_DIR_MSVC = REPO_ROOT / "build"     # secondary: MSVC build
LOG_DIR    = REPO_ROOT / "build" / "emon_logs"  # logs always go to build/

CMAKE = Path(r"C:\Program Files\CMake\bin\cmake.exe")
CTEST = Path(r"C:\Program Files\CMake\bin\ctest.exe")

# ---------------------------------------------------------------------------
# Utilities
# ---------------------------------------------------------------------------

def find_rom() -> Path | None:
    """Locate a Crystal ROM in references/. Returns None if not found."""
    rom_env = os.environ.get("ENGINEMON_ROM")
    if rom_env:
        p = Path(rom_env)
        return p if p.exists() else None
    refs = REPO_ROOT / "references"
    if refs.is_dir():
        for f in sorted(refs.iterdir()):
            if f.suffix.lower() in (".gbc", ".gb"):
                return f
    return None


def ensure_log_dir():
    LOG_DIR.mkdir(parents=True, exist_ok=True)


def run(cmd: list[str], log_path: Path | None = None, cwd: Path | None = None) -> int:
    """
    Run a command.  If log_path is given, tee stdout+stderr there while also
    streaming to the terminal.  Returns exit code.
    """
    if cwd is None:
        cwd = REPO_ROOT

    print(f"  $ {' '.join(str(c) for c in cmd)}")

    if log_path:
        log_path.parent.mkdir(parents=True, exist_ok=True)
        with open(log_path, "w", encoding="utf-8", errors="replace") as log:
            proc = subprocess.Popen(
                cmd, cwd=cwd,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, encoding="utf-8", errors="replace"
            )
            for line in proc.stdout:
                sys.stdout.write(line)
                log.write(line)
            proc.wait()
        return proc.returncode
    else:
        return subprocess.call(cmd, cwd=cwd)


def first_error(log_path: Path) -> str:
    """Return the first error line from a log file."""
    if not log_path or not log_path.exists():
        return ""
    with open(log_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            stripped = line.strip()
            if any(pat in stripped for pat in ("error C", "error LNK", "FAILED", ": error ")):
                return stripped[:200]
    return ""


def section(title: str):
    print(f"\n{'=' * 60}")
    print(f"  {title}")
    print(f"{'=' * 60}")


def ok(msg: str):
    print(f"  \033[32mPASS\033[0m  {msg}")


def fail(msg: str):
    print(f"  \033[31mFAIL\033[0m  {msg}", file=sys.stderr)


# ---------------------------------------------------------------------------
# Subcommands
# ---------------------------------------------------------------------------

def cmd_build(preset: str = "all", clean: bool = False, configure: bool = False) -> int:
    section(f"Build  [preset: {preset}]")
    ensure_log_dir()

    if not CMAKE.exists():
        fail(f"CMake not found: {CMAKE}")
        return 1

    # Configure if build dir missing or explicitly requested
    if configure or not (BUILD_DIR / "CMakeCache.txt").exists():
        print("  Configuring...")
        rom = find_rom()
        rom_arg = f"-DENGINEMON_ROM_PATH={rom}" if rom else ""
        cfg_cmd = [str(CMAKE), "--preset", "default"]
        if rom_arg:
            cfg_cmd.append(rom_arg)
        rc = run(cfg_cmd, LOG_DIR / "configure.log")
        if rc != 0:
            fail(f"Configure failed (exit {rc})  log: {LOG_DIR / 'configure.log'}")
            return 1    if clean:
        print("  Cleaning...")
        run([str(CMAKE), "--build", "--preset", preset, "--target", "clean"],
            LOG_DIR / "clean.log")

    t0 = time.perf_counter()
    log_path = LOG_DIR / f"build_{preset}.log"
    rc = run([str(CMAKE), "--build", "--preset", preset], log_path)
    elapsed = time.perf_counter() - t0

    if rc == 0:
        ok(f"build complete in {elapsed:.1f}s  log: {log_path}")
    else:
        err = first_error(log_path)
        fail(f"build failed (exit {rc}) in {elapsed:.1f}s")
        if err:
            print(f"        {err}", file=sys.stderr)
        fail(f"full log: {log_path}")

    return rc


def cmd_test(preset: str = "all", rom: Path | None = None) -> int:
    section(f"Test  [preset: {preset}]")
    ensure_log_dir()

    if not CTEST.exists():
        fail(f"ctest not found: {CTEST}")
        return 1

    # Ensure ROM is in cache if needed
    if rom is None:
        rom = find_rom()
    if rom and (BUILD_DIR / "CMakeCache.txt").exists():
        subprocess.run(
            [str(CMAKE), "-B", str(BUILD_DIR), f"-DENGINEMON_ROM_PATH={rom}"],
            capture_output=True
        )
    t0 = time.perf_counter()
    log_path = LOG_DIR / f"test_{preset}.log"
    rc = run(
        [str(CTEST), "--preset", preset, "--test-dir", str(BUILD_DIR),
         "--output-log", str(log_path), "--output-on-failure"],
        cwd=REPO_ROOT
    )
    elapsed = time.perf_counter() - t0

    if rc == 0:
        ok(f"all tests passed in {elapsed:.1f}s  log: {log_path}")
    else:
        fail(f"tests failed (exit {rc}) in {elapsed:.1f}s")
        fail(f"full log: {log_path}")

    return rc


def cmd_verify(rom: Path | None = None, no_build: bool = False) -> int:
    section("Verify  [build + test all + smoke]")
    ensure_log_dir()

    if rom is None:
        rom = find_rom()
    if rom is None:
        fail("No Crystal ROM found.  Set ENGINEMON_ROM or place .gbc in references/")
        return 1

    print(f"  ROM: {rom}")
    overall = 0

    # Build
    if not no_build:
        rc = cmd_build("all")
        if rc != 0:
            return rc

    # Test
    rc = cmd_test("all", rom)
    if rc != 0:
        overall = rc

    # Smoke: compile a fresh package and run emon_smoke
    section("Smoke")
    # clang build: binaries at build_clang/tools/  (Ninja flat layout)
    # MSVC build:  binaries at build/tools/Release/
    tools_dir = BUILD_DIR / "tools"
    if not (tools_dir / "emon_smoke.exe").exists():
        tools_dir = BUILD_DIR / "tools" / "Release"
    smoke_exe   = tools_dir / "emon_smoke.exe"
    compile_exe = tools_dir / "emon_compile.exe"

    import tempfile
    smoke_pkg = Path(tempfile.mktemp(suffix=".emon"))
    try:
        if not compile_exe.exists() or not smoke_exe.exists():
            fail("emon_compile or emon_smoke not found — run build first")
            overall = 1
        else:
            t0 = time.perf_counter()
            rc = run(
                [str(compile_exe), str(rom), str(smoke_pkg), "--no-cache"],
                LOG_DIR / "smoke_compile.log"
            )
            if rc != 0 or not smoke_pkg.exists():
                fail(f"emon_compile failed (exit {rc})")
                overall = 1
            else:
                rc = run([str(smoke_exe), str(smoke_pkg)], LOG_DIR / "smoke_run.log")
                elapsed = time.perf_counter() - t0
                if rc == 0:
                    ok(f"smoke passed in {elapsed:.1f}s")
                else:
                    fail(f"smoke failed (exit {rc})")
                    overall = 1
    finally:
        if smoke_pkg.exists():
            smoke_pkg.unlink()

    section("Summary")
    if overall == 0:
        ok("OVERALL: PASS")
    else:
        fail("OVERALL: FAIL")

    return overall


def cmd_status() -> int:
    section("Status")

    # Show available presets
    if CMAKE.exists() and (REPO_ROOT / "CMakePresets.json").exists():
        print("\n  Build presets:")
        subprocess.run([str(CMAKE), "--build", "--list-presets"], cwd=REPO_ROOT)
        print("\n  Test presets:")
        subprocess.run([str(CTEST), "--test-dir", str(BUILD_DIR), "--list-presets"],
                       cwd=REPO_ROOT)

    # Check required binaries
    # clang build (flat layout): build_clang/tests/, build_clang/tools/, build_clang/runtime/
    # MSVC build (Release subdir): build/tests/Release/, etc.
    def _find_bin(rel: str) -> Path:
        flat = BUILD_DIR / rel
        if flat.exists():
            return flat
        return BUILD_DIR / Path(rel).parent / "Release" / Path(rel).name

    required = [
        _find_bin("tests/runtime_test_engine.exe"),
        _find_bin("tests/runtime_test_compiler.exe"),
        _find_bin("tests/oracle_test.exe"),
        _find_bin("tests/corpus_test.exe"),
        _find_bin("tests/linker_test.exe"),
        _find_bin("tools/emon_compile.exe"),
        _find_bin("tools/emon_smoke.exe"),
        _find_bin("runtime/enginemon_tiles.exe"),
    ]
    print("\n  Binary status:")
    missing = 0
    for exe in required:
        marker = "OK   " if exe.exists() else "MISS "
        if not exe.exists():
            missing += 1
        print(f"    {marker} {exe.relative_to(REPO_ROOT)}")
    if missing:
        print(f"\n  {missing} binaries missing — run: python tools/emon.py build")

    # ROM
    rom = find_rom()
    print(f"\n  ROM: {rom or 'NOT FOUND  (set ENGINEMON_ROM or place .gbc in references/)'}")

    return 0


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(
        prog="emon.py",
        description="Enginemon canonical build + test CLI",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    # build
    p_build = sub.add_parser("build", help="Build canonical targets")
    p_build.add_argument("--preset", default="all",
                         help="Build preset name (default: all)")
    p_build.add_argument("--clean",     action="store_true")
    p_build.add_argument("--configure", action="store_true",
                         help="Force re-configure before building")

    # test
    p_test = sub.add_parser("test", help="Run tests via CTest")
    p_test.add_argument("--preset", default="all",
                        help="Test preset name (default: all)")
    p_test.add_argument("--rom", type=Path, default=None)

    # verify
    p_verify = sub.add_parser("verify", help="Build + test + smoke")
    p_verify.add_argument("--rom", type=Path, default=None)
    p_verify.add_argument("--no-build", action="store_true")

    # status
    sub.add_parser("status", help="Show preset list and binary status")

    args = parser.parse_args()

    if args.command == "build":
        return cmd_build(args.preset, args.clean, args.configure)
    elif args.command == "test":
        return cmd_test(args.preset, args.rom)
    elif args.command == "verify":
        return cmd_verify(args.rom, args.no_build)
    elif args.command == "status":
        return cmd_status()
    else:
        parser.print_help()
        return 1


if __name__ == "__main__":
    sys.exit(main())

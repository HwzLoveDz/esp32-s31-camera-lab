#!/usr/bin/env python3
"""Build Camera Lab using the already activated ESP-IDF Python environment.

No SDK download, environment modification, cleanup, or flash is performed.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys


def build_environment():
    env = os.environ.copy()
    paths = []
    if os.name == "nt":
        # A Windows launcher can pass both PATH and Path. Python folds their
        # names and may keep the older value, losing an activated SDK's tools.
        # Preserve every native PATH entry, then give children one PATH key.
        import ctypes
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.GetEnvironmentStringsW.restype = ctypes.c_void_p
        kernel32.FreeEnvironmentStringsW.argtypes = [ctypes.c_void_p]
        block = kernel32.GetEnvironmentStringsW()
        if not block:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            offset = 0
            while True:
                entry = ctypes.wstring_at(block + offset)
                if not entry:
                    break
                name, _, value = entry.partition("=")
                if name.casefold() == "path":
                    paths.extend(value.split(os.pathsep))
                offset += len(entry.encode("utf-16-le", errors="surrogatepass")) + 2
        finally:
            kernel32.FreeEnvironmentStringsW(block)
    else:
        paths = env.get("PATH", "").split(os.pathsep)
    # Deduplicate without changing search priority or adding the current dir.
    unique = {}
    for path in paths:
        if path:
            unique.setdefault(os.path.normcase(path), path)
    env["PATH"] = os.pathsep.join(unique.values())
    return env


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--cmake", help="CMake executable path (otherwise search activated PATH)")
    parser.add_argument("--ninja", help="Ninja executable path (otherwise search activated PATH)")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    sdk = os.environ.get("IDF_PATH")
    if not sdk or not (Path(sdk) / "components/soc/esp32s31").is_dir():
        parser.error("Activate an ESP-IDF checkout with esp32s31 support; see docs/BUILD.md.")
    if args.jobs < 1:
        parser.error("--jobs must be at least 1")
    env = build_environment()
    programs = {}
    for tool in ("cmake", "ninja"):
        programs[tool] = shutil.which(getattr(args, tool) or tool, path=env["PATH"])
        if not programs[tool]:
            parser.error(f"{tool} is missing from PATH; activate the SDK environment first.")
    env["PATH"] = os.pathsep.join(
        [str(Path(programs[tool]).parent) for tool in ("cmake", "ninja")] + [env["PATH"]]
    )
    build = Path(args.build_dir)
    if not build.is_absolute():
        build = root / build
    subprocess.run(
        [programs["cmake"], "-G", "Ninja", "-S", str(root), "-B", str(build),
         "-DIDF_TARGET=esp32s31", f"-DPYTHON={sys.executable}",
         f"-DCMAKE_MAKE_PROGRAM={programs['ninja']}",
         "-DCONFIGDEP_ENABLE=OFF"], check=True, cwd=root,
        env=env,
    )
    subprocess.run([programs["cmake"], "--build", str(build), "--parallel", str(args.jobs)],
                   check=True, cwd=root, env=env)
    print(f"App image: {build / 'camera_lab.bin'}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as exc:
        sys.exit(exc.returncode)

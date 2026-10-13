#!/usr/bin/env python3
# Copyright Fallen Signal Studios. All Rights Reserved.
"""
Builds and runs the offline icicle geometry checks without Unreal Engine.

Compiles Source/Icify/Private/IcifyIcicleBuilder.cpp (the exact code the plugin ships) together with
Scripts/Offline/IcicleCheck.cpp against the small CoreMinimal.h shim in Scripts/Offline, with every warning an error,
then runs it. The checks cover vertex counts, finite values, unit normals, index ranges, vertex colours, closed
(watertight) surfaces, consistent Unreal winding, outward normals, tip length, the root cap, determinism and seed
sensitivity, degenerate specs and appending to a shared mesh.

Usage: python3 Scripts/CheckIcicles.py [--obj OUT.obj]   (also writes a sample cluster of icicles as an OBJ)
Requires a C++17 compiler (clang++ or g++; $CXX wins). Exit code 0 when everything passes.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCES = [
    os.path.join(ROOT, "Source", "Icify", "Private", "IcifyIcicleBuilder.cpp"),
    os.path.join(ROOT, "Scripts", "Offline", "IcicleCheck.cpp"),
]
INCLUDES = [os.path.join(ROOT, "Scripts", "Offline"), os.path.join(ROOT, "Source", "Icify", "Public")]


def main():
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    parser.add_argument("--obj", help="also write a sample cluster of icicles to this OBJ file")
    args = parser.parse_args()

    compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
    if not compiler:
        print("ERROR: no C++ compiler found (install clang or g++, or set CXX)")
        return 1
    with tempfile.TemporaryDirectory() as work:
        binary = os.path.join(work, "iciclecheck")
        command = [compiler, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Wshadow", "-Werror"]
        for include in INCLUDES:
            command += ["-I", include]
        command += SOURCES + ["-o", binary]
        build = subprocess.run(command, capture_output=True, text=True)
        if build.returncode != 0:
            print("FAIL build (%s):\n%s" % (os.path.basename(compiler), (build.stdout + build.stderr).strip()))
            return 1
        print("ok   build with %s, warnings as errors" % os.path.basename(compiler))
        run = subprocess.run([binary] + ([os.path.abspath(args.obj)] if args.obj else []), capture_output=True, text=True)
        output = (run.stdout + run.stderr).strip()
        print(output)
        return run.returncode


if __name__ == "__main__":
    sys.exit(main())

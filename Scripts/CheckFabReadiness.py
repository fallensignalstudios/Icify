#!/usr/bin/env python3
# Copyright Fallen Signal Studios. All Rights Reserved.
"""
Static Fab-readiness checks for the Icify plugin.

Checks what can be checked without an engine: copyright headers, .uplugin descriptor fields, prebuilt Content
assets (and which engine version saved them), the plugin icon, source hygiene and Build.cs settings.

Usage:
    python3 Scripts/CheckFabReadiness.py [--root PATH] [--copyright TEXT] [--max-content-engine 5.5]
                                         [--engine-version 5.6] [--only a,b] [--strict]

Exit codes: 0 = no errors (warnings allowed unless --strict), 1 = errors found, 2 = usage error.
Python 3.8+, standard library only.
"""

import argparse
import json
import os
import re
import struct
import subprocess
import sys

DEFAULT_COPYRIGHT = "Copyright Fallen Signal Studios. All Rights Reserved."

REQUIRED_CONTENT = [
    "Content/BP_FreezeZone.uasset",
    "Content/Materials/MF_IcifyFrost.uasset",
    "Content/Materials/M_IcifyMaskDebug.uasset",
    "Content/Materials/M_IcifyExample.uasset",
    "Content/Materials/M_IcifyIcicle.uasset",
    "Content/Materials/T_IcifyNoMask.uasset",
]

SOURCE_EXTENSIONS = (".h", ".hpp", ".inl", ".cpp", ".cs", ".ush", ".usf")
TEXT_SCAN_EXTENSIONS = SOURCE_EXTENSIONS + (".ini", ".uplugin", ".py", ".ps1", ".sh", ".json")
SKIP_DIRS = {".git", "Binaries", "Intermediate", "Saved", "DerivedDataCache", ".vs", ".idea", "__pycache__"}
# Directories that are not shipped in the Fab zip (scripts and docs may legitimately mention paths).
NOT_SHIPPED_DIRS = {"Scripts", "Docs"}

ALL_CHECKS = ["copyright", "descriptor", "content", "content-version", "icon", "hygiene", "buildcs", "repo"]


class Report:
    def __init__(self):
        self.sections = []  # (title, [(level, message)])

    def section(self, title):
        items = []
        self.sections.append((title, items))
        return items

    def counts(self):
        errors = sum(1 for _, items in self.sections for level, _ in items if level == "ERROR")
        warnings = sum(1 for _, items in self.sections for level, _ in items if level == "WARN")
        return errors, warnings

    def print(self):
        for title, items in self.sections:
            errors = sum(1 for level, _ in items if level == "ERROR")
            warnings = sum(1 for level, _ in items if level == "WARN")
            status = "FAIL" if errors else ("WARN" if warnings else "PASS")
            print(f"[{status}] {title}")
            for level, message in items:
                if level != "OK":
                    print(f"    {level:5} {message}")
            if not items or all(level == "OK" for level, _ in items):
                for level, message in items:
                    print(f"    ok    {message}")
        print()


def rel(root, path):
    return os.path.relpath(path, root).replace(os.sep, "/")


def walk_files(root, extensions, include_unshipped=False):
    for directory, dirs, files in os.walk(root):
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS
                         and (include_unshipped or rel(root, os.path.join(directory, d)).split("/")[0] not in NOT_SHIPPED_DIRS))
        for name in sorted(files):
            if name.lower().endswith(tuple(e.lower() for e in extensions)):
                yield os.path.join(directory, name)


def read_text(path):
    with open(path, "rb") as handle:
        data = handle.read()
    if data.startswith(b"\xef\xbb\xbf"):
        data = data[3:]
    return data.decode("utf-8", errors="replace")


# ------------------------------------------------------------------------------------------------ checks

def check_copyright(root, notice, report):
    items = report.section(f'Copyright notice on the first line of every source file ("{notice}")')
    missing = []
    total = 0
    for path in walk_files(root, SOURCE_EXTENSIONS):
        total += 1
        lines = read_text(path).splitlines()
        first = next((line.strip() for line in lines if line.strip()), "")
        if notice not in first or not first.startswith(("//", "/*", "#")):
            missing.append(rel(root, path))
    for path in missing:
        items.append(("ERROR", f"{path}: first line is not the copyright notice"))
    if not missing:
        items.append(("OK", f"{total} source files carry the notice"))
    else:
        items.append(("INFO", f"{len(missing)} of {total} source files are missing it"))


def check_descriptor(root, engine_version, report):
    items = report.section("Icify.uplugin descriptor")
    candidates = [f for f in os.listdir(root) if f.endswith(".uplugin")]
    if len(candidates) != 1:
        items.append(("ERROR", f"expected exactly one .uplugin at the plugin root, found {len(candidates)}"))
        return None
    path = os.path.join(root, candidates[0])
    try:
        descriptor = json.loads(read_text(path))
    except json.JSONDecodeError as error:
        items.append(("ERROR", f"{candidates[0]} is not valid JSON: {error}"))
        return None
    items.append(("OK", f"{candidates[0]} is valid JSON"))

    for field in ("FriendlyName", "Description", "Category", "CreatedBy", "VersionName", "DocsURL", "SupportURL"):
        value = descriptor.get(field)
        if not isinstance(value, str) or not value.strip():
            items.append(("ERROR", f'"{field}" is missing or empty'))
    if not any(isinstance(descriptor.get(f), str) and descriptor.get(f).strip() for f in ("FabURL", "MarketplaceURL")):
        items.append(("ERROR", '"FabURL" (or "MarketplaceURL") is missing or empty (fill in once the Fab listing exists)'))
    if not isinstance(descriptor.get("Version"), int):
        items.append(("ERROR", '"Version" must be an integer'))
    if not isinstance(descriptor.get("CreatedByURL"), str) or not descriptor.get("CreatedByURL", "").strip():
        items.append(("WARN", '"CreatedByURL" is empty'))
    if descriptor.get("IsBetaVersion"):
        items.append(("WARN", '"IsBetaVersion" is true: the editor shows a beta warning; set false for the release build'))
    if descriptor.get("IsExperimentalVersion"):
        items.append(("WARN", '"IsExperimentalVersion" is true'))
    if descriptor.get("Installed"):
        items.append(("WARN", '"Installed" is true in the source descriptor (Epic sets this when distributing)'))
    if os.path.isdir(os.path.join(root, "Content")) and not descriptor.get("CanContainContent"):
        items.append(("ERROR", 'Content/ exists but "CanContainContent" is not true'))

    version = descriptor.get("EngineVersion")
    if engine_version:
        if version != f"{engine_version}.0":
            items.append(("ERROR", f'"EngineVersion" is {version!r}, expected "{engine_version}.0" for this package'))
        else:
            items.append(("OK", f'"EngineVersion" is "{version}"'))
    elif version is None:
        items.append(("OK", '"EngineVersion" is unset in the repo (the build scripts set it per package)'))
    elif not re.fullmatch(r"5\.\d+\.0", str(version)):
        items.append(("ERROR", f'"EngineVersion" {version!r} should look like "5.6.0"'))
    else:
        items.append(("WARN", f'"EngineVersion" is set to {version} in the repo; the build scripts set it per package'))

    modules = descriptor.get("Modules", [])
    if not modules:
        items.append(("ERROR", "no modules listed"))
    for module in modules:
        name = module.get("Name", "")
        for key in ("Name", "Type", "LoadingPhase"):
            if not module.get(key):
                items.append(("ERROR", f'module {name or "?"}: "{key}" missing'))
        build_cs = os.path.join(root, "Source", name, f"{name}.Build.cs")
        if name and not os.path.isfile(build_cs):
            items.append(("ERROR", f"module {name}: Source/{name}/{name}.Build.cs not found"))
        for key in ("WhitelistPlatforms", "BlacklistPlatforms"):
            if key in module:
                items.append(("ERROR", f'module {name}: "{key}" is the deprecated name; use PlatformAllowList/PlatformDenyList'))
    for key in ("WhitelistPlatforms", "BlacklistPlatforms"):
        if key in descriptor:
            items.append(("ERROR", f'"{key}" is deprecated; use SupportedTargetPlatforms'))
    if not any(level in ("ERROR", "WARN") for level, _ in items):
        items.append(("OK", "required fields present"))
    return descriptor


def engine_version_of_asset(path):
    """Returns (major, minor, patch, branch, ue5_object_version) for an editor-saved .uasset/.umap, or None."""
    with open(path, "rb") as handle:
        data = handle.read(256 * 1024)
    if len(data) < 32 or data[:4] != b"\xc1\x83\x2a\x9e":
        return None
    legacy = struct.unpack_from("<i", data, 4)[0]
    offset = 8
    ue5_version = None
    if legacy != -4:
        offset += 4  # LegacyUE3Version
    offset += 4  # FileVersionUE4
    if legacy <= -8:
        ue5_version = struct.unpack_from("<i", data, offset)[0]
    # SavedByEngineVersion: uint16 Major, Minor, Patch, uint32 Changelist, FString Branch ("++UE5+Release-5.5").
    for match in re.finditer(rb"\+\+UE5", data):
        start = match.start()
        if start < 14:
            continue
        length = struct.unpack_from("<i", data, start - 4)[0]
        if not 2 <= length <= 256:
            continue
        branch_bytes = data[start:start + length]
        if not branch_bytes.endswith(b"\x00"):
            continue
        major, minor, patch = struct.unpack_from("<HHH", data, start - 14)
        if major == 5 and minor < 100:
            return major, minor, patch, branch_bytes[:-1].decode("ascii", "replace"), ue5_version
    return (None, None, None, None, ue5_version)


def parse_engine(text):
    match = re.fullmatch(r"(\d+)\.(\d+)", text or "")
    if not match:
        raise argparse.ArgumentTypeError(f"expected an engine version like 5.5, got {text!r}")
    return int(match.group(1)), int(match.group(2))


def check_content(root, report):
    items = report.section("Prebuilt Content assets")
    found = []
    for asset in REQUIRED_CONTENT:
        path = os.path.join(root, *asset.split("/"))
        if os.path.isfile(path):
            found.append(path)
        else:
            items.append(("ERROR", f"{asset} is missing (generate it in the editor, save, and commit it)"))
    if len(found) == len(REQUIRED_CONTENT):
        items.append(("OK", f"all {len(found)} required assets present"))
    return found


def check_content_version(root, max_engine, report):
    items = report.section(f"Content saved by an engine no newer than {max_engine[0]}.{max_engine[1]}")
    assets = [p for p in walk_files(os.path.join(root, "Content"), (".uasset", ".umap"))] if os.path.isdir(os.path.join(root, "Content")) else []
    if not assets:
        items.append(("WARN", "no .uasset/.umap files to inspect"))
        return
    versions = set()
    for path in assets:
        info = engine_version_of_asset(path)
        name = rel(root, path)
        if info is None:
            items.append(("ERROR", f"{name}: not an editor package (bad magic) or cooked/truncated"))
            continue
        major, minor, patch, branch, ue5 = info
        if major is None:
            items.append(("WARN", f"{name}: could not find SavedByEngineVersion (FileVersionUE5={ue5}); check it by opening in {max_engine[0]}.{max_engine[1]}"))
            continue
        versions.add((major, minor))
        if (major, minor) > max_engine:
            items.append(("ERROR", f"{name}: saved by {major}.{minor}.{patch} ({branch}); engines older than that cannot load it"))
        else:
            items.append(("OK", f"{name}: saved by {major}.{minor}.{patch}"))
    if len(versions) > 1:
        items.append(("WARN", f"content was saved by mixed engine versions: {sorted(versions)}"))


def check_icon(root, report):
    items = report.section("Plugin icon (Resources/Icon128.png, 128x128 PNG)")
    path = os.path.join(root, "Resources", "Icon128.png")
    if not os.path.isfile(path):
        items.append(("ERROR", "Resources/Icon128.png is missing"))
        return
    with open(path, "rb") as handle:
        header = handle.read(24)
    if header[:8] != b"\x89PNG\r\n\x1a\n" or header[12:16] != b"IHDR":
        items.append(("ERROR", "Resources/Icon128.png is not a PNG"))
        return
    width, height = struct.unpack(">II", header[16:24])
    if (width, height) != (128, 128):
        items.append(("ERROR", f"Resources/Icon128.png is {width}x{height}, expected 128x128"))
    else:
        items.append(("OK", "Resources/Icon128.png is a 128x128 PNG"))


ABSOLUTE_PATH = re.compile(
    r"""(?ix)
    (?<![A-Za-z0-9])[A-Z]:[\\/]{1,2}(?:Users|Program\ Files|Epic|UE_|Unreal|Projects|Dev|Work|Git|src)   # C:\Users\..., D:/Projects
    | /Users/[A-Za-z0-9._-]+                                                                            # macOS home
    | /home/[A-Za-z0-9._-]+                                                                             # Linux home
    | \\\\[A-Za-z0-9._-]+\\                                                                             # UNC share
    """)
NAMED_TODO = re.compile(r"\b(TODO|FIXME|HACK|XXX)\b\s*(\(\s*[A-Za-z][\w.\- ]*\)|[:\-]?\s*@[A-Za-z]\w*|\s+[A-Z][a-z]+:)")
PLAIN_TODO = re.compile(r"\b(TODO|FIXME|HACK|XXX)\b")
OPTIMIZE = re.compile(r"#\s*pragma\s+optimize|\bPRAGMA_DISABLE_OPTIMIZATION\b|\bUE_DISABLE_OPTIMIZATION\b|__attribute__\s*\(\(\s*optnone|#\s*pragma\s+clang\s+optimize\s+off")
EPIC_COPYRIGHT = re.compile(r"Copyright\s+Epic\s+Games", re.IGNORECASE)


def check_hygiene(root, report, extra_names):
    items = report.section("Source hygiene (absolute paths, usernames, #pragma optimize, named TODOs)")
    names = {n.lower() for n in extra_names if n}
    for env in ("USER", "USERNAME", "LOGNAME"):
        value = os.environ.get(env, "")
        if value and value.lower() not in {"root", "user", "runner", "builder", "admin"}:
            names.add(value.lower())
    plain_todos = 0
    for path in walk_files(root, TEXT_SCAN_EXTENSIONS):
        name = rel(root, path)
        for number, line in enumerate(read_text(path).splitlines(), 1):
            where = f"{name}:{number}"
            if ABSOLUTE_PATH.search(line):
                items.append(("ERROR", f"{where}: absolute or user-specific path: {line.strip()[:120]}"))
            lowered = line.lower()
            for user in names:
                if re.search(rf"(?<![a-z0-9]){re.escape(user)}(?![a-z0-9])", lowered):
                    items.append(("ERROR", f"{where}: mentions developer name '{user}'"))
            if OPTIMIZE.search(line):
                items.append(("ERROR", f"{where}: optimisation pragma left in: {line.strip()[:120]}"))
            if NAMED_TODO.search(line):
                items.append(("ERROR", f"{where}: TODO/FIXME with a name: {line.strip()[:120]}"))
            elif PLAIN_TODO.search(line):
                plain_todos += 1
                items.append(("WARN", f"{where}: {line.strip()[:120]}"))
            if EPIC_COPYRIGHT.search(line):
                items.append(("ERROR", f"{where}: Epic Games copyright line in plugin code (use your own notice)"))
    if plain_todos:
        items.append(("INFO", f"{plain_todos} plain TODO/FIXME comment(s); fine for Fab, but review before release"))
    if not any(level in ("ERROR", "WARN") for level, _ in items):
        items.append(("OK", "no absolute paths, usernames, optimisation pragmas or named TODOs"))


def check_buildcs(root, report):
    items = report.section("Build.cs settings")
    files = list(walk_files(os.path.join(root, "Source"), (".Build.cs",))) if os.path.isdir(os.path.join(root, "Source")) else []
    if not files:
        items.append(("ERROR", "no Build.cs files under Source/"))
    for path in files:
        text = read_text(path)
        name = rel(root, path)
        if not re.search(r"PCHUsage\s*=\s*PCHUsageMode\.(UseExplicitOrSharedPCHs|NoPCHs|NoSharedPCHs)", text):
            items.append(("ERROR", f"{name}: set PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs explicitly"))
        if re.search(r"bEnforceIWYU|bUseUnity\s*=\s*false|OptimizeCode\s*=\s*CodeOptimization\.Never", text):
            items.append(("WARN", f"{name}: contains bEnforceIWYU / bUseUnity=false / OptimizeCode=Never (deprecated or debugging setting)"))
        if re.search(r"[A-Z]:\\\\|/Users/|/home/", text):
            items.append(("ERROR", f"{name}: absolute path in Build.cs"))
    if files and not any(level in ("ERROR", "WARN") for level, _ in items):
        items.append(("OK", f"{len(files)} Build.cs file(s) set PCHUsage explicitly"))


def check_repo(root, report):
    items = report.section("Repository layout (what ships)")
    for folder in ("Binaries", "Intermediate", "Saved", "DerivedDataCache"):
        if os.path.isdir(os.path.join(root, folder)):
            items.append(("WARN", f"{folder}/ exists locally; the build scripts exclude it from the Fab zip"))
    try:
        tracked = subprocess.run(["git", "-C", root, "ls-files"], capture_output=True, text=True, check=True).stdout.splitlines()
    except (OSError, subprocess.CalledProcessError):
        tracked = []
    for path in tracked:
        top = path.split("/")[0]
        if top in ("Binaries", "Intermediate", "Saved", "DerivedDataCache"):
            items.append(("ERROR", f"{path} is committed (build output must not be versioned or shipped)"))
        if path.lower().endswith((".exe", ".dll", ".pdb", ".lib", ".dylib", ".so", ".a")):
            items.append(("ERROR", f"{path}: binary file committed"))
    if not os.path.isfile(os.path.join(root, "Config", "FilterPlugin.ini")):
        items.append(("WARN", "Config/FilterPlugin.ini is missing: BuildPlugin only stages its default file set, so add one listing extra files (e.g. /Shaders/..., /README.md) if you rely on its -Package output"))
    if not os.path.isdir(os.path.join(root, "Shaders")):
        items.append(("ERROR", "Shaders/ is missing (the material custom nodes include /Plugin/Icify/Private/Icify.ush)"))
    if not any(level in ("ERROR", "WARN") for level, _ in items):
        items.append(("OK", "nothing unexpected"))


def main(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(description="Static Fab-readiness checks for the Icify plugin.")
    parser.add_argument("--root", default=os.path.dirname(here), help="plugin root (default: the folder above Scripts/)")
    parser.add_argument("--copyright", default=DEFAULT_COPYRIGHT, help=f'required first-line notice (default: "{DEFAULT_COPYRIGHT}")')
    parser.add_argument("--max-content-engine", type=parse_engine, default=(5, 5),
                        help="newest engine allowed to have saved Content/ assets, e.g. 5.5 (default 5.5, the oldest supported)")
    parser.add_argument("--engine-version", default=None, help='expect "EngineVersion": "<ver>.0" in the descriptor (staged packages)')
    parser.add_argument("--only", default="", help=f"comma-separated subset of: {', '.join(ALL_CHECKS)}")
    parser.add_argument("--forbid-name", action="append", default=[], help="developer name/username that must not appear (repeatable)")
    parser.add_argument("--strict", action="store_true", help="treat warnings as errors")
    args = parser.parse_args(argv)

    root = os.path.abspath(args.root)
    if not os.path.isdir(root):
        print(f"error: {root} is not a directory", file=sys.stderr)
        return 2
    selected = [c.strip() for c in args.only.split(",") if c.strip()] or ALL_CHECKS
    unknown = [c for c in selected if c not in ALL_CHECKS]
    if unknown:
        print(f"error: unknown check(s) {unknown}; choose from {ALL_CHECKS}", file=sys.stderr)
        return 2
    if args.engine_version:
        parse_engine(args.engine_version)

    print(f"Fab readiness: {root}\n")
    report = Report()
    if "copyright" in selected:
        check_copyright(root, args.copyright, report)
    if "descriptor" in selected:
        check_descriptor(root, args.engine_version, report)
    if "content" in selected:
        check_content(root, report)
    if "content-version" in selected:
        check_content_version(root, args.max_content_engine, report)
    if "icon" in selected:
        check_icon(root, report)
    if "hygiene" in selected:
        check_hygiene(root, report, args.forbid_name)
    if "buildcs" in selected:
        check_buildcs(root, report)
    if "repo" in selected:
        check_repo(root, report)

    report.print()
    errors, warnings = report.counts()
    failed = errors > 0 or (args.strict and warnings > 0)
    print(f"{'NOT READY' if failed else 'READY'}: {errors} error(s), {warnings} warning(s)"
          + (" (warnings count as errors: --strict)" if args.strict else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

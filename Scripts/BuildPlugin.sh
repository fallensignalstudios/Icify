#!/usr/bin/env bash
# Copyright Fallen Signal Studios. All Rights Reserved.
#
# Builds and packages Icify for Fab on Mac or Linux, once per engine version. Same steps as BuildPlugin.ps1:
#   1. find the engine (--engine-root / --engine VER=DIR, then $UE_5_x_ROOT, then the default install path)
#      and check Engine/Build/Build.version,
#   2. stage Source, Content, Shaders, Resources, Config and the .uplugin, with "EngineVersion": "<ver>.0" set
#      (the repository is never modified; --content-root DIR uses DIR/<ver>/ as Content/),
#   3. run Scripts/CheckFabReadiness.py on the staged copy,
#   4. RunUAT BuildPlugin -StrictIncludes; fail on UAT failure or any compiler warning/error in the log,
#   5. zip the staged copy (top folder "Icify", no Binaries/Intermediate/Saved/DerivedDataCache/.git/Scripts/Docs).
#
# Usage:
#   Scripts/BuildPlugin.sh [--versions "5.5 5.6 5.7 5.8"] [--engine 5.6=/path/UE_5.6]... [--engine-root DIR]
#                          [--platforms Mac|Linux|Mac+Linux] [--output DIR] [--content-root DIR]
#                          [--skip-build] [--skip-readiness] [--keep-staging]
#
# Defaults: all four versions; platform = the host (Mac on macOS, Linux on Linux); output = <plugin>/Saved/FabBuild.
# Default engine locations: macOS "/Users/Shared/Epic Games/UE_5.x"; Linux "$HOME/UnrealEngine/UE_5.x",
# "$HOME/Epic Games/UE_5.x", "/opt/UnrealEngine/UE_5.x" (Linux has no launcher; set UE_5_x_ROOT).
# Requires bash 3.2+ and python3 (descriptor edit, zip, readiness checks).

set -u -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLUGIN_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
UPLUGIN="$(ls "$PLUGIN_ROOT"/*.uplugin 2>/dev/null | head -n 1)"
[ -n "$UPLUGIN" ] || { echo "No .uplugin in $PLUGIN_ROOT" >&2; exit 2; }
PLUGIN_NAME="$(basename "$UPLUGIN" .uplugin)"

VERSIONS="5.5 5.6 5.7 5.8"
ENGINE_MAP=""          # newline-separated "ver=dir"
SINGLE_ROOT=""
PLATFORMS=""
OUTPUT_DIR="$PLUGIN_ROOT/Saved/FabBuild"
CONTENT_ROOT=""
SKIP_BUILD=0
SKIP_READINESS=0
KEEP_STAGING=0

usage() { sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

while [ $# -gt 0 ]; do
	case "$1" in
		--versions) VERSIONS="$(echo "$2" | tr ',' ' ')"; shift 2 ;;
		--engine) ENGINE_MAP="$ENGINE_MAP"$'\n'"$2"; shift 2 ;;
		--engine-root) SINGLE_ROOT="$2"; shift 2 ;;
		--platforms) PLATFORMS="$2"; shift 2 ;;
		--output) OUTPUT_DIR="$2"; shift 2 ;;
		--content-root) CONTENT_ROOT="$2"; shift 2 ;;
		--skip-build) SKIP_BUILD=1; shift ;;
		--skip-readiness) SKIP_READINESS=1; shift ;;
		--keep-staging) KEEP_STAGING=1; shift ;;
		-h|--help) usage ;;
		*) echo "Unknown option $1" >&2; usage ;;
	esac
done

for V in $VERSIONS; do
	case "$V" in 5.[0-9]|5.[0-9][0-9]) ;; *) echo "Engine version '$V' should look like 5.6" >&2; exit 2 ;; esac
done
if [ -n "$SINGLE_ROOT" ] && [ "$(echo $VERSIONS | wc -w | tr -d ' ')" != "1" ]; then
	echo "--engine-root needs exactly one version; use --engine VER=DIR for several" >&2; exit 2
fi

PYTHON="$(command -v python3 || true)"
[ -n "$PYTHON" ] || { echo "python3 is required" >&2; exit 2; }

HOST="$(uname -s)"
if [ -z "$PLATFORMS" ]; then
	case "$HOST" in
		Darwin) PLATFORMS="Mac" ;;
		Linux) PLATFORMS="Linux" ;;
		*) echo "Unsupported host $HOST (use BuildPlugin.ps1 on Windows)" >&2; exit 2 ;;
	esac
fi

mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"
VERSION_NAME="$("$PYTHON" -c 'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8-sig"))["VersionName"])' "$UPLUGIN")"

find_engine() {
	local ver="$1" var dir candidates=()
	[ -n "$SINGLE_ROOT" ] && candidates+=("$SINGLE_ROOT")
	dir="$(printf '%s\n' "$ENGINE_MAP" | sed -n "s/^$ver=//p" | head -n 1)"
	[ -n "$dir" ] && candidates+=("$dir")
	var="UE_${ver//./_}_ROOT"
	[ -n "${!var:-}" ] && candidates+=("${!var}")
	case "$HOST" in
		Darwin) candidates+=("/Users/Shared/Epic Games/UE_$ver") ;;
		Linux) candidates+=("$HOME/UnrealEngine/UE_$ver" "$HOME/Epic Games/UE_$ver" "/opt/UnrealEngine/UE_$ver") ;;
	esac
	for dir in "${candidates[@]}"; do
		if [ -f "$dir/Engine/Build/BatchFiles/RunUAT.sh" ]; then
			(cd "$dir" && pwd)
			return 0
		fi
	done
	return 1
}

check_engine_version() {
	"$PYTHON" - "$1/Engine/Build/Build.version" "$2" <<'PY'
import json, sys
try:
    info = json.load(open(sys.argv[1], encoding="utf-8-sig"))
except OSError:
    print("missing Engine/Build/Build.version"); sys.exit(1)
found = f"{info['MajorVersion']}.{info['MinorVersion']}"
if found != sys.argv[2]:
    print(f"engine is {found}, not {sys.argv[2]}"); sys.exit(1)
PY
}

# Copies a folder without build output, VCS data, or OS clutter.
copy_filtered() {
	"$PYTHON" - "$1" "$2" <<'PY'
import os, shutil, sys
src, dst = sys.argv[1], sys.argv[2]
skip_dirs = {"Binaries", "Intermediate", "Saved", "DerivedDataCache", ".git", ".vs", ".idea", "Scripts", "Docs", "__pycache__"}
skip_files = {".gitkeep", ".gitignore", ".gitattributes", ".DS_Store", "Thumbs.db"}
shutil.copytree(src, dst, ignore=lambda d, names: [n for n in names if n in skip_dirs or n in skip_files], dirs_exist_ok=True)
PY
}

set_engine_version() {
	"$PYTHON" - "$1" "$2" <<'PY'
import json, re, sys
path, ver = sys.argv[1], sys.argv[2] + ".0"
text = open(path, encoding="utf-8-sig").read()
if re.search(r'"EngineVersion"\s*:', text):
    text = re.sub(r'"EngineVersion"\s*:\s*"[^"]*"', f'"EngineVersion": "{ver}"', text)
else:
    text, n = re.subn(r'("VersionName"\s*:\s*"[^"]*",)(\r?\n)(\s*)', lambda m: f'{m.group(1)}{m.group(2)}{m.group(3)}"EngineVersion": "{ver}",{m.group(2)}{m.group(3)}', text, count=1)
    if n != 1:
        sys.exit("could not insert EngineVersion")
json.loads(text)
open(path, "w", encoding="utf-8", newline="").write(text)
PY
}

make_zip() {
	"$PYTHON" - "$1" "$2" "$PLUGIN_NAME" <<'PY'
import os, sys, zipfile
staged, out, name = sys.argv[1], sys.argv[2], sys.argv[3]
excluded = {"Binaries", "Intermediate", "Saved", "DerivedDataCache", ".git", "Scripts", "Docs"}
count = 0
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as archive:
    for directory, dirs, files in os.walk(staged):
        dirs.sort()
        for f in sorted(files):
            full = os.path.join(directory, f)
            arc = name + "/" + os.path.relpath(full, staged).replace(os.sep, "/")
            if excluded.intersection(arc.split("/")):
                sys.exit(f"excluded path in staging: {arc}")
            archive.write(full, arc)
            count += 1
print(count)
PY
}

WARN_RE='warning C[0-9]{4}|: warning:|warning LNK[0-9]{4}'
ERR_RE='error C[0-9]{4}|: error:|: fatal error|fatal error C[0-9]{4}|error LNK[0-9]{4}|ld: error|clang: error|Undefined symbols for architecture'

SUMMARY=""
FAILED=0
row() { SUMMARY="$SUMMARY$(printf '%-7s | %-9s | %-18s | %8s | %6s | %s' "$@")"$'\n'; }

for VER in $VERSIONS; do
	echo
	echo "=== $PLUGIN_NAME $VERSION_NAME for UE $VER ($PLATFORMS) ==="
	READY="-"; BUILD="-"; NWARN=0; NERR=0; ZIP="-"; OK=1

	if ! ROOT="$(find_engine "$VER")"; then
		echo "UE $VER not found (use --engine $VER=DIR or set UE_${VER//./_}_ROOT)" >&2
		row "$VER" "-" "engine not found" 0 0 "-"; FAILED=$((FAILED + 1)); continue
	fi
	if ! MSG="$(check_engine_version "$ROOT" "$VER")"; then
		echo "$ROOT: $MSG" >&2
		row "$VER" "-" "wrong engine" 0 0 "$MSG"; FAILED=$((FAILED + 1)); continue
	fi
	echo "Engine: $ROOT"

	VDIR="$OUTPUT_DIR/$VER"
	STAGE="$OUTPUT_DIR/Staging/$VER/$PLUGIN_NAME"
	PACKAGE="$VDIR/Package"
	LOG="$VDIR/BuildPlugin.log"
	rm -rf "$STAGE" "$PACKAGE"
	mkdir -p "$VDIR" "$STAGE"

	for F in Source Content Shaders Resources Config; do
		[ -d "$PLUGIN_ROOT/$F" ] && copy_filtered "$PLUGIN_ROOT/$F" "$STAGE/$F"
	done
	if [ -n "$CONTENT_ROOT" ]; then
		if [ ! -d "$CONTENT_ROOT/$VER" ]; then
			echo "--content-root given but $CONTENT_ROOT/$VER does not exist" >&2
			row "$VER" "-" "no content" 0 0 "-"; FAILED=$((FAILED + 1)); continue
		fi
		rm -rf "$STAGE/Content"
		copy_filtered "$CONTENT_ROOT/$VER" "$STAGE/Content"
	fi
	cp "$UPLUGIN" "$STAGE/"
	if ! set_engine_version "$STAGE/$(basename "$UPLUGIN")" "$VER"; then
		row "$VER" "-" "descriptor edit" 0 0 "-"; FAILED=$((FAILED + 1)); continue
	fi

	if [ "$SKIP_READINESS" = 1 ]; then
		READY="skipped"
	elif "$PYTHON" "$SCRIPT_DIR/CheckFabReadiness.py" --root "$STAGE" --engine-version "$VER" --max-content-engine "$VER" \
			--only copyright,descriptor,content,content-version,icon,hygiene,buildcs > "$VDIR/FabReadiness.txt" 2>&1; then
		READY="pass"
	else
		READY="FAIL"; OK=0
		echo "Fab readiness failed; see $VDIR/FabReadiness.txt"
	fi

	if [ "$SKIP_BUILD" = 1 ]; then
		BUILD="skipped"
	else
		UAT="$ROOT/Engine/Build/BatchFiles/RunUAT.sh"
		echo "$UAT BuildPlugin -Plugin=$STAGE/$(basename "$UPLUGIN") -Package=$PACKAGE -TargetPlatforms=$PLATFORMS -StrictIncludes"
		"$UAT" BuildPlugin "-Plugin=$STAGE/$(basename "$UPLUGIN")" "-Package=$PACKAGE" "-TargetPlatforms=$PLATFORMS" -StrictIncludes -NoP4 2>&1 | tee "$LOG"
		UAT_EXIT=${PIPESTATUS[0]}
		grep -E "$WARN_RE" "$LOG" | sed 's/^[[:space:]]*//' | sort -u > "$VDIR/CompilerWarnings.txt" || true
		grep -E "$ERR_RE" "$LOG" | sed 's/^[[:space:]]*//' | sort -u > "$VDIR/CompilerErrors.txt" || true
		NWARN=$(wc -l < "$VDIR/CompilerWarnings.txt" | tr -d ' ')
		NERR=$(wc -l < "$VDIR/CompilerErrors.txt" | tr -d ' ')
		if [ "$UAT_EXIT" != 0 ]; then BUILD="FAIL (UAT $UAT_EXIT)"
		elif [ "$NERR" != 0 ]; then BUILD="FAIL (errors)"
		elif [ "$NWARN" != 0 ]; then BUILD="FAIL (warnings)"
		else BUILD="pass"; fi
		[ "$BUILD" = "pass" ] || OK=0
	fi

	if [ "$BUILD" = "pass" ] || [ "$BUILD" = "skipped" ]; then
		ZIPFILE="$OUTPUT_DIR/${PLUGIN_NAME}_${VERSION_NAME}_UE${VER}.zip"
		rm -f "$ZIPFILE"
		if COUNT="$(make_zip "$STAGE" "$ZIPFILE")"; then ZIP="$ZIPFILE ($COUNT files)"; else ZIP="zip failed"; OK=0; fi
	fi

	[ "$OK" = 1 ] || FAILED=$((FAILED + 1))
	if [ "$OK" = 1 ] && [ "$KEEP_STAGING" = 0 ]; then rm -rf "$OUTPUT_DIR/Staging/$VER"; fi
	row "$VER" "$READY" "$BUILD" "$NWARN" "$NERR" "$ZIP"
done

echo
echo "=== Summary ($PLUGIN_NAME $VERSION_NAME, platforms $PLATFORMS) ==="
printf '%-7s | %-9s | %-18s | %8s | %6s | %s\n' Version Readiness Build Warnings Errors Zip
printf '%s\n' "--------+-----------+--------------------+----------+--------+-----"
printf '%s' "$SUMMARY"
if [ "$FAILED" != 0 ]; then
	echo "$FAILED version(s) not ready. Logs: $OUTPUT_DIR/<version>/"
	exit 1
fi
echo "All versions built cleanly. Upload the zips in $OUTPUT_DIR to Fab."
exit 0

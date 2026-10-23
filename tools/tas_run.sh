#!/bin/bash
# Linux version of tas_run.ps1: runs a TAS script (docs/tas.md) on a fresh copy of a test
# save folder and collects the log, telemetry and screenshots in out/tas/<tag>.*. Low
# impact by default (muted, 1x internal resolution, nice 10); scripted input never rumbles.
#   tools/tas_run.sh -s tools/tas/x.tas [-6] [-S base|full|none] [-t tag] [-T seconds] [-F] [-x scale] [-e exe] [-a "more args"]
# For a headless machine, start an X server first and export DISPLAY (docs/linux.md).
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
out=$root/out
script="" fps60=0 save=base tag="" secs=600 full=0 scale=1 exe="$out/BlackKnightRecomp" extra=""
while getopts "s:6S:t:T:Fx:e:a:" o; do
    case $o in
    s) script=$OPTARG ;; 6) fps60=1 ;; S) save=$OPTARG ;; t) tag=$OPTARG ;; T) secs=$OPTARG ;;
    F) full=1 ;; x) scale=$OPTARG ;; e) exe=$OPTARG ;; a) extra=$OPTARG ;;
    *) exit 2 ;;
    esac
done
[ -f "$script" ] || { echo "TAS script not found: $script"; exit 2; }
[ -n "$tag" ] || tag="$(basename "$script" .tas)_$([ $fps60 = 1 ] && echo 60 || echo 30)"
exedir=$(cd "$(dirname "$exe")" && pwd)
mkdir -p "$out/tas"
nand="$out/tasnand/run_$tag"
rm -rf "$nand"
if [ "$save" = none ]; then mkdir -p "$nand"
else
    [ -d "$out/tasnand/$save" ] || { echo "missing test save folder $out/tasnand/$save"; exit 2; }
    cp -r "$out/tasnand/$save" "$nand"
fi
csv="$out/tas/$tag.csv"
args=(--nobox --skip-launcher --tas "$(realpath "$script")" --telemetry "$csv" --nand "$nand")
[ $fps60 = 1 ] && args+=(--60fps)
[ $full = 0 ] && args+=(--mute --scale "$scale")
# shellcheck disable=SC2206
[ -n "$extra" ] && args+=($extra)
start=$(date +%s)
cd "$exedir"
if [ $full = 0 ]; then nice -n 10 timeout -k 5 "$secs" "$exe" "${args[@]}" > /dev/null 2>&1
else timeout -k 5 "$secs" "$exe" "${args[@]}" > /dev/null 2>&1
fi
code=$?
cp "$exedir/BlackKnightRecomp.log" "$out/tas/$tag.log"
# this run's screenshots (scripts use the same names at 30 and 60 fps)
if [ -d "$exedir/shots" ]; then
    dst="$out/tas/shots/$tag"
    mkdir -p "$dst"
    find "$exedir/shots" -maxdepth 1 -name '*.bmp' -newermt "@$start" -exec mv {} "$dst/" \;
fi
fails=$(grep -E "TAS: FAIL|\[E\]" "$out/tas/$tag.log" | head -5)
if [ $code = 0 ] && [ -z "$fails" ]; then echo "$tag : OK"
elif [ $code = 124 ] || [ $code = 137 ]; then echo "$tag : TIMEOUT"
else echo "$tag : FAILED (exit $code)"
fi
[ -n "$fails" ] && echo "$fails"
exit 0

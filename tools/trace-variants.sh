#!/bin/sh
# Trace Game Capture HD once per encoder setting, so register diffs show what each setting controls.
# Usage: tools/trace-variants.sh <outdir> name='{"MaxHeight":...}' ...
set -e
mkdir -p "$1"; out=$(cd "$1" && pwd); shift
here=$(cd "$(dirname "$0")" && pwd)
for spec in "$@"; do
	name=${spec%%=*}; json=${spec#*=}
	mkdir -p "$out/$name"
	defaults write com.elgato.GameCaptureHD beddo-Menc -string "$json"
	open -n --env DYLD_INSERT_LIBRARIES="$here/usbtrace/usbtrace.dylib" --env USBTRACE_DIR="$out/$name" -a "Game Capture HD"
	sleep "${TRACE_SECONDS:-30}"
	pkill -f "Game Capture HD.app/Contents/MacOS" || true
	sleep 3
	echo "$name: $(wc -l < "$out/$name/trace.log") lines, $(du -h "$out/$name/pipe3.bin" | cut -f1) video"
done

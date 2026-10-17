#!/usr/bin/env bash
# A shareable build: the generated demo song, no log address, zipped as Pulse-<version>.zip with its SHA-256.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
PULSE_RELEASE=1 bash "$here/build.sh"
ver=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["contentVersion"])' "$here/param.json")
dist=$here/../build/ps5/dist
[[ ! -e $dist/PPSA99993/assets/loghost.txt ]] || { echo "release carries a log address" >&2; exit 2; }
rm -f "$dist/Pulse-$ver.zip"
(cd "$dist" && zip -qr "Pulse-$ver.zip" PPSA99993)
sha256sum "$dist/Pulse-$ver.zip"

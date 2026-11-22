#!/bin/bash
# Build the already source-checked candidate and stage a test ZIP; no installer is needed.
set -euo pipefail
task_root=$PWD
bash scripts/wincross/configure.sh
source scripts/wincross/env.sh
ninja -C build-windows-x86_64 -j12
stage="$PWD/build-windows-x86_64/package/Slipstream_win64"
rm -rf "$PWD/build-windows-x86_64/package"
mkdir -p "$stage"
(
 cd build-windows-x86_64/newview
 "$task_root/.venv/bin/python" "$task_root/indra/newview/viewer_manifest.py" \
  --actions="copy package" --arch=x86_64 --platform=windows --artwork="$task_root/indra/newview" \
  --openal=ON --tracy=OFF --build="$task_root/build-windows-x86_64/newview" --buildtype=Release \
  --channel="Slipstream Test" --configuration=. --dest="$stage" --grid=agni \
  --source="$task_root/indra/newview" --versionfile=viewer_version.txt
)
(cd build-windows-x86_64/package && 7z a -tzip -mx=7 -mmt=12 depth-prepass-b.zip Slipstream_win64 >/dev/null)

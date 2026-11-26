#!/bin/bash
# Cross-build the Windows x64 viewer on Linux (clang-cl + lld-link + xwin) and
# package it as a zip. Run from the repo root. Prerequisites on the host:
#   clang/lld/llvm (clang-cl, lld-link, llvm-rc, llvm-mt, llvm-lib, llvm-dlltool),
#   7z, the Vulkan loader + headers, and the MSVC CRT/SDK splatted by xwin
#   (`xwin --accept-license splat --output ~/.xwin`; XWIN_ROOT overrides the path).
# Output: build-windows-x86_64/package/<ARTIFACT_BASE>_win64.zip (see scripts/ci/version.sh), plus
# <ARTIFACT_BASE>_Setup.exe when makensis is installed (scripts/wincross/make_installer.sh)
set -euo pipefail
B=build-windows-x86_64
# Version/channel/artifact name (scripts/ci/version.sh; the tag comes from CI).
eval "$(bash scripts/ci/version.sh | sed 's/^/V_/; s/=\(.*\)$/="\1"/')"
export VIEWER_CHANNEL="${VIEWER_CHANNEL:-$V_CHANNEL}"
CRT=${WINCROSS_CRT:-$HOME/.cache/wincross/crt}

bash scripts/wincross/xwin_case_links.sh
[ -f "$CRT/vcruntime140.dll" ] || python3 scripts/wincross/fetch_vcredist.py "$CRT"
[ -f "$B/vk/vulkan-1.lib" ] || bash scripts/wincross/make_vulkan_lib.sh "$B/vk"
bash scripts/wincross/configure.sh
source scripts/wincross/env.sh

# <Prestige> CI helpers (.github/workflows/prestige-windows.yml): WINCROSS_SHARD=i/n compiles only
# every n-th object file, starting at the i-th (0-based), into the compile cache (ccache) and stops.
# The final job then builds everything with those results cached, links and packages. A file that
# fails here does not fail the helper: the final job compiles it again and reports the error.
if [ -n "${WINCROSS_SHARD:-}" ]; then
    shard=${WINCROSS_SHARD%/*}
    shards=${WINCROSS_SHARD#*/}
    mapfile -t objs < <(ninja -C "$B" -t targets all | awk -F': ' '$2 ~ /_COMPILER__/ {print $1}' \
                        | sort | awk -v n="$shards" -v i="$shard" '(NR - 1) % n == i')
    echo "helper $WINCROSS_SHARD: ${#objs[@]} object files"
    if [ "${#objs[@]}" -gt 0 ]; then
        ninja -C "$B" ${NINJA_JOBS:+-j"$NINJA_JOBS"} -k 0 "${objs[@]}" \
            || echo "helper $WINCROSS_SHARD: some files did not compile (the final job reports them)"
    fi
    exit 0
fi
# </Prestige>

ninja -C "$B" ${NINJA_JOBS:+-j"$NINJA_JOBS"}

# Stage exactly what viewer_manifest.py ships, then zip it.
STAGE="$PWD/$B/package/Prestige_win64"
rm -rf "$PWD/$B/package"; mkdir -p "$STAGE"
( cd "$B/newview" && "$OLDPWD/.venv/bin/python" "$OLDPWD/indra/newview/viewer_manifest.py" \
    "--actions=copy package" --arch=x86_64 --platform=windows \
    --artwork="$OLDPWD/indra/newview" --openal=ON --tracy=OFF \
    --build="$OLDPWD/$B/newview" --buildtype=Release "--channel=$VIEWER_CHANNEL" --configuration=. \
    --dest="$STAGE" --grid=agni --source="$OLDPWD/indra/newview" \
    --versionfile="$OLDPWD/$B/newview/viewer_version.txt" )
# <Prestige> code signing (scripts/wincross/sign.sh; a no-op until the PRESTIGE_SIGN_* secrets
# exist): every .exe / .dll Prestige ships unsigned is signed before it is zipped and packed,
# so Windows Smart App Control lets the viewer, its plugins and its DLLs run.
if bash scripts/wincross/sign.sh --configured; then
    mapfile -t unsigned < <(bash scripts/wincross/sign.sh --unsigned "$STAGE")
    [ "${#unsigned[@]}" -eq 0 ] || bash scripts/wincross/sign.sh "${unsigned[@]}"
fi
# </Prestige>
( cd "$B/package" && 7z a -tzip -mx=7 "${V_ARTIFACT_BASE}_win64.zip" Prestige_win64 >/dev/null )
ls -la "$B/package/${V_ARTIFACT_BASE}_win64.zip"

# Installer (#168), an extra asset beside the zip. Needs makensis (apt install nsis);
# without it the zip is still produced and the installer is skipped.
if command -v makensis >/dev/null; then
    DISPVER="$V_FULL_VERSION"
    [ "$V_KIND" = "release" ] || DISPVER="$V_FULL_VERSION ${V_KIND^}"
    APPNAME="Prestige"
    [ "$V_KIND" != "jetstream" ] || APPNAME="Prestige Jetstream"   # installs beside Beta / Release
    bash scripts/wincross/make_installer.sh "$STAGE" "$B/package/${V_ARTIFACT_BASE}_Setup.exe" "$V_FULL_VERSION" "$DISPVER" "$APPNAME"
    bash scripts/wincross/sign.sh "$B/package/${V_ARTIFACT_BASE}_Setup.exe"   # <Prestige/> no-op unsigned
else
    echo "makensis not found: skipping the Windows installer (apt install nsis)"
fi

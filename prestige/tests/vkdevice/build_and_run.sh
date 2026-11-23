#!/bin/bash
# Linux dev check for the ported Vulkan device code (no autobuild, no full viewer build).
# Needs: g++ (C++20), libboost-dev libglm-dev libapr1-dev libexpat1-dev, libvulkan1, and a Vulkan
# driver (Mesa's lavapipe works: apt install mesa-vulkan-drivers; then SLIPSTREAM_VK_ALLOW_CPU=1).
# usage: prestige/tests/vkdevice/build_and_run.sh   (from the repository root)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
OUT=${OUT:-/tmp/prestige-vkdevice}
mkdir -p "$OUT/shim/expat"
ln -sf /usr/include/expat.h "$OUT/shim/expat/expat.h"
ln -sf /usr/include/expat_external.h "$OUT/shim/expat/expat_external.h"
cd "$ROOT/indra"
INC=""
for d in llcommon llmath llrender llimage llxml llfilesystem llwindow llui; do INC="$INC -I$d"; done
FLAGS="-std=c++20 -w -O1 -DLL_LINUX=1 -DLINUX=1 -DPRESTIGE_VULKAN=1 $INC -Inewview/vkgfx -Inewview/vkgfx/compat \
  -Inewview/vkgfx/third_party/vulkan/include -Inewview/vkgfx/third_party/vma -I/usr/include/apr-1.0 -I$OUT/shim"
SRCS="newview/vkgfx/vk_compat.cpp newview/vkgfx/vk_requirements.cpp newview/vkgfx/vk_device.cpp newview/vkgfx/vk_vma.cpp
      newview/vkgfx/vk_swapchain.cpp newview/vkgfx/vk_pipeline.cpp newview/vkgfx/vk_pipecache.cpp newview/vkgfx/vk_pipeworker.cpp
      newview/vkgfx/vk_descpool.cpp newview/vkgfx/llcleanreplayvk.cpp newview/vkgfx/compat/llmemorygovernor.cpp
      newview/vkgfx/compat/ll_thread_name.cpp $ROOT/prestige/tests/vkdevice/llcommon_stubs.cpp $ROOT/prestige/tests/vkdevice/vkdevice_test.cpp"
OBJS=""
for f in $SRCS; do
  o="$OUT/$(basename "$f" .cpp).o"
  echo "compile $f"
  g++ $FLAGS -c "$f" -o "$o"
  OBJS="$OBJS $o"
done
LOADER=$(ls /usr/lib/x86_64-linux-gnu/libvulkan.so* | head -1)
g++ $OBJS "$LOADER" -lpthread -ldl -o "$OUT/vkdevice_test"
"$OUT/vkdevice_test"

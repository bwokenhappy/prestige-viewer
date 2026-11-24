# -*- cmake -*-
#
# Prestige Firestorm-Vulkan port: the Vulkan renderer from Slipstream, being ported into
# the Firestorm viewer in phases (see PORTING.md at the top of the repository).
#
# PRESTIGE_VULKAN OFF (default): the stock Firestorm OpenGL viewer. Nothing here is compiled.
# PRESTIGE_VULKAN ON: compiles the ported vkgfx code into the viewer and links the Vulkan
#                     loader. The OpenGL renderer still draws everything until a later phase
#                     switches the world and the UI over.
#
# Provides ll::vulkan: the vendored Khronos headers (newview/vkgfx/third_party/vulkan, the
# version Slipstream's code is written against), Vulkan Memory Allocator, the loader, and the
# PRESTIGE_VULKAN=1 define. On Windows the loader import library comes from the Vulkan SDK
# (VULKAN_SDK environment variable); the CI installs it.
include_guard()

option(PRESTIGE_VULKAN "Build the Vulkan renderer being ported from Slipstream (work in progress)" OFF)

add_library( ll::vulkan INTERFACE IMPORTED )

if (PRESTIGE_VULKAN)
  set(PRESTIGE_VKGFX_DIR "${CMAKE_CURRENT_LIST_DIR}/../newview/vkgfx")
  get_filename_component(PRESTIGE_VKGFX_DIR "${PRESTIGE_VKGFX_DIR}" ABSOLUTE)

  find_package( Vulkan QUIET )
  if (Vulkan_FOUND)
    set(_prestige_vk_loader "${Vulkan_LIBRARY}")
  else ()
    find_library( _prestige_vk_loader NAMES vulkan-1 vulkan libvulkan.so.1
                  HINTS "$ENV{VULKAN_SDK}/Lib" "$ENV{VULKAN_SDK}/lib" )
  endif ()
  if (NOT _prestige_vk_loader)
    message(FATAL_ERROR "PRESTIGE_VULKAN is ON but the Vulkan loader was not found. "
                        "Install the Vulkan SDK (Windows) or libvulkan-dev (Linux), or configure with -DPRESTIGE_VULKAN=OFF.")
  endif ()

  # Vendored headers first so Slipstream's code sees the header version it was written for.
  target_include_directories( ll::vulkan SYSTEM BEFORE INTERFACE
      "${PRESTIGE_VKGFX_DIR}/third_party/vulkan/include"
      "${PRESTIGE_VKGFX_DIR}/third_party/vma" )
  target_link_libraries( ll::vulkan INTERFACE "${_prestige_vk_loader}" )
  target_compile_definitions( ll::vulkan INTERFACE PRESTIGE_VULKAN=1 )

  # Shader compiler, needed from the phase that brings in the renderer's shaders.
  find_program( GLSLC_EXECUTABLE glslc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" )

  message(STATUS "Prestige Vulkan port: ON (loader ${_prestige_vk_loader}, glslc ${GLSLC_EXECUTABLE})")
else ()
  message(STATUS "Prestige Vulkan port: OFF (OpenGL Firestorm viewer)")
endif ()

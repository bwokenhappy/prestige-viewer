# -*- cmake -*-
#
# AMD FSR SDK runtime for Windows (#1600): FSR Upscaling 4.1.2 (ML) and FSR Frame Generation
# 4.0.2 on Vulkan. The viewer loads amd_fidelityfx_loader_vk.dll at run time
# (vkgfx/llcleanffxvk.cpp, LoadLibraryExW from the executable folder); the loader finds its
# effect DLLs beside it. Without them, or on a GPU / driver without FSR 4, the viewer keeps
# its own source-built FSR 3.1 (vkgfx/llcleanfsr3vk.cpp, indra/vkclean/third_party/fidelityfx).
#
# AMD ships these only as signed binaries in the FidelityFX-SDK repository
# (Kits/FidelityFX/signedbin), and applications must use them as signed. docs/license.md lists
# them under the MIT licence (notice: indra/newview/licenses-win32.txt). Each DLL is fetched
# from AMD's v2.4.0 commit and verified (SHA256) into the 3p download cache (SL_3P_CACHE_DIR),
# so offline rebuilds work from there, then staged into SHARED_LIB_STAGING_DIR for
# viewer_manifest.py. To bump: change FFX_SDK_COMMIT and the three hashes. There is no Linux
# build of these: Linux keeps FSR 3.1.
include_guard()

include(Prebuilt)   # SL_3P_CACHE_DIR

if (NOT WINDOWS OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x64)$")
  return()
endif ()

set(FFX_SDK_VERSION 2.4.0)
# The v2.4.0 tag's commit (a tag can move; a commit cannot)
set(FFX_SDK_COMMIT 7539ae8b6568ad29ed78a753404351daf3b5cca7)
set(FFX_SDK_BASE_URL
    "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/raw/${FFX_SDK_COMMIT}/Kits/FidelityFX/signedbin"
    CACHE STRING "AMD FSR SDK signed DLLs (a mirror must serve the same bytes)")
set(FFX_SDK_DLLS
    "amd_fidelityfx_loader_vk.dll|38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9"
    "amd_fidelityfx_upscaler_vk.dll|4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7"
    "amd_fidelityfx_framegeneration_vk.dll|d002dabff8c5e71eaa715fc99438cd4dc81230262a7897b16bc402efec3498b4")

set(_ffx_cache "${SL_3P_CACHE_DIR}/fidelityfx-sdk-${FFX_SDK_VERSION}")
set(_ffx_staged_files)
set(_ffx_copy_commands)
foreach (_entry IN LISTS FFX_SDK_DLLS)
  string(REPLACE "|" ";" _pair "${_entry}")
  list(GET _pair 0 _name)
  list(GET _pair 1 _sha)
  set(_file "${_ffx_cache}/${_name}")
  set(_ok FALSE)
  if (EXISTS "${_file}")
    file(SHA256 "${_file}" _got)
    if (_got STREQUAL "${_sha}")
      set(_ok TRUE)
    else ()
      message(WARNING "FSR SDK: ${_file} does not match its SHA256; downloading it again")
      file(REMOVE "${_file}")
    endif ()
  endif ()
  if (NOT _ok)
    file(MAKE_DIRECTORY "${_ffx_cache}")
    string(RANDOM LENGTH 8 _rnd)
    set(_part "${_file}.part-${_rnd}")
    message(STATUS "FSR SDK: downloading ${_name} (${FFX_SDK_VERSION})")
    file(DOWNLOAD "${FFX_SDK_BASE_URL}/${_name}" "${_part}"
         STATUS _status
         TLS_VERIFY ON
         INACTIVITY_TIMEOUT 120
         EXPECTED_HASH SHA256=${_sha})
    list(GET _status 0 _code)
    if (NOT _code EQUAL 0)
      list(GET _status 1 _msg)
      file(REMOVE "${_part}")
      message(FATAL_ERROR "FSR SDK: failed to download ${FFX_SDK_BASE_URL}/${_name}: ${_msg}")
    endif ()
    file(RENAME "${_part}" "${_file}")
  endif ()
  list(APPEND _ffx_staged_files "${_file}")
  list(APPEND _ffx_copy_commands
       COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_file}" "${SHARED_LIB_STAGING_DIR}/${_name}")
endforeach ()

# Copy beside the other staged shared libraries (the viewer target depends on it, newview).
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/fidelityfx_sdk.staged"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${SHARED_LIB_STAGING_DIR}"
  ${_ffx_copy_commands}
  COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_BINARY_DIR}/fidelityfx_sdk.staged"
  DEPENDS ${_ffx_staged_files}
  VERBATIM)
add_custom_target(fidelityfx_sdk_stage ALL DEPENDS "${CMAKE_BINARY_DIR}/fidelityfx_sdk.staged")

# Preserve fallback requests even when the host already loaded that family/style.
function(sl_rml_fallback_patch name before after needle replacement)
  set(source "${RMLUI_SOURCE_DIR}/Source/Core/FontEngineDefault/${name}")
  file(SHA256 "${source}" hash)
  if(hash STREQUAL after)
    return()
  endif()
  if(NOT hash STREQUAL before)
    message(FATAL_ERROR "RmlUi fallback patch requires pinned 6.3 ${name}")
  endif()
  file(READ "${source}" text)
  string(REPLACE "${needle}" "${replacement}" text "${text}")
  file(WRITE "${source}" "${text}")
  file(SHA256 "${source}" hash)
  if(NOT hash STREQUAL after)
    message(FATAL_ERROR "Unexpected RmlUi fallback output ${name}")
  endif()
endfunction()
sl_rml_fallback_patch("FontFamily.cpp" "27750d10658c077fbc83ee73bcf4f2a89a3dac78321593acdc64b0852107aa84" "9725c23030c0f4f02bd509f7aba663aecc0acedd820fb29674855632f716a48a" [=[return {FontProvider::FontFaceLoadResult::Duplicate, nullptr};]=] [=[return {FontProvider::FontFaceLoadResult::Duplicate, face.face.get()};]=])
sl_rml_fallback_patch("FontProvider.cpp" "314e8be0e6f2012fe8ea6ee58cd9c0495d49df58f181ce1d55df2b23a08a03ab" "cbbcba45242ab7beb67fc8128a527662ba0a9a49ee2a06438a82ec6c8799ee79" [=[if (result != FontFaceLoadResult::Success)
		return result;]=] [=[if (result == FontFaceLoadResult::Error)
		return result;]=])

# Application-owned, on-demand fallback, after the pinned engine's eager chain.
set(_source "${RMLUI_SOURCE_DIR}/Source/Core/FontEngineDefault/FontFaceHandleDefault.cpp")
file(READ "${_source}" _text)
file(SHA256 "${_source}" _before)
if(_before STREQUAL "6cdabe6cf9700e2ff2e441d4e9412d289a48097a22a346f8b14fd87c91bef6d4")
  return()
endif()
if(NOT _before STREQUAL "ba935cc21d8484d7c2a67057bdef89e13562b970b639fedf995d6cde7b55c332")
  message(FATAL_ERROR "RmlUi fallback hook requires pinned 6.3 source")
endif()
set(_anchor "namespace Rml {")
set(_hook [=[namespace Rml {

using SlFallbackResolver = bool (*)(unsigned int);
static SlFallbackResolver slFallbackResolver = nullptr;
extern "C" void slRmlSetFallbackResolver(SlFallbackResolver resolver)
{
    slFallbackResolver = resolver;
}
]=])
string(REPLACE "${_anchor}" "${_hook}" _text "${_text}")
set(_anchor "			// If we still have not found a glyph, use the replacement character.")
set(_hook [=[            // Ask the application's classic/system resolver only after eager faces fail.
            // A successful callback adds a new fallback face; retry once through that chain.
            if (it_glyph == glyphs.end() && slFallbackResolver &&
                slFallbackResolver(static_cast<unsigned int>(character)))
                return GetOrAppendGlyph(character, true);

]=])
string(FIND "${_text}" "${_anchor}" _found)
if(_found LESS 0)
  message(FATAL_ERROR "Pinned fallback insertion point changed")
endif()
string(REPLACE "${_anchor}" "${_hook}${_anchor}" _text "${_text}")
file(WRITE "${_source}" "${_text}")
file(SHA256 "${_source}" _after)
if(NOT _after STREQUAL "6cdabe6cf9700e2ff2e441d4e9412d289a48097a22a346f8b14fd87c91bef6d4")
  message(FATAL_ERROR "Unexpected RmlUi fallback hook output")
endif()

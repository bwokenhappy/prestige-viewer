// #703 / #1242 The new interface's widget kit (see llnewuikit.h).
#include "llviewerprecompiledheaders.h"
#include "llnewuikit.h"

#include "lldir.h"
#include "llfile.h"
#include "llviewercontrol.h"
#include "llfontfreetypesvg.h"
#include "llfontgl.h"
#include "llwindow.h"
#include <set>

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/DataVariable.h>

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/FontEngineInterface.h>

extern "C" bool slRmlSetSvgHooks(void* hooks);

extern "C" void slRmlSetFallbackResolver(bool (*resolver)(unsigned int));

namespace
{
// Faces, not character misses: bounded by the installed font set. No new per-message cache.
std::set<std::pair<std::string, int>> sNativeFallbackFaces;
bool loadNativeFallback(const std::string& path, int index = 0)
{
    if (!LLFile::isfile(path)) return false;
    auto key = std::make_pair(path, index);
    if (sNativeFallbackFaces.find(key) != sNativeFallbackFaces.end()) return false;
    // Insert before loading so an allocation failure cannot leave a repeatedly loaded face.
    auto inserted = sNativeFallbackFaces.insert(key);
    if (Rml::GetFontEngineInterface()->LoadFontFace(path, index, true, Rml::Style::FontWeight::Auto)) return true;
    sNativeFallbackFaces.erase(inserted.first);
    return false;
}
bool resolveNativeGlyph(unsigned int character)
{
    try
    {
        std::string path;
        S32 index = 0;
        return LLWindow::findFallbackFontForChar(character, path, index) && loadNativeFallback(path, index);
    }
    catch (const std::bad_alloc&) { return false; }
}
}

void LLNewUIKit::loadFonts()
{
    for (const char* face : { "Inter-Regular.ttf", "Inter-Medium.ttf", "Inter-SemiBold.ttf", "Inter-Bold.ttf" })
        Rml::LoadFontFace(gDirUtilp->getExpandedFilename(LL_PATH_FONTS, face));
    // The kit uses a separate FreeType library. Install classic's existing SVG
    // rasterizer there as well: loading Twemoji alone leaves its glyphs unrenderable.
    SVG_RendererHooks hooks = { LLFontFreeTypeSvgRenderer::OnInit, LLFontFreeTypeSvgRenderer::OnFree,
                               LLFontFreeTypeSvgRenderer::OnRender, LLFontFreeTypeSvgRenderer::OnPresetGlypthSlot };
    if (!slRmlSetSvgHooks(&hooks))
        LL_WARNS("RmlUi") << "Could not install the classic emoji renderer" << LL_ENDL;
    Rml::LoadFontFace(gDirUtilp->getExpandedFilename(LL_PATH_FONTS, "TwemojiSVG.ttf"), true);
    // Share classic's configured default faces and platform search order, not a short
    // hardcoded list. Absolute fontconfig paths must not be prefixed with fonts/.
    try
    {
        sNativeFallbackFaces.clear();
        const std::vector<std::string> roots = {
            LLFontGL::getFontPathLocal(),
            gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "fonts", "") + gDirUtilp->getDirDelimiter(),
            LLFontGL::getFontPathSystem()
        };
        auto loadFile = [&](const std::string& file)
        {
            if (loadNativeFallback(file)) return;
            for (const auto& root : roots)
                if (loadNativeFallback(root + file)) break;
        };
        for (const auto& face : LLFontGL::getFallbackFontFiles()) loadFile(face);
        slRmlSetFallbackResolver(resolveNativeGlyph);
    }
    catch (const std::bad_alloc&)
    {
        // Keep every successfully admitted face and use the same lazy system resolver.
        slRmlSetFallbackResolver(resolveNativeGlyph);
        LL_WARNS("RmlUi") << "Native fallback font admission ran out of memory" << LL_ENDL;
    }
}

bool LLNewUISettingBinding::bind(Rml::DataModelConstructor& model, const std::string& variable, const std::string& control)
{
    LLControlVariable* var = gSavedSettings.getControl(control);
    if (!var)
        return false;
    const std::string name = control;
    const bool ok = model.BindFunc(
        variable,
        [name](Rml::Variant& out)
        {
            LLControlVariable* v = gSavedSettings.getControl(name);
            if (!v)
                return;
            const LLSD value = v->getValue();
            switch (v->type())
            {
                case TYPE_BOOLEAN: out = value.asBoolean(); break;
                case TYPE_S32:
                case TYPE_U32: out = value.asInteger(); break;
                case TYPE_F32: out = float(value.asReal()); break;
                default: out = Rml::String(value.asString()); break;
            }
        },
        [name](const Rml::Variant& in)
        {
            LLControlVariable* v = gSavedSettings.getControl(name);
            if (!v)
                return;
            switch (v->type())
            {
                case TYPE_BOOLEAN: v->setValue(LLSD(in.Get<bool>())); break;
                case TYPE_S32:
                case TYPE_U32: v->setValue(LLSD(in.Get<int>())); break;
                case TYPE_F32: v->setValue(LLSD(in.Get<float>())); break;
                default: v->setValue(LLSD(in.Get<Rml::String>())); break;
            }
        });
    if (!ok)
        return false;
    try
    {
        Rml::DataModelHandle handle = mModel;
        mConnections.emplace_back(var->getSignal()->connect(
            [handle, variable](LLControlVariable*, const LLSD&, const LLSD&) mutable { handle.DirtyVariable(variable); }));
    }
    catch (const std::bad_alloc&)
    {
        // bound, but changes made elsewhere show at the next full refresh only
    }
    return true;
}

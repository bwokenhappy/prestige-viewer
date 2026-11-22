// Real pinned RmlUi fallback hook: lazy/system-equivalent admission versus eager faces.
#include <RmlUi/Core.h>
#include <RmlUi/Core/FontEngineInterface.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <cstdlib>
#include <cstring>
#include <RmlUi/Core/StringUtilities.h>
#include <iostream>
#include <set>
#include <stdexcept>
#include <vector>
extern "C" void slRmlSetFallbackResolver(bool (*resolver)(unsigned int));
namespace
{
struct Font { std::string path; FT_Face face{}; };
std::vector<Font> fonts;
std::set<size_t> admitted;
size_t calls=0;
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
bool resolve(unsigned int character)
{
    ++calls;
    for(size_t i=0;i<fonts.size();++i)
        if(FT_Get_Char_Index(fonts[i].face,character))
        {
            if(!admitted.insert(i).second) return false;
            return Rml::GetFontEngineInterface()->LoadFontFace(fonts[i].path,0,true,Rml::Style::FontWeight::Auto);
        }
    return false;
}
struct Sink final : Rml::RenderInterface
{
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override { return 1; }
    void RenderGeometry(Rml::CompiledGeometryHandle,Rml::Vector2f,Rml::TextureHandle) override {}
    void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
    Rml::TextureHandle LoadTexture(Rml::Vector2i&,const Rml::String&) override { return 0; }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>,Rml::Vector2i) override { return 1; }
    void ReleaseTexture(Rml::TextureHandle) override {}
    void EnableScissorRegion(bool) override {}
    void SetScissorRegion(Rml::Rectanglei) override {}
};
std::vector<int> measure(const char* primary,int mode)
{
    Sink sink; Rml::SetRenderInterface(&sink); require(Rml::Initialise(),"initialise");
    require(Rml::LoadFontFace(primary),"primary");
    admitted.clear();
    // The viewer host loads these normally before LLNewUIKit asks for fallbacks.
    // Re-loading as fallback used to return success without registering the face.
    for(auto& font:fonts) require(Rml::LoadFontFace(font.path,false),"host preload");
    if(mode==1) for(auto& font:fonts) require(Rml::LoadFontFace(font.path,true),"eager fallback");
    slRmlSetFallbackResolver(mode==2 ? resolve : nullptr);
    auto* engine=Rml::GetFontEngineInterface();
    const auto handle=engine->GetFontFaceHandle("inter",Rml::Style::FontStyle::Normal,Rml::Style::FontWeight::Normal,24);
    require(handle!=0,"primary handle");
    std::vector<int> widths;
    const Rml::String language = "en";
    const Rml::TextShapingContext shaping{language};
    for(const char* text:{"ᴀʟᴛ ꜱᴏᴜɴᴅ", "Ā Ɔ ǅ Ȣ Ⱡ ⱥ Ḁ Ꞩ", "Ελληνικά Кириллица", "日本語 中文 한국어", "العربية עברית", "┌─┐ ∑ √ ∞ ≠"})
        widths.push_back(engine->GetStringWidth(handle,Rml::StringView(text,text+std::strlen(text)),shaping));
    slRmlSetFallbackResolver(nullptr);
    Rml::Shutdown(); return widths;
}
}
int main(int argc,char** argv)
{
    try
    {
        require(argc>=4,"Inter plus ordered system fallback faces required");
        FT_Library library{}; require(!FT_Init_FreeType(&library),"FreeType");
        for(int i=2;i<argc;++i) { Font font; font.path=argv[i]; require(!FT_New_Face(library,argv[i],0,&font.face),"fallback face"); fonts.push_back(std::move(font)); }
        const auto broken=measure(argv[1],0),eager=measure(argv[1],1),lazy=measure(argv[1],2);
        require(eager==lazy,"lazy glyph metrics must match eager reference for every script");
        require(broken!=lazy,"fixture must expose missing baseline glyphs");
        require(calls>0 && !admitted.empty(),"real missing-glyph hook must admit faces");
        for(auto& font:fonts) FT_Done_Face(font.face);
        FT_Done_FreeType(library);
        std::cout << "PASS: small caps, Latin A-D, Greek/Cyrillic, CJK, Arabic/Hebrew, box/math; eager == lazy, broken baseline differs\n";
        return 0;
    }
    catch(const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}

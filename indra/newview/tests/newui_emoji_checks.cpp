// CPU-only: the actual classic OT-SVG renderer, native RmlUi font engine and shipped font.
#include "llfontfreetypesvg.h"
#include <RmlUi/Core.h>
#include <cstdlib>
#include <iostream>

extern "C" bool slRmlSetSvgHooks(void* hooks);
namespace
{
struct Sink final : Rml::RenderInterface
{
    size_t colourPixels=0;
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override { return 1; }
    void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
    void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
    Rml::TextureHandle LoadTexture(Rml::Vector2i&, const Rml::String&) override { return 0; }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> bytes, Rml::Vector2i) override
    {
        for(size_t i=0;i+3<bytes.size();i+=4)
            if(bytes[i+3] && (bytes[i]!=bytes[i+1] || bytes[i+1]!=bytes[i+2])) ++colourPixels;
        return 1;
    }
    void ReleaseTexture(Rml::TextureHandle) override {}
    void EnableScissorRegion(bool) override {}
    void SetScissorRegion(Rml::Rectanglei) override {}
};
void require(bool ok,const char* message) { if(!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); } }
size_t run(const char* regular, const char* emoji, bool hooksOn)
{
    Sink sink; Rml::SetRenderInterface(&sink); require(Rml::Initialise(),"RmlUi initialise");
    if(hooksOn)
    {
        SVG_RendererHooks hooks={LLFontFreeTypeSvgRenderer::OnInit,LLFontFreeTypeSvgRenderer::OnFree,
                                LLFontFreeTypeSvgRenderer::OnRender,LLFontFreeTypeSvgRenderer::OnPresetGlypthSlot};
        require(slRmlSetSvgHooks(&hooks),"classic SVG hook installation");
    }
    require(Rml::LoadFontFace(regular),"regular font");
    require(Rml::LoadFontFace(emoji,true),"shipped emoji font");
    auto* context=Rml::CreateContext("emoji",{640,240}); require(context,"context");
    auto* doc=context->LoadDocumentFromMemory("<rml><head><style>body { font-family: Inter; font-size: 24px; color: white; }</style></head>"
        "<body><div>Sender 😀</div><div>Message 🎨</div><input type='text' value='Input 😀'/></body></rml>");
    require(doc,"document"); doc->Show(); context->Update(); context->Render();
    const size_t count=sink.colourPixels;
    Rml::RemoveContext("emoji"); Rml::Shutdown(); return count;
}
}
int main(int argc,char** argv)
{
    require(argc==3,"regular and emoji font paths required");
    const auto broken=run(argv[1],argv[2],false), fixed=run(argv[1],argv[2],true);
    require(broken==0,"baseline lacks the colour emoji renderer");
    require(fixed>0,"actual SVG emoji must produce coloured pixels");
    std::cout << "PASS: no-hook baseline " << broken << ", classic SVG renderer " << fixed << " coloured pixels; no GPU\n";
}

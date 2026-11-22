// CPU-only real-RmlUi regression. No Vulkan, viewer, account or private chat.
// Covers upward history overflow (which does not grow scroll height) and shared
// People/Inventory viewport clipping/hit-testing. Run with the bundled Inter font.
#include "llnewuikitstyle.h"
#include "rmluilab_newui.h"
#include <RmlUi/Core.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <RmlUi/Core/ElementText.h>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
struct Sink final : Rml::RenderInterface
{
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override { return 1; }
    void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
    void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
    Rml::TextureHandle LoadTexture(Rml::Vector2i&, const Rml::String&) override { return 0; }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override { return 1; }
    void ReleaseTexture(Rml::TextureHandle) override {}
    void EnableScissorRegion(bool) override {}
    void SetScissorRegion(Rml::Rectanglei) override {}
};
void require(bool ok, const char* message)
{
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
bool inHead(Rml::Element* e)
{
    for (; e; e = e->GetParentNode()) if (e->GetId() == "whead") return true;
    return false;
}
void run(Rml::Context* context, bool fixed, bool bottom, float density, LLNewUIKit::Theme theme)
{
    context->SetDensityIndependentPixelRatio(density);
    std::string sheet = LLNewUIKit::stylesheet(LLNewUIKit::tokens(theme, false));
    if (!fixed)
    {
        const auto pos = sheet.find("clip: always;");
        require(pos != std::string::npos, "production viewport must force clipping");
        sheet.erase(pos, std::string("clip: always;").size());
    }
    std::string rml = "<rml><head><style>" + sheet + "</style></head>"
        "<body class='window' style='left:100dp;top:100dp;width:500dp;height:400dp'>"
        "<div class='wframe'><div id='whead' class='whead'><div class='wtitle'>Synthetic title</div></div>"
        "<div class='wbody'><div id='history' class='viewport'><div id='msgs' style='position:absolute;left:0;right:0;";
    rml += bottom ? "bottom:0'>" : "top:0'>";
    for (int i = 0; i < 500; ++i)
        rml += "<div id='row" + std::to_string(i) + "' style='height:22dp'>Synthetic message</div>";
    rml += "</div></div><div style='height:32dp;flex:0 0 32dp'>Compose</div></div></div></body></rml>";
    auto* doc = context->LoadDocumentFromMemory(rml);
    require(doc != nullptr, "load synthetic window with production kit");
    doc->Show(); context->Update();
    auto* history = doc->GetElementById("history");
    auto* messages = doc->GetElementById("msgs");
    Rml::Rectanglei clip;
    const bool clipped = Rml::ElementUtilities::GetClippingRegion(doc->GetElementById("row490"), clip);
    auto* titleHit = context->GetElementAtPoint({130.f * density, 115.f * density});
    if (!fixed && bottom)
    {
        require(messages->GetAbsoluteTop() < 0, "baseline really overflows upward");
        require(history->GetScrollHeight() == history->GetClientHeight(), "negative overflow does not grow scroll height");
        require(!clipped && !inHead(titleHit), "baseline reproduces missing clip and title theft");
    }
    else
    {
        require(clipped && clip.Top() >= history->GetAbsoluteTop() - 1.f, "clip begins at viewport, not window title");
        require(inHead(titleHit), "title bar remains hit-testable");
        require(clip.Bottom() <= history->GetAbsoluteTop() + history->GetClientHeight() + 1.f, "clip ends inside viewport");
        // Moving/resizing does not reopen the clip or make content steal the header.
        doc->SetProperty("left", "160dp"); doc->SetProperty("top", "140dp"); doc->SetProperty("height", "260dp");
        context->Update();
        require(inHead(context->GetElementAtPoint({190.f * density, 155.f * density})), "title remains hit-testable after move/resize");
    }
    doc->Close(); context->Update();
}
}
void safeRichText(Rml::Context* context)
{
    std::string secret="MUST_NOT_RENDER";
    const std::string raw="😀 {{secret}} <script>bad</script> & \"quoted\"";
    std::string rich=LLNewUIKit::escapeText(raw);
    auto model=context->CreateDataModel("safety"); require(bool(model),"safety model");
    require(model.Bind("secret",&secret) && model.Bind("rich",&rich),"safety variables");
    auto* doc=context->LoadDocumentFromMemory("<rml><head><style>body{font-family:Inter;font-size:14px;}</style></head>"
        "<body><div data-model='safety'><div id='payload' data-rml='rich'></div></div></body></rml>");
    require(doc,"safe rich-text document"); doc->Show(); context->Update();
    auto* payload=doc->GetElementById("payload"); require(payload && payload->GetNumChildren()==1,"untrusted text creates no elements");
    auto* text=rmlui_dynamic_cast<Rml::ElementText*>(payload->GetFirstChild());
    require(text && text->GetText()==raw,"untrusted binding syntax remains literal and emoji bytes survive");
    doc->Close(); context->Update(); context->RemoveDataModel("safety");
}
void reachableGrip(Rml::Context* context,float density,LLNewUIKit::Theme theme)
{
    context->SetDensityIndependentPixelRatio(density);
    const auto rml=LLNewUIKit::themed("<rml><head><style>body.window{left:100dp;top:100dp;width:400dp;height:300dp;}"
        "#cover{position:absolute;right:0;bottom:0;width:80dp;height:40dp;z-index:5;}</style></head>"
        "<body class='window'><div class='wframe'><div class='whead'>Synthetic title</div>"
        "<div class='wbody'><div id='cover'>Actions</div></div><wgrip/></div></body></rml>", LLNewUIKit::tokens(theme,false));
    auto* doc=context->LoadDocumentFromMemory(rml); require(doc,"resize document"); doc->Show(); context->Update();
    auto* hit=context->GetElementAtPoint({494.f*density,394.f*density});
    while(hit && hit->GetId()!="wgrip") hit=hit->GetParentNode();
    require(hit,"resize grip remains above bottom-row controls");
    doc->Close(); context->Update();
}

void realNearbyLayout(Rml::Context* context)
{
    const auto scenes=labNewUIScenes(context);
    for(const auto& scene:scenes)
    {
        if (scene.name.find("newui-navigation-")==0 || scene.name.find("newui-chat-grouped-")==0)
        {
            context->SetDensityIndependentPixelRatio(1.f);
            std::vector<Rml::ElementDocument*> docs;
            for (const auto& text:scene.rmls) { auto* d=context->LoadDocumentFromMemory(text); require(d,"native extra document"); d->Show(); docs.push_back(d); }
            scene.prepare(context,docs); context->Update(); context->Update();
            if (scene.name.find("newui-navigation-")==0)
            {
                auto* hit=context->GetElementAtPoint({10.f,600.f});
                require(hit && hit->IsClassSet("scrim"),"navigation outside-click scrim spans the viewport");
                require(docs.front()->GetElementById("address")->IsVisible(true),"address input visible");
            }
            else
            {
                Rml::ElementList heads; docs.front()->GetElementsByClassName(heads,"grouphead");
                require(!heads.empty() && heads.front()->IsVisible(true),"grouped chat sender headers render");
                auto* messages=docs.front()->GetElementById("msgs");
                const auto firstRow=[](Rml::Element* container)->Rml::Element*
                {
                    Rml::ElementList rows; container->GetElementsByClassName(rows,"msg");
                    for (auto* candidate:rows) if (candidate->IsVisible(true)) return candidate;
                    return nullptr;
                };
                auto* row=firstRow(messages); require(row,"initial message row renders");
                Rml::ElementList payloads; row->GetElementsByClassName(payloads,"text");
                require(!payloads.empty() && payloads.front()->GetFirstChild(),"initial rich message renders");
                auto* textChild=payloads.front()->GetFirstChild();
                labAppendSyntheticChatMessage(); context->Update(); context->Update();
                require(firstRow(messages)==row,"append retains existing message row DOM");
                require(payloads.front()->GetFirstChild()==textChild,"append retains unchanged rich-text children");
            }
            for (auto* d:docs) d->Close();
            context->Update();
            continue;
        }
        if(scene.name.find("newui-nearby-")!=0) continue;
        for(float density:{1.f,1.1f,1.5f})
            for(const auto dims:{Rml::Vector2i{320,460},Rml::Vector2i{380,640}})
            {
                context->SetDensityIndependentPixelRatio(density);
                std::vector<Rml::ElementDocument*> docs;
                for(const auto& text:scene.rmls) { auto* doc=context->LoadDocumentFromMemory(text); require(doc,"production Nearby document"); doc->Show(); docs.push_back(doc); }
                scene.prepare(context,docs);
                auto* doc=docs.front(); doc->SetProperty("width",std::to_string(dims.x)+"dp"); doc->SetProperty("height",std::to_string(dims.y)+"dp");
                context->Update(); context->Update();
                auto* list=doc->GetElementById("list"); auto* range=doc->GetElementById("radarRange");
                require(list && list->GetClientHeight()>=23.f*density,"Nearby minimum leaves at least one full row");
                require(range && range->IsVisible() && range->GetAbsoluteTop()>=list->GetAbsoluteTop()+list->GetClientHeight()-1.f,"range stays below list");
                if(scene.name.find("newui-nearby-menu-")==0)
                {
                    auto* menu=doc->GetElementById("menu"); auto* body=doc->GetElementById("pbody");
                    require(menu && menu->IsVisible(true),"Nearby context menu renders");
                    require(menu->GetAbsoluteTop()+menu->GetOffsetHeight() <= body->GetAbsoluteTop()+body->GetClientHeight()+1.f,"full Nearby menu fits its body");
                    require(menu->GetScrollHeight() > menu->GetClientHeight(),"long Nearby menu scrolls instead of overflowing");
                }
                for(auto* d:docs) d->Close();
                context->Update();
            }
    }
}

int main(int argc, char** argv)
{
    require(argc == 2, "pass bundled Inter-Regular.ttf path");
    Sink sink; Rml::SetRenderInterface(&sink); require(Rml::Initialise(), "initialise RmlUi");
    require(Rml::LoadFontFace(argv[1]), "load bundled font");
    auto* context = Rml::CreateContext("clipping", {1920, 1080}); require(context != nullptr, "create CPU context");
    for (auto theme : {LLNewUIKit::Theme::Dark, LLNewUIKit::Theme::Light})
        for (float density : {1.f, 1.1f, 1.5f})
        {
            reachableGrip(context,density,theme);
            run(context, false, true, density, theme);
            run(context, true, true, density, theme); // Conversations + Nearby history
            run(context, true, false, density, theme); // positive-overflow People/Inventory rows
        }
    safeRichText(context);
    realNearbyLayout(context);
    Rml::RemoveContext("clipping"); Rml::Shutdown();
    std::cout << "PASS: baseline clipping reproduced; both themes, three scales, move/resize, raised grip and safe rich text\n";
}

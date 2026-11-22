// Actual RmlUi/official GL3 rasterization using Mesa llvmpipe only. No viewer/GPU.
#include "rmluilab_newui.h"
#include "RmlUi_Renderer_GL3.h"
#include "llfontfreetypesvg.h"
#include <RmlUi/Core.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
// EGL must establish Khronos calling conventions before GLAD's embedded subset.
#include "RmlUi_Include_GL3.h"
#include <png.h>
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>
extern "C" bool slRmlSetSvgHooks(void* hooks);
namespace
{
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
class Renderer final : public RenderInterface_GL3
{
public:
    std::filesystem::path iconDirectory;
private:
    Rml::TextureHandle LoadTexture(Rml::Vector2i& size, const Rml::String& source) override
    {
        png_image image{}; image.version = PNG_IMAGE_VERSION;
        // RmlUi resolves in-memory document URLs and can discard leading ../ segments.
        // This lab maps the exported skin image name into its explicit asset root.
        const auto file = iconDirectory / std::filesystem::path(source).filename();
        if (!png_image_begin_read_from_file(&image, file.c_str()))
            throw std::runtime_error("Missing exported UI asset: " + source);
        image.format = PNG_FORMAT_RGBA;
        std::vector<unsigned char> bytes(PNG_IMAGE_SIZE(image));
        if (!png_image_finish_read(&image, nullptr, bytes.data(), 0, nullptr)) { png_image_free(&image); return 0; }
        size = {int(image.width), int(image.height)};
        png_image_free(&image);
        for (size_t i=0; i<bytes.size(); i+=4)
            for (size_t channel=0; channel<3; ++channel) bytes[i+channel] = static_cast<unsigned char>((unsigned(bytes[i+channel])*bytes[i+3]+127)/255);
        return GenerateTexture(bytes, size);
    }
};
class System final : public Rml::SystemInterface
{
public:
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override
    { std::cerr << int(type) << ": " << message << '\n'; return true; }
};
void audit(Rml::ElementDocument* doc)
{
    if(auto* bar=doc->GetElementById("bar"))
    {
        auto* place=doc->GetElementById("location"); auto* search=doc->GetElementById("command");
        require(place && search && place->GetOffsetWidth()>20.f && search->GetOffsetWidth()>20.f,"bar fields have width");
        require(std::abs(place->GetAbsoluteTop()-search->GetAbsoluteTop())<8.f,"top bar stays in one row");
        require(search->GetAbsoluteLeft()+search->GetOffsetWidth()<=bar->GetAbsoluteLeft()+bar->GetOffsetWidth()+2.f,"bar children fit");
        float previous=bar->GetAbsoluteLeft();
        for(int i=0;i<bar->GetNumChildren();++i)
        {
            auto* child=bar->GetChild(i);
            if(!child->IsVisible(true)) continue;
            require(child->GetAbsoluteLeft()>=previous-1.f,"bar children keep horizontal order");
            require(child->GetAbsoluteTop()>=bar->GetAbsoluteTop()-1.f && child->GetAbsoluteTop()+child->GetOffsetHeight()<=bar->GetAbsoluteTop()+bar->GetOffsetHeight()+1.f,"bar children stay in its row");
            previous=child->GetAbsoluteLeft()+child->GetOffsetWidth();
        }
    }
    if(auto* picker=doc->GetElementById("conversationPicker"); picker && picker->IsVisible(true))
    {
        auto* search=doc->GetElementById("conversationSearch"); auto* head=doc->GetElementById("chead");
        require(search && search->GetOffsetWidth()>180.f,"picker search is full width");
        require(head && picker->GetAbsoluteTop()>=head->GetAbsoluteTop()+head->GetOffsetHeight()-1.f,"picker does not hide window controls");
        Rml::ElementList rows; picker->GetElementsByClassName(rows,"opt");
        for(auto* row:rows) require(row->GetId().starts_with("conversationOption-"),"picker keyboard targets resolve");
    }
    Rml::ElementList distances; doc->GetElementsByClassName(distances,"distance");
    float edge=-1.f;
    for(auto* distance:distances)
    {
        if(!distance->IsVisible(true) || distance->GetOffsetWidth()<=0) continue;
        const float x=distance->GetAbsoluteLeft()+distance->GetOffsetWidth();
        if(edge>=0) require(std::abs(edge-x)<1.f,"Nearby distance columns align");
        edge=x;
    }
}
void save(const std::filesystem::path& path,int width,int height)
{
    std::vector<unsigned char> raw(size_t(width)*size_t(height)*4), top(raw.size());
    glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,raw.data());
    const size_t stride=size_t(width)*4;
    for (int y=0;y<height;++y) std::copy_n(raw.data()+size_t(height-1-y)*stride,stride,top.data()+size_t(y)*stride);
    png_image image{}; image.version=PNG_IMAGE_VERSION; image.width=unsigned(width); image.height=unsigned(height); image.format=PNG_FORMAT_RGBA;
    require(png_image_write_to_file(&image,path.c_str(),0,top.data(),0,nullptr)!=0,"write PNG");
}
}
int main(int argc,char** argv)
{
    try
    {
        require(argc==6,"source-root icons output width height");
        require(std::getenv("LIBGL_ALWAYS_SOFTWARE") && std::string(std::getenv("LIBGL_ALWAYS_SOFTWARE"))=="1","software-only environment required");
        const int width=std::stoi(argv[4]),height=std::stoi(argv[5]);
        require(width>0 && width<=3840 && height>0 && height<=2160,"bounded render dimensions");
        auto display=eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,nullptr);
        require(display!=EGL_NO_DISPLAY && eglInitialize(display,nullptr,nullptr),"Mesa EGL initialise");
        require(eglBindAPI(EGL_OPENGL_API),"OpenGL API");
        const EGLint attributes[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_DEPTH_SIZE,24,EGL_STENCIL_SIZE,8,EGL_NONE};
        EGLConfig config{}; EGLint count=0; require(eglChooseConfig(display,attributes,&config,1,&count) && count==1,"EGL config");
        const EGLint surfaceAttributes[]={EGL_WIDTH,width,EGL_HEIGHT,height,EGL_NONE};
        auto surface=eglCreatePbufferSurface(display,config,surfaceAttributes);
        const EGLint contextAttributes[]={EGL_CONTEXT_MAJOR_VERSION,3,EGL_CONTEXT_MINOR_VERSION,3,EGL_CONTEXT_OPENGL_PROFILE_MASK,EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,EGL_NONE};
        auto glContext=eglCreateContext(display,config,EGL_NO_CONTEXT,contextAttributes);
        require(eglMakeCurrent(display,surface,surface,glContext),"EGL current");
        require(RmlGL3::Initialize(),"GL3 initialize");
        const std::string device=reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        require(device.find("llvmpipe")!=std::string::npos || device.find("softpipe")!=std::string::npos,"refusing hardware renderer");
        std::cout << "CPU rasterizer: " << device << '\n';
        {
            Renderer renderer; renderer.iconDirectory=argv[2]; require(bool(renderer),"official renderer"); renderer.SetViewport(width,height);
            System system; Rml::SetSystemInterface(&system); Rml::SetRenderInterface(&renderer); require(Rml::Initialise(),"RmlUi init");
            SVG_RendererHooks hooks={LLFontFreeTypeSvgRenderer::OnInit,LLFontFreeTypeSvgRenderer::OnFree,LLFontFreeTypeSvgRenderer::OnRender,LLFontFreeTypeSvgRenderer::OnPresetGlypthSlot};
            require(slRmlSetSvgHooks(&hooks),"classic emoji rasterizer");
            const std::filesystem::path root=argv[1];
            for (const char* file:{"Inter-Regular.ttf","Inter-Medium.ttf","Inter-SemiBold.ttf","Inter-Bold.ttf"})
                require(Rml::LoadFontFace((root/"indra/newview/fontsets"/file).string()),"Inter font");
            // Fonts are supplied by the same package and system, never substituted bitmap text.
            for (const auto& dir:{root/"build-linux-x86_64/newview/packaged/fonts",root/"indra/newview/fonts",std::filesystem::path("/usr/share/fonts/truetype/dejavu")})
                for (const char* file:{"TwemojiSVG.ttf","DejaVuSans.ttf","DejaVuSansMono.ttf"})
                    if (std::filesystem::is_regular_file(dir/file)) Rml::LoadFontFace((dir/file).string(),true);
            // The broad-script fixture needs only these system faces, not every installed font.
            for (const char* file:{"/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
                "/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc"})
                if(std::filesystem::is_regular_file(file)) Rml::LoadFontFace(file,true);
            labRegisterImageDecorator(argv[2]);
            std::filesystem::create_directories(argv[3]);
            for (float density:{1.f,1.5f})
            {
                auto* context=Rml::CreateContext("software",{width,height}); require(context,"context");
                context->SetDensityIndependentPixelRatio(density);
                auto scenes=labNewUIScenes(context);
                for (auto& scene:scenes)
                {
                    const bool wanted=scene.name.starts_with("newui-main-dark-on-dark") || scene.name.starts_with("newui-nearby-") || scene.name.starts_with("newui-chat-") || scene.name.starts_with("newui-chat-grouped-") || scene.name.starts_with("newui-picker-");
                    if(!wanted || scene.name.starts_with("newui-nearby-menu-")) continue;
                    std::vector<Rml::ElementDocument*> docs;
                    for (auto& markup:scene.rmls) { auto* doc=context->LoadDocumentFromMemory(markup); require(doc,"document"); docs.push_back(doc); doc->Show(); }
                    if(scene.prepare) scene.prepare(context,docs);
                    for(int frame=0;frame<3;++frame) { context->Update(); glClearColor(0.08f,0.1f,0.13f,1.f); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT); renderer.BeginFrame(); context->Render(); renderer.EndFrame(); }
                    for(auto* doc:docs) audit(doc);
                    auto name=scene.name+"_"+std::to_string(width)+"x"+std::to_string(height)+"_"+(density==1.f?"1":"1.5")+".png";
                    save(std::filesystem::path(argv[3])/name,width,height); std::cout << name << '\n';
                    for(auto* doc:docs) doc->Close();
                    context->Update();
                }
                Rml::RemoveContext("software");
            }
            Rml::Shutdown();
        }
        RmlGL3::Shutdown(); eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT); eglDestroyContext(display,glContext); eglDestroySurface(display,surface); eglTerminate(display);
        return 0;
    }
    catch(const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}

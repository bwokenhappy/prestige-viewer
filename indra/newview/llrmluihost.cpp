#include "llviewerprecompiledheaders.h"
#include "llrmluihost.h"
#include "llrmluiatlas.h"
#include "llrmluiclassic.h"
#include "llnewui.h"
#include "llnewuistatus.h"
#include "slcameramode.h"
#include "slcameraview.h"
#include "slvideostudio.h"
#include "slvideostudioview.h"
#include "llclipboard.h"
#include "llui.h"
#include "llview.h"
#include "vkgfx/llrmluivk.h"
#include "vkgfx/llvulkancontext.h"
#include "vkgfx/vk_ui_surface.h"
#include "vkgfx/vk_ui_texture_host.h"
#include "llviewercontrol.h"
#include "lldir.h"
#include "llstartup.h"
#include "pipeline.h"   // RENDER_DEBUG_FEATURE_UI: the interface hidden

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ObserverPtr.h>
#include <RmlUi/Core/SystemInterface.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <new>
#include <vector>

namespace
{
class DemoSystem final : public Rml::SystemInterface
{
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

public:
    double GetElapsedTime() override { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); }
    bool   LogMessage(Rml::Log::Type type, const Rml::String& message) override
    {
        if (type <= Rml::Log::LT_WARNING)
            LL_WARNS("RmlUi") << message << LL_ENDL;
        return true;
    }
    // The cursor the hovered element asks for; camera mode applies it over its overlay.
    void SetMouseCursor(const Rml::String& cursor_name) override { cursor = cursor_name; }
    std::string cursor;
    // Copy, cut and paste in RmlUi text fields use the viewer's clipboard (#1572: an address pasted
    // into camera mode's e-mail card).
    void SetClipboardText(const Rml::String& text) override
    {
        const LLWString w = utf8str_to_wstring(text);
        LLClipboard::instance().copyToClipboard(w, 0, (S32)w.size());
    }
    void GetClipboardText(Rml::String& text) override
    {
        LLWString w;
        if (LLClipboard::instance().pasteFromClipboard(w))
            text = wstring_to_utf8str(w);
    }
};
struct Demo
{
    DemoSystem                 system;
    std::unique_ptr<LLRmlUiVK> renderer;
    Rml::Context*              context  = nullptr;
    Rml::ElementDocument*      document = nullptr;
    uint32_t                   width = 0, height = 0;
    uint32_t                   operationWidth = 0, operationHeight = 0;
    float                      density     = 0;
    bool                       initialized = false, installed = false, hidden = false, poisoned = false;
    bool                       demoShown = false, classicShown = false, newShown = false;
    // The full-screen overlay modes, one at a time, each over the hidden interface: camera mode
    // (#1572) and Video Studio (#1668). Each is requested once (sticky), and may fail to start.
    enum class Overlay
    {
        None,
        Camera,
        Studio
    };
    bool                       cameraRequested = false, cameraBroken = false;
    bool                       studioRequested = false, studioBroken = false;
    Overlay                    shown = Overlay::None;   // the overlay document showing
    // Transient failures (a device reset being recovered, a capture that failed) are waited out;
    // camera mode falls back to the classic window (the studio leaves) only if they last this long.
    std::chrono::steady_clock::time_point overlayFailingSince{};
    void overlayFailed(Overlay which, std::chrono::steady_clock::time_point now) noexcept
    {
        if (which == Overlay::None)
            return;
        if (overlayFailingSince == std::chrono::steady_clock::time_point{})
            overlayFailingSince = now;
        else if (now - overlayFailingSince > std::chrono::seconds(10))
            (which == Overlay::Camera ? cameraBroken : studioBroken) = true;
    }
    Overlay overlayWanted() const noexcept
    {
        if (cameraRequested && !cameraBroken && SLCameraMode::active())
            return Overlay::Camera;
        if (studioRequested && !studioBroken && SLVideoStudio::active())
            return Overlay::Studio;
        return Overlay::None;
    }
    bool overlayRequested() const noexcept { return cameraRequested || studioRequested; }
    static void releaseOverlays() noexcept
    {
        SLCameraView::release();
        SLVideoStudioView::release();
    }
    void stopOverlay() noexcept
    {
        if (shown == Overlay::Camera)
            SLCameraView::stop();
        else if (shown == Overlay::Studio)
            SLVideoStudioView::stop();
        shown = Overlay::None;
    }
    static bool syncOverlay(Overlay which, float scale)
    {
        return which == Overlay::Camera ? SLCameraView::sync(scale) : which == Overlay::Studio ? SLVideoStudioView::sync(scale) : false;
    }
    static float overlayDpScale(Overlay which)
    {
        return which == Overlay::Camera ? SLCameraView::dpScale() : which == Overlay::Studio ? SLVideoStudioView::dpScale() : 1.f;
    }
    // The documents hidden while the camera document shows (the interface is hidden).
    std::vector<Rml::ObserverPtr<Rml::Element>> suspended;
    const char*                phase = "idle";
    // At most one shell and one poisoned CPU runtime per process. No destructor
    // can safely traverse a partially initialized/updated RmlUi render manager.
    LLRmlUiVK*                            quarantinedShell = nullptr;
    std::chrono::steady_clock::time_point retryAt{};

    void poison(const char* reason) noexcept
    {
        if (poisoned)
            return;
        poisoned         = true;
        const auto state = renderer ? renderer->memoryState() : LLRmlUiVK::MemoryState{};
        // std::bad_alloc does not expose the failed request size. Report that
        // explicitly, alongside known committed capacities, rather than invent
        // a zero-sized request. stdio avoids allocating a C++ logging message.
        std::fprintf(stderr,
                     "[RmlUi] disabled for process: reason=%s phase=%s request_bytes=unknown "
                     "memory_state=cpu_allocation_failure buffer_capacity_bytes=%llu external_image_bytes=%llu "
                     "image_budget_bytes=%llu vk_soft_failures=%u target_pixels=%ux%u last_use=%llu completed=%llu; Classic UI continues\n",
                     reason, phase, static_cast<unsigned long long>(state.buffers), static_cast<unsigned long long>(state.images),
                     static_cast<unsigned long long>(state.imageBudget), state.softFailures, operationWidth, operationHeight,
                     static_cast<unsigned long long>(state.lastUse), static_cast<unsigned long long>(state.completed));
        if (renderer)
            renderer->hide();
        // The LLView renderer takes every view back; no RmlUi call is made for that.
        LLView::sPresentHook = nullptr;
        // The classic menu bar and status bar the new interface hid (LLView calls only).
        LLNewUIStatus::restoreClassic();
        // Do not call RemoveContext, Shutdown, or any other RmlUi operation.
        // Its CPU state and interface shell remain valid, inert process state.
        initialized = false;
        hidden      = true;
        context     = nullptr;
        document    = nullptr;
        releaseOverlays();
        suspended.clear();
        shown = Overlay::None;
    }
    bool retire(uint64_t completed) noexcept
    {
        if (!renderer)
            return true;
        renderer->collect(completed);
        if (renderer->lastSerial() > completed)
            return false;
        if (poisoned)
        {
            if (!renderer->releaseGPUState(completed))
                return false;
            quarantinedShell = renderer.release();
        }
        else
            renderer.reset();
        return true;
    }

    void close() noexcept
    {
        if (renderer)
            renderer->hide();
        if (poisoned)
            return;
        if (initialized)
        {
            try
            {
                phase = "remove_context";
                LLRmlUiClassic::instance().stop();
                LLNewUI::instance().stop();
                LLRmlUiAtlas::instance().reset();
                releaseOverlays();
                suspended.clear();
                demoShown = classicShown = newShown = false;
                shown = Overlay::None;
                if (context)
                    Rml::RemoveContext("vk-rml-main");
                phase = "shutdown";
                Rml::Shutdown();
            }
            catch (const std::bad_alloc&)
            {
                poison("bad_alloc");
                return;
            }
        }
        Rml::SetRenderInterface(nullptr);
        Rml::SetSystemInterface(nullptr);
        context     = nullptr;
        document    = nullptr;
        initialized = false;
        hidden      = true;
    }
    void shutdown(uint64_t completed) noexcept
    {
        close();
        // The provider calls this hook after actual device completion, before
        // destroying the world bridge. No frame-age estimate and no extra wait.
        retire(completed);
        installed = false;
    }
    bool create(VkDeviceContext& device, LLCleanRendererVK& world, const VkUISurfaceTarget& target)
    {
        renderer = LLRmlUiVK::create(device, slWorldUITextureHost(world));
        if (!renderer)
            return false;
        Rml::SetSystemInterface(&system);
        Rml::SetRenderInterface(renderer.get());
        phase       = "initialise";
        initialized = Rml::Initialise();
        if (!initialized)
        {
            poison("initialise_returned_false");
            return false;
        }
        phase = "load_font";
        for (const char* face : { "DejaVuSans.ttf", "DejaVuSans-Bold.ttf", "DejaVuSans-Oblique.ttf", "DejaVuSans-BoldOblique.ttf",
                                  "DejaVuSansMono.ttf" })
            if (!Rml::LoadFontFace(gDirUtilp->getExpandedFilename(LL_PATH_FONTS, face)) && face[9] == '.')
                return false; // the regular face is required; the others are optional
        phase = "atlas";
        LLRmlUiAtlas::instance().registerDecorator();
        LLRmlUiAtlas::instance().attach(*renderer);
        phase   = "create_context";
        context = Rml::CreateContext("vk-rml-main", { int(target.extent.width), int(target.extent.height) }, renderer.get());
        if (!context || !renderer->begin())
        {
            if (renderer->allocationFailed())
                poison("backend_bad_alloc");
            return false;
        }
        hidden = false;
        width = height = 0;
        density        = 0;
        return true;
    }
    // Documents follow the settings: the proof overlay (session toggle) and the classic
    // presenter (UIRenderer, fixed for the session).
    bool documents()
    {
        const bool wantDemo    = gSavedSettings.getBOOL("RenderVKRmlUiDemo");
        // 1: the classic UI translated onto RmlUi; 2: the new interface (its own documents, the
        // LLView UI otherwise as Classic).
        const bool wantClassic = slRmlUiMode() == 1;
        const bool wantNew     = slRmlUiMode() == 2;
        if (wantClassic && !classicShown)
        {
            phase        = "classic_start";
            classicShown = LLRmlUiClassic::instance().start(context);
            if (!classicShown)
                return false;
        }
        if (wantNew && !newShown)
        {
            phase    = "new_ui_start";
            newShown = LLNewUI::instance().start(context);
            if (!newShown)
                return false;
        }
        if (wantDemo != demoShown)
        {
            if (wantDemo)
            {
                phase = "load_document";
                if (!document)
                    document = context->LoadDocumentFromMemory(R"rml(
<rml><head><style>
body { width: 400dp; margin: 72dp; padding: 24dp; font-family: DejaVu Sans;
 font-size: 18dp; color: #ffffff; background-color: #17304c99;
 border: 1dp #a1cee388; border-radius: 18dp; backdrop-filter: blur(8px);
 box-shadow: 0dp 10dp 22dp #00000088; }
h1 { font-size: 26dp; margin: 0dp 0dp 16dp 0dp; }
img { width: 64dp; height: 64dp; margin: 12dp 0dp; }
</style></head><body><h1>Vulkan UI</h1><p>Retained RmlUi geometry over the world.</p>
<img src="rmlui:checker"/><p>Live scene blur, text and image.</p></body></rml>
)rml");
                if (!document)
                    return false;
                document->Show();
            }
            else if (document)
                document->Hide();
            demoShown = wantDemo;
            width     = 0; // re-capture
        }
        // An overlay mode (camera mode, Video Studio): its document alone, over the hidden
        // interface (the others wait hidden).
        const Overlay want = overlayWanted();
        if (shown != Overlay::None && want != shown)
        {
            stopOverlay();
            suspend(false);
            width = 0;
        }
        if (want != Overlay::None && shown == Overlay::None)
        {
            phase = want == Overlay::Camera ? "camera_start" : "studio_start";
            suspend(true);
            const bool ok = want == Overlay::Camera ? SLCameraView::start(context) : SLVideoStudioView::start(context);
            if (ok)
                shown = want;
            else
            {
                // Not fatal for the other documents: snapshots fall back to the classic window,
                // the studio leaves.
                (want == Overlay::Camera ? cameraBroken : studioBroken) = true;
                suspend(false);
                LL_WARNS("RmlUi") << (want == Overlay::Camera ? "Camera mode's overlay could not start; snapshots use the classic window"
                                                              : "Video Studio's overlay could not start")
                                  << LL_ENDL;
            }
            width = 0;
        }
        return true;
    }
    // Hide every other document while camera mode shows, show them again after.
    void suspend(bool hide)
    {
        if (hide)
        {
            suspended.clear();
            for (int i = 0, n = context->GetNumDocuments(); i < n; ++i)
                if (Rml::ElementDocument* d = context->GetDocument(i); d && d->IsVisible())
                {
                    suspended.push_back(d->GetObserverPtr());
                    d->Hide();
                }
            return;
        }
        for (auto& e : suspended)
            if (e) // only documents were put here; one closed meanwhile is gone (null)
                static_cast<Rml::ElementDocument*>(e.get())->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        suspended.clear();
    }
    void record(const VkUISurfaceTarget& target) noexcept
    {
        if (target.surfaceID != NativeSurfaceMain)
            return; // VR / auxiliary targets: later (one context per target)
        // The interface is hidden (Ctrl+Alt+F1, camera mode #1573): so is its RmlUi layer. The
        // context and its documents stay; nothing is recorded until it is back.
        // (Before login the interface always draws, as in the VK UI pass.)
        // Camera mode's document is the exception: it draws while the interface is hidden.
        const bool ui_hidden = LLStartUp::getStartupState() >= STATE_STARTED
                               && !gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI);
        if (target.layerSegment > 0)
        {
            // A later z slot of this frame: the list captured for segment 0, its next part.
            if (renderer && !poisoned && !hidden && context && (!ui_hidden || shown != Overlay::None))
                renderer->record(target);
            return;
        }
        operationWidth      = target.extent.width;
        operationHeight     = target.extent.height;
        auto&      provider = LLVulkanContext::instance();
        const bool demoOn   = gSavedSettings.getBOOL("RenderVKRmlUiDemo");
        const bool wanted   = !poisoned && (slRmlUiMode() != 0 || demoOn || overlayRequested());
        if (renderer)
            renderer->collect(target.completedSerial);
        if (!wanted)
        {
            if (!hidden)
                close();
            return;
        }
        const Overlay overlay = overlayWanted();
        const bool camera = overlay != Overlay::None;   // an overlay mode is up
        if (overlay != shown && shown != Overlay::None && context)
        {
            // The overlay mode closed (even with the interface still hidden): its document goes
            // now, so the next entry starts it afresh.
            stopOverlay();
            suspend(false);
            width = 0;
        }
        if (ui_hidden && !camera)
            return;
        // Classic mode with only an overlay mode's reason: nothing per frame while it is closed
        // (once its document is hidden).
        if (slRmlUiMode() == 0 && !demoOn && !camera && shown == Overlay::None)
            return;
        if (hidden && renderer && renderer->lastSerial() > target.completedSerial)
            return;
        if (hidden && renderer)
        {
            renderer.reset();
            hidden = false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < retryAt)
            return;
        try
        {
            if (!renderer)
            {
                auto* device = provider.uiDeviceContext();
                auto* world  = provider.uiWorldRenderer();
                if (!device || !world || !create(*device, *world, target))
                {
                    if (camera)
                        overlayFailed(overlay, now); // waited out; the classic window only if it lasts
                    close();
                    retryAt = now + std::chrono::seconds(5);
                    return;
                }
            }
            if (!context || !documents())
            {
                if (camera)
                    overlayFailed(overlay, now);
                close();
                retryAt = now + std::chrono::seconds(5);
                return;
            }
            // Classic documents are laid out in LLView UI units: dp = framebuffer pixels per
            // UI unit (UI scale x display scale), exact for the root view. The proof document
            // alone uses the display scale.
            float scale = std::isfinite(target.dpiScaleX) && target.dpiScaleX > 0 ? target.dpiScaleX : 1.f;
            if (classicShown || newShown || shown != Overlay::None)
                if (LLView* root = LLUI::getInstance()->getRootView(); root && root->getRect().getHeight() > 0)
                    scale = float(target.extent.height) / float(root->getRect().getHeight());
            phase              = "classic_sync";
            // While an overlay mode shows, the other documents are hidden and not refreshed.
            bool changed = shown == Overlay::None && classicShown && LLRmlUiClassic::instance().sync();
            if (shown != Overlay::None)
            {
                phase = shown == Overlay::Camera ? "camera_sync" : "studio_sync";
                changed |= syncOverlay(shown, scale);
            }
            else if (newShown)
            {
                // Glass (the backdrop blur through RmlUi filters) needs full-target layers and
                // masks: within the UI image budget up to ~2560x1600, and only on SDR targets
                // (HDR targets are linear nits). Otherwise the solid surfaces: the automatic
                // fallback until #1242's fixed-cost blur source replaces the per-panel filters.
                const bool glass = target.displayPaperWhiteNits <= 0.f &&
                                   uint64_t(target.extent.width) * target.extent.height <= 2560ull * 1600ull;
                changed |= LLNewUI::instance().sync(scale, glass);
            }
            // An overlay mode sizes its document from the window (its dpScale).
            const float ratio = shown != Overlay::None ? scale * overlayDpScale(shown) : scale;
            if (changed || width != target.extent.width || height != target.extent.height || density != ratio)
            {
                const auto t0 = std::chrono::steady_clock::now();
                if (!renderer->begin())
                {
                    if (renderer->allocationFailed())
                        poison("backend_bad_alloc");
                    return;
                }
                phase = "configure_context";
                context->SetDimensions({ int(target.extent.width), int(target.extent.height) });
                context->SetDensityIndependentPixelRatio(ratio);
                phase = "layout_update";
                context->Update();
                phase = "render_document";
                context->Render();
                if (renderer->allocationFailed())
                {
                    poison("backend_bad_alloc");
                    return;
                }
                if (!renderer->end())
                {
                    if (camera)
                        overlayFailed(overlay, now); // waited out; the classic window only if it lasts
                    close();
                    retryAt = now + std::chrono::seconds(5);
                    return;
                }
                width   = target.extent.width;
                height  = target.extent.height;
                density = ratio;
                captureMs.add(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            }
            else
                captureMs.add(0.0);
            if (camera)
                overlayFailingSince = {}; // drawing again
            phase = "record_backend";
            if (!renderer->record(target))
            {
                // Nothing of the UI layer is drawn this frame: say why (rate-limited).
                if (now >= failReportAt)
                {
                    failReportAt  = now + std::chrono::seconds(5);
                    const auto ms = renderer->memoryState();
                    LL_WARNS("RmlUi") << "UI layer not drawn: record failed at llrmluivk.cpp:" << renderer->failureLine()
                                      << " images=" << ms.images << "/" << ms.imageBudget << " target=" << target.extent.width << "x"
                                      << target.extent.height << LL_ENDL;
                }
            }
            if (renderer->allocationFailed())
            {
                poison("backend_bad_alloc");
                return;
            }
            phase = "idle";
            if (now >= reportAt)
            {
                reportAt = now + std::chrono::seconds(60);
                const auto st = LLRmlUiClassic::instance().stats();
                LL_INFOS("RmlUi") << "rmlui_cpu capture_avg=" << captureMs.average() << " ms capture_peak=" << captureMs.peak
                                  << " ms captures=" << captureMs.nonzero << "/" << captureMs.count << " classic_sync=" << st.syncMs
                                  << " ms (peak " << st.peakSyncMs << ") presented=" << st.claimed << " nodes=" << st.nodes
                                  << " elements_created=" << st.created << " elements_destroyed=" << st.destroyed
                                  << " roots_remirrored=" << st.represented << LL_ENDL;
                LLRmlUiClassic::instance().resetCounters();
                captureMs = {};
            }
        }
        catch (const std::bad_alloc&)
        {
            poison("bad_alloc");
        }
    }
    struct Average
    {
        double   sum = 0, peak = 0;
        uint32_t count = 0, nonzero = 0;
        void     add(double ms)
        {
            sum += ms;
            peak = std::max(peak, ms);
            ++count;
            nonzero += ms > 0 ? 1u : 0u;
        }
        double average() const { return count ? sum / count : 0; }
    } captureMs;
    std::chrono::steady_clock::time_point reportAt{}, failReportAt{};
};
Demo demo;
} // namespace

uint32_t slRmlUiMode() noexcept
{
    // Fixed for the session (restart required): views switch renderer only at startup.
    static const uint32_t mode = std::min<uint32_t>(gSavedSettings.getU32("UIRenderer"), 2u);
    return mode;
}

void slRmlUiTick() noexcept
{
    auto& provider = LLVulkanContext::instance();
    if (!provider.isInitialized())
        return;
    const bool wanted =
        !demo.poisoned && (slRmlUiMode() != 0 || gSavedSettings.getBOOL("RenderVKRmlUiDemo") || demo.overlayRequested());
    if (demo.installed)
    {
        const uint64_t completed = provider.completedUISurfaceSerial();
        if (demo.renderer)
            demo.renderer->collect(completed);
        if (!wanted)
        {
            if (!demo.hidden)
                demo.close();
            if (demo.retire(completed))
            {
                provider.setMainUIRecorder({});
                provider.setUIRecorderShutdown({});
                demo.installed = false;
            }
        }
        return;
    }
    if (!wanted)
        return;
    try
    {
        provider.setUIRecorderShutdown([](uint64_t completed) { demo.shutdown(completed); });
        if (provider.setMainUIRecorder([](const VkUISurfaceTarget& target) { demo.record(target); }))
        {
            demo.installed = true;
        }
        else
            provider.setUIRecorderShutdown({});
    }
    catch (const std::bad_alloc&)
    {
        demo.phase = "register_recorder";
        demo.poison("bad_alloc");
    }
}

void slRmlUiRequestCamera() noexcept
{
    demo.cameraRequested = true;
}

bool slRmlUiCameraUsable() noexcept
{
    return LLVulkanContext::instance().isInitialized() && !demo.poisoned && !demo.cameraBroken;
}

bool slRmlUiCameraFailed() noexcept
{
    return demo.poisoned || demo.cameraBroken;
}

void slRmlUiRequestStudio() noexcept
{
    demo.studioRequested = true;
}

bool slRmlUiStudioUsable() noexcept
{
    return LLVulkanContext::instance().isInitialized() && !demo.poisoned && !demo.studioBroken;
}

bool slRmlUiStudioFailed() noexcept
{
    return demo.poisoned || demo.studioBroken;
}

const std::string& slRmlUiCursor() noexcept
{
    return demo.system.cursor;
}

ECursorType slRmlUiCursorType() noexcept
{
    const std::string& name = demo.system.cursor;
    if (name == "text")
        return UI_CURSOR_IBEAM;
    if (name == "pointer")
        return UI_CURSOR_HAND;
    if (name == "ew-resize")
        return UI_CURSOR_SIZEWE;
    if (name == "nwse-resize")
        return UI_CURSOR_SIZENWSE; // a window's resize grip
    if (name == "move")
        return UI_CURSOR_SIZEALL;  // a window's title bar
    return UI_CURSOR_ARROW;
}

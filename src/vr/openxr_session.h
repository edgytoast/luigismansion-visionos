#pragma once
#if defined(_WIN32) || defined(LM_VR_VISIONOS)
#ifdef _WIN32
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#define XR_NO_PROTOTYPES
#include <windows.h>
#include <d3d12.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#else
// Apple Vision Pro: OpenXR from the vendored provider (CompositorServices + ARKit), linked in
// place of a loader; its swapchain images are IOSurfaces with MTLSharedEvent fences both ways.
#include <openxr/openxr.h>
#endif
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lmvr {
#ifdef _WIN32
using EyeImage = ID3D12Resource;
using SwapFormat = DXGI_FORMAT;
#else
// One acquired swapchain image: the IOSurface (and the provider's MTLTexture over it), the
// compositor's read fence to wait on before writing, and the write fence the copy reports back.
struct EyeImage {
    void* ioSurface = nullptr;
    void* texture = nullptr;
    std::uint32_t width = 0, height = 0;
    void* acquireEvent = nullptr;  // id<MTLSharedEvent>, borrowed; null: never shown
    std::uint64_t acquireValue = 0;
    void* writeEvent = nullptr;    // set by the copy: the event its writes signal, and the value
    std::uint64_t writeValue = 0;
};
using SwapFormat = std::int64_t;  // MTLPixelFormat
#endif
// Host-owned session. Every successful beginFrame must be paired with endFrame.
// Image access is explicit so the renderer can synchronize GPU work before release.
struct ControllerState {
    XrPosef grip{{0,0,0,1},{0,0,0}}, aim{{0,0,0,1},{0,0,0}};
    bool gripValid=false,aimValid=false;
    std::uint64_t gripFlags=0,aimFlags=0;
    bool gripActive=false,aimActive=false;
    float squeeze=0,trigger=0;
    std::array<float,5> fingerCurl{};std::uint8_t fingerValid=0;
    XrVector2f stick{};
    bool inputActive=false;
    bool stickClick=false;
    std::uint32_t buttons=0; // recomp::vr::GameButton logical bits
};
class OpenXrSession {
public:
    OpenXrSession();
    ~OpenXrSession();
    OpenXrSession(const OpenXrSession&) = delete;
    OpenXrSession& operator=(const OpenXrSession&) = delete;
    void prepare(const std::wstring& loaderPath);
#ifdef _WIN32
    const XrGraphicsRequirementsD3D12KHR& requirements() const;
    void attach(ID3D12Device* device, ID3D12CommandQueue* queue);
#else
    void attach(); // the provider renders through the system Metal device
    bool running() const; // the session began and was not stopped (the Digital Crown stops it)
    bool focused() const;
    bool alive() const;
    // The mansion in your room (the provider's mixed space): `environment` clears every frame to
    // the room, so menu panels float in it; `projectionAlpha` blends the eyes by their alpha.
    void setRoom(bool environment, bool projectionAlpha);
#endif
    bool poll(); // false when runtime requests exit/loss
    bool beginFrame(); // false if session not running; true even if shouldRender=false
    bool shouldRender() const;
    void pulse(unsigned hand,float amplitude=0.25f,float seconds=0.035f);
    const std::array<ControllerState,2>& controllers() const;
    const std::array<XrView, 2>& views() const;
    const std::array<XrViewConfigurationView, 2>& viewSizes() const;
    EyeImage* acquireEye(unsigned eye);
    bool reuseEye(unsigned eye); // resubmit last released image with its original eye pose
    void releaseEye(unsigned eye); // submit writes on session queue; restore RENDER_TARGET state
    void endFrame(bool submitEyes, const XrPosef* panelPose = nullptr, float panelAspect = 4.0f/3.0f, bool stereoPanel = false, const XrPosef* overlayPose = nullptr);
    SwapFormat format() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
#endif

#pragma once
#ifdef _WIN32
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#define XR_NO_PROTOTYPES
#include <windows.h>
#include <d3d12.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lmvr {
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
    const XrGraphicsRequirementsD3D12KHR& requirements() const;
    void attach(ID3D12Device* device, ID3D12CommandQueue* queue);
    bool poll(); // false when runtime requests exit/loss
    bool beginFrame(); // false if session not running; true even if shouldRender=false
    bool shouldRender() const;
    void pulse(unsigned hand,float amplitude=0.25f,float seconds=0.035f);
    const std::array<ControllerState,2>& controllers() const;
    const std::array<XrView, 2>& views() const;
    const std::array<XrViewConfigurationView, 2>& viewSizes() const;
    ID3D12Resource* acquireEye(unsigned eye);
    bool reuseEye(unsigned eye); // resubmit last released image with its original eye pose
    void releaseEye(unsigned eye); // submit writes on session queue; restore RENDER_TARGET state
    void endFrame(bool submitEyes, const XrPosef* panelPose = nullptr, float panelAspect = 4.0f/3.0f, bool stereoPanel = false, const XrPosef* overlayPose = nullptr);
    DXGI_FORMAT format() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
#endif

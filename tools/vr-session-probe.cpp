#include "vr/openxr_session.h"
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <chrono>
#include <iostream>
#include <thread>
using Microsoft::WRL::ComPtr;
void checkHr(HRESULT result) { if (FAILED(result)) throw std::runtime_error("D3D12 failure: "+std::to_string(result)); }
int wmain(int argc, wchar_t** argv) {
    if (argc!=2) { std::cerr << "Usage: lmvr-session-probe <absolute OpenXR loader path>\n"; return 2; }
    try {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        lmvr::OpenXrSession xr; // destroyed before the graphics device and queue
        xr.prepare(argv[1]);
        const auto& required=xr.requirements();
        checkHr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        checkHr(factory->EnumAdapterByLuid(required.adapterLuid,IID_PPV_ARGS(&adapter)));
        checkHr(D3D12CreateDevice(adapter.Get(),required.minFeatureLevel,IID_PPV_ARGS(&device)));
        D3D12_COMMAND_QUEUE_DESC desc{}; desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        checkHr(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue)));
        xr.attach(device.Get(),queue.Get());
        std::cout << "OpenXR D3D12 session and two eye swapchains created; format " << xr.format() << '\n';
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        unsigned frames=0, tracked=0, eyeImages=0;
        while (frames<30 && std::chrono::steady_clock::now()<deadline && xr.poll()) {
            if (!xr.beginFrame()) {std::this_thread::sleep_for(std::chrono::milliseconds(10));continue;}
            if (xr.shouldRender()) {
                ++tracked;
                for (unsigned eye=0;eye<2;++eye) {
                    auto* image=xr.acquireEye(eye);
                    if (!image) throw std::runtime_error("Null eye resource");
                    const auto size=image->GetDesc();
                    if (size.Width!=xr.viewSizes()[eye].recommendedImageRectWidth || size.Height!=xr.viewSizes()[eye].recommendedImageRectHeight)
                        throw std::runtime_error("Eye resource size mismatch");
                    xr.releaseEye(eye); ++eyeImages;
                }
            }
            xr.endFrame(false); // no content submitted: this is a lifecycle/tracking probe
            ++frames;
        }
        std::cout << "Frames: " << frames << "; valid tracked frames: " << tracked << "; acquired/released eye images: " << eyeImages << '\n';
        return frames && tracked && eyeImages ? 0 : 1;
    } catch (const std::exception& error) {std::cerr << error.what() << '\n';return 1;}
}

#include "vr/openxr_session.h"
#include "vr/dawn_eye_output.h"
#include <dawn/native/D3D12Backend.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

static std::atomic<unsigned> errors=0;

void verifyPixel(ID3D12Resource* image, ID3D12Device* device, ID3D12CommandQueue* queue) {
    auto hr=[](HRESULT result) {if(FAILED(result)) throw std::runtime_error("Eye readback failed");};
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width=256; buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1;
    buffer.SampleDesc.Count=1; buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
    hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
    hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)));
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource=image;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    commands->ResourceBarrier(1,&barrier);
    const auto desc=image->GetDesc();
    D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=image;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint.Footprint={desc.Format,1,1,1,256};
    const UINT x=UINT(desc.Width/2),y=desc.Height/2;
    D3D12_BOX pixel{x,y,0,x+1,y+1,1};
    commands->CopyTextureRegion(&to,0,0,0,&from,&pixel);
    std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);commands->ResourceBarrier(1,&barrier);
    hr(commands->Close());ID3D12CommandList* list=commands.Get();queue->ExecuteCommandLists(1,&list);
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    const auto event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(!event) throw std::runtime_error("Cannot create readback event");
    hr(queue->Signal(fence.Get(),1));hr(fence->SetEventOnCompletion(1,event));
    const auto waited=WaitForSingleObject(event,10000);CloseHandle(event);
    if(waited!=WAIT_OBJECT_0) throw std::runtime_error("Eye readback GPU timeout");
    unsigned char* data=nullptr;D3D12_RANGE range{0,4};hr(readback->Map(0,&range,reinterpret_cast<void**>(&data)));
    const bool bgra=desc.Format==DXGI_FORMAT_B8G8R8A8_TYPELESS || desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const int r=data[bgra?2:0],g=data[1],b=data[bgra?0:2],a=data[3];D3D12_RANGE written{0,0};readback->Unmap(0,&written);
    if(abs(r-51)>2 || abs(g-102)>2 || abs(b-153)>2 || a!=255) throw std::runtime_error("Eye color readback mismatch: "+std::to_string(r)+","+std::to_string(g)+","+std::to_string(b));
}

int wmain(int argc, wchar_t** argv) {
    if(argc!=2) {std::cerr<<"Usage: lmvr-dawn-probe <absolute OpenXR loader path>\n";return 2;}
    try {
        wgpu::Device device;
        const char* toggle="allow_unsafe_apis";
        wgpu::DawnTogglesDescriptor toggles{}; toggles.enabledToggleCount=1; toggles.enabledToggles=&toggle;
        dawn::native::DawnInstanceDescriptor nativeInstance{}; nativeInstance.nextInChain=&toggles;
        wgpu::InstanceDescriptor instanceInfo{}; instanceInfo.nextInChain=&nativeInstance;
        auto instance=wgpu::CreateInstance(&instanceInfo);
        lmvr::OpenXrSession xr;
        xr.prepare(argv[1]);
        dawn::native::d3d::RequestAdapterOptionsLUID luid;
        luid.adapterLUID=xr.requirements().adapterLuid;
        wgpu::RequestAdapterOptions options{}; options.nextInChain=&luid; options.backendType=wgpu::BackendType::D3D12;
        wgpu::Adapter adapter;
        bool complete=false;
        instance.RequestAdapter(&options,wgpu::CallbackMode::AllowProcessEvents,
            [&](wgpu::RequestAdapterStatus status,wgpu::Adapter found,wgpu::StringView message) {
                if(status==wgpu::RequestAdapterStatus::Success) adapter=std::move(found);
                else std::cerr<<std::string_view(message)<<'\n';
                complete=true;
            });
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(!complete && std::chrono::steady_clock::now()<deadline) {instance.ProcessEvents();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        if(!adapter) throw std::runtime_error("Cannot obtain runtime-required Dawn adapter");
        const wgpu::FeatureName required=wgpu::FeatureName::SharedTextureMemoryD3D12Resource;
        if(!adapter.HasFeature(required)) throw std::runtime_error("Adapter lacks D3D12 texture import");
        wgpu::DeviceDescriptor create{}; create.requiredFeatureCount=1; create.requiredFeatures=&required;
        create.SetUncapturedErrorCallback([](const wgpu::Device&,wgpu::ErrorType,wgpu::StringView message){++errors;std::cerr<<std::string_view(message)<<'\n';});
        complete=false;
        adapter.RequestDevice(&create,wgpu::CallbackMode::AllowProcessEvents,
            [&](wgpu::RequestDeviceStatus status,wgpu::Device found,wgpu::StringView message) {
                if(status==wgpu::RequestDeviceStatus::Success) device=std::move(found);
                else std::cerr<<std::string_view(message)<<'\n';
                complete=true;
            });
        deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(!complete && std::chrono::steady_clock::now()<deadline) {instance.ProcessEvents();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        if(!device) throw std::runtime_error("Cannot create Dawn D3D12 device");
        xr.attach(dawn::native::d3d12::GetD3D12Device(device.Get()).Get(),dawn::native::d3d12::GetD3D12CommandQueue(device.Get()).Get());
        lmvr::DawnEyeOutput output[2];
        for(unsigned eye=0;eye<2;++eye) output[eye].initialize(device,xr.format(),xr.viewSizes()[eye].recommendedImageRectWidth,xr.viewSizes()[eye].recommendedImageRectHeight);
        wgpu::TextureDescriptor sourceDesc{}; sourceDesc.size={16,16,1}; sourceDesc.format=wgpu::TextureFormat::RGBA8Unorm;
        sourceDesc.usage=wgpu::TextureUsage::RenderAttachment|wgpu::TextureUsage::TextureBinding;
        auto source=device.CreateTexture(&sourceDesc);
        wgpu::RenderPassColorAttachment attachment{}; attachment.view=source.CreateView(); attachment.loadOp=wgpu::LoadOp::Clear;
        attachment.storeOp=wgpu::StoreOp::Store; attachment.clearValue={0.2,0.4,0.6,1};
        wgpu::RenderPassDescriptor passDesc{}; passDesc.colorAttachmentCount=1;passDesc.colorAttachments=&attachment;
        auto encoder=device.CreateCommandEncoder(); auto pass=encoder.BeginRenderPass(&passDesc); pass.End();
        auto commands=encoder.Finish();device.GetQueue().Submit(1,&commands);
        auto sampler=device.CreateSampler();
        deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        unsigned frames=0,copies=0;
        while(copies<20 && std::chrono::steady_clock::now()<deadline && xr.poll()) {
            if(!xr.beginFrame()) {std::this_thread::sleep_for(std::chrono::milliseconds(10));continue;}
            if(xr.shouldRender()) for(unsigned eye=0;eye<2;++eye) {
                auto* image=xr.acquireEye(eye);
                output[eye].copy(source.CreateView(),sampler,image);
                if(copies<2) verifyPixel(image,dawn::native::d3d12::GetD3D12Device(device.Get()).Get(),dawn::native::d3d12::GetD3D12CommandQueue(device.Get()).Get());
                xr.releaseEye(eye); ++copies;
            }
            xr.endFrame(false); ++frames; instance.ProcessEvents();
        }
        std::cout<<"Dawn/OpenXR transfer: "<<frames<<" frames, "<<copies<<" eye copies, "<<errors<<" validation errors. No layers submitted.\n";
        return copies>=20 && !errors ? 0 : 1;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}

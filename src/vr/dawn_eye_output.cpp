#include "vr/dawn_eye_output.h"
#if defined(_WIN32) || defined(LM_VR_VISIONOS)
#ifdef _WIN32
#include <dawn/native/D3D12Backend.h>
#include <wrl/client.h>
#endif
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lmvr {
namespace {
#ifdef _WIN32
void hr(HRESULT result) {if (FAILED(result)) throw std::runtime_error("VR image transfer D3D12 failure: "+std::to_string(result));}
wgpu::TextureFormat webgpuFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return wgpu::TextureFormat::RGBA8UnormSrgb;
    case DXGI_FORMAT_R8G8B8A8_UNORM: return wgpu::TextureFormat::RGBA8Unorm;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return wgpu::TextureFormat::BGRA8UnormSrgb;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return wgpu::TextureFormat::BGRA8Unorm;
    default: throw std::runtime_error("Unsupported VR output format");
    }
}
unsigned formatFamily(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return 1;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return 2;
    default: return 0;
    }
}
#endif
constexpr const char* shader=R"(
@group(0) @binding(0) var image: texture_2d<f32>;
@group(0) @binding(1) var filtering: sampler;
@group(0) @binding(2) var<uniform> fade: vec4f;
struct Vertex { @builtin(position) position: vec4f, @location(0) uv: vec2f };
@vertex fn vs(@builtin(vertex_index) index: u32) -> Vertex {
    let positions=array<vec2f,3>(vec2f(-1,-1),vec2f(3,-1),vec2f(-1,3));
    let p=positions[index];
    return Vertex(vec4f(p,0,1),vec2f(p.x*0.5+0.5,0.5-p.y*0.5));
}
override srgb_output: bool = true;
@fragment fn fs(v: Vertex) -> @location(0) vec4f {
    let color=textureSample(image,filtering,v.uv);
    // Aurora's ordinary non-sRGB framebuffer contains display-encoded color.
    // Decode before writing an sRGB attachment to avoid applying gamma twice.
    let linear=select(color.rgb/12.92,pow((color.rgb+vec3f(0.055))/1.055,vec3f(2.4)),color.rgb>vec3f(0.04045));
    return vec4f(select(color.rgb,linear,srgb_output)*fade.x,select(1.0,color.a,fade.y>0.5));
}
)";
}
#ifdef _WIN32
struct DawnEyeOutput::Impl {
    wgpu::Device device;
    wgpu::SharedTextureMemory memory;
    wgpu::Texture texture;
    wgpu::RenderPipeline pipeline;
    wgpu::Buffer fade;
    Microsoft::WRL::ComPtr<ID3D12Device> native;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12Resource> intermediate;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    HANDLE event=nullptr;
    uint64_t serial=0;
    unsigned width=0,height=0;
    ~Impl() {if(event) CloseHandle(event);}
    void wait() {
        hr(queue->Signal(fence.Get(),++serial));
        if (fence->GetCompletedValue()<serial) {
            hr(fence->SetEventOnCompletion(serial,event));
            if (WaitForSingleObject(event,10000)!=WAIT_OBJECT_0) throw std::runtime_error("VR image transfer GPU timeout");
        }
    }
};
DawnEyeOutput::DawnEyeOutput():impl(std::make_unique<Impl>()) {}
DawnEyeOutput::~DawnEyeOutput()=default;
void DawnEyeOutput::initialize(const wgpu::Device& device, DXGI_FORMAT format, unsigned width, unsigned height) {
    auto& s=*impl;
    if (s.device || !width || !height) throw std::logic_error("Invalid VR image transfer initialization");
    if (!device.HasFeature(wgpu::FeatureName::SharedTextureMemoryD3D12Resource))
        throw std::runtime_error("Dawn device lacks SharedTextureMemoryD3D12Resource");
    s.device=device; s.width=width; s.height=height;
    s.native=dawn::native::d3d12::GetD3D12Device(device.Get());
    s.queue=dawn::native::d3d12::GetD3D12CommandQueue(device.Get());
    if (!s.native || !s.queue) throw std::runtime_error("VR image transfer requires a D3D12 Dawn device");
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC resource{}; resource.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource.Width=width; resource.Height=height; resource.DepthOrArraySize=resource.MipLevels=1;
    resource.Format=format; resource.SampleDesc.Count=1;
    resource.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    hr(s.native->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&resource,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&s.intermediate)));
    dawn::native::d3d12::SharedTextureMemoryD3D12ResourceDescriptor shared;
    shared.resource=s.intermediate;
    wgpu::SharedTextureMemoryDescriptor import{}; import.nextInChain=&shared;
    s.memory=device.ImportSharedTextureMemory(&import);
    wgpu::SharedTextureMemoryProperties properties{};
    if (!s.memory.GetProperties(&properties)) throw std::runtime_error("Cannot import VR intermediate texture");
    wgpu::TextureDescriptor texture{}; texture.size={width,height,1}; texture.format=webgpuFormat(format);
    texture.usage=wgpu::TextureUsage::RenderAttachment|wgpu::TextureUsage::CopySrc;
    s.texture=s.memory.CreateTexture(&texture);
    wgpu::ShaderSourceWGSL wgsl{}; wgsl.code=shader;
    wgpu::ShaderModuleDescriptor moduleInfo{}; moduleInfo.nextInChain=&wgsl;
    const auto module=device.CreateShaderModule(&moduleInfo);
    wgpu::ColorTargetState color{}; color.format=texture.format;
    wgpu::ConstantEntry constant{}; constant.key="srgb_output";
    constant.value=(format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)?1:0;
    wgpu::FragmentState fragment{}; fragment.module=module; fragment.entryPoint="fs";
    fragment.targetCount=1; fragment.targets=&color; fragment.constantCount=1; fragment.constants=&constant;
    wgpu::RenderPipelineDescriptor pipeline{}; pipeline.vertex.module=module; pipeline.vertex.entryPoint="vs"; pipeline.fragment=&fragment;
    s.pipeline=device.CreateRenderPipeline(&pipeline);
    wgpu::BufferDescriptor uniform{}; uniform.size=16;
    uniform.usage=wgpu::BufferUsage::Uniform|wgpu::BufferUsage::CopyDst;
    s.fade=device.CreateBuffer(&uniform);
    hr(s.native->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&s.allocator)));
    hr(s.native->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,s.allocator.Get(),nullptr,IID_PPV_ARGS(&s.commands)));
    hr(s.commands->Close());
    hr(s.native->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&s.fence)));
    s.event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(!s.event) throw std::runtime_error("Cannot create VR GPU fence event");
}
void DawnEyeOutput::copy(const wgpu::TextureView& source, const wgpu::Sampler& sampler, ID3D12Resource* eye, float gain, bool preserveAlpha) {
    auto& s=*impl;
    if (!s.device || !eye) throw std::logic_error("VR image transfer not initialized");
    const auto desc=eye->GetDesc();
    if (desc.Width!=s.width || desc.Height!=s.height || !formatFamily(desc.Format) ||
        formatFamily(desc.Format)!=formatFamily(s.intermediate->GetDesc().Format))
        throw std::runtime_error("VR eye texture dimensions or format changed");
    wgpu::SharedTextureMemoryBeginAccessDescriptor begin{}; begin.initialized=false;
    if (!s.memory.BeginAccess(s.texture,&begin)) throw std::runtime_error("Cannot begin VR shared texture access");
    const float fade[4]{gain,preserveAlpha?1.f:0.f,0,0};s.device.GetQueue().WriteBuffer(s.fade,0,fade,sizeof(fade));
    wgpu::BindGroupEntry entries[3]{}; entries[0].binding=0; entries[0].textureView=source; entries[1].binding=1; entries[1].sampler=sampler;
    entries[2].binding=2;entries[2].buffer=s.fade;entries[2].size=16;
    wgpu::BindGroupDescriptor group{}; group.layout=s.pipeline.GetBindGroupLayout(0); group.entryCount=3; group.entries=entries;
    auto encoder=s.device.CreateCommandEncoder();
    wgpu::RenderPassColorAttachment attachment{}; attachment.view=s.texture.CreateView();
    attachment.loadOp=wgpu::LoadOp::Clear; attachment.storeOp=wgpu::StoreOp::Store;
    wgpu::RenderPassDescriptor passInfo{}; passInfo.colorAttachmentCount=1; passInfo.colorAttachments=&attachment;
    auto pass=encoder.BeginRenderPass(&passInfo); pass.SetPipeline(s.pipeline);
    pass.SetBindGroup(0,s.device.CreateBindGroup(&group)); pass.Draw(3); pass.End();
    auto buffer=encoder.Finish(); s.device.GetQueue().Submit(1,&buffer);
    wgpu::SharedTextureMemoryEndAccessState end{};
    if (!s.memory.EndAccess(s.texture,&end)) throw std::runtime_error("Cannot end VR shared texture access");
    // EndAccess flushes Dawn's queue; native operations below use that same queue.
    // The simultaneous-access intermediate decays to COMMON at each submission.
    hr(s.allocator->Reset()); hr(s.commands->Reset(s.allocator.Get(),nullptr));
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource=eye; barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET; barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
    s.commands->ResourceBarrier(1,&barrier);
    s.commands->CopyResource(eye,s.intermediate.Get());
    std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter); s.commands->ResourceBarrier(1,&barrier);
    hr(s.commands->Close()); ID3D12CommandList* list=s.commands.Get(); s.queue->ExecuteCommandLists(1,&list);
    // Conservative first implementation: wait before reusing the intermediate or releasing the eye.
    s.wait();
}
#else
// Apple Vision Pro. The provider's swapchain images are IOSurfaces ('BGRA') that the compositor
// reads as BGRA8Unorm_sRGB. Each is imported into Dawn once (SharedTextureMemoryIOSurface) and
// the eye is drawn straight into it as BGRA8Unorm: the game's display-encoded colour goes in
// unchanged (srgb_output off), which the compositor's sRGB read then decodes, like the Windows
// path's non-sRGB swapchains. Dawn waits for the compositor's last read of the image (its
// MTLSharedEvent) before writing, and the write fence goes back to the provider, which the
// compositor waits on before reading: no CPU wait.
// The depth comes as the EFB depth texture (texture_depth_2d) or aurora's R32Float room snapshot
// (texture_2d<f32>, gfx::room_depth); DEPTH_TYPE and DEPTH_LOAD are filled in for each.
constexpr const char* roomShader=R"(
@group(0) @binding(0) var image: texture_2d<f32>;
@group(0) @binding(1) var filtering: sampler;
@group(0) @binding(2) var<uniform> fade: vec4f;
@group(0) @binding(3) var depth: DEPTH_TYPE;
struct Vertex { @builtin(position) position: vec4f, @location(0) uv: vec2f };
@vertex fn vs(@builtin(vertex_index) index: u32) -> Vertex {
    let positions=array<vec2f,3>(vec2f(-1,-1),vec2f(3,-1),vec2f(-1,3));
    let p=positions[index];
    return Vertex(vec4f(p,0,1),vec2f(p.x*0.5+0.5,0.5-p.y*0.5));
}
// The room shows where the frame drew nothing: no depth (reversed Z: 0 is nothing; aurora's
// snapshot from before the game's scene copy cleared it, gfx::room_depth) and black,
// all around (a 3x3 neighbourhood, so thin dark railings and edges drawn without depth keep
// their pixels). The HUD and text, drawn without depth, keep theirs by their colour.
// Premultiplied; the gain (a camera cut's fade) crossfades into the room.
@fragment fn fs(v: Vertex) -> @location(0) vec4f {
    let color=textureSample(image,filtering,v.uv);
    let size=vec2i(textureDimensions(depth));
    let at=clamp(vec2i(v.uv*vec2f(size)),vec2i(0),size-vec2i(1));
    var deepest=0.0;
    for(var dy=-1;dy<=1;dy++) {
        for(var dx=-1;dx<=1;dx++) {
            deepest=max(deepest,textureLoad(depth,clamp(at+vec2i(dx,dy),vec2i(0),size-vec2i(1)),0)DEPTH_LOAD);
        }
    }
    let drawn=deepest>0.0 || (fade.z<0.5 && max(color.r,max(color.g,color.b))>0.006);
    let a=select(0.0,1.0,drawn)*fade.x;
    // Premultiplied for a compositor that blends in linear light (it reads the image as sRGB):
    // the display-encoded colour scales by a^(1/2.2), so a fade halfway is half the scene's light.
    return vec4f(color.rgb*pow(a,1.0/2.2),a);
}
)";
struct DawnEyeOutput::Impl {
    wgpu::Device device;
    wgpu::RenderPipeline pipeline;
    wgpu::RenderPipeline roomPipeline,roomFloatPipeline;
    wgpu::TextureView roomDepth;
    bool roomDepthFloat=false;
    wgpu::Buffer fade;
    unsigned width=0,height=0;
    struct Surface {void* ioSurface=nullptr; wgpu::SharedTextureMemory memory; wgpu::Texture texture;};
    std::vector<Surface> surfaces;
    std::vector<std::pair<void*,wgpu::SharedFence>> fences;
    Surface& surface(void* ioSurface) {
        for (auto& known : surfaces) if (known.ioSurface==ioSurface) return known;
        Surface made; made.ioSurface=ioSurface;
        wgpu::SharedTextureMemoryIOSurfaceDescriptor io{}; io.ioSurface=ioSurface;
        wgpu::SharedTextureMemoryDescriptor import{}; import.nextInChain=&io; import.label="LMVR eye image";
        made.memory=device.ImportSharedTextureMemory(&import);
        if (!made.memory) throw std::runtime_error("Cannot import a VR eye image (IOSurface)");
        wgpu::SharedTextureMemoryProperties properties{};
        made.memory.GetProperties(&properties);
        wgpu::TextureDescriptor texture{}; texture.label="LMVR eye image"; texture.size={width,height,1};
        texture.format=properties.format; texture.usage=wgpu::TextureUsage::RenderAttachment;
        made.texture=made.memory.CreateTexture(&texture);
        if (!made.texture) throw std::runtime_error("Cannot wrap a VR eye image");
        surfaces.push_back(std::move(made));
        return surfaces.back();
    }
    wgpu::SharedFence fence(void* event) {
        if (!event) return nullptr;
        for (const auto& [known,imported] : fences) if (known==event) return imported;
        wgpu::SharedFenceMTLSharedEventDescriptor metal{}; metal.sharedEvent=event;
        wgpu::SharedFenceDescriptor desc{}; desc.nextInChain=&metal; desc.label="LMVR compositor read";
        auto imported=device.ImportSharedFence(&desc);
        fences.emplace_back(event,imported);
        return imported;
    }
};
DawnEyeOutput::DawnEyeOutput():impl(std::make_unique<Impl>()) {}
DawnEyeOutput::~DawnEyeOutput()=default;
void DawnEyeOutput::initialize(const wgpu::Device& device, SwapFormat, unsigned width, unsigned height) {
    auto& s=*impl;
    if (s.device || !width || !height) throw std::logic_error("Invalid VR image transfer initialization");
    if (!device.HasFeature(wgpu::FeatureName::SharedTextureMemoryIOSurface) || !device.HasFeature(wgpu::FeatureName::SharedFenceMTLSharedEvent))
        throw std::runtime_error("Dawn device lacks SharedTextureMemoryIOSurface / SharedFenceMTLSharedEvent");
    s.device=device; s.width=width; s.height=height;
    wgpu::ShaderSourceWGSL wgsl{}; wgsl.code=shader;
    wgpu::ShaderModuleDescriptor moduleInfo{}; moduleInfo.nextInChain=&wgsl;
    const auto module=device.CreateShaderModule(&moduleInfo);
    wgpu::ColorTargetState color{}; color.format=wgpu::TextureFormat::BGRA8Unorm; // the IOSurfaces' own
    wgpu::ConstantEntry constant{}; constant.key="srgb_output"; constant.value=0;
    wgpu::FragmentState fragment{}; fragment.module=module; fragment.entryPoint="fs";
    fragment.targetCount=1; fragment.targets=&color; fragment.constantCount=1; fragment.constants=&constant;
    wgpu::RenderPipelineDescriptor pipeline{}; pipeline.label="LMVR eye copy"; pipeline.vertex.module=module; pipeline.vertex.entryPoint="vs"; pipeline.fragment=&fragment;
    s.pipeline=device.CreateRenderPipeline(&pipeline);
    for (const bool floatDepth : {false,true}) {
        std::string code=roomShader;
        const auto fill=[&](const std::string& key,const std::string& value){code.replace(code.find(key),key.size(),value);};
        fill("DEPTH_TYPE",floatDepth?"texture_2d<f32>":"texture_depth_2d");
        fill("DEPTH_LOAD",floatDepth?".r":"");
        wgpu::ShaderSourceWGSL roomWgsl{}; roomWgsl.code=code.c_str();
        wgpu::ShaderModuleDescriptor roomInfo{}; roomInfo.nextInChain=&roomWgsl;
        const auto roomModule=device.CreateShaderModule(&roomInfo);
        wgpu::FragmentState roomFragment{}; roomFragment.module=roomModule; roomFragment.entryPoint="fs";
        roomFragment.targetCount=1; roomFragment.targets=&color;
        wgpu::RenderPipelineDescriptor room{}; room.label=floatDepth?"LMVR eye copy (room, snapshot)":"LMVR eye copy (room)"; room.vertex.module=roomModule; room.vertex.entryPoint="vs"; room.fragment=&roomFragment;
        (floatDepth?s.roomFloatPipeline:s.roomPipeline)=device.CreateRenderPipeline(&room);
    }
    wgpu::BufferDescriptor uniform{}; uniform.size=16;
    uniform.usage=wgpu::BufferUsage::Uniform|wgpu::BufferUsage::CopyDst;
    s.fade=device.CreateBuffer(&uniform);
}
void DawnEyeOutput::setRoomMask(const wgpu::TextureView& depth, bool floatDepth) {impl->roomDepth=depth;impl->roomDepthFloat=floatDepth;}
void DawnEyeOutput::copy(const wgpu::TextureView& source, const wgpu::Sampler& sampler, EyeImage* eye, float gain, bool preserveAlpha) {
    auto& s=*impl;
    if (!s.device || !eye || !eye->ioSurface) throw std::logic_error("VR image transfer not initialized");
    if (eye->width!=s.width || eye->height!=s.height) throw std::runtime_error("VR eye texture dimensions changed");
    auto& target=s.surface(eye->ioSurface);
    wgpu::SharedTextureMemoryBeginAccessDescriptor begin{}; begin.initialized=false; // every pixel is written
    const auto readFence=s.fence(eye->acquireEvent);
    if (readFence) {begin.fenceCount=1;begin.fences=&readFence;begin.signaledValueCount=1;begin.signaledValues=&eye->acquireValue;}
    if (target.memory.BeginAccess(target.texture,&begin)!=wgpu::Status::Success) throw std::runtime_error("Cannot begin VR shared texture access");
    // LMVR_ROOM_MASK=depth (test aid): the room mask from depth alone, without the colour test.
    static const float depthOnly=[]{const char* v=std::getenv("LMVR_ROOM_MASK");return v&&std::string(v)=="depth"?1.f:0.f;}();
    const float fade[4]{gain,preserveAlpha?1.f:0.f,depthOnly,0};s.device.GetQueue().WriteBuffer(s.fade,0,fade,sizeof(fade));
    const bool room=s.roomDepth && s.roomPipeline && s.roomFloatPipeline && !preserveAlpha;
    const auto& pipeline=room?(s.roomDepthFloat?s.roomFloatPipeline:s.roomPipeline):s.pipeline;
    wgpu::BindGroupEntry entries[4]{}; entries[0].binding=0; entries[0].textureView=source; entries[1].binding=1; entries[1].sampler=sampler;
    entries[2].binding=2;entries[2].buffer=s.fade;entries[2].size=16;
    entries[3].binding=3;entries[3].textureView=s.roomDepth;
    wgpu::BindGroupDescriptor group{}; group.layout=pipeline.GetBindGroupLayout(0); group.entryCount=room?4:3; group.entries=entries;
    auto encoder=s.device.CreateCommandEncoder();
    wgpu::RenderPassColorAttachment attachment{}; attachment.view=target.texture.CreateView();
    attachment.loadOp=wgpu::LoadOp::Clear; attachment.storeOp=wgpu::StoreOp::Store; attachment.clearValue={0,0,0,(preserveAlpha||room)?0.0:1.0};
    wgpu::RenderPassDescriptor passInfo{}; passInfo.colorAttachmentCount=1; passInfo.colorAttachments=&attachment;
    auto pass=encoder.BeginRenderPass(&passInfo); pass.SetPipeline(pipeline);
    pass.SetBindGroup(0,s.device.CreateBindGroup(&group)); pass.Draw(3); pass.End();
    auto buffer=encoder.Finish(); s.device.GetQueue().Submit(1,&buffer);
    wgpu::SharedTextureMemoryEndAccessState end{};
    if (target.memory.EndAccess(target.texture,&end)!=wgpu::Status::Success) throw std::runtime_error("Cannot end VR shared texture access");
    eye->writeEvent=nullptr;eye->writeValue=0;
    for (size_t i=0;i<end.fenceCount && i<end.signaledValueCount;++i) {
        wgpu::SharedFenceMTLSharedEventExportInfo metal{};
        wgpu::SharedFenceExportInfo info{}; info.nextInChain=&metal;
        end.fences[i].ExportInfo(&info);
        if (info.type==wgpu::SharedFenceType::MTLSharedEvent && metal.sharedEvent) {
            eye->writeEvent=metal.sharedEvent;
            if (end.signaledValues[i]>eye->writeValue) eye->writeValue=end.signaledValues[i];
        }
    }
}
#endif
}
#endif

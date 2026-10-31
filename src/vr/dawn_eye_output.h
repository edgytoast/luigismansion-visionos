#pragma once
#ifdef _WIN32
#include <webgpu/webgpu_cpp.h>
#include <d3d12.h>
#include <memory>

namespace lmvr {
// Copies a completed game eye render through a Dawn-importable intermediate into
// an acquired OpenXR resource. Uses the SAME native command queue as Dawn.
class DawnEyeOutput {
public:
    DawnEyeOutput();
    ~DawnEyeOutput();
    DawnEyeOutput(const DawnEyeOutput&) = delete;
    DawnEyeOutput& operator=(const DawnEyeOutput&) = delete;
    void initialize(const wgpu::Device& device, DXGI_FORMAT format, unsigned width, unsigned height);
    void copy(const wgpu::TextureView& source, const wgpu::Sampler& sampler, ID3D12Resource* eye, float gain = 1.0f, bool preserveAlpha = false);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
#endif

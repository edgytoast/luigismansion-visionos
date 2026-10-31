#pragma once
#if defined(_WIN32) || defined(LM_VR_VISIONOS)
#include <webgpu/webgpu_cpp.h>
#ifdef _WIN32
#include <d3d12.h>
#endif
#include "vr/openxr_session.h"
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
    void initialize(const wgpu::Device& device, SwapFormat format, unsigned width, unsigned height);
    // Apple Vision Pro: draws straight into the acquired image's IOSurface (imported into Dawn
    // once per surface) after the compositor's read fence, and leaves the write fence in `eye`.
    void copy(const wgpu::TextureView& source, const wgpu::Sampler& sampler, EyeImage* eye, float gain = 1.0f, bool preserveAlpha = false);
#ifndef _WIN32
    // Apple Vision Pro, the mansion in your room: with a depth view (the frame's EFB depth, single
    // sample, or aurora's R32Float snapshot of it when `floatDepth`; the source's size), the next
    // copies are transparent where the frame drew nothing (no depth, reversed Z, and black),
    // premultiplied, and the gain fades into the room rather than to black. A null view: opaque.
    void setRoomMask(const wgpu::TextureView& depth, bool floatDepth);
#endif
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
#endif

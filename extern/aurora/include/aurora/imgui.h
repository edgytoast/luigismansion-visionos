#ifndef AURORA_IMGUI_H
#define AURORA_IMGUI_H

#include <aurora/aurora.h>
#include <imgui.h>

#ifdef __cplusplus
#include <cstdint>

extern "C" {
#else
#include "stdint.h"
#endif

ImTextureID aurora_imgui_add_texture(uint32_t width, uint32_t height, const void* rgba8);

/* Called on the main thread at the start of every ImGui frame, before ImGui::NewFrame, with the
 * window size (also the first frame). The callback may change the font atlas (clear it and add
 * fonts at another size) and the style: an atlas left unbuilt is rebuilt and uploaded right
 * after it returns, once the previous frame's ImGui draw has finished on the render worker.
 * Fonts cannot be changed between NewFrame and Render, so this is the place for a UI scale. */
typedef void (*AuroraImGuiFrameCallback)(const AuroraWindowSize* size);
void aurora_imgui_set_frame_callback(AuroraImGuiFrameCallback callback);

#ifdef __cplusplus
}
#endif

#endif

# Apple Vision Pro: the game as LuigisMansionVisionGame.framework, which the SwiftUI app
# (visionos/App) embeds. Included at the end of the base tree's CMakeLists.txt (the port's
# patch adds `include(${LM_VISIONOS_CMAKE})`) when visionos/scripts/build-visionos.sh
# configures it for visionOS, so every target and source list of the base is visible here.
#
# - The recompiled build's runtime (RECOMP_RUNTIME_SOURCES: memory/CPU helpers, scheduler, HLE SDK,
#   frame driver, audio, the F1 menu, the VR host) with the game module recompiled AT BUILD TIME
#   from the player's own main.dol (LMVR_RECOMP_DIR, made by the host's lmrecompile) and linked
#   in: visionOS can't compile or load code at run time, so the shipped app's first-run module
#   build is out (LM_STATIC_MODULE, upstream's developer build). The generated code is
#   disc-derived and lives only in .scratch.
# - Without a recompiled module (no disc yet), visionos/game/no_game_module.cpp stands in: the
#   app builds, launches and reports that the game wasn't built in.
# - The VR backend (src/vr) with its Metal binding over the vendored OpenXR provider
#   (visionos/openxr-provider, GPL-3.0-or-later), the visionOS host (visionos/host) and no
#   updater (visionos/game/update_off.cpp replaces src/port/update.cpp).
# - Not aurora::main: SwiftUI owns main(); the host runs the game's main (lmvr_game_main) on a
#   thread of its own.
# - Exports ONLY the C bridge (visionos/exports.txt).
include_guard(GLOBAL)
# The host and the provider are Objective-C++ (the base project enables C and C++ only).
enable_language(OBJC OBJCXX)

get_filename_component(LMVR_VISIONOS_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(LMVR_HOST_DIR "${LMVR_VISIONOS_DIR}/host")
set(LMVR_GAME_DIR "${LMVR_VISIONOS_DIR}/game")

# The OpenXR provider: OpenXR's entry points over CompositorServices and ARKit, linked in place of
# a loader. The base tree's Khronos headers (extern/openxr-sdk, Apache-2.0) are the ones used.
set(LMVR_XR_PROVIDER_DIR "${LMVR_VISIONOS_DIR}/openxr-provider")
add_library(lmvr_openxr_provider STATIC
        "${LMVR_XR_PROVIDER_DIR}/src/xr_visionos_runtime.mm"
        "${LMVR_XR_PROVIDER_DIR}/src/xr_visionos_input.mm"
        "${LMVR_XR_PROVIDER_DIR}/src/xr_visionos_hand_tracking.mm"
        "${LMVR_XR_PROVIDER_DIR}/src/xr_visionos_compositor.mm"
        "${LMVR_XR_PROVIDER_DIR}/src/xr_visionos_controllers.mm"
        "${LMVR_XR_PROVIDER_DIR}/src/xr_visionos_smaa.mm")
target_include_directories(lmvr_openxr_provider PUBLIC "${LMVR_XR_PROVIDER_DIR}/include")
target_include_directories(lmvr_openxr_provider SYSTEM PUBLIC "${CMAKE_SOURCE_DIR}/extern/openxr-sdk/include")
target_compile_features(lmvr_openxr_provider PUBLIC cxx_std_20)
target_compile_options(lmvr_openxr_provider PRIVATE -fobjc-arc -w)

# The game module: recompiled from the player's main.dol (build-visionos.sh runs lmrecompile on the
# Mac first), or the stand-in.
set(LMVR_RECOMP_DIR "" CACHE PATH "lmrecompile's output for the player's main.dol (func_table.cpp, funcs_NNN.cpp)")
if (LMVR_RECOMP_DIR AND EXISTS "${LMVR_RECOMP_DIR}/func_table.cpp")
    file(GLOB _lmvr_module_sources "${LMVR_RECOMP_DIR}/funcs_*.cpp")
    list(APPEND _lmvr_module_sources "${LMVR_RECOMP_DIR}/func_table.cpp")
    # Generated code: one huge function per guest function, lots of gotos (as upstream's lmrecomp).
    set_source_files_properties(${_lmvr_module_sources} PROPERTIES
            COMPILE_OPTIONS "-Wno-unused-label;-Wno-unused-variable;-Wno-unused-but-set-variable")
    set(LMVR_HAS_MODULE ON)
    message(STATUS "lmvr: game module from ${LMVR_RECOMP_DIR}")
else ()
    set(_lmvr_module_sources "${LMVR_GAME_DIR}/no_game_module.cpp")
    set(LMVR_HAS_MODULE OFF)
    message(STATUS "lmvr: no recompiled game module (no disc yet): the app builds with a stand-in")
endif ()
# The module's own runtime glue is in the runtime for a static build (module_rt.cpp is the DLL's).
add_library(lmvr_game_module OBJECT ${_lmvr_module_sources})
target_include_directories(lmvr_game_module PRIVATE "${CMAKE_SOURCE_DIR}/src" "${LMVR_RECOMP_DIR}")
target_compile_definitions(lmvr_game_module PRIVATE TARGET_PC=1 LM_STATIC_MODULE=1)
target_compile_features(lmvr_game_module PRIVATE cxx_std_20)

set(_lmvr_runtime ${RECOMP_RUNTIME_SOURCES})
# (the base lists it relative to its root)
list(REMOVE_ITEM _lmvr_runtime "${CMAKE_SOURCE_DIR}/src/port/update.cpp" "src/port/update.cpp")

add_library(LuigisMansionVisionGame SHARED
        ${_lmvr_runtime}
        "${CMAKE_SOURCE_DIR}/src/vr/openxr_session.cpp"
        "${CMAKE_SOURCE_DIR}/src/vr/dawn_eye_output.cpp"
        "${LMVR_GAME_DIR}/update_off.cpp"
        "${LMVR_GAME_DIR}/no_game.cpp"
        "${LMVR_HOST_DIR}/game_host.mm"
        "${LMVR_HOST_DIR}/sense_pad.mm"
        "${LMVR_HOST_DIR}/lmvr_window.cpp"
        $<TARGET_OBJECTS:lmvr_game_module>)
target_include_directories(LuigisMansionVisionGame PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/extern/aurora/include/dolphin"
        "${CMAKE_SOURCE_DIR}/extern/aurora/include"
        "${CMAKE_SOURCE_DIR}/extern/aurora/lib"  # the VR host: webgpu/gpu.hpp, gfx/render_worker.hpp
        "${LMVR_HOST_DIR}"
        "$<TARGET_PROPERTY:dawn::webgpu_dawn,INTERFACE_INCLUDE_DIRECTORIES>")
target_compile_definitions(LuigisMansionVisionGame PRIVATE
        TARGET_PC=1 LM_STATIC_MODULE=1 LM_VR_BUILD=1 LM_VR_OPENXR=1 LM_VR_VISIONOS=1
        "LM_VERSION=\"${PROJECT_VERSION}\"")
target_compile_features(LuigisMansionVisionGame PRIVATE cxx_std_20)
set_source_files_properties("${LMVR_HOST_DIR}/game_host.mm" "${LMVR_HOST_DIR}/sense_pad.mm"
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_link_libraries(LuigisMansionVisionGame PRIVATE
        aurora::core aurora::gx aurora::vi aurora::pad aurora::si aurora::os aurora::dvd aurora::card
        lmvr_openxr_provider)
if (DEFINED AURORA_SDL3_TARGET AND TARGET ${AURORA_SDL3_TARGET})
    target_link_libraries(LuigisMansionVisionGame PRIVATE ${AURORA_SDL3_TARGET})
endif ()
foreach (_framework UIKit Foundation Metal QuartzCore IOSurface GameController CoreHaptics AVFoundation
        AudioToolbox CoreAudio CoreMotion CoreGraphics CompositorServices ARKit)
    target_link_libraries(LuigisMansionVisionGame PRIVATE "-framework ${_framework}")
endforeach ()
target_link_options(LuigisMansionVisionGame PRIVATE
        "LINKER:-exported_symbols_list,${LMVR_VISIONOS_DIR}/exports.txt")
set_target_properties(LuigisMansionVisionGame PROPERTIES
        FRAMEWORK TRUE
        MACOSX_FRAMEWORK_IDENTIFIER "dev.lmvr.vision.game"
        MACOSX_FRAMEWORK_BUNDLE_VERSION "1"
        MACOSX_FRAMEWORK_SHORT_VERSION_STRING "0.1"
        OUTPUT_NAME LuigisMansionVisionGame
        INSTALL_NAME_DIR "@rpath"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        LINKER_LANGUAGE CXX
        LINK_DEPENDS "${LMVR_VISIONOS_DIR}/exports.txt")
message(STATUS "lmvr: LuigisMansionVisionGame.framework (module: ${LMVR_HAS_MODULE})")

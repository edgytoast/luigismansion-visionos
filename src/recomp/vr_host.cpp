#include "recomp/vr_host.h"
#include "recomp/test_checkpoints.h"
#if defined(LM_VR_OPENXR) && (defined(_WIN32) || defined(LM_VR_VISIONOS))
#include "vr/openxr_session.h"
#include "vr/dawn_eye_output.h"
#include "port/install.h"
#include "recomp/hle.h"
#include "recomp/menu.h"
#include "webgpu/gpu.hpp"
#include "gfx/render_worker.hpp"
#ifdef _WIN32
#include <dawn/native/D3D12Backend.h>
#endif
#ifdef LM_VR_VISIONOS
#include <aurora/gfx.hpp>
#include "lmvr_game_hooks.h"  // the port's visionOS host
#endif
#include <imgui.h>
#include "imgui.hpp"
#include <SDL3/SDL.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <mutex>

namespace recomp::vrhost {
namespace {
bool sEnabled=false, sFrame=false, sRender=false, sRecenter=true;
bool sPanel=true,sHoldSearchTransition=false;
bool sStereoPanel=false;
bool sScriptedWorld=false,sForwardShot=false,sCreditsView=false;
float sCreditsFocus=0;
std::array<vr::Aperture,2> sSceneApertures{};
std::array<bool,2> sSceneApertureActive{};
std::atomic<bool> sHideRoomFrames{true};
std::atomic<bool> sConservativeVisibility{true};
std::atomic<bool> sTrackedArms{true};
std::atomic<bool> sShortForearms{false};
std::atomic<bool> sInvertRightY{false};
std::atomic<bool> sExpandedStoryScenes{true};
std::atomic<bool> sWorldLockedSet{true},sSetSlide{false};
std::atomic<float> sHudX{0},sHudY{-0.48f},sHudSpread{0.08f};
std::array<std::atomic<float>,2> sHandRollDegrees{60.8f,-60.8f};
std::atomic<bool> sMirrorAvatarTracking{true};
std::atomic<float> sMirrorHandHeight{-0.25f};
std::atomic<bool> sHandDirectedTools{true},sMotionCounterPull{true};
vr::Vec3 sPullLast{};vr::CapturePulls sCapturePulls;unsigned sCaptureActor=0;bool sMotionPullActive=false;unsigned sPullHand=0;
bool sHookedActive=false;
bool sDesktopMenuPointer=false;
bool sMenuChordHeld=false,sUiPointerDown=false;unsigned sUiPointerHand=1;
std::chrono::steady_clock::time_point sCaptureFeedback{};
float sCapturedYaw=0;
std::uint64_t sCaptureContext=0;
std::chrono::steady_clock::time_point sCaptureStamp{},sCaptureFrame{};
std::atomic<bool> sGrabGameBoy{false};
std::atomic<bool> sBeltEquipment{true},sInventoryToolbar{false},sPhysicalDoors{false};
std::atomic<float> sToolbarDrop{0.50f},sToolbarReach{0.23f};
vr::Vec3 sWorldContact{};bool sWorldVisible=false,sWorldHover=false;
std::array<float,2> sWorldPulse{};
std::atomic<bool> sFlashlightLeft{true},sPoltergustLeft{false},sGameBoyLeft{false};
std::array<vr::Matrix,2> sToolAim{},sAvatarToolAim{};
std::array<bool,2> sToolAimValid{},sAvatarToolAimValid{};
std::array<vr::GrabState,2> sAvatarGrabs{};
vr::Pose sHolsterPose{},sAvatarHolsterPose{};
std::array<vr::GrabTarget,4> sBeltTargets{},sAvatarHolsterTargets{};
std::array<unsigned,2> sHover{};
bool sGameBoySession=false,sGameBoyTriggerDown=false;
vr::Matrix sAvatarHeadTilt=vr::poseMatrix({});
std::atomic<float> sHandOutwardDegrees{0.0f};
std::atomic<float> sHandHeightAdjustment{0},sHandForwardAdjustment{0};
float sPanelAspect=4.0f/3.0f;
float sAnimationDepth=0.85f;
float sDialogueLift=0.18f;
float sEyeHeight=1.80f,sEyeForward=0.20f;
float sScriptedFocus=200.0f;
unsigned sEye=0;
std::unique_ptr<lmvr::OpenXrSession> sSession;
std::unique_ptr<lmvr::DawnEyeOutput> sOutput[3];
vr::CameraOptions sOptions;
vr::Pose sOrigin;
EyeCamera sCameras[2];
std::array<AvatarHand,2> sHands{};
vr::InteractionHands sInteractions;
vr::Matrix sHeadMatrix=vr::poseMatrix({});
vr::Vec3 sGazeForward{0,0,-1};
float sHeading=0,sHeadYaw=0;bool sHeadingValid=false,sTurnReady=true;
std::mutex sMovementMutex;
std::array<float,4> sMovement{1,0,0,1};
XrVector2f sMoveStick{};
float sToolLeft=0,sToolRight=0;
bool sMovementActive=false,sVrPadConnected=false,sControllerConnected=false;
vr::ButtonLatch sButtons;
XrVector2f sMenuStick{};
XrVector2f sCaptureStick{};
std::uint64_t sEquipmentContext=0,sLastEquipmentContext=0;
std::array<bool,4> sEquipmentOwned{};
std::array<vr::HandInput,2> sAvatarInput{};
float sAvatarBodyHeading=0;
float sAvatarHeading=0,sAvatarHeight=1.8f,sAvatarForward=0.20f;
std::chrono::steady_clock::time_point sTransition{};
float sGain=1;
double sSyncMs=0, sCopyMs=0;
vr::Pose pose(const XrPosef& p) {return {{p.orientation.x,p.orientation.y,p.orientation.z,p.orientation.w},{p.position.x,p.position.y,p.position.z}};}
vr::Matrix inverseUniform(const vr::Matrix& m) {
    vr::Matrix out{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
    const float length2=m[0]*m[0]+m[4]*m[4]+m[8]*m[8];
    for(int r=0;r<3;++r) {
        for(int c=0;c<3;++c) out[r*4+c]=m[c*4+r]/length2;
        out[r*4+3]=-(out[r*4]*m[3]+out[r*4+1]*m[7]+out[r*4+2]*m[11]);
    }
    return out;
}
std::string sDefaultSettings;
void writeSettings(std::ostream& file) {
    file<<int(sOptions.mode)<<' '<<sOptions.miniatureScale<<' '<<sOptions.viewingDistance<<' '<<sOptions.unitsPerMetre<<' '<<sOptions.verticalOffset<<' '<<int(sHideRoomFrames.load())<<' '<<sAnimationDepth<<' '<<sDialogueLift<<' '<<int(sConservativeVisibility.load())<<' '<<sEyeHeight<<' '<<sEyeForward<<' '<<int(sTrackedArms.load())<<' '<<sHandOutwardDegrees.load()<<' '<<sHandHeightAdjustment.load()<<' '<<sHandForwardAdjustment.load()<<' '<<int(sShortForearms.load())<<' '<<int(sMirrorAvatarTracking.load())<<' '<<int(sHandDirectedTools.load())<<' '<<int(sBeltEquipment.load())<<' '<<int(sFlashlightLeft.load())<<' '<<int(sPoltergustLeft.load())<<' '<<int(sGameBoyLeft.load())<<' '<<0<<' '<<0<<' '<<int(sMotionCounterPull.load())<<' '<<int(sInventoryToolbar.load())<<' '<<int(sPhysicalDoors.load())<<' '<<sToolbarDrop.load()<<' '<<sToolbarReach.load()<<' '<<38<<' '<<sHandRollDegrees[0].load()<<' '<<sHandRollDegrees[1].load()<<' '<<sMirrorHandHeight.load()<<' '<<int(sGrabGameBoy.load())<<' '<<int(sInvertRightY.load())<<' '<<sHudX.load()<<' '<<sHudY.load()<<' '<<sHudSpread.load()<<' '<<int(sExpandedStoryScenes.load())<<' '<<int(sWorldLockedSet.load())<<' '<<int(sSetSlide.load())<<'\n';
}
void readSettings(std::istream& file) {
    int mode=0;float scale=0.05f,distance=0.8f,units=100;
    if(file>>mode>>scale>>distance>>units) {
        if((mode>=0 && mode<=2) && scale>=0.01f && scale<=0.5f && distance>=-2 && distance<=5 && units>=1 && units<=1000)
            sOptions={vr::ViewMode(mode),units,scale,distance};
        float height=0;
        if(file>>height && height>=-2 && height<=2) sOptions.verticalOffset=height;
        int hide=1;
        if(file>>hide) sHideRoomFrames.store(hide!=0);
        float depth=0.85f;
        if(file>>depth && std::isfinite(depth) && depth>=0 && depth<=1) sAnimationDepth=depth;
        float lift=0.18f;
        if(file>>lift && std::isfinite(lift) && lift>=-0.1f && lift<=0.4f) sDialogueLift=lift;
        int visibility=1;
        if(file>>visibility) sConservativeVisibility.store(visibility!=0);
        float eyeHeight=1.80f;
        if(file>>eyeHeight && std::isfinite(eyeHeight) && eyeHeight>=0.8f && eyeHeight<=2.2f) sEyeHeight=eyeHeight;
        float forward=0.08f;
        if(file>>forward && std::isfinite(forward) && forward>=0 && forward<=0.5f) sEyeForward=forward;
        int arms=1;if(file>>arms) sTrackedArms.store(arms!=0);
        float outward=0;if(file>>outward && std::isfinite(outward) && outward>=-20 && outward<=30) sHandOutwardDegrees.store(outward);
        float offset=0;if(file>>offset && std::isfinite(offset) && std::fabs(offset)<=0.25f)sHandHeightAdjustment.store(offset);
        if(file>>offset && std::isfinite(offset) && std::fabs(offset)<=0.25f)sHandForwardAdjustment.store(offset);
        int sleeves=0;if(file>>sleeves)sShortForearms.store(sleeves!=0);
        int mirror=1;if(file>>mirror)sMirrorAvatarTracking.store(mirror!=0);
        int tools=1;if(file>>tools)sHandDirectedTools.store(tools!=0);
        int belt=1;if(file>>belt)sBeltEquipment.store(belt!=0);
        int side=0;if(file>>side)sFlashlightLeft.store(side!=0);if(file>>side)sPoltergustLeft.store(side!=0);if(file>>side)sGameBoyLeft.store(side!=0);
        float legacyClearance=0;file>>legacyClearance; // retired guessed palm offset
        int legacyFollow=0;file>>legacyFollow;
        int pull=1;if(file>>pull)sMotionCounterPull.store(pull!=0);
        int toolbar=1;if(file>>toolbar)sInventoryToolbar.store(toolbar!=0);
        int doors=0;if(file>>doors)sPhysicalDoors.store(doors!=0);
        float drop=0.50f,reach=0.23f;if(file>>drop&&std::isfinite(drop)&&drop>=0.30f&&drop<=0.80f)sToolbarDrop.store(drop);
        if(file>>reach&&std::isfinite(reach)&&reach>=0.10f&&reach<=0.45f)sToolbarReach.store(reach);
        int layout=0;if(!(file>>layout)||layout<38){sFlashlightLeft.store(true);sPoltergustLeft.store(false);sGameBoyLeft.store(false);}
        for(unsigned h=0;h<2;++h){float roll=0;if(file>>roll&&std::isfinite(roll)&&roll>=-180&&roll<=180)sHandRollDegrees[h].store(roll);}
        sGrabGameBoy.store(false);
        float mirrorHeight=-0.25f;if(file>>mirrorHeight&&std::isfinite(mirrorHeight)&&mirrorHeight>=-0.6f&&mirrorHeight<=0.3f)sMirrorHandHeight.store(mirrorHeight);
        int grabDevice=0;if(file>>grabDevice)sGrabGameBoy.store(grabDevice!=0);
        sInvertRightY.store(false);int invert=0;if(file>>invert)sInvertRightY.store(invert!=0);
    }
    float hx=0,hy=-0.48f,hs=0.08f;
    if(file>>hx>>hy>>hs && std::isfinite(hx)&&std::isfinite(hy)&&std::isfinite(hs)){sHudX.store(std::clamp(hx,-0.6f,0.6f));sHudY.store(std::clamp(hy,-0.6f,0.6f));sHudSpread.store(std::clamp(hs,-0.4f,0.4f));}
    else {sHudX.store(0);sHudY.store(-0.48f);sHudSpread.store(0.08f);}
    sExpandedStoryScenes.store(true);int expanded=1;if(file>>expanded)sExpandedStoryScenes.store(expanded!=0);
    sWorldLockedSet.store(true);sSetSlide.store(false);
    int locked=1;if(file>>locked)sWorldLockedSet.store(locked!=0);
    int slide=0;if(file>>slide)sSetSlide.store(slide!=0);
    sInventoryToolbar.store(false); // retired invisible/key toolbar; retain legacy ini fields
 }

std::filesystem::path modeFile(int mode) {
    const char* names[]{"vr-diorama.ini","vr-life-size.ini","vr-first-person.ini"};
    return port::configDir()/names[std::clamp(mode,0,2)];
}
void save() {
    std::ofstream mode(modeFile(int(sOptions.mode)));writeSettings(mode);
    std::ofstream legacy(port::configDir()/"vr.ini");writeSettings(legacy);
}
void loadMode(int mode) {
    std::istringstream defaults(sDefaultSettings);readSettings(defaults);
    std::ifstream file(modeFile(mode));if(file)readSettings(file);
    sOptions.mode=vr::ViewMode(mode);
    {std::lock_guard lock(sMovementMutex);sMovementActive=false;sHookedActive=false;sCapturePulls.reset();sCaptureContext=0;sAvatarGrabs={};}
    log("[vr settings] loaded independent mode %d profile",mode);
}

}
bool prepare() {
    const char* requested=std::getenv("LM_VR");
    if(!requested || std::string(requested)!="1") return false;
#ifdef _WIN32
    const wchar_t* loader=_wgetenv(L"LM_OPENXR_LOADER");
    if(!loader || !std::filesystem::path(loader).is_absolute())
        throw std::runtime_error("VR launch requires LM_OPENXR_LOADER pointing to the installed OpenXR loader DLL");
    sSession=std::make_unique<lmvr::OpenXrSession>(); sSession->prepare(loader);
    const auto& luid=sSession->requirements().adapterLuid;
    const auto packed=(uint64_t(uint32_t(luid.HighPart))<<32)|luid.LowPart;
    char text[32];std::snprintf(text,sizeof(text),"%llx",static_cast<unsigned long long>(packed));
    _putenv_s("LM_XR_ADAPTER_LUID",text);
#else
    // Apple Vision Pro: the OpenXR provider is linked in (no loader); its system is the headset.
    sSession=std::make_unique<lmvr::OpenXrSession>(); sSession->prepare(L"");
#endif
    std::ostringstream defaults;writeSettings(defaults);sDefaultSettings=defaults.str();
    std::ifstream file(port::configDir()/"vr.ini");
    readSettings(file);
    const int savedMode=int(sOptions.mode);
    if(std::filesystem::exists(modeFile(savedMode)))loadMode(savedMode);
    else {std::ofstream migrated(modeFile(savedMode));writeSettings(migrated);}
    if(const char* first=std::getenv("LM_VR_FIRST_PERSON"); first && std::string(first)=="1") {if(sOptions.mode!=vr::ViewMode::FirstPerson)loadMode(2);}
    if(const char* view=std::getenv("LM_VR_VIEW_MODE")) {
        const std::string requestedMode(view);
        if(requestedMode=="diorama")loadMode(0);
        else if(requestedMode=="life-size")loadMode(1);
        else if(requestedMode=="first-person")loadMode(2);
    }
    sEnabled=true; log("[vr] OpenXR system prepared; separate VR profile in use");
    return true;
}
bool enabled() {return sEnabled;}
bool sessionHeld() {
#ifdef LM_VR_VISIONOS
    // Not running yet, or stopped by the Digital Crown (the provider waits in IDLE for a new
    // layer), or no longer focused once it was (headset off, visionOS over the game).
    static bool everFocused=false,everRan=false,wasRunning=false;
    if(!sEnabled||!sSession) return false;
    if(!sSession->poll()) return false; // lost: beginFrame reports it
    if(sSession->focused() && !everFocused) {everFocused=true;lmvr_visionos_mark_session_focused();}
    // A space reopened after the Digital Crown has an origin of its own (where you stand now):
    // the diorama recentres in front of you, as at the start.
    const bool running=sSession->running();
    if(running && !wasRunning && everRan) {recenter();log("[vr] session running again: recentred");}
    if(running) everRan=true;
    wasRunning=running;
    return !running || (everFocused && !sSession->focused());
#else
    return false;
#endif
}
void attach() {
    if(!sEnabled) return;
    auto device=aurora::webgpu::g_device;
#ifdef _WIN32
    sSession->attach(dawn::native::d3d12::GetD3D12Device(device.Get()).Get(),dawn::native::d3d12::GetD3D12CommandQueue(device.Get()).Get());
#else
    sSession->attach();
    // Nobody sees SDL's offscreen window: the frame reaches the eyes through vr_present_target
    // (drawn whatever the window does), so skip acquiring and presenting its surface.
    aurora::gfx::set_surface_present_suppressed(true);
#endif
    for(unsigned eye=0;eye<3;++eye) {
        sOutput[eye]=std::make_unique<lmvr::DawnEyeOutput>();
        const auto& view=sSession->viewSizes()[eye<2?eye:0];
        sOutput[eye]->initialize(device,sSession->format(),view.recommendedImageRectWidth,view.recommendedImageRectHeight);
    }
    log("[vr] Dawn/OpenXR output attached");
}
void updateSettingsInput() {
    if(!sEnabled||!sSession)return;
    const auto& controllers=sSession->controllers();
    auto& io=ImGui::GetIO();
    if(menu::isOpen()) {
        static float mouseX=0,mouseY=0;static bool mouseInitialized=false;
        float x=0,y=0;const auto buttons=SDL_GetGlobalMouseState(&x,&y);
        if(mouseInitialized && (std::fabs(x-mouseX)>.5f||std::fabs(y-mouseY)>.5f||buttons))sDesktopMenuPointer=true;
        mouseX=x;mouseY=y;mouseInitialized=true;
        if(controllers[0].trigger>.15f||controllers[1].trigger>.15f)sDesktopMenuPointer=false;
        if(sDesktopMenuPointer) {
            if(sUiPointerDown){io.AddMouseButtonEvent(0,false);sUiPointerDown=false;}
            io.MouseDrawCursor=false;
        } else {
        // Keep one controller captured through the entire drag. Idle trigger noise
        // on the other controller must not steal a slider.
        if(!sUiPointerDown) {
            for(unsigned h=0;h<2;++h)
                if(controllers[h].trigger>0.15f && controllers[h].trigger>controllers[1-h].trigger && sToolAimValid[h])sUiPointerHand=h;
            std::array<float,2> probe{};
            if((!sToolAimValid[sUiPointerHand] || !vr::settingsPanelPointer(sToolAim[sUiPointerHand],sPanelAspect,probe)) &&
               sToolAimValid[1-sUiPointerHand] && vr::settingsPanelPointer(sToolAim[1-sUiPointerHand],sPanelAspect,probe))sUiPointerHand=1-sUiPointerHand;
        }
        std::array<float,2> uv{-1,-1};const bool hit=sToolAimValid[sUiPointerHand]&&vr::settingsPanelPointer(sToolAim[sUiPointerHand],sPanelAspect,uv);
        // Outside the panel, retain finite coordinates while dragging so a slider
        // can reach its endpoint without jumping to the opposite end.
        if(std::isfinite(uv[0]) && std::isfinite(uv[1]) && (hit||sUiPointerDown)) {
            uv[0]=std::clamp(uv[0],0.f,1.f);uv[1]=std::clamp(uv[1],0.f,1.f);
        }
        const float scroll=controllers[1].stick.y;if(std::fabs(scroll)>0.25f)io.AddMouseWheelEvent(0,scroll*std::clamp(io.DeltaTime,0.f,0.05f)*5.f);
        io.MouseDrawCursor=true;io.AddMousePosEvent(uv[0]*io.DisplaySize.x,uv[1]*io.DisplaySize.y);
        const bool down=sToolAimValid[sUiPointerHand]&&controllers[sUiPointerHand].trigger>(sUiPointerDown?0.35f:0.55f)&&(hit||sUiPointerDown);
        if(down!=sUiPointerDown){io.AddMouseButtonEvent(0,down);sUiPointerDown=down;}
        }
    } else {
        if(sUiPointerDown){io.AddMouseButtonEvent(0,false);sUiPointerDown=false;}
        io.MouseDrawCursor=false;
    }
}
void beginFrame() {
    aurora::imgui::set_input_callback(updateSettingsInput);
    sRender=false;
    if(!sEnabled) return;
#ifdef LM_VR_VISIONOS
    aurora::gfx::request_room_depth(lmvr_visionos_room_enabled());  // before this frame records
#endif
    if(!sSession->poll()) throw std::runtime_error("OpenXR runtime requested session exit; restart to reconnect");
    sFrame=sSession->beginFrame();
    if(!sFrame) {
        std::lock_guard lock(sMovementMutex);sMovementActive=false;sControllerConnected=false;sMenuStick={};sMoveStick={};sButtons.sample(0,false);sInteractions.cancel();return;
    }
    {
        std::lock_guard lock(sMovementMutex);
        const auto& controls=sSession->controllers();
        sControllerConnected=controls[0].inputActive||controls[1].inputActive;
        sButtons.sample(controls[0].buttons|controls[1].buttons,sControllerConnected);
        sMenuStick=controls[0].stick;
        sCaptureStick=controls[1].stick;
        sToolLeft=controls[0].trigger;sToolRight=controls[1].trigger;
    }
    if(!sSession->shouldRender()){std::lock_guard lock(sMovementMutex);sMovementActive=false;return;}
    const auto& views=sSession->views();
    if(sRecenter) {
        sOrigin=pose(views[0].pose);
        for(unsigned k=0;k<3;++k) sOrigin.position[k]=(pose(views[0].pose).position[k]+pose(views[1].pose).position[k])*0.5f;
        sOrigin=vr::uprightRecenter(sOrigin);
        sRecenter=false;
    }
    for(unsigned eye=0;eye<2;++eye) {
        const auto& view=views[eye]; auto& camera=sCameras[eye];
        camera.transform=vr::eyeFromGameCamera(pose(view.pose),sOrigin,sOptions);
        camera.inverse=inverseUniform(camera.transform);
        camera.projection=vr::gxProjection({view.fov.angleLeft,view.fov.angleRight,view.fov.angleUp,view.fov.angleDown},0.01f,1000.0f);
        camera.hud=vr::hudMapping(camera.projection,pose(view.pose),sOrigin);
        auto headPose=pose(views[0].pose);
        for(unsigned k=0;k<3;++k)headPose.position[k]=(pose(views[0].pose).position[k]+pose(views[1].pose).position[k])*0.5f;
        camera.dialogueHud=vr::hudMapping(camera.projection,pose(view.pose),headPose);
        camera.eyeFromRoom=vr::multiply(vr::inversePose(pose(view.pose)),vr::poseMatrix(sOrigin));
    }
    const auto reference=vr::inversePose(sOrigin);
    const auto head=vr::multiply(reference,vr::poseMatrix(pose(views[0].pose)));
    if(std::hypot(head[2],head[10])>0.1f)sHeadYaw=std::atan2(head[2],head[10]);
    sHeadMatrix=head;
    const auto& right=sSession->controllers()[1];
    for(unsigned h=0;h<2;++h) {const auto& hand=sSession->controllers()[h];sToolAimValid[h]=hand.aimValid;if(hand.aimValid)sToolAim[h]=vr::multiply(reference,vr::poseMatrix(pose(hand.aim)));}
    const auto& controllers=sSession->controllers();
    const bool chord=controllers[0].inputActive&&controllers[1].inputActive&&controllers[0].stickClick&&controllers[1].stickClick;
    if(chord&&!sMenuChordHeld){menu::toggle();discardControllerButtons();}
    sMenuChordHeld=chord;
    checkpoints::clicks(controllers[0].inputActive&&controllers[0].stickClick,controllers[1].inputActive&&controllers[1].stickClick,menu::isOpen());
    // Recenter on release so a two-stick menu chord cannot also recenter.
    static bool recenterHeld=false,recenterChord=false;
    const bool rightClick=right.inputActive&&right.stickClick;
    if(rightClick){if(!recenterHeld)recenterChord=false;recenterChord|=chord;}
    if(!checkpoints::shortcuts()&&recenterHeld&&!rightClick&&diorama()&&!recenterChord&&!menu::isOpen())recenter();
    recenterHeld=rightClick;

    if(!right.gripValid || std::fabs(right.stick.x)<0.3f) sTurnReady=true;
    if(firstPerson() && !menu::isOpen() && !sPanel && !sHookedActive && right.gripValid && sTurnReady && std::fabs(right.stick.x)>0.7f) {
        sHeading+=right.stick.x>0?-0.523598776f:0.523598776f;sTurnReady=false;sSession->pulse(1,0.15f,0.025f);
    }
    {
        const auto now=std::chrono::steady_clock::now();
        const float dt=sCaptureFrame.time_since_epoch().count()?std::chrono::duration<float>(now-sCaptureFrame).count():0;
        sCaptureFrame=now;
        std::lock_guard lock(sMovementMutex);
        bool holding=false;unsigned holdingHand=0;
        for(unsigned h=0;h<2;++h){const auto& source=sSession->controllers()[h];
            if(sAvatarGrabs[h].target&&sAvatarGrabs[h].kind==vr::GrabKind::Poltergust&&source.inputActive&&source.gripValid&&source.squeeze>0.35f&&source.trigger>=0.65f){holding=true;holdingHand=h;}}
        const bool hooked=firstPerson()&&!sPanel&&holding&&sCaptureContext==sEquipmentContext&&
            sCaptureStamp.time_since_epoch().count()&&now-sCaptureStamp<std::chrono::milliseconds(180);
        sHookedActive=hooked;
        if(!holding){sCaptureStamp={};sCaptureContext=0;sCapturePulls.reset();}
        const bool motion=hooked&&sMotionCounterPull.load();
        const double seconds=std::chrono::duration<double>(now.time_since_epoch()).count();
        vr::Vec3 delta{};
        if(motion) {
            const auto local=vr::multiply(reference,vr::poseMatrix(pose(sSession->controllers()[holdingHand].grip)));
            const vr::Vec3 relative{local[3]-head[3],local[7]-head[7],local[11]-head[11]};
            if(sMotionPullActive&&holdingHand==sPullHand)for(unsigned k=0;k<3;++k)delta[k]=relative[k]-sPullLast[k];
            sPullLast=relative;sPullHand=holdingHand;
        }
        sCapturePulls.step(delta,sHeading,dt,seconds,motion);
        sMotionPullActive=motion;
    }
    {std::lock_guard lock(sMovementMutex);for(unsigned h=0;h<2;++h)if(sWorldPulse[h]>0){sSession->pulse(h,sWorldPulse[h],0.05f);sWorldPulse[h]=0;}}
    auto targets=sInventoryToolbar.load()?vr::toolbarTargets(sHeadMatrix,sHeadYaw,sPoltergustLeft.load(),sToolbarDrop.load(),sToolbarReach.load()):vr::beltTargets(sHeadMatrix,sHeadYaw,sFlashlightLeft.load(),sPoltergustLeft.load(),sGameBoyLeft.load());
    {
        std::lock_guard lock(sMovementMutex);
        if(sEquipmentContext!=sLastEquipmentContext) {sInteractions.cancel();sLastEquipmentContext=sEquipmentContext;}
        targets[0].available=sEquipmentOwned[0];targets[1].available=sEquipmentOwned[2];targets[2].available=sEquipmentOwned[1];targets[3].available=sEquipmentOwned[3]&&sGrabGameBoy.load();
    }
    for(unsigned h=0;h<2;++h) {
        const auto& source=sSession->controllers()[h];auto& hand=sHands[h];
        hand={};hand.input.valid=source.inputActive && source.gripValid && firstPerson() && (!sPanel||sGameBoySession);
        hand.input.grip=source.squeeze;hand.input.trigger=source.trigger;
        hand.input.fingerCurl=source.fingerCurl;hand.input.fingerValid=source.fingerValid;
        if(hand.input.valid) {
            const auto local=vr::multiply(reference,vr::poseMatrix(pose(source.grip)));
            // Quaternion reference orientation is yaw-only; compose explicitly.
            const auto& a=sOrigin.orientation;const auto& b=pose(source.grip).orientation;
            hand.input.pose.orientation={a[3]*b[0]-a[1]*b[2],a[3]*b[1]-a[1]*b[3],a[3]*b[2]+a[1]*b[0],a[3]*b[3]+a[1]*b[1]};
            hand.input.pose.position={local[3],local[7],local[11]};
            const float side=h?1.0f:-1.0f;
            hand.arm=vr::solveArm({side*0.14f,-0.50f,0.03f},hand.input.pose.position,{side,-0.6f,0.2f},0.227f,0.214f);
        }
    }
    if(!sBeltEquipment.load()||!firstPerson()||(sPanel&&!sGameBoySession))sInteractions.cancel();
    const auto grabs=sInteractions.update({sHands[0].input,sHands[1].input},targets);
    if(std::getenv("LM_VR_GRAB_TRACE")) {
        static unsigned frames=0;if(++frames%180==0) {
            log("[vr grab trace] belt %d toolbar %d panel %d movement %d context %llX owned %d%d%d%d head %.2f %.2f %.2f",int(sBeltEquipment.load()),int(sInventoryToolbar.load()),int(sPanel),int(sMovementActive),static_cast<unsigned long long>(sEquipmentContext),int(targets[0].available),int(targets[1].available),int(targets[2].available),int(targets[3].available),sHeadMatrix[3],sHeadMatrix[7],sHeadMatrix[11]);
            for(unsigned h=0;h<2;++h) {const auto& hand=sHands[h].input;const auto& t=targets[0];log("[vr grab trace] hand %u valid %d grip %.3f position %.2f %.2f %.2f flash %.2f %.2f %.2f held %u",h,int(hand.valid),hand.grip,hand.pose.position[0],hand.pose.position[1],hand.pose.position[2],t.position[0],t.position[1],t.position[2],grabs[h].target);}
        }
    }
    sBeltTargets=targets;
    sHolsterPose={{0,std::sin(sHeadYaw/2),0,std::cos(sHeadYaw/2)},targets[0].position};
    for(unsigned h=0;h<2;++h) {
        auto available=targets;
        for(auto& t:available)for(const auto& g:grabs)if(g.target) {
            const bool tool=t.kind==vr::GrabKind::Flashlight||t.kind==vr::GrabKind::Poltergust;
            const bool ownedTool=g.kind==vr::GrabKind::Flashlight||g.kind==vr::GrabKind::Poltergust;
            if(t.id==g.target||(tool&&ownedTool&&t.kind==g.kind))t.available=false;
        }
        const auto hover=grabs[h].target?0:vr::hoverTarget(sHands[h].input,available);
        if(hover&&hover!=sHover[h])sSession->pulse(h,0.35f,0.06f);
        sHover[h]=hover;
    }
    for(unsigned h=0;h<2;++h) {
        sHands[h].grab=grabs[h];
        if(grabs[h].grabbed) {
            sSession->pulse(h);log("[vr grab] hand %u, tool %u, target %u",h,unsigned(grabs[h].kind),grabs[h].target);
            if(grabs[h].kind==vr::GrabKind::GameBoy) {
                std::lock_guard lock(sMovementMutex);
                const auto& controls=sSession->controllers();
                sButtons.sample(controls[0].buttons|controls[1].buttons|vr::Search,sControllerConnected);
            }
        }
    }
    bool deviceHeld=false,deviceTrigger=false;
    for(unsigned h=0;h<2;++h)if(grabs[h].kind==vr::GrabKind::GameBoy&&sHands[h].input.valid){deviceHeld=true;deviceTrigger=sHands[h].input.trigger>=0.65f;}
    {
        std::lock_guard lock(sMovementMutex);
        const auto& controls=sSession->controllers();unsigned extra=0;
        if(sGameBoySession&&!deviceHeld)extra|=vr::Back;
        if(deviceHeld&&deviceTrigger&&!sGameBoyTriggerDown)extra|=vr::Confirm;
        if(extra)sButtons.sample(controls[0].buttons|controls[1].buttons|extra,sControllerConnected);
        for(unsigned h=0;h<2;++h){sAvatarInput[h]=sHands[h].input;sAvatarGrabs[h]=grabs[h];}
        sAvatarToolAim=sToolAim;sAvatarToolAimValid=sToolAimValid;sAvatarHolsterPose=sHolsterPose;sAvatarHolsterTargets=targets;
        sAvatarHeading=sHeading;sAvatarHeight=sEyeHeight;sAvatarForward=sEyeForward;
    }
    sGameBoySession=deviceHeld;sGameBoyTriggerDown=deviceHeld&&deviceTrigger;
    sRender=true;sEye=0;
    if(sTransition.time_since_epoch().count())
        sGain=std::clamp(std::chrono::duration<float>(std::chrono::steady_clock::now()-sTransition).count()/0.35f,0.0f,1.0f);
}
bool rendering() {return sRender;}
bool panelMode() {return sPanel && !sStereoPanel;}
float animationDepth() {return sAnimationDepth;}
bool expandedStoryScenes(){return diorama() && sExpandedStoryScenes.load();}
void setCreditsView(bool active){if(active!=sCreditsView)sCreditsFocus=0;sCreditsView=active;}
std::array<float,3> hudLayout(){return {sHudX.load(),sHudY.load(),sHudSpread.load()};}
float dialogueLift() {return sOptions.mode==vr::ViewMode::Diorama && sScriptedWorld ? std::min(sDialogueLift,0.08f) : sDialogueLift;}
bool gameBoyGrabbable(){return sGrabGameBoy.load();}
bool diorama() {return sEnabled && sOptions.mode==vr::ViewMode::Diorama;}
namespace {
// The world-locked set (dioramaSet). Placed at a room or map change, a real camera cut outside
// scripted scenes, the return to the world from a panel; recentred (a shift, never a turn) when
// Luigi leaves the comfortable zone; held otherwise.
struct DioramaSetState {
    bool valid=false,worldBefore=false,cameraSeen=false,provisional=false;
    unsigned map=~0u;
    int room=-2,pendingRoom=-2,pendingFrames=0;
    vr::SetPlacement placement;
    float dy=0;         // the camera's height above Luigi at the placement (game units)
    float centre[2]{};  // where Luigi stood (in the room, metres) after the last placement or recentre
    float shift[2]{};   // horizontal offset of the set since the placement (recentres, the slide)
    std::array<float,3> lastCamera{};
    float lastHeading=0;
    std::chrono::steady_clock::time_point last{};
};
DioramaSetState sSet;
float snapQuarter(float yaw) {const float q=1.5707963f;return std::round(yaw/q)*q;}
}
bool dioramaSet(const vr::Matrix& view,const vr::PlayerAnchor& player,unsigned map,int room,bool cut,vr::Matrix& setFromWorld) {
    if(!diorama() || !sWorldLockedSet.load()) {sSet.valid=false;return false;}
    if(sEye==0) {
        const auto now=std::chrono::steady_clock::now();
        const float dt=sSet.last.time_since_epoch().count()?std::clamp(std::chrono::duration<float>(now-sSet.last).count(),0.f,0.25f):0.f;
        sSet.last=now;
        const bool returned=!sSet.worldBefore;  // first tracked-world frame after a panel
        sSet.worldBefore=true;
        const vr::Matrix world=vr::worldFromView(view);
        // The placement keeps the composition the game camera had at that moment, without its
        // pitch or roll: its position goes where the original diorama put it (behind your eyes:
        // the Depth offset slider), turned only about the vertical, snapped to 90 degrees.
        const std::array<float,3> camera{world[3],world[7],world[11]};
        const float heading=vr::viewHeading(view);
        // What it looks at: Luigi when tracked, else 6 m ahead of the camera.
        std::array<float,3> focus{world[3]-world[2]*600.f,world[7]-world[6]*600.f,world[11]-world[10]*600.f};
        if(player.valid) focus=player.position;
        const float scale=sOptions.miniatureScale/sOptions.unitsPerMetre;
        // A camera cut counts when the camera really jumped since the last frame (the replay's cut
        // flag also marks view-structure changes, and repeats for every present of a tape).
        bool jumped=false;
        if(sSet.cameraSeen) {
            const float moved=std::hypot(camera[0]-sSet.lastCamera[0],camera[1]-sSet.lastCamera[1],camera[2]-sSet.lastCamera[2]);
            const float turned=std::fabs(std::remainder(heading-sSet.lastHeading,6.2831853f));
            jumped=moved>250.f || turned>0.5236f;
        }
        sSet.lastCamera=camera;sSet.lastHeading=heading;sSet.cameraSeen=true;
        // A room change counts once the new room has held for a few frames (no flicker at
        // open passages).
        bool roomChanged=false;
        if(room!=sSet.room) {
            if(room!=sSet.pendingRoom){sSet.pendingRoom=room;sSet.pendingFrames=0;}
            roomChanged=++sSet.pendingFrames>=3 || !sSet.valid;
        } else sSet.pendingFrames=0;
        // The game's placeholder camera (no rotation at all) before its own is set up: a placement
        // made from it is provisional, replaced by the first real camera even in a cutscene.
        const bool placeholder=std::fabs(view[0]-1.f)<1e-5f && std::fabs(view[5]-1.f)<1e-5f && std::fabs(view[10]-1.f)<1e-5f;
        const bool upgrade=sSet.valid && sSet.provisional && !placeholder;
        const bool cameraCut=(cut || jumped) && jumped && !sScriptedWorld;
        const bool onlyReturned=returned && sSet.valid && map==sSet.map && !roomChanged && !cameraCut && !upgrade;
        const bool refit=!sSet.valid || map!=sSet.map || roomChanged || returned || cameraCut || upgrade;
        const auto placedAt=[&](const vr::Matrix& m,const std::array<float,3>& p,float& x,float& z) {
            x=m[0]*p[0]+m[1]*p[1]+m[2]*p[2]+m[3];
            z=m[8]*p[0]+m[9]*p[1]+m[10]*p[2]+m[11];
        };
        if(refit) {
            sSet.placement.anchor=camera;
            sSet.placement.yaw=snapQuarter(heading);
            sSet.dy=focus[1]-camera[1];
            sSet.shift[0]=sSet.shift[1]=0;
            sSet.valid=true;sSet.map=map;sSet.room=room;sSet.pendingFrames=0;sSet.provisional=placeholder;
        }
        // Where the set is now: the camera's place behind you plus the shift; Luigi's floor 0.45 m
        // below your eyes at the default Scene height (the sliders apply live).
        sSet.placement.scale=scale;
        sSet.placement.at={sSet.shift[0],sOptions.verticalOffset-0.25f-scale*sSet.dy,-sOptions.viewingDistance+sSet.shift[1]};
        float x=0,z=0;
        placedAt(vr::setFromWorld(sSet.placement),focus,x,z);
        if(refit) {
            // Luigi starts in front of you: if the snapped heading or a close camera put him to the
            // side or behind, the set shifts (never turns) to bring him ahead.
            if(std::fabs(std::atan2(x,-z))>1.0472f || z>-0.2f) {
                sSet.shift[0]-=x;sSet.shift[1]+=-0.85f-z;
                sSet.placement.at={sSet.shift[0],sSet.placement.at[1],-sOptions.viewingDistance+sSet.shift[1]};
                placedAt(vr::setFromWorld(sSet.placement),focus,x,z);
            }
            sSet.centre[0]=x;sSet.centre[1]=z;
            // A short fade covers the step, except on a return from a panel (setWorldScene decides
            // those: the map and the search camera switch directly).
            if(!onlyReturned) {sTransition=std::chrono::steady_clock::now();sGain=0;}
            log("[vr set] placed: map %u room %d, camera %.0f %.0f %.0f, Luigi at %.2f %.2f m, heading %.0f deg%s",map,room,
                camera[0],camera[1],camera[2],x,z,sSet.placement.yaw*57.29578f,returned?" (back from a panel)":"");
        } else if(player.valid && !sScriptedWorld) {
            const float dx=x-sSet.centre[0],dz=z-sSet.centre[1];
            const float fromCentre=std::hypot(dx,dz);
            const float bearing=std::fabs(std::atan2(x,-z));
            if(sSetSlide.load()) {
                // Option: a lazy slide (no turning) that keeps Luigi within 0.35 m of the centre.
                constexpr float deadZone=0.35f;
                if(fromCentre>deadZone) {
                    const float k=1.f-std::exp(-dt/1.2f);
                    const float excess=(fromCentre-deadZone)/fromCentre;
                    sSet.shift[0]-=dx*excess*k;sSet.shift[1]-=dz*excess*k;
                }
            } else if(fromCentre>1.0f || bearing>1.0472f) {
                // Past 1 m, or out of the forward +/-60 degrees: Luigi back where he stood, in one
                // step behind a short fade.
                sSet.shift[0]-=dx;sSet.shift[1]-=dz;
                sTransition=std::chrono::steady_clock::now();sGain=0;
                log("[vr set] recentred: Luigi had moved %.2f m (bearing %.0f deg)",fromCentre,bearing*57.29578f);
            }
            sSet.placement.at={sSet.shift[0],sSet.placement.at[1],-sOptions.viewingDistance+sSet.shift[1]};
        }
    }
    if(!sSet.valid) return false;
    setFromWorld=vr::setFromWorld(sSet.placement);
    return true;
}
bool conservativeVisibility() {return sEnabled && sConservativeVisibility.load(std::memory_order_relaxed);}
const std::array<AvatarHand,2>& avatarHands() {return sHands;}
float firstPersonHeading(float initialYaw,unsigned actorId) {
    static unsigned previousActor=0;
    if(actorId && actorId!=previousActor) {previousActor=actorId;sHeadingValid=false;}
    if(!sHeadingValid) {sHeading=initialYaw;sHeadingValid=true;}
    return sHeading;
}
void setMovementView(const vr::Matrix& authoredView) {
    std::lock_guard lock(sMovementMutex);
    sMovement=vr::locomotionMapping(authoredView,sHeading+sHeadYaw);
    sVrPadConnected=sSession->controllers()[0].inputActive;
    sMoveStick=sVrPadConnected?sSession->controllers()[0].stick:XrVector2f{};
    sMovementActive=firstPerson() && !sPanel && sRender;
    for(unsigned h=0;h<2;++h) {sAvatarInput[h]=sHands[h].input;sAvatarGrabs[h]=sHands[h].grab;}
    sAvatarToolAim=sToolAim;sAvatarToolAimValid=sToolAimValid;sAvatarHolsterPose=sHolsterPose;
    sAvatarBodyHeading=sHeading+sHeadYaw;
    sAvatarHeadTilt=vr::multiply(vr::poseMatrix({{0,std::sin(-sHeadYaw/2),0,std::cos(-sHeadYaw/2)}, {}}),sHeadMatrix);
    sAvatarHeadTilt[3]=sAvatarHeadTilt[7]=sAvatarHeadTilt[11]=0;
    sGazeForward=vr::worldGaze(sHeadMatrix,sHeading);
    sAvatarHeading=sHeading;sAvatarHeight=sEyeHeight;sAvatarForward=sEyeForward;
}
bool avatarTargets(std::array<vr::HandInput,2>& hands,float& heading,float& height,float& forward) {
    std::lock_guard lock(sMovementMutex);
    if(!firstPerson()||!sMovementActive) return false;
    hands=sAvatarInput;
    if(!sTrackedArms.load()) for(auto& hand:hands) hand.valid=false;heading=sAvatarHeading;height=sAvatarHeight;forward=sAvatarForward;return true;
}
void setEquipmentSnapshot(std::uint64_t context,bool flashlight,bool poltergust,bool key,bool gameBoy) {
    std::lock_guard lock(sMovementMutex);sEquipmentContext=context;sEquipmentOwned={flashlight,poltergust,key,gameBoy};
}
bool readNativeToolTriggers(float& left,float& right) {
    std::lock_guard lock(sMovementMutex);left=right=0;
    if(!sControllerConnected)return false;
    if(!firstPerson()){left=sToolLeft;right=sToolRight;return sEnabled;}
    if(!firstPerson()||!sMovementActive||!sEquipmentOwned[1])return false;
    if(sBeltEquipment.load()) {
        const auto activation=vr::equipmentTriggers(sAvatarGrabs,sAvatarInput);
        left=activation[0];right=activation[1];
    } else {left=sToolLeft;right=sToolRight;}
    return true;
}
void discardControllerButtons() {std::lock_guard lock(sMovementMutex);sButtons.sample(0,false);}
std::uint32_t readControllerButtons(bool& connected) {
    std::lock_guard lock(sMovementMutex);connected=sControllerConnected;return sButtons.consume();
}
bool applyMovement(std::int8_t& x,std::int8_t& y,bool directionalMenu) {
    std::lock_guard lock(sMovementMutex);
    if(!firstPerson()||!sMovementActive || directionalMenu) {
        if(sControllerConnected && (!firstPerson() || sPanel || directionalMenu)) {
            vr::mergeNativeStick(x,y,sMenuStick.x,sMenuStick.y);
        }
        return sControllerConnected;
    }
    // A dialogue/event view changes stick coordinates, not input ownership.
    // Training scripts can leave gameplay active, and choices need raw up/down.
    // Let the guest enforce its own movement locks instead of dropping VR axes.
    if(sHookedActive&&sMotionCounterPull.load()&&std::hypot(sCaptureStick.x,sCaptureStick.y)>0.2f) {
        // Preserve the native directional resistance and HP rules. The VR
        // right stick replaces the original gameplay stick during attachment.
        x=y=0;
        vr::routeControllerStick(x,y,sCaptureStick.x,sCaptureStick.y,true,sMovement);
    } else vr::routeControllerStick(x,y,sMoveStick.x,sMoveStick.y,true,sMovement);
    return sControllerConnected;

}
bool invertRightStickY(){return sEnabled&&sInvertRightY.load();}
bool readCameraStick(std::int8_t& x,std::int8_t& y) {
    std::lock_guard lock(sMovementMutex);
    if(!sEnabled||firstPerson()||!sControllerConnected)return false;
    vr::mergeNativeStick(x,y,sCaptureStick.x,sCaptureStick.y);
    return true;
}
bool firstPerson() {return sEnabled && sOptions.mode==vr::ViewMode::FirstPerson;}
float firstPersonEyeHeight() {return sEyeHeight;}
float firstPersonEyeForward() {return sEyeForward;}
float worldUnitsPerMetre() {return sOptions.unitsPerMetre;}
bool gazeDirection(vr::Vec3& direction) {std::lock_guard lock(sMovementMutex);if(!firstPerson()||!sMovementActive)return false;direction=sGazeForward;return true;}
std::array<float,2> handPositionAdjustment(){return {sHandHeightAdjustment.load(),sHandForwardAdjustment.load()};}
bool controllerToolPose(vr::Matrix& grip,vr::Matrix& aim,float& heading,float& height,float& forward,vr::GrabKind kind) {
    std::lock_guard lock(sMovementMutex);
    if(!firstPerson()||(!sMovementActive&&kind!=vr::GrabKind::GameBoy)||!sHandDirectedTools.load()||!sEquipmentOwned[0])return false;
    unsigned hand=1;
    if(sBeltEquipment.load()) {
        bool held=false;
        for(unsigned h=0;h<2;++h)if(sAvatarGrabs[h].target&&(kind==vr::GrabKind::None||sAvatarGrabs[h].kind==kind)&&(sAvatarGrabs[h].kind==vr::GrabKind::Flashlight||sAvatarGrabs[h].kind==vr::GrabKind::Poltergust||kind==vr::GrabKind::GameBoy)){hand=h;held=true;}
        if(!held&&kind==vr::GrabKind::GameBoy)return false;
        if(!held){grip=aim=vr::poseMatrix(sAvatarHolsterPose);heading=sAvatarHeading;height=sAvatarHeight;forward=sAvatarForward;return true;}
    }
    if(!sAvatarInput[hand].valid)return false;
    grip=vr::poseMatrix(sAvatarInput[hand].pose);aim=sAvatarToolAimValid[hand]?sAvatarToolAim[hand]:grip;
    heading=sAvatarHeading;height=sAvatarHeight;forward=sAvatarForward;return true;
}
bool holsterPose(vr::GrabKind kind,vr::Matrix& result,float& heading,float& height,float& forward) {
    std::lock_guard lock(sMovementMutex);
    if(!firstPerson()||!sMovementActive||!sBeltEquipment.load())return false;
    for(const auto& g:sAvatarGrabs)if(g.target&&g.kind==kind)return false;
    for(const auto& t:sAvatarHolsterTargets)if(t.kind==kind&&t.available) {
        result=vr::poseMatrix({sAvatarHolsterPose.orientation,t.position});
        if(kind==vr::GrabKind::Flashlight)result=vr::multiply(result,vr::poseMatrix({{-0.70710678f,0,0,0.70710678f},{}}));
        heading=sAvatarHeading;height=sAvatarHeight;forward=sAvatarForward;return true;
    }
    return false;
}
bool beltMode(){std::lock_guard lock(sMovementMutex);return firstPerson()&&sBeltEquipment.load()&&sMovementActive;}
vr::GrabKind heldEquipment(){std::lock_guard lock(sMovementMutex);if(!firstPerson()||!sMovementActive||!sBeltEquipment.load())return vr::GrabKind::None;for(const auto& g:sAvatarGrabs)if(g.target)return g.kind;return vr::GrabKind::None;}
bool heldEquipmentControl(unsigned& hand,float& trigger,vr::GrabKind kind) {
    std::lock_guard lock(sMovementMutex);trigger=0;
    if(!firstPerson()||!sMovementActive||!sBeltEquipment.load())return false;
    for(unsigned h=0;h<2;++h)if(sAvatarGrabs[h].target&&sAvatarInput[h].valid&&(kind==vr::GrabKind::None||sAvatarGrabs[h].kind==kind)&&(sAvatarGrabs[h].kind==vr::GrabKind::Flashlight||sAvatarGrabs[h].kind==vr::GrabKind::Poltergust)) {hand=h;trigger=vr::equipmentTriggers(sAvatarGrabs,sAvatarInput)[sAvatarGrabs[h].kind==vr::GrabKind::Flashlight?2:1];return true;}
    return false;
}
void drawSettingsPointer() {
    if(!sEnabled||!menu::isOpen()||sDesktopMenuPointer||!sToolAimValid[sUiPointerHand])return;
    std::array<float,2> uv{};
    if(!vr::settingsPanelPointer(sToolAim[sUiPointerHand],sPanelAspect,uv)&&!sUiPointerDown)return;
    const auto size=ImGui::GetIO().DisplaySize;
    ImVec2 p{std::clamp(uv[0],0.f,1.f)*size.x,std::clamp(uv[1],0.f,1.f)*size.y};
    auto* draw=ImGui::GetForegroundDrawList();
    const ImU32 color=sUiPointerDown?IM_COL32(255,200,65,255):IM_COL32(70,225,255,255);
    draw->AddCircleFilled(p,5,color);draw->AddCircle(p,10,IM_COL32(10,20,25,255),24,3);
    draw->AddText({p.x+14,p.y+10},color,sUiPointerHand?"Right":"Left");
}
void setSceneAperture(const vr::Aperture* aperture) {
    sSceneApertureActive[sEye]=aperture!=nullptr;
    if(aperture)sSceneApertures[sEye]=*aperture;
}
void drawBelt() {
    if(sRender && sSceneApertureActive[sEye] && !sPanel){
        const auto size=ImGui::GetIO().DisplaySize;
        auto* draw=ImGui::GetBackgroundDrawList();
        for(const auto& mask:vr::outsideAperture(sSceneApertures[sEye]))if(mask.size()>=3){
            std::vector<ImVec2> points;for(auto p:mask)points.push_back({(p[0]+1)*size.x*.5f,(1-p[1])*size.y*.5f});
            // ImGui expects clockwise polygons in screen coordinates.
            std::reverse(points.begin(),points.end());
            const auto flags=draw->Flags;draw->Flags&=~ImDrawListFlags_AntiAliasedFill;
            draw->AddConvexPolyFilled(points.data(),int(points.size()),IM_COL32(0,0,0,255));draw->Flags=flags;
        }
    }

    if(!sRender)return;
    if(sPanel)return;
    const auto view=vr::inversePose(pose(sSession->views()[sEye].pose));
    const auto reference=vr::poseMatrix(sOrigin);
    const auto eye=vr::multiply(view,reference);
    const auto& projection=sCameras[sEye].projection;
    const auto size=ImGui::GetIO().DisplaySize;
    auto* draw=ImGui::GetBackgroundDrawList();
    {vr::Vec3 point{};bool visible=false,hover=false;{std::lock_guard lock(sMovementMutex);point=sWorldContact;visible=sWorldVisible;hover=sWorldHover;}
        if(visible&&physicalDoors()) {
            vr::Vec3 q{};for(unsigned r=0;r<3;++r){q[r]=eye[r*4+3];for(unsigned k=0;k<3;++k)q[r]+=eye[r*4+k]*point[k];}
            if(q[2]<-0.05f){float clip[4]{};for(unsigned r=0;r<4;++r){clip[r]=projection[r*4+3];for(unsigned k=0;k<3;++k)clip[r]+=projection[r*4+k]*q[k];}
                if(std::fabs(clip[3])>0.001f)draw->AddCircle({(clip[0]/clip[3]+1)*size.x/2,(1-clip[1]/clip[3])*size.y/2},std::clamp(size.y*0.018f/-q[2],5.f,65.f),hover?IM_COL32(255,220,70,255):IM_COL32(180,195,180,160),24,hover?3.f:1.f);}
        }}
    if(!beltMode()||!sInventoryToolbar.load())return; // no belt, key box or tool cards
    const float c=std::cos(sHeadYaw),sn=std::sin(sHeadYaw);
    struct Face {ImVec2 p[4];float z;ImU32 color;};std::vector<Face> faces;
    auto box=[&](vr::Vec3 center,vr::Vec3 half,ImU32 color) {
        ImVec2 screen[8];float depth[8];
        for(unsigned v=0;v<8;++v) {
            const float x=(v&1?1:-1)*half[0],y=(v&2?1:-1)*half[1],z=(v&4?1:-1)*half[2];
            vr::Vec3 p{center[0]+c*x+sn*z,center[1]+y,center[2]-sn*x+c*z},q{};
            for(unsigned r=0;r<3;++r){q[r]=eye[r*4+3];for(unsigned k=0;k<3;++k)q[r]+=eye[r*4+k]*p[k];}
            if(q[2]>-0.025f)return;
            float clip[4]{};for(unsigned r=0;r<4;++r){clip[r]=projection[r*4+3];for(unsigned k=0;k<3;++k)clip[r]+=projection[r*4+k]*q[k];}
            if(std::fabs(clip[3])<0.001f)return;
            screen[v]={(clip[0]/clip[3]+1)*size.x/2,(1-clip[1]/clip[3])*size.y/2};depth[v]=q[2];
        }
        static constexpr unsigned indices[6][4]={{0,1,3,2},{4,6,7,5},{0,4,5,1},{2,3,7,6},{0,2,6,4},{1,5,7,3}};
        for(const auto& f:indices){Face face{};face.color=color;for(unsigned k=0;k<4;++k){face.p[k]=screen[f[k]];face.z+=depth[f[k]];}faces.push_back(face);}
    };
    auto center=sBeltTargets[1].position;for(unsigned k=0;k<3;++k)center[k]=(sBeltTargets[0].position[k]+sBeltTargets[1].position[k])/2;center[1]+=0.06f;
    box(center,{0.31f,0.025f,0.035f},IM_COL32(67,45,24,255));
    for(const auto& t:sBeltTargets) {
        if(!t.available&&!sInventoryToolbar.load())continue;
        bool held=false;for(const auto& hand:sHands)held|=hand.grab.target==t.id;
        if(held)continue;
        const bool hover=sHover[0]==t.id||sHover[1]==t.id;
        if(t.kind==vr::GrabKind::Poltergust)continue; // shoulder grab, not a belt slot
        if(sInventoryToolbar.load())continue; // centered cards below replace stacked marker boxes
        const ImU32 rim=!t.available?IM_COL32(45,48,43,255):hover?IM_COL32(255,216,70,255):IM_COL32(115,120,112,255);
        box(t.position,{0.065f,0.055f,0.055f},rim);
        auto p=t.position;p[1]+=0.065f;
        // Compact physical markers: flashlight lens and the vacuum nozzle mouth.
        box(p,{t.kind==vr::GrabKind::GameBoy?0.055f:0.04f,0.045f,0.045f},t.kind==vr::GrabKind::GameBoy?IM_COL32(90,110,150,255):IM_COL32(44,70,42,255));
        p[1]+=0.047f;box(p,{0.035f,0.007f,0.034f},hover?IM_COL32(255,247,166,255):IM_COL32(205,218,209,255));
    }
    std::sort(faces.begin(),faces.end(),[](const auto& a,const auto& b){return a.z<b.z;});
    for(const auto& f:faces)draw->AddConvexPolyFilled(f.p,4,f.color);
    if(sInventoryToolbar.load())for(const auto& t:sBeltTargets) {
        if(t.kind==vr::GrabKind::Poltergust)continue;
        if(!t.available&&!sInventoryToolbar.load())continue;
        bool held=false;for(const auto& hand:sHands)held|=hand.grab.target==t.id;if(held)continue;
        vr::Vec3 q{};for(unsigned r=0;r<3;++r){q[r]=eye[r*4+3];for(unsigned k=0;k<3;++k)q[r]+=eye[r*4+k]*t.position[k];}
        if(q[2]>=-0.05f)continue;float clip[4]{};for(unsigned r=0;r<4;++r){clip[r]=projection[r*4+3];for(unsigned k=0;k<3;++k)clip[r]+=projection[r*4+k]*q[k];}
        if(std::fabs(clip[3])<0.001f)continue;
        ImVec2 p{(clip[0]/clip[3]+1)*size.x/2,(1-clip[1]/clip[3])*size.y/2};
        const float radius=std::clamp(size.y*0.012f/-q[2],4.f,45.f);
        const ImU32 ink=t.available?IM_COL32(245,250,230,255):IM_COL32(95,100,90,255);
        const bool hovered=sHover[0]==t.id||sHover[1]==t.id;
        draw->AddRectFilled({p.x-radius*1.25f,p.y-radius*1.25f},{p.x+radius*1.25f,p.y+radius*1.25f},IM_COL32(32,44,34,230),5);
        draw->AddRect({p.x-radius*1.25f,p.y-radius*1.25f},{p.x+radius*1.25f,p.y+radius*1.25f},hovered?IM_COL32(255,221,85,255):IM_COL32(135,151,137,255),5,0,hovered?3.f:1.5f);
        // Colored silhouettes of the retail equipment, kept as vector UI so
        // the toolbar remains sharp at either eye resolution.
        const auto at=[&](float x,float y){return ImVec2{p.x+x*radius,p.y+y*radius};};
        const auto poly=[&](std::initializer_list<ImVec2> points,ImU32 color){std::vector<ImVec2> vertices;for(const auto& q:points)vertices.push_back(at(q.x,q.y));draw->AddConvexPolyFilled(vertices.data(),int(vertices.size()),color);};
        const ImU32 shell=t.available?IM_COL32(232,235,224,255):IM_COL32(98,102,94,255);
        const ImU32 dark=IM_COL32(39,44,42,255),metal=IM_COL32(130,144,139,255);
        if(t.kind==vr::GrabKind::Flashlight) {
            draw->AddRectFilled(at(-.17f,-.05f),at(.17f,1.f),dark,3);
            for(float y:{.30f,.48f,.66f,.84f})draw->AddLine(at(-.15f,y),at(.15f,y),metal,1);
            poly({{-.21f,.1f},{.21f,.1f},{.53f,-.72f},{-.53f,-.72f}},shell);
            draw->AddEllipseFilled(at(0,-.72f),{radius*.54f,radius*.20f},metal);
            draw->AddEllipseFilled(at(0,-.75f),{radius*.43f,radius*.13f},t.available?IM_COL32(255,242,170,255):dark);
            draw->AddCircleFilled(at(.05f,.26f),radius*.08f,IM_COL32(184,41,32,255));
        } else if(t.kind==vr::GrabKind::Poltergust) {
            draw->AddRectFilled(at(-.86f,-.32f),at(-.21f,.74f),t.available?IM_COL32(157,38,31,255):metal,4);
            draw->AddRectFilled(at(-.78f,-.53f),at(-.30f,-.28f),metal,2);
            draw->AddBezierCubic(at(-.53f,.67f),at(-.65f,1.25f),at(.95f,1.30f),at(.50f,.12f),metal,radius*.18f);
            draw->AddLine(at(.40f,.07f),at(.50f,.71f),dark,radius*.20f);
            poly({{.13f,-.70f},{.88f,-.70f},{.72f,.12f},{.29f,.12f}},shell);
            draw->AddEllipseFilled(at(.50f,-.67f),{radius*.42f,radius*.24f},metal);
            draw->AddEllipseFilled(at(.50f,-.68f),{radius*.32f,radius*.17f},t.available?IM_COL32(66,155,111,255):dark);
            for(unsigned blade=0;blade<3;++blade){const float angle=blade*2.0944f;draw->AddLine(at(.50f,-.68f),at(.50f+.27f*std::cos(angle),-.68f+.14f*std::sin(angle)),shell,2);}
        } else if(t.kind==vr::GrabKind::GameBoy) {
            draw->AddRectFilled(at(-.70f,-1.f),at(.70f,1.f),shell,5);
            draw->AddRectFilled(at(-.58f,-.87f),at(.58f,.21f),dark,3);
            draw->AddRectFilled(at(-.45f,-.73f),at(.45f,.05f),t.available?IM_COL32(95,153,134,255):metal,2);
            draw->AddRectFilled(at(-.45f,.44f),at(-.12f,.56f),dark);
            draw->AddRectFilled(at(-.34f,.33f),at(-.22f,.67f),dark);
            draw->AddCircleFilled(at(.34f,.47f),radius*.13f,IM_COL32(153,43,46,255));
            draw->AddCircleFilled(at(.12f,.67f),radius*.10f,IM_COL32(153,43,46,255));
            for(float y:{.76f,.84f,.92f})draw->AddLine(at(.28f,y),at(.53f,y),metal,1);
        } else {
            draw->AddCircle(at(-.32f,-.50f),radius*.39f,ink,20,4);
            draw->AddLine(at(-.06f,-.20f),at(.64f,.67f),ink,5);
            draw->AddLine(at(.34f,.32f),at(.12f,.52f),ink,4);
            draw->AddLine(at(.56f,.58f),at(.34f,.78f),ink,4);
        }
    }
}
bool physicalDoors(){return firstPerson()&&sPhysicalDoors.load();}
bool worldHands(std::array<vr::HandInput,2>& hands,float& heading,float& height,float& forward) {
    std::lock_guard lock(sMovementMutex);if(!firstPerson()||!sMovementActive||menu::isOpen())return false;
    hands=sAvatarInput;for(unsigned h=0;h<2;++h)hands[h].occupied=sAvatarGrabs[h].target!=0;
    heading=sAvatarHeading;height=sAvatarHeight;forward=sAvatarForward;return true;
}
void worldContact(vr::Vec3 point,bool visible,unsigned hand,bool hover,bool grabbed,bool completed) {
    std::lock_guard lock(sMovementMutex);const bool entered=hover&&(!sWorldVisible||!sWorldHover||std::hypot(point[0]-sWorldContact[0],point[2]-sWorldContact[2])>0.15f);
    sWorldContact=point;sWorldVisible=visible;sWorldHover=hover;
    if(hand<2&&(entered||grabbed||completed))sWorldPulse[hand]=std::max(sWorldPulse[hand],completed?0.7f:grabbed?0.5f:0.25f);
}
bool shortForearms(){return sShortForearms.load();}
bool mirrorAvatarTracking(){return sMirrorAvatarTracking.load();}
bool avatarHeadTilt(vr::Matrix& tilt){std::lock_guard lock(sMovementMutex);if(!firstPerson()||!sMovementActive)return false;tilt=sAvatarHeadTilt;return true;}
void captureFocus(vr::Vec3 player,vr::Vec3 ghost,unsigned actor) {
    std::lock_guard lock(sMovementMutex);const auto now=std::chrono::steady_clock::now();
    const double seconds=std::chrono::duration<double>(now.time_since_epoch()).count();
    if(sCaptureContext!=sEquipmentContext){sCapturePulls.reset();sMotionPullActive=false;sCaptureActor=0;}
    sCapturePulls.observe(actor,player,ghost,seconds);
    const auto focus=sCapturePulls.focus(seconds);
    if(!focus.actor)return;
    if(focus.actor!=sCaptureActor){log("[vr capture] primary target %08X; independent per-ghost counter-pull tracking",focus.actor);}
    sCapturedYaw=focus.bearing;sCaptureActor=focus.actor;sCaptureContext=sEquipmentContext;sCaptureStamp=now;
}
bool captureDamageAllowed(unsigned actor) {
    // Damage remains entirely the original game's decision, for every target.
    return true;
}
void captureDamageFeedback(unsigned actor) {
    if(!firstPerson()||!sMotionCounterPull.load())return;
    std::lock_guard lock(sMovementMutex);const auto now=std::chrono::steady_clock::now();
    if(now-sCaptureFeedback<std::chrono::milliseconds(90))return;
    if(sPullHand<2)sWorldPulse[sPullHand]=std::max(sWorldPulse[sPullHand],0.35f);
    sCaptureFeedback=now;
}

float bodyHeading() {std::lock_guard lock(sMovementMutex);return sAvatarBodyHeading;}
float mirrorHandHeight(){return sMirrorHandHeight.load();}
float handRoll(unsigned hand){return sHandRollDegrees[hand?1:0].load()*0.01745329252f;}
float handOutwardYaw() {return sHandOutwardDegrees.load()*0.01745329252f;}
bool hideRoomFrames() {return sEnabled && sHideRoomFrames.load(std::memory_order_relaxed);}
unsigned eyeIndex() {return sEye;}
void selectEye(unsigned eye) {if(eye>1) throw std::logic_error("Invalid VR eye");sEye=eye;aurora::webgpu::set_vr_overlay_separate(menu::isOpen());}
const EyeCamera* activeEye() {
    if(!sRender || (sPanel && !sStereoPanel)) return nullptr;
    auto& camera=sCameras[sEye];camera.cinematic=sStereoPanel;
    if(sScriptedWorld && !sPanel && !firstPerson()) {
        const auto& views=sSession->views();
        auto eye=pose(views[sEye].pose);
        for(unsigned k=0;k<3;++k) {
            const float centre=(pose(views[0].pose).position[k]+pose(views[1].pose).position[k])*0.5f;
            eye.position[k]=centre+sAnimationDepth*(eye.position[k]-centre);
        }
        camera.transform=sCreditsView?vr::eyeFromCreditsCamera(eye,sOrigin,sOptions,sScriptedFocus):
            vr::eyeFromScriptedCamera(eye,sOrigin,sOptions,sScriptedFocus,sForwardShot);
        camera.inverse=inverseUniform(camera.transform);
    }
    if(sStereoPanel) {
        camera.eyeOffset=sEye?3.2f:-3.2f;
        camera.transform=vr::poseMatrix({});camera.transform[3]=-camera.eyeOffset;
        camera.inverse=vr::poseMatrix({});camera.inverse[3]=camera.eyeOffset;
    }
    return &camera;
}
void holdSearchTransition(bool hold){sHoldSearchTransition=hold;}
void copyEye(unsigned eye) {
    if(sHoldSearchTransition&&!menu::isOpen()&&sSession->reuseEye(eye))return;
    if(!sRender || eye>1) throw std::logic_error("Invalid VR eye output state");
    const auto started=std::chrono::steady_clock::now();
    aurora::gfx::render_worker::synchronize();
    const auto synced=std::chrono::steady_clock::now();
    aurora::webgpu::set_vr_overlay_separate(menu::isOpen());
    const auto& source=aurora::webgpu::vr_present_source();
    static unsigned width=0,height=0;
    if(width!=source.size.width || height!=source.size.height) {
        width=source.size.width;height=source.size.height;
        log("[vr] game eye render source: %ux%u",width,height);
    }
    sPanelAspect=float(source.size.width)/float(source.size.height);
#ifdef LM_VR_VISIONOS
    {
        // The mansion in your room (the launcher's mixed space): the tracked world's eyes are
        // transparent where the frame drew nothing; menu panels stay whole screens.
        // The scene's depth: aurora's snapshot from before the frame's scene copy cleared it (the
        // game pastes its 3D scene back as a flat picture), else the EFB depth itself.
        const bool roomSpace=lmvr_visionos_room_enabled();
        const auto snapshot=aurora::gfx::room_depth();
        const bool efbDepth=!snapshot && aurora::webgpu::g_graphicsConfig.msaaSamples<=1 && aurora::webgpu::g_depthBuffer.view;
        const bool room=roomSpace && !sPanel && !sStereoPanel && (snapshot || efbDepth);
        sOutput[eye]->setRoomMask(!room?wgpu::TextureView{}:snapshot?snapshot:aurora::webgpu::g_depthBuffer.view,bool(snapshot));
        sSession->setRoom(roomSpace,room);
    }
#endif
    auto* image=sSession->acquireEye(eye);
    sOutput[eye]->copy(source.view,source.sampler,image,sGain);
    sSession->releaseEye(eye);
    if(eye==0&&menu::isOpen()) {
        const auto& overlay=aurora::webgpu::vr_overlay_target();
        auto* uiImage=sSession->acquireEye(2);
        sOutput[2]->copy(overlay.view,overlay.sampler,uiImage,1.f,true);
        sSession->releaseEye(2);
    }
    const auto done=std::chrono::steady_clock::now();
    sSyncMs+=std::chrono::duration<double,std::milli>(synced-started).count();
    sCopyMs+=std::chrono::duration<double,std::milli>(done-synced).count();
}
void endFrame(bool submit) {
    XrPosef panel{};panel.orientation={sOrigin.orientation[0],sOrigin.orientation[1],sOrigin.orientation[2],sOrigin.orientation[3]};
    const auto origin=vr::poseMatrix(sOrigin);
    panel.position={origin[3]-1.6f*origin[2],origin[7]-1.6f*origin[6],origin[11]-1.6f*origin[10]};
    const bool settingsPanel=menu::isOpen();
    if(sFrame) sSession->endFrame(submit && sRender,sPanel?&panel:nullptr,sPanelAspect,sStereoPanel,settingsPanel?&panel:nullptr);
    if(submit && sRender) {
        static unsigned submitted=0;
        static auto started=std::chrono::steady_clock::now();
        ++submitted;
        if(submitted==1 || submitted%180==0) {
            const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
            const unsigned interval=submitted==1?1:180;
            log("[vr] submitted stereo frame %u, mode %d, panel %d, %.1f fps, render sync %.1f ms / transfer %.1f ms per stereo frame",submitted,int(sOptions.mode),sPanel,seconds>0?interval/seconds:0,sSyncMs/interval,sCopyMs/interval);
            started=std::chrono::steady_clock::now();sSyncMs=sCopyMs=0;
        }
    }
    sFrame=false;sRender=false;
}
void recenter() {sHeadingValid=false;sInteractions.cancel();sRecenter=true;sTransition=std::chrono::steady_clock::now();}
void cameraCut(std::uint64_t sequence) {
    static std::uint64_t last=0;
    if(sEnabled && sequence!=last) {last=sequence;sTransition=std::chrono::steady_clock::now();sGain=0;}
}
void setWorldScene(bool world, bool cinematic, bool scriptedWorld, float scriptedFocus, bool forwardShot, bool directMap) {
    if(sHoldSearchTransition&&!menu::isOpen())return;
    sForwardShot=forwardShot;
    if(sCreditsView){if(std::isfinite(scriptedFocus)&&scriptedFocus>1)sCreditsFocus=scriptedFocus;
        sScriptedFocus=sCreditsFocus;}else sScriptedFocus=scriptedFocus;
    if(!world) {std::lock_guard lock(sMovementMutex);sMovementActive=false;sSet.worldBefore=false;}
    const bool changed=(sPanel==world) || (sStereoPanel!=cinematic) || (!firstPerson() && !diorama() && sScriptedWorld!=scriptedWorld);
    sScriptedWorld=scriptedWorld;
    sStereoPanel=cinematic;
    if(sPanel==world) {
        sPanel=!world;
        if(world) recenter(); // establish the first room reference after the title/menu panel
    }
    static bool previousMap=false;
    const bool direct=directMap||previousMap;
    previousMap=directMap;
    if(direct) {
        // Map entry/exit should be a direct switch, not the comfort fade used
        // for authored cinematics. Age the timer too, so beginFrame retains it.
        sTransition=std::chrono::steady_clock::now()-std::chrono::seconds(1);sGain=1;
    } else if(changed) {sTransition=std::chrono::steady_clock::now();sGain=0;}
    if(changed) log("[vr] presentation: %s",world?"tracked world":cinematic?"stereo cinema screen":"flat menu screen");
}
void drawControlOptions() {
    if(!sEnabled)return;
    bool invert=sInvertRightY.load();
    if(ImGui::Checkbox("Invert right stick up/down",&invert)){sInvertRightY.store(invert);save();}
    ImGui::SetItemTooltip("Invert vertical right-stick gameplay input for gamepads and motion controllers. Saved separately for the current VR mode.");
}
namespace {
bool resettableSlider(const char* label,float* value,float low,float high,const char* format,float defaults) {
    defaults=menu::sliderResetDefault(label,*value);
    ImGui::PushID(label);ImGui::BeginGroup();
    ImGui::SetNextItemWidth(std::max(80.f,ImGui::CalcItemWidth()-64.f));
    bool changed=ImGui::SliderFloat(label,value,low,high,format);
    ImGui::SameLine();if(ImGui::SmallButton("Reset")){*value=defaults;changed=true;}
    ImGui::SetItemTooltip("Restore saved baseline: %.3f",defaults);
    ImGui::EndGroup();ImGui::PopID();return changed;
}
}
void drawMenu() {
    if(!sEnabled) {ImGui::TextWrapped("Launch with tools/launch-isolated.ps1 -EnableVR to enable OpenXR.");return;}
    int mode=int(sOptions.mode);bool changed=false;
    changed|=ImGui::RadioButton("Miniature diorama",&mode,0);ImGui::SameLine();
    changed|=ImGui::RadioButton("Life-size camera",&mode,1);
    changed|=ImGui::RadioButton("First person (experimental)",&mode,2);
    if(mode!=int(sOptions.mode)){save();loadMode(mode);recenter();}
    ImGui::TextDisabled("Settings save separately for this view mode.");
    ImGui::TextWrapped("Press both thumbsticks to toggle F1. Aim either controller at the menu; trigger selects or drags. Right stick scrolls.");
    if(mode==2) {
        changed|=resettableSlider("Luigi eye height (m)",&sEyeHeight,0.8f,2.2f,"%.2f",1.8f);
        changed|=resettableSlider("Eye forward offset (m)",&sEyeForward,0.0f,0.5f,"%.2f",0.20f);
        ImGui::SetItemTooltip("Lower values move the eye back relative to Luigi. Default 0.20 m; object scale and headset field of view stay unchanged.");
        for(unsigned h=0;h<2;++h) {
            float roll=sHandRollDegrees[h].load();
            if(resettableSlider(h?"Right hand rotation (degrees)":"Left hand rotation (degrees)",&roll,-180.f,180.f,"%.1f",h?-60.8f:60.8f)){sHandRollDegrees[h].store(roll);changed=true;}
            ImGui::SetItemTooltip("Wrist roll: positive is counterclockwise viewed from your wrist toward the fingers. Updates gloves, held tools and reflections live. Ctrl-click to enter an exact degree value.");
        }
        if(ImGui::Button("Reset hand rotations")){sHandRollDegrees[0].store(menu::sliderResetDefault("Left hand rotation (degrees)",sHandRollDegrees[0].load()));sHandRollDegrees[1].store(menu::sliderResetDefault("Right hand rotation (degrees)",sHandRollDegrees[1].load()));changed=true;}
        float outward=sHandOutwardDegrees.load();
        if(resettableSlider("Hand outward angle (degrees)",&outward,-20,30,"%.0f",0.f)) {sHandOutwardDegrees.store(outward);changed=true;}
        ImGui::SetItemTooltip("Fine glove alignment: positive turns both hands away from the centre, without moving the tracked wrist position.");
        float handHeight=sHandHeightAdjustment.load(),handForward=sHandForwardAdjustment.load();
        if(resettableSlider("Hand height adjustment (m)",&handHeight,-0.25f,0.25f,"%.2f",0.f)){sHandHeightAdjustment.store(handHeight);changed=true;}
        ImGui::SetItemTooltip("Optional glove fit: negative lowers the gloves. Zero preserves tracked placement.");
        if(resettableSlider("Hand reach adjustment (m)",&handForward,-0.25f,0.25f,"%.2f",0.f)){sHandForwardAdjustment.store(handForward);changed=true;}
        ImGui::SetItemTooltip("Positive moves gloves farther from your face in the upright recentered tracking frame. Does not change finger poses.");
        if(ImGui::Button("Reset hand position adjustments")){sHandHeightAdjustment.store(0);sHandForwardAdjustment.store(0);changed=true;}
        bool pull=sMotionCounterPull.load();
        if(ImGui::Checkbox("Right-stick capture resistance",&pull)){sMotionCounterPull.store(pull);changed=true;}
        ImGui::SetItemTooltip("Aim the vacuum physically. While attached, counter the ghost's movement with the right stick. Native capture damage rules apply; physical strokes do not gate damage. Snap turning pauses while attached.");
        bool arms=sTrackedArms.load();
        if(ImGui::Checkbox("Tracked Luigi gloves (experimental)",&arms)) {sTrackedArms.store(arms);changed=true;}
        bool sleeves=sShortForearms.load();
        if(ImGui::Checkbox("Short green forearms",&sleeves)){sShortForearms.store(sleeves);changed=true;}
        ImGui::SetItemTooltip("Native green forearm sleeves follow each glove without shoulder connections. Turn off to restore floating gloves. Reflections retain full native arms.");
        bool mirror=sMirrorAvatarTracking.load();
        if(ImGui::Checkbox("Tracked mirror avatar",&mirror)){sMirrorAvatarTracking.store(mirror);changed=true;}
        ImGui::SetItemTooltip("Reflection hands and fingers follow controllers; elbows are estimated and head tilt follows the headset. Turn off for authored mirror arm poses.");
        float mirrorHeight=sMirrorHandHeight.load();
        if(resettableSlider("Mirror hand height (m)",&mirrorHeight,-0.6f,0.3f,"%.2f",-0.25f)){sMirrorHandHeight.store(mirrorHeight);changed=true;}
        ImGui::SetItemTooltip("Mirror-only arm fit. Lower this if reflected hands stay above your real resting hands. Local glove placement is unchanged. Saved for this view mode.");
        bool tools=sHandDirectedTools.load();
        if(ImGui::Checkbox("Hand-directed flashlight and Poltergust",&tools)){sHandDirectedTools.store(tools);changed=true;}
        ImGui::SetItemTooltip("The holding controller moves and aims its tool; that controller trigger activates it. The opposite trigger uses the current vacuum element. Without holsters, right controller aims. Tracking loss or disabling this uses headset aim. Holster controls let either hand take a tool; hold grip to carry it. Release grip to return it to its hip or shoulder mount.");
        bool belt=sBeltEquipment.load();
        if(ImGui::Checkbox("Grip equipment from holsters",&belt)){sBeltEquipment.store(belt);sInteractions.cancel();changed=true;}
        ImGui::SetItemTooltip("Default: flashlight on left hip, Poltergust over right shoulder, Game Boy Horror on right hip. Hover for a gold highlight, hold grip to equip; release to holster. One of each tool can be held, one per hand. Disable for previous trigger controls.");
        bool doors=sPhysicalDoors.load();
        if(ImGui::Checkbox("Experimental physical wooden doors",&doors)){sPhysicalDoors.store(doors);changed=true;}
        ImGui::SetItemTooltip("Empty hand: grip a highlighted knob and twist your wrist. Walk through the open door using normal movement. It closes after you and both tracked hands clear the passage, or release and grip/twist its knob again to close it. Only unlocked ordinary wooden doors are supported. Locked/key and special doors retain native controls and presentation. Experimental and off by default.");
        bool grabDevice=sGrabGameBoy.load();
        if(ImGui::Checkbox("Enable grabbable Game Boy Horror",&grabDevice)){sGrabGameBoy.store(grabDevice);sInteractions.cancel();changed=true;}
        ImGui::SetItemTooltip("Shows the device on your first-person body and enables its hip grab. Off by default; normal game buttons still open its map/camera.");
        bool flashLeft=sFlashlightLeft.load(),vacLeft=sPoltergustLeft.load(),gbLeft=sGameBoyLeft.load();
        if(ImGui::Checkbox("Flashlight on left hip",&flashLeft)){sFlashlightLeft.store(flashLeft);sInteractions.cancel();changed=true;}
        if(ImGui::Checkbox("Poltergust over left shoulder",&vacLeft)){sPoltergustLeft.store(vacLeft);sInteractions.cancel();changed=true;}
        if(ImGui::Checkbox("Game Boy Horror on left hip",&gbLeft)){sGameBoyLeft.store(gbLeft);sInteractions.cancel();changed=true;}
        ImGui::TextWrapped("Grip the Game Boy Horror holster to open search camera. Aim with that hand and use its trigger to inspect; release grip to close. Existing buttons remain available.");
        ImGui::TextWrapped("Xbox or left VR stick moves relative to your view; right VR stick snap turns. Floating Luigi gloves follow tracked controllers. Grip and trigger animate fingers when joint sensing is unavailable. Body follows horizontal head direction. Grip tools directly from their model holsters. Physical keys and doors are under development.");
    }
    if(mode==0) {
        changed|=resettableSlider("Miniature scale",&sOptions.miniatureScale,0.01f,0.5f,"%.3f",0.10f);
        changed|=resettableSlider("Depth offset (m)",&sOptions.viewingDistance,-2,5,"%.2f",-0.25f);
        ImGui::SetItemTooltip("Lower values bring the miniature closer.");
        changed|=resettableSlider("Scene height (m)",&sOptions.verticalOffset,-2,2,"%.2f",-0.20f);
    }
    if(ImGui::TreeNode("HUD placement")) {
        float x=sHudX.load(),y=sHudY.load(),spread=sHudSpread.load();
        bool layout=resettableSlider("HUD horizontal",&x,-0.6f,0.6f,"%.2f",0.f);
        layout|=resettableSlider("HUD vertical",&y,-0.6f,0.6f,"%.2f",-0.48f);
        layout|=resettableSlider("HUD spacing",&spread,-0.4f,0.4f,"%.2f",0.08f);
        ImGui::TextWrapped("Positive spacing moves left and right HUD items apart. Includes health, money/radar and the Boo counter. Dialogue and menus keep their own placement.");
        if(ImGui::Button("Reset HUD placement")){x=menu::sliderResetDefault("HUD horizontal",x);y=menu::sliderResetDefault("HUD vertical",y);spread=menu::sliderResetDefault("HUD spacing",spread);layout=true;}
        if(layout){sHudX.store(x);sHudY.store(y);sHudSpread.store(spread);changed=true;}
        ImGui::TreePop();
    }
    if(diorama()) {
        bool follows=!sWorldLockedSet.load();
        if(ImGui::Checkbox("The set follows the game camera (original)",&follows)){sWorldLockedSet.store(!follows);changed=true;}
        ImGui::SetItemTooltip("Off (default): the room stays put in front of you, placed once per room and at camera cuts.\n"
                              "On: the miniature turns and moves with every game camera move, as in the original VR build.");
        if(!follows) {
            bool slide=sSetSlide.load();
            if(ImGui::Checkbox("Slide the set to keep Luigi near the centre",&slide)){sSetSlide.store(slide);changed=true;}
            ImGui::SetItemTooltip("Off (default): when Luigi leaves the middle of the room you're looking at, the set recentres in one step behind a short fade.\n"
                                  "On: it slides slowly, without turning.");
        }
        bool expanded=sExpandedStoryScenes.load();
        if(ImGui::Checkbox("All story and item scenes in diorama 3D",&expanded)){sExpandedStoryScenes.store(expanded);changed=true;}
        ImGui::SetItemTooltip("Experimental general rule for boss, event, door and item animations.\n"
                              "Disable to restore build 125's accepted scene rules. Menus and results retain their own framing.");
    }
    changed|=resettableSlider("Animation depth",&sAnimationDepth,0.0f,1.0f,"%.2f",0.85f);
    ImGui::SetItemTooltip("Stereo strength for scripted animations: 0 is flat, 1 is full strength. Room gameplay and the map keep their own depth.");
    changed|=resettableSlider("Dialogue lift (m)",&sDialogueLift,-0.1f,0.4f,"%.2f",0.18f);
    ImGui::SetItemTooltip("Raises speech bubbles and their text together, without moving health displays.");
    if(ImGui::Button("Recenter headset (F6)")) recenter();
    bool visibility=sConservativeVisibility.load();
    if(ImGui::Checkbox("Conservative room visibility",&visibility)) {sConservativeVisibility.store(visibility);changed=true;}
    ImGui::SetItemTooltip("Avoids original-camera portal clipping in VR. Closed door visibility checks remain active; may cost GPU time in connected rooms.");
    bool hide=sHideRoomFrames.load();
    if(ImGui::Checkbox("Hide authored ceiling/frame pieces",&hide)) {sHideRoomFrames.store(hide);changed=true;}
    ImGui::SetItemTooltip("Experimental cutaway for the room's fadeable outer frame group. Other walls remain.");
    if(mode==2 && ImGui::TreeNode("Tracked avatar diagnostics")) {
        for(unsigned h=0;h<2;++h) {
            const auto& hand=sHands[h];
            ImGui::Text("%s: %s, intent %d, grip %.2f, twist %.2f",h?"Right":"Left",hand.input.valid?"tracked":"inactive",int(hand.grab.kind),hand.input.grip,hand.grab.twist);
        }
        ImGui::TextWrapped("Belt grabs respect equipment ownership. Hold one of each tool, one per hand. Each trigger activates its own tool; release grip to holster. Physical keys and doors are not connected.");
        ImGui::TreePop();
    }
    ImGui::TextWrapped("Experimental stereo preview. Xbox controls remain active. Scale, effects and camera transitions are still being validated.");
    if(changed) {save();sTransition=std::chrono::steady_clock::now();}
}
void shutdown() {
    if(!sSession) return;
    aurora::gfx::render_worker::synchronize();
    sOutput[0].reset();sOutput[1].reset();sOutput[2].reset();sSession.reset();sEnabled=false;sRender=false;sFrame=false;
}
}
#else
namespace recomp::vrhost {
bool prepare(){return false;} bool enabled(){return false;} bool sessionHeld(){return false;} void attach(){} void beginFrame(){}
bool rendering(){return false;} bool panelMode(){return false;} void selectEye(unsigned){} unsigned eyeIndex(){return 0;} const EyeCamera* activeEye(){return nullptr;}
float firstPersonHeading(float yaw,unsigned){return yaw;}
void setMovementView(const vr::Matrix&){}
bool applyMovement(std::int8_t&,std::int8_t&,bool){return false;}
bool readNativeToolTriggers(float& left,float& right){left=right=0;return false;}
void discardControllerButtons(){}
std::uint32_t readControllerButtons(bool& connected){connected=false;return 0;}
void setEquipmentSnapshot(std::uint64_t,bool,bool,bool,bool){}
bool avatarTargets(std::array<vr::HandInput,2>&,float&,float&,float&){return false;}
const std::array<AvatarHand,2>& avatarHands(){static const std::array<AvatarHand,2> empty{};return empty;}
bool invertRightStickY(){return false;}
bool readCameraStick(std::int8_t&,std::int8_t&){return false;}
bool firstPerson(){return false;}
float firstPersonEyeHeight(){return 1.80f;}
float firstPersonEyeForward(){return 0.20f;}
float worldUnitsPerMetre(){return 100.0f;}
bool gazeDirection(vr::Vec3&){return false;}
std::array<float,2> handPositionAdjustment(){return {0,0};}
bool holsterPose(vr::GrabKind,vr::Matrix&,float&,float&,float&){return false;}
bool beltMode(){return false;}
vr::GrabKind heldEquipment(){return vr::GrabKind::None;}
bool heldEquipmentControl(unsigned&,float& trigger,vr::GrabKind){trigger=0;return false;}
void drawSettingsPointer(){}
void setSceneAperture(const vr::Aperture*){}
void drawBelt(){}
bool controllerToolPose(vr::Matrix&,vr::Matrix&,float&,float&,float&,vr::GrabKind){return false;}
bool physicalDoors(){return false;}
bool worldHands(std::array<vr::HandInput,2>&,float&,float&,float&){return false;}
void worldContact(vr::Vec3,bool,unsigned,bool,bool,bool){}
bool shortForearms(){return false;}
bool mirrorAvatarTracking(){return false;}
bool avatarHeadTilt(vr::Matrix&){return false;}
void captureFocus(vr::Vec3,vr::Vec3,unsigned){}
void captureDamageFeedback(unsigned){}
bool captureDamageAllowed(unsigned){return true;}
float bodyHeading(){return 0;}
float mirrorHandHeight(){return -0.25f;}
float handRoll(unsigned hand){return hand?-1.0611601852f:1.0611601852f;}
float handOutwardYaw(){return 0.13962634f;}
bool hideRoomFrames(){return false;}
float animationDepth(){return 1.0f;}
bool expandedStoryScenes(){return false;}
void setCreditsView(bool){}
std::array<float,3> hudLayout(){return {0,0,0};}
float dialogueLift(){return 0.0f;}
bool conservativeVisibility(){return false;}
bool diorama(){return false;}
bool dioramaSet(const vr::Matrix&,const vr::PlayerAnchor&,unsigned,int,bool,vr::Matrix&){return false;}
bool gameBoyGrabbable(){return false;}
void drawControlOptions(){}
void holdSearchTransition(bool){}
void copyEye(unsigned){} void endFrame(bool){} void recenter(){} void drawMenu(){} void shutdown(){}
void cameraCut(std::uint64_t){}
void setWorldScene(bool,bool,bool,float,bool,bool){}
}
#endif

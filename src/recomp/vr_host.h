#pragma once
#include "recomp/vr_camera.h"
#include "recomp/vr_avatar.h"
#include <cstdint>
namespace recomp::vrhost {
struct EyeCamera { vr::Matrix transform, inverse, projection; vr::Matrix eyeFromRoom{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}; bool cinematic=false; float eyeOffset=0; std::array<float,4> dialogueHud{1,1,0,0};
    std::array<float,4> hud{1,1,0,0}; };
bool prepare(); // opt-in LM_VR=1; before Aurora selects a graphics adapter
bool enabled();
bool sessionHeld(); // Apple Vision Pro: the session is stopped (Digital Crown) or lost focus after its first
void attach();
void beginFrame();
bool rendering();
bool panelMode();
bool invertRightStickY();
bool readCameraStick(std::int8_t& x,std::int8_t& y);
bool firstPerson();
float firstPersonEyeHeight();
float firstPersonEyeForward();
float worldUnitsPerMetre();
bool gazeDirection(vr::Vec3& direction); // synchronized world-space headset forward
void captureFocus(vr::Vec3 player,vr::Vec3 ghost,unsigned actor);
bool captureDamageAllowed(unsigned actor);
void captureDamageFeedback(unsigned actor);
float bodyHeading(); // snapshot of virtual turn plus horizontal head direction
std::array<float,2> handPositionAdjustment(); // height and forward, metres
bool shortForearms();
bool physicalDoors();
bool worldHands(std::array<vr::HandInput,2>& hands,float& heading,float& height,float& forward);
void worldContact(vr::Vec3 point,bool visible,unsigned hand,bool hover,bool grabbed,bool completed);
bool mirrorAvatarTracking();
bool beltMode();
bool holsterPose(vr::GrabKind kind,vr::Matrix& pose,float& heading,float& height,float& forward);
vr::GrabKind heldEquipment();
bool heldEquipmentControl(unsigned& hand,float& trigger,vr::GrabKind kind=vr::GrabKind::None);
void drawSettingsPointer();
void setSceneAperture(const vr::Aperture* aperture); // per-eye authored-shot boundary
void drawBelt(); // stereo waist markers, host render thread
bool controllerToolPose(vr::Matrix& grip,vr::Matrix& aim,float& heading,float& height,float& forward,vr::GrabKind kind=vr::GrabKind::None);
bool avatarHeadTilt(vr::Matrix& tilt);
float mirrorHandHeight();
float handRoll(unsigned hand);
float handOutwardYaw(); // calibrated glove yaw, radians
struct AvatarHand { vr::HandInput input; vr::ArmPose arm; vr::GrabState grab; };
const std::array<AvatarHand,2>& avatarHands(); // render-thread snapshot, not guest state
float firstPersonHeading(float initialYaw,unsigned actorId=0);
void setMovementView(const vr::Matrix& authoredView);
std::uint32_t readControllerButtons(bool& connected);
void discardControllerButtons();
bool readNativeToolTriggers(float& left,float& right); // interim button/analog fallback
void setEquipmentSnapshot(std::uint64_t context,bool flashlight,bool poltergust,bool key,bool gameBoy);
bool applyMovement(std::int8_t& x,std::int8_t& y,bool directionalMenu=false); // thread-safe guest input bridge
bool avatarTargets(std::array<vr::HandInput,2>& hands,float& heading,float& eyeHeight,float& forward);
bool hideRoomFrames();
float animationDepth();
bool expandedStoryScenes();
void setCreditsView(bool active);
bool conservativeVisibility();
bool diorama();
// The diorama's world-locked set (on by default): set-from-world for this frame, placed once per
// room or cut (and when Luigi leaves the comfortable zone, in one step behind a fade), never
// following the game camera. False: the original camera-relative diorama. Eye 0 updates it.
bool dioramaSet(const vr::Matrix& view,const vr::PlayerAnchor& player,unsigned map,int room,bool cut,vr::Matrix& setFromWorld);
bool gameBoyGrabbable();
std::array<float,3> hudLayout();
float dialogueLift(); // metres above the existing speech layout
void selectEye(unsigned eye);
unsigned eyeIndex();
const EyeCamera* activeEye();
void holdSearchTransition(bool hold);
void copyEye(unsigned eye);
void endFrame(bool submit);
void recenter();
void cameraCut(std::uint64_t sequence);
void setWorldScene(bool world, bool cinematic = false, bool scriptedWorld = false, float scriptedFocus = 200.0f, bool forwardShot = false, bool directMap = false);
void drawControlOptions();
void drawMenu();
void shutdown();
}

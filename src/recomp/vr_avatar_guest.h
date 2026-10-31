#pragma once
#include "recomp/ctx.h"
#include "recomp/vr_avatar.h"
#include <vector>
#include <utility>
namespace recomp::vravatar {
// Guest-thread native actor-pass scope; nesting restores the previous state.
class ReflectionScope {
    bool previous_;
public:
    explicit ReflectionScope(unsigned nativeFlags);
    ~ReflectionScope();
    ReflectionScope(const ReflectionScope&)=delete;
    ReflectionScope& operator=(const ReflectionScope&)=delete;
};
void notePlayerSceneResource(const char* name);
bool doorAnimationActive();
bool gameOverActive();
bool rescueEventCamera();
bool endingRescueActive();
bool hideKeyGhostPlayerDraw(u32 actor);
u32 hiddenKeyGhostPlayer();
bool ownsGameBoyHorror();
bool wellSceneHudHidden();
bool searchInspectionEvent();
bool conversationEventCamera();
bool phoneEventCamera();
bool boundedDioramaCamera();
s32 encounterOverlayEntry(const char* path);
bool isEncounterOverlay(s32 entry);
u32 encounterOverlayLength(s32 entry);
s32 encounterOverlayRead(s32 entry,void* dst,s32 length,s32 offset);
void setPlayer(u32 actor);
void worldFrame(Ctx& c);
void traceNativeHose(u32 player,unsigned pass);
class NativeHosePose {
    struct Saved {u32 address;std::vector<u8> bytes;};
    std::vector<Saved> saved_;
public:
    NativeHosePose(Ctx& c,u32 player,unsigned pass);
    ~NativeHosePose();
    NativeHosePose(const NativeHosePose&)=delete;
    NativeHosePose& operator=(const NativeHosePose&)=delete;
};
bool testDoorMovement(std::int8_t& x,std::int8_t& y);
void setEquipmentContext(std::uint64_t context);
bool toolModelReady(vr::GrabKind kind);
void drawOtherTool(Ctx& c,u32 source,bool transparent);
void aimGameBoyCamera(u32 camera);
bool toolAim(u32 owner,vr::ToolTarget& target,const vr::Vec3* sourceOrigin=nullptr,const vr::Vec3* sourceAxis=nullptr,vr::GrabKind kind=vr::GrabKind::None);
void aimSuction(u32 record);
u32 heldToolVector(u32 source,bool direction,u32 original);
bool toolVectorValid(u32 address);
bool captureDamageAllowed(u32 ghost);
void noteCapturedGhost(u32 ghost);
void notePickup(); // key-get particle: explicit native pickup notification
bool reactionPresentation(); // short native startle only; ends with the controller state
bool pickupPresentation(); // guest thread, bounded to actor and pose lifetime
vr::PlayerAnchor captureAnchor();
bool diagnosticEquipment(); // synthetic held tools/activation only for the isolated fixture
bool diagnosticEnabled(); // opt-in synthetic pose for automated flat-render checks
// Guest-thread draw-only skeleton override; original matrices restored after draw.
class DrawPose {
    u32 object_=0,joints_=0,views_=0,normals_=0;
    bool hidden_=false;
    std::vector<u8> saved_,savedViews_,savedNormals_;
    std::vector<std::pair<u32,u8>> materialFlags_;
    struct PacketRestore {u32 address,pointer,size;};
    std::vector<PacketRestore> packets_;
public:
    DrawPose(Ctx& context,u32 modelObject,bool shadowPass=false);
    ~DrawPose();
    bool hidden() const {return hidden_;}
    DrawPose(const DrawPose&)=delete;
    DrawPose& operator=(const DrawPose&)=delete;
};
}

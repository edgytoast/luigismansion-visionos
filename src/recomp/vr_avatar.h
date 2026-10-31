#pragma once
#include "recomp/vr_camera.h"
#include <span>
#include <vector>
#include <cstdint>
#include <string>
#include <string_view>
namespace recomp::vr {
using Vec3=std::array<float,3>;
struct ToolTarget {Vec3 origin{},direction{0,0,-1},up{0,1,0};bool tracked=false;};
ToolTarget controllerToolTarget(const Matrix& grip,const Matrix& aim,Vec3 actor,float heading,float eyeHeight,float eyeForward,float units,std::array<float,2> fit,float muzzleOffset=0.12f);
Matrix alignTool(Vec3 nativeOrigin,Vec3 nativeDirection,Vec3 nativeUp,const ToolTarget& target);

bool reflectedActorPass(unsigned flags); // native reflection prepass, including translucent actors
bool mirroredTransform(const Matrix& matrix); // reflected camera or model basis
Vec3 worldGaze(const Matrix& recenteredHead,float virtualHeading);
struct ArmPose { Vec3 shoulder{},elbow{},wrist{}; bool valid=false; };
ArmPose solveReflectedArm(Vec3 collar,Vec3 shoulder,Vec3 elbow,Vec3 wrist,Vec3 target,Vec3 bendHint);
Matrix rotationAboutPoint(Vec3 pivot,const Matrix& rotation);
ArmPose solveArm(Vec3 shoulder,Vec3 wrist,Vec3 bendHint,float upperLength,float lowerLength);
// Rotate the clavicle toward extended/raised hands, with a bounded shoulder shift.
Vec3 shoulderTarget(Vec3 root,Vec3 shoulder,Vec3 wrist,float armLength,float maxShift);
bool pickupPoseState(unsigned state,unsigned substate);
bool startlePoseState(unsigned state,unsigned substate,unsigned reactionKind);
Matrix avatarBodyAlignment(const Matrix& worldFromModel,Vec3 actorPosition,float actorYaw,float viewYaw);
Matrix equipmentGripFrame(const Matrix& grip,const Matrix& aim,unsigned hand,float outwardRadians,float palmReachMetres,float palmClearanceMetres=0);
Matrix calibratedHand(const Matrix& controller,unsigned hand,float outwardRadians);
Matrix calibratedHand(const Matrix& controller,unsigned hand,float outwardRadians,float rollRadians);
Vec3 closedFingerGripCentre(std::array<Matrix,3> chain,Vec3 backNormal);
Matrix equipmentContactFrame(const Matrix& grip,const Matrix& aim,unsigned hand,float outwardRadians,Vec3 contactMetres);
Matrix equipmentContactFrame(const Matrix& grip,const Matrix& aim,unsigned hand,float outwardRadians,Vec3 contactMetres,float rollRadians);
Matrix toolHoldingHand(const Matrix& target,Vec3 contact,unsigned hand);
Matrix reanchorGripInverse(const Matrix& worldToHand,Vec3 handleWorld);
Matrix equipmentMeshGripInverse(const Matrix& mesh,float handleY);
bool equipmentHandleProjection(const Matrix& mesh,Vec3 contact,float& handleY,float& residual);
class DoorPassage {
    unsigned entry_=255;float side_=0,clear_=0;
public:
    void opened(unsigned room,float signedDistance);
    bool step(unsigned room,unsigned otherRoom,float signedDistance,std::span<const float> hands,float seconds);
};
bool doorThresholdContactDepth(float sphereHeight,float radius,float floor,float& depth);
bool doorThresholdHeight(float floorA,float floorB,float placementHeight,float& height);
bool doorApertureAllows(const Matrix& placement,Vec3 center,Vec3 normal,Vec3 planePoint,Vec3 player);
bool retargetHose(std::span<const Vec3> native,const Matrix& packDelta,const Matrix& nozzleDelta,std::vector<Vec3>& result);
Matrix detachedForearmSkin(Vec3 bindElbow,Vec3 bindWrist,const Matrix& trackedWrist,float sleeveLength);
Matrix boneTransform(Vec3 oldStart,Vec3 oldEnd,Vec3 newStart,Vec3 newEnd);
Matrix limbTransform(Vec3 oldStart,Vec3 oldEnd,Vec3 oldNormal,Vec3 newStart,Vec3 newEnd,Vec3 newNormal);
Matrix handTransform(Vec3 wrist,Vec3 palm,Vec3 up,const Matrix& target);
enum class GrabKind { None,Flashlight,Key,Poltergust,Door,GameBoy };
struct GrabTarget {
    std::uint32_t id=0;
    GrabKind kind=GrabKind::None;
    Vec3 position{};
    float radius=0.15f;
    bool available=true;
};
struct HandInput { Pose pose; bool valid=false,occupied=false; float grip=0,trigger=0;
    std::array<float,5> fingerCurl{};std::uint8_t fingerValid=0; // thumb to little
};
std::array<float,5> toolGripCurls(const HandInput& input);
std::array<float,2> counterPullStick(Vec3 start,Vec3 current,float headYaw);
float opposedPullDistance(Vec3 delta,float heading,float escapeYaw,float seconds);
int captureHealthSource(int health,bool damageAllowed);
struct CaptureFocus {unsigned actor=0;float bearing=0,escape=0;};
class CapturePulls {
    struct Target {unsigned actor=0;Vec3 position{};float bearing=0,escape=0,stroke=0;double seen=0,damageUntil=0;};
    std::array<Target,16> targets_{};unsigned primary_=0;
public:
    void reset();
    void observe(unsigned actor,Vec3 player,Vec3 ghost,double now);
    CaptureFocus focus(double now) const;
    void step(Vec3 handDelta,float heading,float seconds,double now,bool active);
    bool allows(unsigned actor,double now) const;
};

float captureFollowStep(float currentYaw,float targetYaw,float seconds);
std::array<float,5> gloveCurls(const HandInput& input); // thumb, index, middle, ring, little
void curlFinger(std::span<Matrix> chain,Vec3 backNormal,float curl,bool thumb=false);
float jointCurl(std::span<const Vec3> positions,bool thumb=false);
unsigned capCuffRims(std::vector<std::uint8_t>& data,unsigned stride,const std::array<bool,10>& cuffs,std::span<const Vec3> positions,const std::array<Vec3,10>& axes,std::span<const std::uint8_t> source={});
bool filterSleeveTriangles(std::span<const std::uint8_t> data,unsigned stride,const std::array<bool,10>& sleeves,std::vector<std::uint8_t>& out);
struct GrabState {
    std::uint32_t target=0;
    GrabKind kind=GrabKind::None;
    bool grabbed=false,released=false,use=false;
    float twist=0; // signed wrist rotation in radians about initial grip forward
};
std::array<float,3> equipmentTriggers(const std::array<GrabState,2>& grabs,const std::array<HandInput,2>& hands); // element, vacuum, flashlight
// Input-only intent. Guest adapters must validate inventory, reach, room and locks.
class GrabController {
    GrabState state_;
    bool down_=false,armed_=true;
    Pose start_;
public:
    GrabState update(const HandInput& input,std::span<const GrabTarget> targets);
    void reset();
    void cancel(); // require release before a new grab after scene/focus changes
};
class InteractionHands {
    GrabController controllers_[2];
    std::array<GrabState,2> state_{};
public:
    std::array<GrabState,2> update(const std::array<HandInput,2>& hands,std::span<const GrabTarget> targets);
    void cancel();
};
// Physical-object intent only. The guest adapter owns lock/key validation,
// collision, animation progress and rewards; no action is synthesized as A.
bool ordinaryDoorEligible(unsigned type,unsigned flags,unsigned roomA,unsigned roomB,unsigned currentRoom);
bool ordinaryDoorClosable(unsigned type,unsigned flags,unsigned roomA,unsigned roomB,unsigned currentRoom);
unsigned ordinaryDoorOpenFlags(unsigned flags,bool reverse);
unsigned ordinaryDoorCloseFlags(unsigned flags);
enum class WorldAction { Door,Chest,Shake,KeyInsert };
struct WorldTarget {
    std::uint32_t id=0;WorldAction action=WorldAction::Door;
    Vec3 contact{},axis{0,1,0};float reach=0.12f;
    bool available=true,twoHands=false;
};
struct WorldIntent {std::uint32_t id=0;float amount=0;bool began=false,completed=false,cancelled=false;};
class WorldGesture {
    std::uint32_t id_=0;WorldAction action_=WorldAction::Door;
    std::array<Pose,2> start_{};std::array<bool,2> participants_{};
    Vec3 last_{};float distance_=0;unsigned reversals_=0;float lastDirection_=0;
    bool spent_=false,armed_=true;
public:
    WorldIntent update(const WorldTarget& target,const std::array<HandInput,2>& hands);
    void cancel();
};
enum GameButton : std::uint32_t { Confirm=1,Back=2,Pause=4,Map=8,Search=16,Status=32,HudPrevious=64,HudNext=128 };
// Preserve short render-frame button presses until the guest's next PADRead.
bool settingsPanelPointer(const Matrix& aim,float aspect,std::array<float,2>& uv);
bool bypassVacuumStartBlend(bool firstPerson,unsigned oldMode,unsigned newMode,unsigned controllerState,unsigned oldMotion,unsigned newMotion);
bool mergeNativeStick(std::int8_t& x,std::int8_t& y,float stickX,float stickY);
void routeControllerStick(std::int8_t& x,std::int8_t& y,float stickX,float stickY,bool headRelative,const std::array<float,4>& mapping);
class ButtonLatch {
    std::uint32_t held_=0,pending_=0,blocked_=0;
public:
    void sample(std::uint32_t buttons,bool focused);
    std::uint32_t consume();
};
// Recentered, upright body frame: metres, eye at origin, forward -Z.
std::array<GrabTarget,4> equipmentTargets();
std::array<GrabTarget,4> beltTargets(const Matrix& head,float yaw,bool flashlightLeft=true,bool poltergustLeft=false,bool gameBoyLeft=false);
bool idleKeyGhostLuigi(std::string& script);
bool handoffKeyGhostKeys(std::string& script);
bool hideEarlyRescueProfessor(std::string& script);
bool visibleSwitchSceneLuigi(std::string& script);
bool liveParlourEncounter(std::string& script); // edit only the verified candle encounter
std::array<GrabTarget,4> toolbarTargets(const Matrix& head,float yaw,bool poltergustLeft=false,float drop=0.50f,float reach=0.23f);
std::uint32_t hoverTarget(const HandInput& hand,std::span<const GrabTarget> targets);
}

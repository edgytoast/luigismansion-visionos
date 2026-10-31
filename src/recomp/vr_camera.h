#pragma once
#include <array>
#include <vector>
#include <string_view>

namespace recomp::vr {
// Right-handed coordinates, +Y up, looking down -Z. Distances from OpenXR are metres.
struct Pose {
    std::array<float, 4> orientation{0, 0, 0, 1}; // x, y, z, w
    std::array<float, 3> position{};
};
using Matrix = std::array<float, 16>; // row-major, column-vector multiplication
enum class ViewMode { Diorama, LifeSize, FirstPerson };
struct PlayerAnchor {
    std::array<float,3> position{};
    float yaw=0; // radians; local forward is -Z
    bool valid=false;
    unsigned actorId=0; // identifies gameplay or scripted Luigi for heading handoff
};
Matrix eyeFromPlayer(const Matrix& trackedEyeFromReference, const Matrix& authoredView, const PlayerAnchor& player, float unitsPerMetre, float eyeHeight, float eyeForward=0);
std::array<float,4> locomotionMapping(const Matrix& authoredView,float heading);
enum class Presentation { World, FlatPanel, StereoPanel };
struct SceneState {
    unsigned scene=0,map=0;
    bool cinematic=false,closeup=false,dialogue=false,pause=false,mapScreen=false;
    bool authoredRescue=false,conversationEvent=false,boundedDiorama=false,playerDemo=false,resultsScreen=false,resultsFlat=false;
    bool gameOver=false;
    bool storyGeometry=false;
    bool doorAnimation=false;
};
std::array<float,4> dialogueOverlayHud(std::array<float,4> mapping,float lift);
float remapOverlayCoordinate(float value,float projectionScale,float projectionOffset,float baseScale,float baseOffset,float targetScale,float targetOffset);
bool doorAnimationResource(std::string_view name);
// Actor construction can precede the first scripted DISPOFF. Do not treat
// that initial default visibility as the ending's actual character reveal.
struct EndingActorVisibility {
    unsigned actor=0;
    bool concealed=false;
    unsigned reveals=0;
    bool update(unsigned id,unsigned flags,unsigned requiredReveals=1) {
        if(actor!=id){actor=id;concealed=false;reveals=0;}
        if((flags&0xC000u)==0xC000u)concealed=true;
        if(concealed && !(flags&0xC000u)){concealed=false;++reveals;}
        return reveals>=requiredReveals;
    }
};
bool endingMarioHasEmerged(std::string_view action);
bool endingRetainedActor(std::string_view name,std::string_view action);
bool endingParkedActor(std::string_view name,std::string_view action);
// Visual handoff for the first key ghost: parked intro, live reaction,
// then an atomic scene-key/pickup-key visual handoff.
struct KeyGhostVisibility {
    unsigned firstWaitCursor=0;
    bool introDone=false,handoff=false;
    void update(bool active,unsigned cursor,unsigned waitState,bool handoffReady=false) {
        if(!active)return;
        if(!firstWaitCursor && waitState==4)firstWaitCursor=cursor;
        else if(firstWaitCursor && cursor>firstWaitCursor)introDone=true;
        handoff|=handoffReady;
    }
    bool hidePlayer(bool active) const {return active&&!introDone;}
    bool hidePickup(bool active) const {return active&&!handoff;}
};

bool processingMachineMap(unsigned map);
bool processingMachine(std::string_view name);
unsigned processingMachineDrawFlags(unsigned flags,bool preserveScriptedHide=false);
bool phoneDialogueEvent(unsigned event);
bool playerDemoNeedsPanel(bool callAnchored,bool activePhoneEvent);
bool boundedDioramaEvent(unsigned event, bool graveyardRevealed);
bool dioramaBossReturnEvent(unsigned event,unsigned map);
bool dioramaDialogueEvent(unsigned event); // retail dialogue without authored movie paths
Presentation presentationFor(const SceneState& scene,bool diorama=false,bool roomDrawn=false,bool expandStory=false);
struct CameraOptions {
    ViewMode mode = ViewMode::Diorama;
    float unitsPerMetre = 100.0f; // game scale; must be calibrated against Luigi/room geometry
    float miniatureScale = 0.10f;
    float viewingDistance = -0.25f; // signed setback from the scaled original camera, metres
    float verticalOffset = -0.20f; // diorama placement relative to eye level, metres
};
using Aperture = std::array<std::array<float,2>,4>;
// Original-camera viewing opening, projected separately for each tracked eye.
bool sceneAperture(const Matrix& transform,const Matrix& projection,float fx,float fy,float ox,float oy,float depth,Aperture& aperture);
std::array<std::vector<std::array<float,2>>,4> outsideAperture(const Aperture& aperture);
struct Fov { float left, right, up, down; }; // OpenXR tangent angles, radians
Matrix multiply(const Matrix& a, const Matrix& b);
bool invertAffine(const Matrix& matrix,Matrix& inverse);
Matrix poseMatrix(const Pose& pose);
Matrix inversePose(const Pose& pose);
Pose uprightRecenter(const Pose& head); // preserve physical up; reset position and heading
// Camera-space transform applied to GX world/model-view matrices; no guest simulation changes.
Matrix eyeFromGameCamera(const Pose& eye, const Pose& recenter, const CameraOptions& options);
// GX perspective convention: near -> NDC -1, far -> NDC 0, w = -z.
Matrix eyeFromCreditsCamera(const Pose& eye,const Pose& recenter,const CameraOptions& options,float focusDistance);
Matrix eyeFromScriptedCamera(const Pose& eye, const Pose& recenter, const CameraOptions& options, float focusDistance = 200.0f, bool forwardShot = false);
// Bound worst-case horizontal disparity over the authored perspective frustum.
float comfortableStereoEyeOffset(float nearDistance, float focusDistance, float focalScale);
float orthographicStereoEyeOffset(float focusDistance, float focalScale);
Matrix shadowReceiverForEye(const Matrix& receiver,const Matrix& inverseEye,bool viewSpace);
Matrix gxProjection(const Fov& fov, float nearMetres, float farMetres);
// Screen-layout approximation about a stationary HUD centre; keeps glyphs upright.
std::array<float, 4> hudMapping(const Matrix& projection, const Pose& eye, const Pose& origin);
float cinematicProjectionOffset(float focalScale, float originalOffset, float eyeOffset, float convergence = 200.0f);
}

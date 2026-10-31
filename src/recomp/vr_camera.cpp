#include "recomp/vr_camera.h"
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace recomp::vr {
std::array<float,4> dialogueOverlayHud(std::array<float,4> mapping,float lift) {
    mapping[3]+=mapping[1]*std::clamp(lift,0.f,0.08f)/0.45f;
    return mapping;
}
float remapOverlayCoordinate(float value,float projectionScale,float projectionOffset,float baseScale,float baseOffset,float targetScale,float targetOffset) {
    if(std::fabs(projectionScale)<1e-8f || std::fabs(baseScale)<1e-8f)return value;
    const float clip=projectionScale*value+projectionOffset;
    return ((targetScale*clip+targetOffset-baseOffset)/baseScale-projectionOffset)/projectionScale;
}
bool doorAnimationResource(std::string_view name) {
    if(const auto slash=name.find_last_of("/\\");slash!=std::string_view::npos)name.remove_prefix(slash+1);
    if(name.size()!=12 || (!name.ends_with(".scd")&&!name.ends_with(".sco")))return false;
    const auto family=name.substr(0,6);
    if(family!="codemo"&&family!="hodemo"&&family!="nodemo"&&family!="oodemo")return false;
    return name[6]>='0'&&name[6]<='1'&&name[7]>='0'&&name[7]<='9'&&
        (name[6]-'0')*10+name[7]-'0'>=1 && (name[6]-'0')*10+name[7]-'0'<=14;
}
bool endingMarioHasEmerged(std::string_view action) {
    return action=="EDHS" || action=="EDPM" || action=="EDPE";
}
bool endingRetainedActor(std::string_view name,std::string_view action) {
    return ((name=="dmario" || name=="dwaku") && endingMarioHasEmerged(action)) ||
        (name=="dluige02" && (action=="EDLL" || action=="EDLR"));
}
bool endingParkedActor(std::string_view name,std::string_view action) {
    return (name=="dmario" || name=="dwaku") && (action.empty() || action=="NORMAL");
}
bool processingMachineMap(unsigned map) { return map==4 || map==12; }
bool processingMachine(std::string_view name) {
    constexpr std::string_view machines[]{"kiarm","kiashuk","kifuta","kisentak","kitv","kiusiro","kibet",
        "edarm","edashuk","edfuta","edsentak","edtv","edusiro","edbet"};
    for(auto machine:machines)if(name==machine)return true;
    return false;
}
unsigned processingMachineDrawFlags(unsigned flags,bool preserveScriptedHide) {
    // Native actor renderer: 0x6000 skips drawing, 0x100 hides normal passes,
    // and 0x200 allows drawing regardless of the original room-visible flag.
    return (flags&~(preserveScriptedHide?0x2000u:0x6100u))|0x200u;
}
bool phoneDialogueEvent(unsigned event) {
    // Retail PDEMO resources gb_call01 / gb_endcall / gb_minigame1 identify
    // phone conversations, including their lead-in before call mode is raised.
    constexpr unsigned calls[]{9,10,11,12,15,29,42,44,54,69,70,80,88,91};
    for(unsigned id:calls)if(id==event)return true;
    return false;
}
bool playerDemoNeedsPanel(bool callAnchored,bool activePhoneEvent) {
    return !callAnchored && !activePhoneEvent;
}
bool boundedDioramaEvent(unsigned event,bool graveyardRevealed) {
    // Event 65 shares the first skull reveal and the later Bogmire intro.
    return event==7 || event==48 || event==52 || event==69 || event==71 || event==74 || event==75 || event==76 || event==84 || (event==65&&!graveyardRevealed);
}
bool dioramaBossReturnEvent(unsigned event,unsigned map) { return event==67 && map==11; }
bool dioramaDialogueEvent(unsigned event) {
    if(phoneDialogueEvent(event))return true;
    // Retail ordinary dialogue: SPEAK + WINDOW, no authored camera path.
    // Native door closeups are distinguished by their actual draw, not by
    // the archive ID cached from the last conversation.
    constexpr unsigned events[]{3,4,8,9,13,14,17,18,19,20,21,22,23,24,26,28,32,33,34,36,37,39,41,45,47,49,50,53,55,56,57,58,59,60,63,77,78,81,82,86,89,93,94};
    for(unsigned id:events)if(id==event)return true;
    return false;
}
Presentation presentationFor(const SceneState& state,bool diorama,bool roomDrawn,bool expandStory) {
    if(state.gameOver)return Presentation::FlatPanel;
    if(state.resultsScreen && state.resultsFlat)return Presentation::FlatPanel;
    if(state.mapScreen) return Presentation::StereoPanel;
    if(state.pause) return Presentation::FlatPanel;
    if(state.resultsScreen)return Presentation::StereoPanel;
    // The opening walk relies on authored camera bounds and masks. Preserve
    // its original composition on a framed stereo screen in diorama mode.
    if(diorama && state.scene==4)return Presentation::StereoPanel;
    // Staff-roll typography/photos are composed for the authored full frame,
    // not a room-scale camera. Keep the complete layout on a stereo reading
    // panel instead of placing its scrolling letters at stage depth.
    if(diorama && state.scene==5 && state.map==12)return Presentation::StereoPanel;
    // Door attempts/unlocks explicitly retain native framing with stereo depth.
    if(state.doorAnimation)return Presentation::StereoPanel;
    // General story rule, after explicit UI ownership. Cover every native
    // cinematic and item/door camera instead of maintaining a boss/event allowlist.
    // Real perspective geometry is required for separate opening/ending scenes;
    // pre-rendered movies have no second-eye geometry to reconstruct.
    if(expandStory && diorama &&
       ((state.scene==2 && (state.cinematic||state.closeup||state.playerDemo)) ||
        ((state.scene==0||state.scene==4||state.scene==5||state.scene==6) && state.storyGeometry) ||
        (state.scene==5 && state.map==12)))
        return Presentation::World;
    if(diorama && state.scene==2 && state.boundedDiorama && state.cinematic)return Presentation::World;
    // Dedicated door/player cameras may render without loading another event
    // archive. Their actual draw wins over a cached prior NPC classification.
    // Phone portraits are excluded at the draw marker; retain training's world.
    if(diorama && state.scene==2 && state.map!=3 && state.playerDemo)return Presentation::StereoPanel;
    if(diorama && state.scene==2 && state.conversationEvent)return Presentation::World;
    if(diorama && state.scene==2 && state.map!=3 && state.cinematic && state.authoredRescue)
        return Presentation::StereoPanel;
    // Authored wide camera paths rely on the native frustum to conceal the
    // room shell. Room geometry alone must not promote a tour to a free view.
    // Keep NPC inset/lead-in cameras and training in their surrounding room.
    if(diorama && roomDrawn && state.scene==2 && state.map!=3 &&
       state.cinematic && !state.closeup && !state.dialogue)
        return Presentation::StereoPanel;
    if(diorama && roomDrawn && state.scene==2)return Presentation::World;
    if(!state.dialogue && (state.closeup || (state.map!=3 && state.cinematic))) return Presentation::StereoPanel;
    return state.scene==2?Presentation::World:Presentation::FlatPanel;
}
namespace {
Matrix identity() { return {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; }
bool positive(float value) { return std::isfinite(value) && value > 0; }
}
Matrix shadowReceiverForEye(const Matrix& receiver,const Matrix& inverseEye,bool viewSpace) {
    // TEXMTX receives model-space positions; PT18/PT19 receive view-space
    // positions. Only the latter must undo the added VR camera transform.
    return viewSpace?multiply(receiver,inverseEye):receiver;
}
Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix result{};
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            for (int k = 0; k < 4; ++k) result[row*4+col] += a[row*4+k]*b[k*4+col];
    return result;
}
bool invertAffine(const Matrix& m,Matrix& out) {
    for(float v:m) if(!std::isfinite(v)) return false;
    const float a=m[0],b=m[1],c=m[2],d=m[4],e=m[5],f=m[6],g=m[8],h=m[9],i=m[10];
    const float determinant=a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g);
    if(!std::isfinite(determinant)||std::fabs(determinant)<1e-12f) return false;
    const float s=1/determinant;
    out={(e*i-f*h)*s,(c*h-b*i)*s,(b*f-c*e)*s,0,
         (f*g-d*i)*s,(a*i-c*g)*s,(c*d-a*f)*s,0,
         (d*h-e*g)*s,(b*g-a*h)*s,(a*e-b*d)*s,0,0,0,0,1};
    for(int r=0;r<3;++r) out[r*4+3]=-(out[r*4]*m[3]+out[r*4+1]*m[7]+out[r*4+2]*m[11]);
    return true;
}
Matrix poseMatrix(const Pose& pose) {
    float x=pose.orientation[0], y=pose.orientation[1], z=pose.orientation[2], w=pose.orientation[3];
    const float length = std::sqrt(x*x+y*y+z*z+w*w);
    if (!positive(length)) throw std::invalid_argument("Invalid VR orientation");
    x/=length; y/=length; z/=length; w/=length;
    for (float component : pose.position)
        if (!std::isfinite(component)) throw std::invalid_argument("Invalid VR position");
    return {1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w), pose.position[0],
            2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w), pose.position[1],
            2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y), pose.position[2],
            0,0,0,1};
}
Matrix inversePose(const Pose& pose) {
    const auto m = poseMatrix(pose);
    auto inverse = identity();
    for (int row=0; row<3; ++row) {
        for (int col=0; col<3; ++col) inverse[row*4+col] = m[col*4+row];
        inverse[row*4+3] = -(inverse[row*4]*m[3]+inverse[row*4+1]*m[7]+inverse[row*4+2]*m[11]);
    }
    return inverse;
}
Pose uprightRecenter(const Pose& head) {
    const auto rotation=poseMatrix(head);
    const float yaw=std::atan2(rotation[2],rotation[10]);
    Pose origin=head;
    origin.orientation={0,std::sin(yaw*0.5f),0,std::cos(yaw*0.5f)};
    return origin;
}
Matrix eyeFromGameCamera(const Pose& eye, const Pose& recenter, const CameraOptions& options) {
    if (!positive(options.unitsPerMetre) || !positive(options.miniatureScale) ||
        !std::isfinite(options.viewingDistance) || !std::isfinite(options.verticalOffset))
        throw std::invalid_argument("Invalid VR camera scale or distance");
    auto scene = identity();
    const float scale = (options.mode == ViewMode::Diorama ? options.miniatureScale : 1.0f) / options.unitsPerMetre;
    scene[0] = scene[5] = scene[10] = scale;
    if (options.mode == ViewMode::Diorama) {scene[11] = -options.viewingDistance;scene[7]=options.verticalOffset;}
    // eyeFromReference * referenceFromRecenter * recenterFromGameCamera
    return multiply(multiply(inversePose(eye), poseMatrix(recenter)), scene);
}
std::array<float,4> locomotionMapping(const Matrix& view,float heading) {
    const float right=std::hypot(view[0],view[2]),forward=std::hypot(view[8],view[10]);
    if(!positive(right)||!positive(forward)||!std::isfinite(heading)) return {1,0,0,1};
    const float c=std::cos(heading),s=std::sin(heading);
    return {(view[0]*c-view[2]*s)/right,(-view[0]*s-view[2]*c)/right,
            (-view[8]*c+view[10]*s)/forward,(view[8]*s+view[10]*c)/forward};
}
Matrix eyeFromPlayer(const Matrix& trackedEyeFromReference,const Matrix& authoredView,const PlayerAnchor& player,float unitsPerMetre,float eyeHeight,float eyeForward) {
    if(!player.valid || !positive(unitsPerMetre) || !positive(eyeHeight) || !std::isfinite(player.yaw) || !std::isfinite(eyeForward))
        throw std::invalid_argument("Invalid first person anchor");
    Pose body{{0,std::sin(player.yaw*0.5f),0,std::cos(player.yaw*0.5f)},player.position};
    body.position[1]+=eyeHeight*unitsPerMetre;
    body.position[0]-=std::sin(player.yaw)*eyeForward*unitsPerMetre;
    body.position[2]-=std::cos(player.yaw)*eyeForward*unitsPerMetre;
    // Authored model-view -> world -> Luigi's upright eye -> tracked headset.
    auto worldFromCamera=identity();
    for(int r=0;r<3;++r) {
        for(int c=0;c<3;++c) worldFromCamera[r*4+c]=authoredView[c*4+r];
        worldFromCamera[r*4+3]=-(worldFromCamera[r*4]*authoredView[3]+worldFromCamera[r*4+1]*authoredView[7]+worldFromCamera[r*4+2]*authoredView[11]);
    }
    return multiply(trackedEyeFromReference,multiply(inversePose(body),worldFromCamera));
}
Matrix eyeFromCreditsCamera(const Pose& eye,const Pose& recenter,const CameraOptions& options,float focusDistance) {
    auto framed=options;
    // Keep the authored camera outside the scene. Scale the whole credits stage
    // to reading distance instead of advancing through foreground text/photos.
    if(positive(focusDistance) && framed.mode==ViewMode::Diorama) {
        framed.miniatureScale=framed.unitsPerMetre*.9f/focusDistance;
        framed.viewingDistance=0;
    }
    framed.verticalOffset=-0.2f;
    return eyeFromGameCamera(eye,recenter,framed);
}
Matrix eyeFromScriptedCamera(const Pose& eye,const Pose& recenter,const CameraOptions& options,float focusDistance,bool forwardShot) {
    auto framed=options;
    if(forwardShot && framed.mode==ViewMode::Diorama){
        // Only the verified foyer tour/key-ghost events: stay just in front of
        // the authored camera rather than pulling the view behind its shell.
        framed.viewingDistance=-0.05f;
        return eyeFromGameCamera(eye,recenter,framed);
    }
    if(!positive(focusDistance)) return eyeFromGameCamera(eye,recenter,framed);
    const float scale=(framed.mode==ViewMode::Diorama?framed.miniatureScale:1.0f)/framed.unitsPerMetre;
    if(framed.mode==ViewMode::Diorama) {
        // Only move a shot back when its actor is closer than the comfortable
        // distance. Wide establishing shots retain their original placement.
        // Do not inherit gameplay's forward miniature offset in an authored
        // tour: it moves the viewer inside walls which the native camera clears.
        framed.viewingDistance=std::max({0.0f,framed.viewingDistance,1.05f-scale*focusDistance});
        return eyeFromGameCamera(eye,recenter,framed);
    }
    auto setback=identity();
    setback[11]=-std::max(0.0f,0.8f-scale*focusDistance)*framed.unitsPerMetre;
    return multiply(eyeFromGameCamera(eye,recenter,framed),setback);
}
bool sceneAperture(const Matrix& transform,const Matrix& projection,float fx,float fy,float ox,float oy,float depth,Aperture& aperture) {
    if(!positive(depth)||!std::isfinite(fx)||!std::isfinite(fy)||std::fabs(fx)<1e-6f||std::fabs(fy)<1e-6f)return false;
    const auto clipFromCamera=multiply(projection,transform);
    constexpr float corners[4][2]={{-0.90f,-0.90f},{0.90f,-0.90f},{0.90f,0.90f},{-0.90f,0.90f}};
    for(unsigned i=0;i<4;++i){
        // GX perspective clip x = fx*x + ox*z, w = -z.
        const float v[4]={(corners[i][0]+ox)*depth/fx,(corners[i][1]+oy)*depth/fy,-depth,1};
        float q[4]{};for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)q[r]+=clipFromCamera[r*4+c]*v[c];
        if(!std::isfinite(q[3])||q[3]<=0.001f)return false;
        for(unsigned k=0;k<2;++k){aperture[i][k]=q[k]/q[3];if(!std::isfinite(aperture[i][k]))return false;}
    }
    return true;
}
std::array<std::vector<std::array<float,2>>,4> outsideAperture(const Aperture& aperture) {
    std::array<std::vector<std::array<float,2>>,4> masks;
    float area=0;for(unsigned i=0;i<4;++i){const auto a=aperture[i],b=aperture[(i+1)%4];area+=a[0]*b[1]-a[1]*b[0];}
    if(!std::isfinite(area)||std::fabs(area)<1e-8f)return masks;
    for(unsigned edge=0;edge<4;++edge){
        const auto a=aperture[edge],b=aperture[(edge+1)%4];
        const auto side=[&](std::array<float,2> p){return ((b[0]-a[0])*(p[1]-a[1])-(b[1]-a[1])*(p[0]-a[0]))*(area>0?1.f:-1.f);};
        const Aperture screen{{{-1,-1},{1,-1},{1,1},{-1,1}}};
        auto prev=screen.back();float prevD=side(prev);
        for(const auto point:screen){const float d=side(point);
            if((d<0)!=(prevD<0)){const float t=prevD/(prevD-d);masks[edge].push_back({prev[0]+t*(point[0]-prev[0]),prev[1]+t*(point[1]-prev[1])});}
            if(d<0)masks[edge].push_back(point);
            prev=point;prevD=d;
        }
    }
    return masks;
}
float comfortableStereoEyeOffset(float nearDistance,float focusDistance,float focalScale) {
    if(!positive(nearDistance) || !positive(focusDistance) || !positive(std::fabs(focalScale)))
        throw std::invalid_argument("Invalid stereo comfort bounds");
    const float inverseDepth=std::max(std::fabs(1.0f/nearDistance-1.0f/focusDistance),1.0f/focusDistance);
    // At most 3% of image width between eyes, including the nearest clipped point.
    return std::min(focusDistance*0.016f,0.06f/(2.0f*std::fabs(focalScale)*inverseDepth));
}
float orthographicStereoEyeOffset(float focusDistance,float focalScale) {
    if(!positive(focusDistance) || !positive(std::fabs(focalScale)))
        throw std::invalid_argument("Invalid orthographic stereo bounds");
    return std::min(focusDistance*0.016f,0.03f/std::fabs(focalScale));
}
Matrix gxProjection(const Fov& fov, float nearMetres, float farMetres) {
    if (!positive(nearMetres) || !positive(farMetres) || farMetres <= nearMetres)
        throw std::invalid_argument("Invalid VR clipping planes");
    const float left=std::tan(fov.left), right=std::tan(fov.right);
    const float up=std::tan(fov.up), down=std::tan(fov.down);
    if (!std::isfinite(left) || !std::isfinite(right) || !std::isfinite(up) || !std::isfinite(down) ||
        right <= left || up <= down) throw std::invalid_argument("Invalid VR field of view");
    return {2/(right-left),0,(right+left)/(right-left),0,
            0,2/(up-down),(up+down)/(up-down),0,
            0,0,-nearMetres/(farMetres-nearMetres),-farMetres*nearMetres/(farMetres-nearMetres),
            0,0,-1,0};
}
std::array<float,4> hudMapping(const Matrix& projection,const Pose& eye,const Pose& origin) {
    // A stationary reference in the recentered room, independent of scene scale.
    // Keep glyph proportions fixed as the head rotates. The projected anchor
    // stays in the room, but independent perspective derivatives would stretch
    // width and height differently at oblique viewing angles.
    const auto view=multiply(inversePose(eye),poseMatrix(origin));
    constexpr float halfWidth=0.6f,halfHeight=0.45f,height=-0.12f,distance=1.6f;
    const float x=view[1]*height-view[2]*distance+view[3];
    const float y=view[5]*height-view[6]*distance+view[7];
    const float depth=-(view[9]*height-view[10]*distance+view[11]);
    if(depth<0.15f) return {0,0,4,4}; // anchor behind/too close to the eye: outside view
    const auto localEye=multiply(inversePose(origin),poseMatrix(eye));
    const float layoutDepth=std::max(0.15f,distance+localEye[11]);
    return {projection[0]*halfWidth/layoutDepth,
            projection[5]*halfHeight/layoutDepth,
            projection[0]*x/depth-projection[2],
            projection[5]*y/depth-projection[6]};
}
float cinematicProjectionOffset(float focalScale,float originalOffset,float eyeOffset,float convergence) {
    if(!positive(convergence) || !std::isfinite(focalScale) || !std::isfinite(originalOffset) || !std::isfinite(eyeOffset))
        throw std::invalid_argument("Invalid cinematic stereo projection");
    return originalOffset-focalScale*eyeOffset/convergence;
}
}

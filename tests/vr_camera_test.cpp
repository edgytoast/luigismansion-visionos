#include "recomp/vr_camera.h"
#include <cmath>
#include <cstdio>
#include <iostream>
#include <stdexcept>
using namespace recomp::vr;
void close(float actual, float expected) {
    if (std::fabs(actual-expected)>0.0001f) {std::cerr<<"VR camera mismatch: "<<actual<<" expected "<<expected<<" difference "<<actual-expected<<std::endl;throw std::runtime_error("VR camera numerical check failed");}
}
std::array<float,4> transform(const Matrix& m, float x, float y, float z) {
    std::array<float,4> out{};
    const float v[]{x,y,z,1};
    for(int r=0;r<4;++r) for(int c=0;c<4;++c) out[r]+=m[r*4+c]*v[c];
    return out;
}
int main() {
    for(unsigned map=0;map<=13;++map)
        if(processingMachineMap(map)!=(map==4||map==12))throw std::runtime_error("Processing visibility must cover normal and Mario-rescue lab maps only");
    for(auto family:{"codemo","hodemo","nodemo","oodemo"})for(unsigned n=1;n<=14;++n){
        char resource[100];std::snprintf(resource,sizeof(resource),"/Ajioka/ADemo/%s%02u.scd",family,n);
        if(!doorAnimationResource(resource))throw std::runtime_error("Missing door variant classification");
        SceneState door;door.scene=2;door.map=2;door.playerDemo=true;door.cinematic=true;door.closeup=true;door.doorAnimation=true;
        for(bool expand:{false,true})if(presentationFor(door,true,false,expand)!=Presentation::StereoPanel)
            throw std::runtime_error("Door scene lost framed stereo presentation");
    }
    for(auto resource:{"hodemo00.scd","hodemo15.scd","gbdemo01.scd","opcn.scd","nodemo01.szp","hodemo01.scd.extra"})
        if(doorAnimationResource(resource))throw std::runtime_error("Non-door resource marked as door");

    // Expanded policy is event-independent: all boss maps, scripted states and
    // item/door subviews remain World. Explicit UI owners always win.
    for(unsigned map=0;map<=13;++map)for(unsigned bits=1;bits<8;++bits)for(bool room:{false,true}) {
        SceneState state;state.scene=2;state.map=map;state.cinematic=bits&1;
        state.closeup=bits&2;state.playerDemo=bits&4;state.storyGeometry=true;
        if(presentationFor(state,true,room,true)!=Presentation::World)
            throw std::runtime_error("Uncovered story/boss/item state in expanded diorama policy");
        const auto baseline=presentationFor(state,true,room,false);
        if(presentationFor(state,false,room,true)!=presentationFor(state,false,room,false))
            throw std::runtime_error("Expanded diorama leaked into another VR mode");
        state.pause=true;
        if(presentationFor(state,true,room,true)!=Presentation::FlatPanel)throw std::runtime_error("Expanded story changed pause menu");
        state.pause=false;state.mapScreen=true;
        if(presentationFor(state,true,room,true)!=Presentation::StereoPanel)throw std::runtime_error("Expanded story changed map/search");
        state.mapScreen=false;state.resultsScreen=true;state.resultsFlat=true;
        if(presentationFor(state,true,room,true)!=Presentation::FlatPanel)throw std::runtime_error("Expanded story changed results");
        state.resultsScreen=false;state.gameOver=true;
        if(presentationFor(state,true,room,true)!=Presentation::FlatPanel)throw std::runtime_error("Expanded story changed Game Over");
        (void)baseline;
    }
    for(unsigned scene:{0u,5u,6u}) {
        SceneState state;state.scene=scene;state.storyGeometry=true;
        if(presentationFor(state,true,true,true)!=Presentation::World)throw std::runtime_error("Rendered separate story scene stayed flat");
        state.storyGeometry=false;
        if(presentationFor(state,true,false,true)!=presentationFor(state,true,false,false))throw std::runtime_error("Video-only scene expanded without geometry");
    }
    for(unsigned scene:{1u,3u,7u}) {
        SceneState state;state.scene=scene;state.storyGeometry=true;
        if(presentationFor(state,true,true,true)!=presentationFor(state,true,true,false))throw std::runtime_error("Title/file/movie screen presentation changed");
    }

    for(auto action:{"NORMAL","EDLM","EDLMKK",""})
        if(endingMarioHasEmerged(action))throw std::runtime_error("Parked/painting/peek phase exposes Mario or grate");
    for(auto action:{"EDHS","EDPM","EDPE"})
        if(!endingMarioHasEmerged(action))throw std::runtime_error("Mario emergence/reaction phase remains hidden");
    for(auto name:{"dmario","dwaku"}) {
        for(auto action:{"NORMAL",""})
            if(!endingParkedActor(name,action)||endingRetainedActor(name,action))
                throw std::runtime_error("Parked Mario/frame visible during machine peek");
        for(auto action:{"EDHS","EDPM","EDPE"})
            if(endingParkedActor(name,action)||!endingRetainedActor(name,action))
                throw std::runtime_error("Mario/frame disappear across reverse shots");
        if(endingParkedActor(name,"EDLM")||endingRetainedActor(name,"EDLM"))
            throw std::runtime_error("Painting-animation visibility must remain native");
    }
    if(endingParkedActor("edarm","NORMAL")||endingRetainedActor("namida","EDPM"))
        throw std::runtime_error("Ending frame fix changed unrelated actors");
    for(bool expanded:{false,true})for(bool geometry:{false,true})for(bool cinema:{false,true}) {
        SceneState opening;opening.scene=4;opening.storyGeometry=geometry;opening.cinematic=cinema;
        if(presentationFor(opening,true,false,expanded)!=Presentation::StereoPanel)
            throw std::runtime_error("Opening walk must keep authored framing with stereo depth");
        if(presentationFor(opening,false,false,expanded)!=presentationFor(opening,false,false,false))
            throw std::runtime_error("Opening policy changed another VR mode");
    }
    KeyGhostVisibility keyGhost;
    keyGhost.update(true,100,4);
    if(!keyGhost.hidePlayer(true)||!keyGhost.hidePickup(true))throw std::runtime_error("Key-ghost intro exposes parked player/pickup");
    keyGhost.update(true,100,4);
    if(!keyGhost.hidePlayer(true))throw std::runtime_error("Luigi restored before intro wait ends");
    keyGhost.update(true,120,4);
    if(keyGhost.hidePlayer(true)||!keyGhost.hidePickup(true))throw std::runtime_error("Ghost drop/flee needs Luigi but not duplicate pickup key");
    keyGhost.update(true,150,4,true);
    if(keyGhost.hidePickup(true))throw std::runtime_error("Native handoff did not expose pickup key");
    keyGhost.update(true,160,0);
    if(keyGhost.hidePickup(true)||keyGhost.hidePlayer(false))throw std::runtime_error("Key handoff regressed while camera returned");
    keyGhost={};
    if(keyGhost.hidePickup(false))throw std::runtime_error("Key cutaway state leaked into another event");
    EndingActorVisibility endingVisibility;
    if(endingVisibility.update(1,0))throw std::runtime_error("Construction visibility revealed ending actors early");
    if(endingVisibility.update(1,0xC000))throw std::runtime_error("Painting shot revealed hidden characters");
    if(!endingVisibility.update(1,0))throw std::runtime_error("Native ending reveal not recognized");
    if(!endingVisibility.update(1,0xC000))throw std::runtime_error("Reverse shot hid revealed ending actors");
    if(endingVisibility.update(2,0))throw std::runtime_error("Actor replacement inherited ending reveal");
    if(!(processingMachineDrawFlags(0x100,true)&0x100))throw std::runtime_error("Grate lost native normal-pass concealment");
    EndingActorVisibility marioVisibility;
    if(marioVisibility.update(1,0xC000,2)||marioVisibility.update(1,0,2))
        throw std::runtime_error("Mario retained before machine-peek concealment");
    if(marioVisibility.update(1,0xC000,2))throw std::runtime_error("Machine peek shows hidden Mario");
    if(!marioVisibility.update(1,0,2)||!marioVisibility.update(1,0xC000,2))
        throw std::runtime_error("Post-emergence reverse shots lose Mario");
    const auto hiddenLid=processingMachineDrawFlags(0xC000,true);
    if(!(hiddenLid&0x4000)||!(hiddenLid&0x8000)||(hiddenLid&0x2000))
        throw std::runtime_error("Animated grate lid lost native scripted concealment");
    for(auto name:{"kiarm","kiashuk","kifuta","kisentak","kitv","kiusiro","kibet","edarm","edashuk","edfuta","edsentak","edtv","edusiro","edbet"})
        if(!processingMachine(name))throw std::runtime_error("Processing machine missing");
    for(auto name:{"luige","dm_luigi","demo_hakase","vhead","kiarm_extra",""})
        if(processingMachine(name))throw std::runtime_error("Non-machine actor must retain native visibility");
    for(unsigned flags:{0u,0x6100u,0xFFFFu,0xFFFFFFFFu}){
        const auto visible=processingMachineDrawFlags(flags);
        if((visible&0x6100u)||!(visible&0x200u)||((visible^flags)&~0x6300u))
            throw std::runtime_error("Machine draw flags changed unrelated actor state");
        if(((visible&~0x6300u)|(flags&0x6300u))!=flags)
            throw std::runtime_error("Machine draw flag restoration failed");
    }
    for(bool diorama:{false,true})for(bool speech:{false,true}){
        if(presentationFor({2,4,true,false,speech,false,false,false,true,false,false,true,true},diorama,true)!=Presentation::FlatPanel)
            throw std::runtime_error("ACCOUNT results must use native flat layout despite room/dialogue");
        if(presentationFor({2,4,true,false,speech,false,true,false,true,false,false,true,true},diorama,true)!=Presentation::FlatPanel)
            throw std::runtime_error("ACCOUNT screen must override stale Game Boy mode");
    }

    for(bool diorama:{false,true})for(bool speech:{false,true}){
        if(presentationFor({2,4,true,false,speech,false,false,false,true,false,false,true},diorama,true)!=Presentation::StereoPanel)
            throw std::runtime_error("Area results must retain native screen framing despite stale room/dialogue");
        if(presentationFor({2,6,false,false,false,false,false,false,false,false,false,true},diorama,false)!=Presentation::StereoPanel)
            throw std::runtime_error("Portrait results must use screen presentation");
    }

    if(!boundedDioramaEvent(65,false)||boundedDioramaEvent(65,true))
        throw std::runtime_error("Graveyard skull reveal must stay diorama without changing the later boss intro");
    for(unsigned event:{3u,4u,8u,9u,17u,26u,28u,32u,37u,63u}){
        if(!dioramaDialogueEvent(event))throw std::runtime_error("Retail NPC dialogue event missing");
        if(presentationFor({2,2,true,false,false,false,false,false,dioramaDialogueEvent(event)},true,false)!=Presentation::World)
            throw std::runtime_error("NPC lead-in must stay in diorama before text/room appears");
    }
    for(unsigned event:{0u,1u,2u,7u,48u,61u,999u})
        if(dioramaDialogueEvent(event))throw std::runtime_error("Authored movie incorrectly classified as dialogue");

    for(unsigned event:{9u,10u,11u,12u,15u,29u,42u,44u,54u,69u,70u,80u,88u,91u}){
        if(!phoneDialogueEvent(event))throw std::runtime_error("Missing phone conversation");
        if(presentationFor({2,2,true,false,false,false,false,false,dioramaDialogueEvent(event)},true,false)!=Presentation::World)
            throw std::runtime_error("Phone lead-in before text must remain in diorama");
    }
    if(playerDemoNeedsPanel(false,true)||playerDemoNeedsPanel(true,false)||!playerDemoNeedsPanel(false,false))
        throw std::runtime_error("Phone/door renderer classification regression");
    for(bool speech:{false,true}){
        if(presentationFor({2,2,true,true,speech,false,false,false,true,false,true},true,false)!=Presentation::StereoPanel)
            throw std::runtime_error("Actual door camera must override stale NPC classification");
        if(presentationFor({2,2,true,true,speech,false,false,false,true,false,false},true,false)!=Presentation::World)
            throw std::runtime_error("Phone/dialogue inset must not become a door movie");
    }
    if(presentationFor({2,3,true,true,true,false,false,false,true,false,true},true,true)!=Presentation::World)
        throw std::runtime_error("Training must retain accepted world presentation");
    for(float yawOffset:{-0.8f,0.f,0.9f}) {
        const std::array<float,4> headMapping{0.7f,0.8f,yawOffset,0.1f};
        const auto speech=dialogueOverlayHud(headMapping,0.18f);
        close(speech[2],yawOffset);
        close(speech[3],0.1f+0.8f*0.08f/0.45f);
    }
    if(presentationFor({2,2,true,true,true,false,false,true},true,true)!=Presentation::StereoPanel)
        throw std::runtime_error("Rescue movie must retain authored framing even after dialogue");
    if(presentationFor({2,2,false,true,true,false,false,true},true,true)!=Presentation::World)
        throw std::runtime_error("Rescue identity must not affect free gameplay after the event");
    if(presentationFor({2,2,true,false,false,false,false,false,true},true,true)!=Presentation::World)
        throw std::runtime_error("Toad lead-in must remain in the diorama before any text appears");
    if(presentationFor({2,2,true,false,true,true,false,false,true},true,true)!=Presentation::FlatPanel)
        throw std::runtime_error("Conversation identity must not override pause menus");
    if(presentationFor({2,4,true,false,false,false,false,false,true},true,false)!=Presentation::World)
        throw std::runtime_error("Gallery/Toad handover must not require a room draw on every frame");
    if(presentationFor({2,2,true,false,false,false,false,false,false,true},true,true)!=Presentation::World)
        throw std::runtime_error("Selected tour/key shot must remain in tracked diorama");
    if(presentationFor({2,2,false,false,false,false,false,false,false,true},true,true)!=Presentation::World)
        throw std::runtime_error("Completed bounded event must return to normal gameplay");
    {
        const Matrix identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        const auto projection=gxProjection({-.78539816f,.78539816f,.78539816f,-.78539816f},.01f,100.f);
        Aperture aperture{};
        if(!sceneAperture(identity,projection,1,1,0,0,10,aperture))throw std::runtime_error("Valid authored aperture rejected");
        close(aperture[0][0],-.9f);close(aperture[0][1],-.9f);close(aperture[2][0],.9f);close(aperture[2][1],.9f);
        const auto masks=outsideAperture(aperture);
        for(const auto& mask:masks){
            if(mask.size()<3)throw std::runtime_error("Missing outside-frustum mask");
            for(auto p:mask)if(std::fabs(p[0])<.899f && std::fabs(p[1])<.899f)throw std::runtime_error("Mask covers the intended view");
        }
        auto shifted=identity;shifted[3]=1;
        Aperture otherEye{};sceneAperture(shifted,projection,1,1,0,0,10,otherEye);
        close(otherEye[0][0]-aperture[0][0],.1f); // boundary is stereo/world anchored
        if(sceneAperture(identity,projection,0,1,0,0,10,aperture))throw std::runtime_error("Degenerate aperture accepted");
        if(sceneAperture(identity,projection,1,1,0,0,-10,aperture))throw std::runtime_error("Behind-camera aperture accepted");
    }
    // A message uses its target anchor without modifying a shared health projection.
    for(float x:{0.f,100.f,640.f}){
        const float fx=2.f/640,tx=-1,base=.7f,baseOffset=.1f,target=.8f,targetOffset=-.2f;
        const float mapped=remapOverlayCoordinate(x,fx,tx,base,baseOffset,target,targetOffset);
        close(base*(fx*mapped+tx)+baseOffset,target*(fx*x+tx)+targetOffset);
        close(remapOverlayCoordinate(x,fx,tx,1,0,1,0),x);
    }
    const auto requirePresentation=[](SceneState state,Presentation expected) {
        if(presentationFor(state)!=expected) throw std::runtime_error("VR scene presentation regression");
    };
    if(presentationFor({2,2,true,true,false,false},true,true)!=Presentation::World)
        throw std::runtime_error("Diorama NPC lead-in must stay in the surrounding room");
    if(presentationFor({2,2,true,true,false,true},true,true)!=Presentation::FlatPanel)
        throw std::runtime_error("Diorama pause menu must retain its readable panel");
    if(presentationFor({4,2,true,false,false,false},true,false)!=Presentation::StereoPanel)
        throw std::runtime_error("Separate demo scenes must retain authored framing");
    if(presentationFor({2,2,true,false,false,false},true,true)!=Presentation::StereoPanel)
        throw std::runtime_error("Diorama authored room tour must retain the native frustum");
    if(presentationFor({2,2,false,false,false,false},true,true)!=Presentation::World)
        throw std::runtime_error("Diorama gameplay must remain in the room");
    if(presentationFor({2,2,true,false,true,false},true,true)!=Presentation::World)
        throw std::runtime_error("Diorama NPC dialogue must remain in the room");
    if(presentationFor({2,3,true,false,false,false},true,true)!=Presentation::World)
        throw std::runtime_error("Diorama training must remain in the room");
    requirePresentation({2,3,true,false,false,false},Presentation::World); // training remains a room
    requirePresentation({2,2,true,false,true,false},Presentation::World); // ordinary NPC dialogue
    requirePresentation({2,2,false,true,false,false},Presentation::StereoPanel); // dedicated door camera
    requirePresentation({2,2,false,true,false,true},Presentation::FlatPanel); // pause owns presentation
    requirePresentation({1,2,false,false,false,false},Presentation::FlatPanel); // title screen
    requirePresentation({2,2,true,false,false,false},Presentation::StereoPanel); // authored cinematic framing
    requirePresentation({4,2,true,false,false,false},Presentation::StereoPanel); // separate opening scene
    requirePresentation({2,2,false,false,false,true},Presentation::FlatPanel); // Game Boy Horror map menu
    requirePresentation({2,3,false,false,false,false},Presentation::World); // event handover with no prompt yet
    requirePresentation({2,2,true,true,true,false},Presentation::World); // dialogue portrait is an inset
    requirePresentation({2,3,false,true,false,false},Presentation::StereoPanel); // training transition closeup
    requirePresentation({2,2,false,false,false,true,true},Presentation::StereoPanel); // map depth with original framing
    for(const float nearDepth:{0.1f,1.0f,5.0f,40.0f}) {
        const float eye=comfortableStereoEyeOffset(nearDepth,200,2);
        for(const float depth:{nearDepth,200.0f,100000.0f}) {
            const float disparity=4*eye*std::fabs(1/depth-1/200.0f);
            if(disparity>0.06001f) throw std::runtime_error("Close-up stereo exceeds comfort bound");
        }
    }
    close(comfortableStereoEyeOffset(5,200,2)*10,comfortableStereoEyeOffset(50,2000,2)); // game-unit invariant
    const float mapEye=orthographicStereoEyeOffset(1000,0.005f);
    const auto mapX=[&](float eye,float z) {return 0.005f*(-eye-eye*z/1000);};
    close(mapX(mapEye,-1000),mapX(-mapEye,-1000));
    if(!(mapX(-mapEye,-500)>mapX(mapEye,-500))) throw std::runtime_error("Orthographic map has no stereo depth");
    CameraOptions scripted;
    const auto scriptedPoint=transform(eyeFromScriptedCamera({}, {},scripted,20),0,0,-20);
    if(scriptedPoint[2]>-1.04999f) throw std::runtime_error("Scripted camera too near the viewer");
    scripted.mode=ViewMode::LifeSize;
    close(transform(eyeFromScriptedCamera({}, {},scripted,20),0,0,-20)[2],-0.8f);
    scripted.mode=ViewMode::Diorama;
    const auto forwardShot=eyeFromScriptedCamera({}, {},scripted,2000,true);
    auto forwardOptions=scripted;forwardOptions.viewingDistance=-.05f;
    const auto expectedForward=eyeFromGameCamera({}, {},forwardOptions);
    for(unsigned k=0;k<16;++k)close(forwardShot[k],expectedForward[k]);
    const auto wideShot=eyeFromScriptedCamera({}, {},scripted,2000);
    auto authoredShotOptions=scripted;authoredShotOptions.viewingDistance=0;
    const auto regularShot=eyeFromGameCamera({}, {},authoredShotOptions);
    for(unsigned k=0;k<16;++k) close(wideShot[k],regularShot[k]);
    const auto cinemaX=[](float eye,float depth) {
        return (1.5f*(-eye)+cinematicProjectionOffset(1.5f,0.2f,eye)*(-depth))/depth;
    };
    close(cinemaX(-0.75f,200),-0.2f);close(cinemaX(0.75f,200),-0.2f);
    if(!(cinemaX(-0.75f,100)>cinemaX(0.75f,100)) || !(cinemaX(-0.75f,400)<cinemaX(0.75f,400)))
        throw std::runtime_error("Cinematic stereo depth ordering failed");
    const Pose tilted{{0.27059805f,0.65328148f,-0.27059805f,0.65328148f},{1,2,3}};
    const auto upright=poseMatrix(uprightRecenter(tilted));
    close(upright[4],0);close(upright[5],1);close(upright[6],0);
    close(upright[2],1);close(upright[10],0);
    close(upright[3],1);close(upright[7],2);close(upright[11],3);
    const float angle=0.7853981634f;
    const Fov asymmetric{-0.6f,0.8f,0.7f,-0.5f};
    const auto p=gxProjection(asymmetric,0.1f,100.0f);
    auto point=transform(p,0,0,-0.1f); close(point[2]/point[3],-1);
    point=transform(p,0,0,-100); close(point[2]/point[3],0);
    point=transform(p,std::tan(asymmetric.left)*2,0,-2); close(point[0]/point[3],-1);
    point=transform(p,std::tan(asymmetric.right)*2,0,-2); close(point[0]/point[3],1);
    point=transform(p,0,std::tan(asymmetric.up)*2,-2); close(point[1]/point[3],1);
    point=transform(p,0,std::tan(asymmetric.down)*2,-2); close(point[1]/point[3],-1);
    if(!boundedDioramaEvent(76,false) || !boundedDioramaEvent(76,true))
        throw std::runtime_error("Astral Hall candle camera left diorama presentation");
    if(!boundedDioramaEvent(71,false) || boundedDioramaEvent(72,false))
        throw std::runtime_error("Ring of Boos lead-in camera policy regression");
    if(!dioramaBossReturnEvent(67,11) || dioramaBossReturnEvent(67,10) || dioramaBossReturnEvent(72,11))
        throw std::runtime_error("Boolossus return policy must remain scoped to its defeat");
    if(!dioramaDialogueEvent(45))throw std::runtime_error("Breaker switch scene must keep its surrounding diorama");
    if(!boundedDioramaEvent(84,false))throw std::runtime_error("Basement water valve must retain diorama framing");
    if(!boundedDioramaEvent(74,false))throw std::runtime_error("King Boo approach must keep diorama from its lead-in");
    if(!boundedDioramaEvent(75,false))throw std::runtime_error("Bowser introduction must remain in diorama");
    SceneState gameOver;gameOver.scene=2;gameOver.map=9;gameOver.gameOver=true;
    gameOver.mapScreen=true;gameOver.cinematic=true;gameOver.boundedDiorama=true;
    if(presentationFor(gameOver,true,true)!=Presentation::FlatPanel)throw std::runtime_error("Game Over must override stereo/world views");
    if(!boundedDioramaEvent(52,false))throw std::runtime_error("Mario rescue needs stable scripted diorama framing");
    SceneState credits;credits.scene=5;credits.map=12;
    for(bool geometry:{false,true})for(bool expanded:{false,true}) {
        credits.storyGeometry=geometry;
        if(presentationFor(credits,true,false,expanded)!=Presentation::StereoPanel)
            throw std::runtime_error("Credits must retain readable authored framing and stereo depth");
    }
    if(presentationFor(credits,false,false,true)!=presentationFor(credits,false,false,false))
        throw std::runtime_error("Credits framing leaked into another VR mode");
    CameraOptions creditsOptions;creditsOptions.mode=ViewMode::Diorama;creditsOptions.miniatureScale=.5f;creditsOptions.unitsPerMetre=100;
    for(float focus:{200.f,1000.f,8000.f}) {
        const auto nearCredits=eyeFromCreditsCamera({}, {},creditsOptions,focus);
        const auto point=transform(nearCredits,0,0,-focus);
        close(point[2],-.9f);close(point[1],-.2f);
        const auto foreground=transform(nearCredits,0,0,-focus*.5f);
        if(foreground[2]>=0)throw std::runtime_error("Credits camera advanced through foreground text/photos");
    }
    Pose hudEye;hudEye.position={0.032f,0.004f,0};
    const auto hud=hudMapping(p,hudEye,{});
    // The two asymmetric eyes recover the same physical HUD centre and bounds.
    close((hud[2]+p[2])*1.6f/p[0]+0.032f,0);
    close((hud[3]+p[6])*1.6f/p[5]+0.004f,-0.12f);
    close((hud[0]+hud[2]+p[2])*1.6f/p[0]+0.032f,0.6f);
    Pose turned;turned.orientation={0,std::sin(0.2f),0,std::cos(0.2f)};
    const auto fixedHud=hudMapping(p,turned,{});
    const auto fixedPoint=transform(multiply(p,inversePose(turned)),0,-0.12f,-1.6f);
    close(fixedHud[2],fixedPoint[0]/fixedPoint[3]);
    close(fixedHud[3],fixedPoint[1]/fixedPoint[3]);
    if(std::fabs(fixedHud[2]-hud[2])<0.1f) throw std::runtime_error("HUD followed head rotation");
    // Turning in place must not stretch or resize either HUD axis.
    const auto neutralHud=hudMapping(p,{},{});
    for(const Pose orientation : {turned, Pose{{std::sin(0.25f),0,0,std::cos(0.25f)},{}},
                                  Pose{{0,0,std::sin(0.3f),std::cos(0.3f)},{}}}) {
        const auto rotatedHud=hudMapping(p,orientation,{});
        close(rotatedHud[0],neutralHud[0]);close(rotatedHud[1],neutralHud[1]);
    }
    const auto resetHud=hudMapping(p,turned,turned);
    close(resetHud[2],-p[2]); // recenter restores the stationary anchor in front
    Pose backwards;backwards.orientation={0,1,0,0};
    close(hudMapping(p,backwards,{})[2],4); // no mirrored UI behind the head
    Pose center{{0,std::sin(angle),0,std::cos(angle)},{3,1,2}};
    CameraOptions options; options.mode=ViewMode::LifeSize;
    auto m=eyeFromGameCamera(center,center,options);
    point=transform(m,0,0,-200); close(point[2],-2); close(point[0],0);
    Pose left, right; left.position[0]=-0.032f; right.position[0]=0.032f;
    const auto pl=gxProjection({-angle,angle,angle,-angle},0.1f,100);
    const auto a=transform(multiply(pl,eyeFromGameCamera(left,{},options)),0,0,-200);
    const auto b=transform(multiply(pl,eyeFromGameCamera(right,{},options)),0,0,-200);
    close(a[0]/a[3]-b[0]/b[3],0.032f); // physical 64 mm IPD, target two metres away
    options.mode=ViewMode::Diorama;
    options.miniatureScale=0.05f;options.viewingDistance=0.8f;
    point=transform(eyeFromGameCamera({}, {}, options),0,0,-200);
    close(point[2],-0.9f); // 2 m scene shrunk to 10 cm, placed 80 cm away
    close(point[1],-0.2f); // miniature sits below the recentered eye line
    // First-person world recovery is independent of the original camera.
    CameraOptions fp;fp.mode=ViewMode::FirstPerson;
    Pose authoredPose{{0,std::sin(0.3f),0,std::cos(0.3f)},{300,150,700}};
    const auto authored=inversePose(authoredPose);
    PlayerAnchor anchor{{100,20,200},0,true};
    auto first=eyeFromPlayer(eyeFromGameCamera({}, {},fp),authored,anchor,100,1.35f);
    auto worldPoint=transform(authored,100,155,0);
    point=transform(first,worldPoint[0],worldPoint[1],worldPoint[2]);
    close(point[0],0);close(point[1],0);close(point[2],-2);
    Pose lean;lean.position={0.1f,0.05f,0};
    first=eyeFromPlayer(eyeFromGameCamera(lean,{},fp),authored,anchor,100,1.35f);
    point=transform(first,worldPoint[0],worldPoint[1],worldPoint[2]);
    close(point[0],-0.1f);close(point[1],-0.05f);
    anchor.yaw=1.5707963268f;
    worldPoint=transform(authored,-100,155,200);
    first=eyeFromPlayer(eyeFromGameCamera({}, {},fp),authored,anchor,100,1.35f);
    point=transform(first,worldPoint[0],worldPoint[1],worldPoint[2]);
    close(point[0],0);close(point[2],-2);
    // A 12 cm setback changes position only; preserve perspective/linear scale.
    anchor.yaw=0;
    const auto nearEye=eyeFromPlayer(eyeFromGameCamera({}, {},fp),authored,anchor,100,1.35f,0.20f);
    const auto setBack=eyeFromPlayer(eyeFromGameCamera({}, {},fp),authored,anchor,100,1.35f,0.08f);
    for(int r=0;r<3;++r)for(int c=0;c<3;++c)close(nearEye[r*4+c],setBack[r*4+c]);
    worldPoint=transform(authored,100,155,0);
    const auto nearPoint=transform(nearEye,worldPoint[0],worldPoint[1],worldPoint[2]);
    const auto backPoint=transform(setBack,worldPoint[0],worldPoint[1],worldPoint[2]);
    close(backPoint[2]-nearPoint[2],-0.12f);
    const Matrix identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    // Model-space shadow lookups stay fixed. Post-transform lookups cancel the eye.
    const auto receiver=poseMatrix({{0,0.2f,0,std::sqrt(0.96f)},{0.2f,0.3f,0.4f}});
    for(float yaw:{0.0f,0.8f,-1.4f}) {
        const auto eye=poseMatrix({{0,std::sin(yaw/2),0,std::cos(yaw/2)},{-0.03f,0.1f,0.2f}});
        Matrix inverse; if(!invertAffine(eye,inverse))throw std::runtime_error("eye inverse");
        const auto modelReceiver=shadowReceiverForEye(receiver,inverse,false);
        const auto postReceiver=shadowReceiverForEye(receiver,inverse,true);
        for(unsigned k=0;k<16;++k)close(modelReceiver[k],receiver[k]);
        const auto viewPoint=transform(eye,1,2,-3);
        const auto projected=transform(postReceiver,viewPoint[0],viewPoint[1],viewPoint[2]);
        const auto expected=transform(receiver,1,2,-3);
        for(unsigned k=0;k<4;++k)close(projected[k],expected[k]);
    }
    auto movement=locomotionMapping(identity,0);
    close(movement[0],1);close(movement[1],0);close(movement[2],0);close(movement[3],1);
    movement=locomotionMapping(identity,1.5707963268f);
    close(movement[1],-1);close(movement[2],1); // turn left: forward becomes camera-left
    movement=locomotionMapping(authored,0.6f);
    close(movement[0],1);close(movement[3],1); // authored camera's yaw cancels
    Matrix scalePose=poseMatrix({{0,std::sin(0.2f),0,std::cos(0.2f)},{200,20,-500}});
    for(int r=0;r<3;++r) {scalePose[r*4]*=2;scalePose[r*4+1]*=3;scalePose[r*4+2]*=0.5f;}
    Matrix affineInverse; if(!invertAffine(scalePose,affineInverse)) return 1;
    const auto recovered=multiply(affineInverse,scalePose);
    for(int k=0;k<16;++k) close(recovered[k],identity[k]);
    auto zero=identity;zero[0]=0;if(invertAffine(zero,affineInverse)) return 1;
    // Recover model-space tracking targets when the actor is translated/rotated/scaled.
    const auto rootJoint=poseMatrix({{0,0,std::sin(0.1f),std::cos(0.1f)},{10,80,0}});
    Matrix jointInverse; if(!invertAffine(rootJoint,jointInverse)) return 1;
    const auto storedView=multiply(multiply(authored,scalePose),rootJoint);
    const auto recoveredModelView=multiply(storedView,jointInverse);
    const auto recoveredOwner=multiply(poseMatrix(authoredPose),recoveredModelView);
    Matrix ownerInverse;if(!invertAffine(recoveredOwner,ownerInverse)) return 1;
    const auto wristWorld=transform(scalePose,25,140,35);
    const auto wristLocal=transform(ownerInverse,wristWorld[0],wristWorld[1],wristWorld[2]);
    close(wristLocal[0]/100,0.25f);close(wristLocal[1]/100,1.40f);close(wristLocal[2]/100,0.35f);
    bool rejected=false;
    try { gxProjection(asymmetric,0,100); } catch(const std::invalid_argument&) { rejected=true; }
    if(!rejected) return 1;
    std::cout << "VR projection, recenter, physical stereo disparity and scale checks passed\n";
}

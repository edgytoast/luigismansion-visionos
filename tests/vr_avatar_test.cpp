#include "recomp/vr_avatar.h"
#include <cmath>
#include <limits>
#include <stdexcept>
#include <iostream>
#include <fstream>
#include <iterator>
#include <source_location>
using namespace recomp::vr;
void require(bool value,const std::source_location location=std::source_location::current()) {if(!value){std::cerr<<"VR avatar behavior failed at line "<<location.line()<<std::endl;throw std::runtime_error("VR avatar behavior failed");}}
float distance(Vec3 a,Vec3 b){float d=0;for(int i=0;i<3;++i)d+=(a[i]-b[i])*(a[i]-b[i]);return std::sqrt(d);}
int main(int argc,char** argv) {
    if(argc>1 && std::string_view(argv[1])=="--switch-stdin") {
        std::string script{std::istreambuf_iterator<char>(std::cin),{}};
        require(visibleSwitchSceneLuigi(script));const auto once=script;
        require(visibleSwitchSceneLuigi(script)&&script==once);
        require(script.find("<DISPOFF>")==std::string::npos);
        require(script.find("<WAIT>(430)")!=std::string::npos);
        std::cout<<"Native switch scene visibility validated.\n";return 0;
    }
    {
        std::string script="<GCAMERA>\"rom14cam.cmn\"\n<ACTOR>\"demo_luigi\"<DISPOFF>\n<WAIT>(430)";
        require(visibleSwitchSceneLuigi(script));const auto once=script;
        require(script.find("<DISPOFF>")==std::string::npos && visibleSwitchSceneLuigi(script)&&script==once);
        std::string other="<ACTOR>\"demo_luigi\"<DISPOFF>";const auto original=other;
        require(!visibleSwitchSceneLuigi(other)&&other==original);
    }
    if(argc>1 && std::string_view(argv[1])=="--rescue-stdin") {
        std::string script{std::istreambuf_iterator<char>(std::cin),{}};
        require(hideEarlyRescueProfessor(script));const auto once=script;
        require(hideEarlyRescueProfessor(script)&&script==once);
        const auto hidden=script.find("<ACTOR>\"demo_hakase\"<ALPHA>(0)");
        const auto shown=script.find("<ACTOR>\"demo_hakase\"<ALPHA>(255)");
        require(hidden<shown && shown<script.find("<ACTOR>\"demo_hakase\"<SETCLEANER>"));
        std::cout<<"Native rescue visibility adaptation accepted.\n";return 0;
    }
    if(argc>1 && std::string_view(argv[1])=="--key-ghost-stdin") {
        std::string script{std::istreambuf_iterator<char>(std::cin),{}};
        require(idleKeyGhostLuigi(script)&&handoffKeyGhostKeys(script));
        require(script.find("<ACT>\"WAIT0\"")!=std::string::npos);
        const auto once=script;require(idleKeyGhostLuigi(script)&&handoffKeyGhostKeys(script)&&script==once);
        std::cout<<"Native key-ghost script accepted; standing command inserted once.\n";return 0;
    }
    {
        std::string script="<ACTOR>\"demo_luigi\"<STOP>\n<GCAMERA>\"ed07cam.cmn\"\n<ACTOR>\"demo_key\"<PATH>\"drop\"";
        require(idleKeyGhostLuigi(script));
        require(script.find("<ACT>\"WAIT0\"")!=std::string::npos);
        const auto once=script;require(idleKeyGhostLuigi(script)&&script==once);
        std::string unrelated="<ACTOR>\"demo_luigi\"<STOP>";const auto original=unrelated;
        require(!idleKeyGhostLuigi(unrelated)&&unrelated==original);
    }

    {
        std::string script="ed07cam.cmn\n<ACTOR>\"demo_key\"<ALPHA>(255)\n<WAIT>(145)\n<ACTOR>\"demo_key2\"<ALPHA>(255)\n<ACTOR>\"demo_key\"<ALPHA>(255)";
        require(handoffKeyGhostKeys(script));
        const auto marker=script.find("<ACTOR>\"demo_key2\"<ALPHA>(255)");
        require(script.find("<ACTOR>\"demo_key\"<ALPHA>(0)")<marker);
        require(script.find("<ACTOR>\"demo_key\"<ALPHA>(255)")<marker);
        require(script.find("<WAIT>(144)")!=std::string::npos);
        const auto once=script;require(handoffKeyGhostKeys(script)&&script==once);
    }
    for(int i=1;i<argc;++i) {
        std::ifstream file(argv[i],std::ios::binary);require(bool(file));
        std::string script{std::istreambuf_iterator<char>(file),{}},source=script;
        require(liveParlourEncounter(script));require(script.size()==source.size());
        for(const auto tag:{"<WAIT>","<SPEAK>","<FLAGON>","<FLAGOFF>","<GPATHLIGHT>","<END>"}) {
            size_t at=0;while((at=source.find(tag,at))!=std::string::npos) {const auto end=source.find('\n',at);require(script.substr(at,end-at)==source.substr(at,end-at));at+=std::char_traits<char>::length(tag);}
        }
        std::cout<<"Verified owned-disc encounter script: "<<argv[i]<<"\n";
    }
    require(startlePoseState(2,3,5));
    require(!startlePoseState(1,0,5));require(!startlePoseState(2,3,0));
    require(!startlePoseState(2,3,4));require(!startlePoseState(2,5,5));
    require(!startlePoseState(2,0x21,5));require(!startlePoseState(2,0x22,5));
    // Native mirror rendering is a prepass, not necessarily a reflected basis.
    for(unsigned flags:{0x10000u,0x10100u,0x10010u,0x10110u})require(reflectedActorPass(flags));
    for(unsigned flags:{0u,2u,0x10u,0x110u,0x30u,0x130u})require(!reflectedActorPass(flags));
    // Reflections may live in either camera or owner, with tiny/nonuniform scale.
    // Ordinary rotations and two-axis flips must remain local-eye candidates.
    for(float yaw:{-2.4f,0.0f,1.7f})for(float scale:{0.001f,1.0f,100.0f}) {
        auto m=poseMatrix({{0,std::sin(yaw/2),0,std::cos(yaw/2)},{20,-40,10}});
        for(unsigned row=0;row<3;++row)for(unsigned col=0;col<3;++col)m[row*4+col]*=scale*(col+1);
        require(!mirroredTransform(m));
        for(unsigned row=0;row<3;++row)m[row*4]*=-1;
        require(mirroredTransform(m));
        for(unsigned row=0;row<3;++row)m[row*4+2]*=-1;
        require(!mirroredTransform(m));
    }
    auto invalidBasis=poseMatrix({});invalidBasis[0]=0;require(!mirroredTransform(invalidBasis));
    invalidBasis[0]=std::numeric_limits<float>::quiet_NaN();require(!mirroredTransform(invalidBasis));

    for(Vec3 target:{Vec3{0,0,0},Vec3{0,0,-0.3f},Vec3{5,1,-9},Vec3{0,1,0}}) {
        const auto arm=solveArm({},target,{0,-1,0},0.3f,0.25f);
        require(arm.valid);require(std::fabs(distance(arm.shoulder,arm.elbow)-0.3f)<0.0001f);
        require(std::fabs(distance(arm.elbow,arm.wrist)-0.25f)<0.0001f);
    }
    const Vec3 root{0,1.35f,0},shoulder{0.14f,1.30f,0};
    require(distance(shoulderTarget(root,shoulder,{0.14f,1.1f,0},0.44f,0.065f),shoulder)<0.00001f);
    for(Vec3 target:{Vec3{0.5f,1.9f,-0.3f},Vec3{0.14f,1.8f,0},Vec3{3,0,-3}}) {
        const auto moved=shoulderTarget(root,shoulder,target,0.44f,0.065f);
        require(distance(moved,shoulder)>0.001f && distance(moved,shoulder)<=0.06501f);
        require(std::fabs(distance(root,moved)-distance(root,shoulder))<0.0001f);
        const auto arm=solveArm(moved,target,{1,-0.6f,0.2f},0.227f,0.214f);
        require(arm.valid && std::fabs(distance(arm.shoulder,arm.elbow)-0.227f)<0.0001f);
    }
    require(!solveArm({}, {},{},0,0.3f).valid);
    const auto point=[](const Matrix& m,Vec3 v) {Vec3 out{};for(int r=0;r<3;++r)out[r]=m[r*4]*v[0]+m[r*4+1]*v[1]+m[r*4+2]*v[2]+m[r*4+3];return out;};
    for(Vec3 endpoint:{Vec3{0,0,-1},Vec3{0,1,0},Vec3{-1,0,0},Vec3{1,0,0}}) {
        const auto bone=boneTransform({}, {1,0,0},{2,3,4},{2+endpoint[0],3+endpoint[1],4+endpoint[2]});
        require(distance(point(bone,{}),{2,3,4})<0.0001f);
        require(distance(point(bone,{1,0,0}),{2+endpoint[0],3+endpoint[1],4+endpoint[2]})<0.0001f);
    }
    // Reflected arms recruit the clavicle and remain connected, with bounded
    // stretch at extreme tracked reaches. Normal reaches keep the requested wrist.
    for(float side:{-1.0f,1.0f})for(Vec3 target:{Vec3{0.25f,1.1f,-0.25f},Vec3{0.3f,1.65f,-0.1f},Vec3{4,3,-5}}) {
        target[0]*=side;
        const Vec3 collar{0,1.35f,0},shoulder{side*0.14f,1.3f,0},elbow{side*0.367f,1.3f,0},wrist{side*0.581f,1.3f,0};
        const auto arm=solveReflectedArm(collar,shoulder,elbow,wrist,target,{side,-0.7f,0.25f});
        require(arm.valid);
        require(distance(arm.shoulder,shoulder)<=0.0662f);
        require(std::fabs(distance(arm.shoulder,collar)-distance(shoulder,collar))<0.0001f);
        require(distance(arm.shoulder,arm.elbow)<=0.227f*1.4001f);
        require(distance(arm.elbow,arm.wrist)<=0.214f*1.4001f);
        if(distance(target,arm.shoulder)<0.441f*1.4f-0.0001f)require(distance(arm.wrist,target)<0.0001f);
    }
    // Head pitch/roll rotates skull points about the neck without moving that
    // pivot or changing head size; inverse rotation restores every point.
    for(float pitch:{-0.6f,0.0f,0.6f}) {
        const Vec3 neck{0,148,0},nose{0,182,20};
        const auto tilt=poseMatrix({{std::sin(pitch/2),0,0,std::cos(pitch/2)}, {}});
        const auto delta=rotationAboutPoint(neck,tilt);
        require(distance(point(delta,neck),neck)<0.0001f);
        require(std::fabs(distance(point(delta,nose),neck)-distance(nose,neck))<0.0001f);
        Matrix inverse;require(invertAffine(delta,inverse));
        require(distance(point(inverse,point(delta,nose)),nose)<0.0001f);
    }
    // Controller aim translates/rotates independently of the headset; metre
    // offsets use the same eye anchor and hand fit as the accepted gloves.
    for(float yaw:{-1.4f,0.0f,2.0f})for(float pitch:{-0.5f,0.0f,0.6f}) {
        const auto grip=poseMatrix({{std::sin(pitch/2),0,0,std::cos(pitch/2)},{0.25f,-0.4f,-0.3f}});
        const auto tool=controllerToolTarget(grip,grip,{10,20,30},yaw,1.8f,0.08f,100,{0.02f,0.03f});
        require(tool.tracked);require(std::fabs(distance(tool.direction,{})-1)<0.00001f);
        const auto expected=worldGaze(grip,yaw);require(distance(tool.direction,expected)<0.00001f);
        const auto noMuzzle=controllerToolTarget(grip,grip,{10,20,30},yaw,1.8f,0.08f,100,{0.02f,0.03f},0);
        require(std::fabs(distance(tool.origin,noMuzzle.origin)-12)<0.0001f);
        const auto transform=alignTool({4,5,6},{0,0,3},{0,1,0},tool);
        require(distance(point(transform,{4,5,6}),tool.origin)<0.0001f);
        const auto tip=point(transform,{4,5,7});
        require(distance({tip[0]-tool.origin[0],tip[1]-tool.origin[1],tip[2]-tool.origin[2]},tool.direction)<0.0001f);
        const auto up=point(transform,{4,6,6});
        require(distance({up[0]-tool.origin[0],up[1]-tool.origin[1],up[2]-tool.origin[2]},tool.up)<0.0001f);
    }
    auto invalidTool=poseMatrix({});invalidTool[3]=std::numeric_limits<float>::quiet_NaN();
    require(!controllerToolTarget(invalidTool,poseMatrix({}),{},0,1.8f,0.08f,100,{0,0}).tracked);
    require(!controllerToolTarget(poseMatrix({}),poseMatrix({}),{},0,1.8f,0.08f,0,{0,0}).tracked);
    // The elbow plane controls sleeve roll even when the bone direction is unchanged.
    const auto sleeve=limbTransform({}, {1,0,0},{0,0,1},{2,3,4},{3,3,4},{0,1,0});
    require(distance(point(sleeve,{1,0,0}),{3,3,4})<0.0001f);
    require(distance(point(sleeve,{0,0,1}),{2,4,4})<0.0001f);
    // Pickup display spans both native phases; NPC/door/ordinary control do not select it.
    require(!pickupPoseState(1,0));require(pickupPoseState(2,0x21));require(pickupPoseState(2,0x22));
    require(!pickupPoseState(2,0x20));require(!pickupPoseState(2,4));require(!pickupPoseState(1,0x21));
    // The mesh shoulder frame must face the VR heading, regardless of game turning.
    for(float ownerYaw:{0.0f,0.7f,-2.0f}) for(float gameYaw:{-2.0f,0.0f,1.5f}) for(float viewYaw:{-1.0f,0.0f,2.8f}) {
        const auto owner=poseMatrix({{0,std::sin(ownerYaw/2),0,std::cos(ownerYaw/2)},{32,0,44}});
        Matrix ownerInverse;require(invertAffine(owner,ownerInverse));
        const Vec3 actor{120,0,-55};
        const auto gameBody=poseMatrix({{0,std::sin(gameYaw/2),0,std::cos(gameYaw/2)},actor});
        const auto viewBody=poseMatrix({{0,std::sin(viewYaw/2),0,std::cos(viewYaw/2)},actor});
        const auto aligned=multiply(owner,multiply(avatarBodyAlignment(owner,actor,gameYaw,viewYaw),multiply(ownerInverse,gameBody)));
        for(Vec3 shoulder:{Vec3{-14,130,0},Vec3{14,130,0}})
            require(distance(point(aligned,shoulder),point(viewBody,shoulder))<0.0001f);
    }
    // Look aim keeps physical pitch across virtual snap turns, ignores translation.
    for(float pitch:{-0.7f,0.0f,0.8f})for(float yaw:{-1.2f,0.0f,2.0f}) {
        const auto head=poseMatrix({{std::sin(pitch/2),0,0,std::cos(pitch/2)},{4,5,6}});
        const auto gaze=worldGaze(head,yaw);
        require(distance(gaze,{-std::sin(yaw)*std::cos(pitch),std::sin(pitch),-std::cos(yaw)*std::cos(pitch)})<0.00001f);
        require(std::fabs(distance(gaze,{})-1)<0.00001f);
    }
    // Separate equipment model views must remain attached in a rotated camera.
    for(float yaw:{-2.0f,0.0f,1.4f}) {
        const auto camera=poseMatrix({{0,std::sin(0.35f),0,std::cos(0.35f)},{10,20,30}});
        Matrix inverse;require(invertAffine(camera,inverse));
        const Vec3 actor{120,10,-55},pack{120,140,-35};
        const auto delta=avatarBodyAlignment(poseMatrix({}),actor,0,yaw);
        const auto viewDelta=multiply(camera,multiply(delta,inverse));
        require(distance(point(viewDelta,point(camera,pack)),point(camera,point(delta,pack)))<0.0002f);
        require(distance(point(delta,actor),actor)<0.0001f);
        require(std::fabs(distance(point(delta,pack),actor)-distance(pack,actor))<0.0001f);
    }
    // Glove calibration never changes tracked position, and mirrored angles turn outward.
    Pose controller{{0,0,0,1},{1,2,3}};
    for(unsigned h=0;h<2;++h) {
        const auto target=calibratedHand(poseMatrix(controller),h,0.14f);
        require(distance(point(target,{}),controller.position)<0.00001f);
        require((h?-1:1)*(-target[2])<0);
        const auto glove=handTransform({5,6,7},{6,6,7},{0,1,0},target);
        require(distance(point(glove,{5,6,7}),controller.position)<0.0001f);
    }
    // Stretch only the sleeve axis to reach the elbow; transverse thickness stays fixed.
    const auto extended=limbTransform({}, {1,0,0},{0,0,1},{2,3,4},{3.5f,3,4},{0,0,1});
    require(distance(point(extended,{1,0,0}),{3.5f,3,4})<0.0001f);
    require(distance(point(extended,{0,1,0}),{2,4,4})<0.0001f);
    Pose roll{{0,0,std::sin(0.4f),std::cos(0.4f)},{}};
    const auto glove=handTransform({}, {1,0,0},{0,1,0},poseMatrix(roll));
    require(distance(point(glove,{1,0,0}),{0,0,-1})<0.0001f);
    require(distance(point(glove,{0,1,0}),{-std::sin(0.8f),std::cos(0.8f),0})<0.0001f);
    // Both halves of a 50/50 cuff envelope must share the tracked glove frame.
    for(float yaw:{-1.3f,0.0f,2.4f}) {
        const auto tracked=poseMatrix({{0,std::sin(yaw/2),0,std::cos(yaw/2)},{22,140,-30}});
        const auto bindWrist=poseMatrix({{0,0,0,1},{58,130,0}});
        const auto bindForearm=poseMatrix({{0,0,0,1},{36,130,0}});
        Matrix iw,ifore;require(invertAffine(bindWrist,iw)&&invertAffine(bindForearm,ifore));
        const auto shared=multiply(tracked,iw);
        const auto rebased=multiply(shared,bindForearm);
        const auto foreSkin=multiply(rebased,ifore),wristSkin=multiply(tracked,iw);
        Matrix cuff{};for(unsigned k=0;k<16;++k)cuff[k]=(foreSkin[k]+wristSkin[k])*0.5f;
        for(Vec3 vertex:{Vec3{57,129,0},Vec3{60,132,2}})
            require(distance(point(cuff,vertex),point(shared,vertex))<0.0001f);
    }
    HandInput sensed;sensed.valid=true;sensed.grip=0.8f;sensed.trigger=0.2f;
    auto curls=gloveCurls(sensed);require(std::fabs(curls[1]-0.2f)<0.0001f && std::fabs(curls[2]-0.8f)<0.0001f);
    sensed.fingerValid=31;sensed.fingerCurl={0.1f,0.3f,0.7f,0.2f,0.8f};curls=gloveCurls(sensed);
    require(std::fabs(curls[0]-0.1f)<0.0001f&&std::fabs(curls[1]-0.3f)<0.0001f&&std::fabs(curls[2]-0.7f)<0.0001f&&std::fabs(curls[3]-0.2f)<0.0001f&&std::fabs(curls[4]-0.8f)<0.0001f);
    std::array<Matrix,3> finger;
    for(unsigned j=0;j<3;++j)finger[j]=poseMatrix({{0,0,0,1},{float(j),0,0}});
    const auto open=finger;curlFinger(finger,{0,1,0},0);require(finger==open);
    curlFinger(finger,{0,1,0},1);
    require(distance(point(finger[0],{}),{})<0.00001f);
    require(std::fabs(distance(point(finger[0],{}),point(finger[1],{}))-1)<0.0001f);
    require(std::fabs(distance(point(finger[1],{}),point(finger[2],{}))-1)<0.0001f);
    require(point(finger[1],{})[1]<-0.5f);
    const std::array<Vec3,4> straight{{{0,0,0},{1,0,0},{2,0,0},{3,0,0}}};
    const std::array<Vec3,4> bent{{{0,0,0},{1,0,0},{1,-1,0},{0,-1,0}}};
    require(jointCurl(straight)<0.00001f&&jointCurl(bent)>0.9f);
    // Mixed torso/sleeve triangles retain torso vertices; strip winding survives expansion.
    std::vector<std::uint8_t> dl{0x98,0,4,0,1,2, 0,3,4, 0,5,6, 3,7,8},filtered;
    std::array<bool,10> slots{};slots[1]=true;
    require(filterSleeveTriangles(dl,3,slots,filtered));
    require(filtered.size()==32&&filtered[0]==0x90&&filtered[2]==3);
    require(filtered[4]==1&&filtered[7]==3&&filtered[10]==5);
    slots[1]=false;require(filterSleeveTriangles(dl,3,slots,filtered));
    require(filtered[2]==6&&filtered[12]==0&&filtered[13]==5&&filtered[15]==0&&filtered[16]==3);
    dl.pop_back();require(!filterSleeveTriangles(dl,3,slots,filtered));
    const std::array<std::uint8_t,3> unsupported{0x61,0,0};require(!filterSleeveTriangles(unsupported,3,slots,filtered));
    // Body, upper arm and forearm are separate palette slots in the shared
    // shirt mesh. Forearms survive the short-sleeve mode; the gloves-only
    // fallback drops them, with torso triangles identical in either mode.
    {
        const std::vector<std::uint8_t> mesh{0x90,0,9,
            0,1,2,0,3,4,0,5,6,
            3,7,8,3,9,10,3,11,12,
            6,13,14,6,15,16,6,17,18};
        std::array<bool,10> hidden{};hidden[1]=true;
        std::vector<std::uint8_t> sleeves,gloves;
        require(filterSleeveTriangles(mesh,3,hidden,sleeves));require(sleeves[2]==6);
        hidden[2]=true;
        require(filterSleeveTriangles(mesh,3,hidden,gloves));require(gloves[2]==3);
        for(unsigned k=3;k<12;++k)require(sleeves[k]==gloves[k]);
    }
    GrabController grab;auto targets=equipmentTargets();HandInput hand;
    hand.valid=true;hand.pose.position=targets[0].position;hand.grip=0.8f;
    require(grab.update(hand,targets).grabbed);
    hand.pose.position={0,0,-0.3f};require(grab.update(hand,targets).kind==GrabKind::Flashlight);
    hand.trigger=1;require(grab.update(hand,targets).use);
    hand.valid=false;require(grab.update(hand,targets).released);
    hand.valid=true;hand.pose.position=targets[0].position;require(!grab.update(hand,targets).grabbed);
    hand.grip=0;grab.update(hand,targets);hand.grip=1;require(grab.update(hand,targets).grabbed);
    targets[0].available=false;require(grab.update(hand,targets).released);
    grab.reset();hand.grip=1;hand.pose.position={0,1,1};require(!grab.update(hand,targets).grabbed);
    hand.pose.position=targets[1].position;require(!grab.update(hand,targets).grabbed); // must release before entering grab zone
    hand.grip=0;grab.update(hand,targets);hand.grip=1;require(grab.update(hand,targets).kind==GrabKind::Key);
    grab.reset();GrabTarget door{10,GrabKind::Door,{},0.2f,true};hand.pose={};hand.grip=1;
    require(grab.update(hand,std::span(&door,1)).grabbed);
    hand.pose.orientation={0,0,std::sin(0.4f),std::cos(0.4f)};
    require(std::fabs(grab.update(hand,std::span(&door,1)).twist-0.8f)<0.0001f);
    hand.grip=std::numeric_limits<float>::quiet_NaN();require(grab.update(hand,std::span(&door,1)).released);
    // A missed 90 Hz button press still reaches the 30 Hz guest exactly once.
    ButtonLatch buttons;buttons.sample(Confirm,true);buttons.sample(0,true);
    require(buttons.consume()==Confirm);require(buttons.consume()==0);
    buttons.sample(Pause,true);require(buttons.consume()==Pause);
    buttons.sample(Pause,false);require(buttons.consume()==0);
    buttons.sample(Pause,true);require(buttons.consume()==0);
    buttons.sample(0,true);buttons.sample(Pause,true);require(buttons.consume()==Pause);
    // Dialogue/choice/training views must receive raw VR axes, not discard them.
    const std::array<float,4> turned{0,-1,1,0};std::int8_t sx=0,sy=0;
    routeControllerStick(sx,sy,0,1,false,turned);require(sx==0&&sy==100);
    routeControllerStick(sx,sy,0,-1,false,turned);require(sx==0&&sy==-100);
    sx=sy=0;routeControllerStick(sx,sy,0,1,true,turned);require(sx==-100&&sy==0);
    sx=20;sy=70;routeControllerStick(sx,sy,0.1f,0.1f,false,turned);require(sx==20&&sy==70);
    sx=sy=0;routeControllerStick(sx,sy,2,-2,false,turned);require(sx==100&&sy==-100);
    // Idle VR axes never erase a real gamepad, and either active device works.
    sx=70;sy=-40;require(!mergeNativeStick(sx,sy,0,0));require(sx==70&&sy==-40);
    require(!mergeNativeStick(sx,sy,0.1f,-0.1f));require(sx==70&&sy==-40);
    sx=sy=0;require(mergeNativeStick(sx,sy,0.6f,-0.8f));require(sx==60&&sy==-80);
    require(!mergeNativeStick(sx,sy,0.5f,0));require(sx==60&&sy==-80);
    // Measured cuff rings close even when split normal/UV seams prevent a
    // topological boundary loop. Only the proximal ring is filled.
    {
        std::vector<std::uint8_t> list{0x90,0,24};std::vector<Vec3> points{{-1,-1,0},{1,-1,0},{1,1,0},{-1,1,0},{-1,-1,2},{1,-1,2},{1,1,2},{-1,1,2}};
        const auto vertex=[&](unsigned id){list.insert(list.end(),{0,0,0,0,std::uint8_t(id)});};
        for(unsigned j=0;j<4;++j){unsigned a=j,b=(j+1)%4;for(unsigned id:{a,b,b+4,a,b+4,a+4})vertex(id);}
        std::array<bool,10> cuffs{};cuffs[0]=true;std::array<Vec3,10> axes{};axes[0]={0,0,1};
        const auto source=list;
        require(capCuffRims(list,5,cuffs,points,axes)==2);
        std::vector<std::uint8_t> cut;require(capCuffRims(cut,5,cuffs,points,axes,source)==2);
        // A rectangular wrist cut may be oblique to the bind axis. Its full
        // boundary must be sealed as well as the approximate axial end rings.
        auto oblique=source;points[0][2]=points[3][2]=0.8f;
        require(capCuffRims(oblique,5,cuffs,points,axes)>=3);
        // A single rim with a tilted bind axis must use every rim vertex.
        std::vector<Vec3> rim{{0,-10,-10},{0,10,-10},{0,10,10},{0,-10,10}};
        std::vector<std::uint8_t> rimList{0x90,0,6};
        for(unsigned id:{0u,1u,2u,0u,2u,3u})rimList.insert(rimList.end(),{0,0,0,0,std::uint8_t(id)});
        axes[0]={21,.2f,1};std::vector<std::uint8_t> sealed;
        require(capCuffRims(sealed,5,cuffs,rim,axes,rimList)==1);
        require(sealed[2]==12); // two complete, double-sided triangles
        axes[0]={};require(capCuffRims(list,5,cuffs,points,axes)==0);
    }
    // Both eyes use one depth-positioned panel; controller rays intersect it
    // in the same normalized coordinates as the rendered UI.
    {std::array<float,2> uv{};auto aim=poseMatrix({});require(settingsPanelPointer(aim,4.f/3,uv));require(std::fabs(uv[0]-.5f)<.0001f&&std::fabs(uv[1]-.5f)<.0001f);aim[3]=.7f;require(settingsPanelPointer(aim,4.f/3,uv)&&std::fabs(uv[0]-1)<.0001f);aim[3]=1;require(!settingsPanelPointer(aim,4.f/3,uv));aim=poseMatrix({{0,1,0,0},{}});require(!settingsPanelPointer(aim,1,uv));}
    // Only the free first-person flashlight-to-vacuum startup blend is
    // bypassed. Scripted controls, damage/startle and other motions stay native.
    require(bypassVacuumStartBlend(true,5,1,1,0,1));
    require(!bypassVacuumStartBlend(false,5,1,1,0,1));
    require(!bypassVacuumStartBlend(true,5,1,2,0,1));
    require(!bypassVacuumStartBlend(true,5,1,1,2,1));
    require(!bypassVacuumStartBlend(true,1,5,1,0,1));
    require(!bypassVacuumStartBlend(true,5,3,1,0,1));
    // Existing right-hand ownership survives a simultaneous left-hand attempt.
    InteractionHands rig;targets=equipmentTargets();
    std::array<HandInput,2> both{};both[0].valid=both[1].valid=true;
    both[1].pose.position=targets[0].position;both[1].grip=1;
    auto states=rig.update(both,targets);require(states[1].target==1);
    both[0].pose.position=targets[0].position;both[0].grip=1;
    states=rig.update(both,targets);require(states[1].target==1&&states[0].target==0);
    rig.cancel();states=rig.update(both,targets);require(states[0].target==0&&states[1].target==0);
    both[0].grip=both[1].grip=0;rig.update(both,targets);
    both[0].grip=both[1].grip=1;states=rig.update(both,targets);
    require((states[0].target!=0)!=(states[1].target!=0));
    targets[0].available=false;states=rig.update(both,targets);require(!states[0].target&&!states[1].target);
    grab.reset();hand={};hand.valid=true;hand.grip=1;
    require(grab.update(hand,std::span(&door,1)).grabbed);
    hand.pose.position={0,0,-1};require(grab.update(hand,std::span(&door,1)).released);
    hand.pose.position={};require(!grab.update(hand,std::span(&door,1)).grabbed);
    grab.reset();hand={};hand.valid=true;hand.grip=1;hand.pose.orientation={1e30f,0,0,0};
    require(!grab.update(hand,std::span(&door,1)).grabbed);
    // Two holsters for one Poltergust still represent one tool.
    rig.cancel();both={};both[0].valid=both[1].valid=true;rig.update(both,targets);
    std::array<GrabTarget,2> holsters{{{3,GrabKind::Poltergust,{-0.2f,0,0},0.1f,true},{4,GrabKind::Poltergust,{0.2f,0,0},0.1f,true}}};
    for(unsigned h=0;h<2;++h) {both[h].pose.position=holsters[h].position;both[h].grip=1;}
    states=rig.update(both,holsters);require((states[0].kind==GrabKind::Poltergust)!=(states[1].kind==GrabKind::Poltergust));
    // A model attached at the original wrist follows the tracked glove's
    // calibrated palm frame, rather than placing its muzzle at the wrist.
    for(unsigned h=0;h<2;++h) {
        auto tracked=calibratedHand(poseMatrix({{0.38268343f,0,0,0.92387953f},{-20,140,30}}),h,0.13962634f);
        const auto attachment=handTransform({10,20,30},{10,20,20},{0,1,0},tracked);
        Vec3 handle{};for(unsigned r=0;r<3;++r)handle[r]=attachment[r*4+3]+attachment[r*4]*10+attachment[r*4+1]*20+attachment[r*4+2]*30;
        require(distance(handle,{-20,140,30})<0.0001f);
        const Vec3 muzzle{10,20,5};Vec3 moved{};
        for(unsigned r=0;r<3;++r)moved[r]=attachment[r*4+3]+attachment[r*4]*muzzle[0]+attachment[r*4+1]*muzzle[1]+attachment[r*4+2]*muzzle[2];
        require(std::fabs(distance(handle,moved)-25)<0.0001f);
        for(unsigned r=0;r<3;++r)require(std::fabs((moved[r]-handle[r])/25+tracked[r*4+2])<0.0001f);
    }
    // Held tools share the calibrated glove frame without moving the glove
    // or wrist. A captured model-space muzzle and the physics ray must agree.
    for(unsigned h=0;h<2;++h) {
        const auto controller=poseMatrix({{0,0.38268343f,0,0.92387953f},{0.3f,-0.4f,-0.2f}});
        const auto held=calibratedHand(controller,h,0.13962634f);
        for(const Vec3 axis:{Vec3{0,0,1},Vec3{0,0,-1},Vec3{0.117f,0.658f,-0.744f}}) {
            const Vec3 handle{4,5,6},forward{4+axis[0],5+axis[1],6+axis[2]};
            const auto attachment=handTransform(handle,forward,{0,1,0},held);
            const float norm=std::sqrt(axis[0]*axis[0]+axis[1]*axis[1]+axis[2]*axis[2]);
            for(unsigned r=0;r<3;++r){float direction=0;for(unsigned k=0;k<3;++k)direction+=attachment[r*4+k]*axis[k]/norm;require(std::fabs(direction+held[r*4+2])<0.0001f);}
        }
        const auto nativeModel=poseMatrix({{0,0,0.38268343f,0.92387953f},{15,25,35}});
        const auto gripFromWorld=handTransform({4,5,6},{4,5,16},{0,1,0},poseMatrix({}));
        const Vec3 meshTip{2,3,20};Vec3 authoredTip{},gripTip{},renderTip{},physicsTip{};
        const auto gripFromModel=multiply(gripFromWorld,nativeModel),heldModel=multiply(held,gripFromModel);
        for(unsigned r=0;r<3;++r){authoredTip[r]=nativeModel[r*4+3];for(unsigned k=0;k<3;++k)authoredTip[r]+=nativeModel[r*4+k]*meshTip[k];}
        for(unsigned r=0;r<3;++r){gripTip[r]=gripFromWorld[r*4+3];for(unsigned k=0;k<3;++k)gripTip[r]+=gripFromWorld[r*4+k]*authoredTip[k];}
        for(unsigned r=0;r<3;++r){renderTip[r]=heldModel[r*4+3];physicsTip[r]=held[r*4+3];for(unsigned k=0;k<3;++k){renderTip[r]+=heldModel[r*4+k]*meshTip[k];physicsTip[r]+=held[r*4+k]*gripTip[k];}}
        require(distance(renderTip,physicsTip)<0.0001f);
    }
    // Runtime aim rotates the barrel without moving its handle away from the
    // tracked palm; runtime aim-space translation is not a grip anchor.
    for(unsigned h=0;h<2;++h) {
        const auto grip=poseMatrix({{0,0,0,1},{0.2f,-0.5f,-0.3f}});
        auto aim=poseMatrix({{0.38268343f,0,0,0.92387953f},{9,9,9}});
        const auto frame=equipmentGripFrame(grip,aim,h,0,0.1f);
        require(std::fabs(frame[3]-0.2f)<0.0001f&&std::fabs(frame[7]+0.5f)<0.0001f&&std::fabs(frame[11]+0.4f)<0.0001f);
        for(unsigned r=0;r<3;++r)for(unsigned k=0;k<3;++k)require(frame[r*4+k]==aim[r*4+k]);
        const auto clear=equipmentGripFrame(grip,aim,h,0,0.1f,0.055f);
        require(std::fabs(clear[7]-frame[7]+0.055f*calibratedHand(grip,h,0)[5])<0.0001f);
        for(unsigned r : {0u,2u})require(std::fabs(clear[r*4+3]-frame[r*4+3]+0.055f*calibratedHand(grip,h,0)[r*4+1])<0.0001f);
        for(unsigned r=0;r<3;++r)for(unsigned k=0;k<3;++k)require(clear[r*4+k]==frame[r*4+k]);
        aim=poseMatrix({});const auto straight=equipmentGripFrame(grip,aim,h,0,0.1f);
        require(frame[3]==straight[3]&&frame[7]==straight[7]&&frame[11]==straight[11]);
    }
    // Mesh +Y must become the controller's -Z aim, independent of native
    // animation parents and of the grip/aim rotation difference. The visible
    // mouth and the gameplay emission point must be the same transformed point.
    for(unsigned h=0;h<2;++h) {
        const auto mesh=poseMatrix({{0,0,0.70710678f,0.70710678f},{4,7,9}});
        const auto inverse=equipmentMeshGripInverse(mesh,70);
        const auto grip=poseMatrix({{0,0,0,1},{0.2f,-0.4f,-0.3f}});
        const auto aim=poseMatrix({{0,0.70710678f,0,0.70710678f},{3,4,5}});
        const auto held=equipmentGripFrame(grip,aim,h,0,0.1f);
        const auto render=multiply(held,multiply(inverse,mesh));
        for(unsigned r=0;r<3;++r) {
            const float handle=render[r*4+3]+70*render[r*4+1];
            const float lip=render[r*4+3]+88*render[r*4+1];
            require(std::fabs(handle-held[r*4+3])<0.0001f);
            require(std::fabs(lip-(held[r*4+3]-18*aim[r*4+2]))<0.0001f);
        }
    }
    // Automatic closing waits for a real linked-room crossing and clearance
    // of both tracked hands. Reaching back through or returning resets the dwell.
    {
        DoorPassage passage;passage.opened(42,100);const std::array<float,2> safe{-80,-100},reach{-10,-100};
        require(!passage.step(42,41,-240,safe,0.1f));
        for(unsigned i=0;i<4;++i)require(!passage.step(41,41,-240,safe,0.1f));
        require(!passage.step(41,41,-240,reach,0.1f));
        require(!passage.step(41,41,-100,safe,0.1f));
        for(unsigned i=0;i<4;++i)require(!passage.step(41,41,-240,safe,0.1f));
        require(passage.step(41,41,-240,safe,0.1f));
        require(!passage.step(41,41,-240,safe,0.1f));
        passage.opened(41,-100);const std::array<float,2> reverse{80,100};
        for(unsigned i=0;i<4;++i)require(!passage.step(42,42,240,reverse,0.1f));
        require(passage.step(42,42,240,reverse,0.1f));
    }
    {
        float depth=0;require(doorThresholdContactDepth(605,55,550,depth)&&depth==0);
        require(doorThresholdContactDepth(604,55,550,depth)&&depth==1);
        require(!doorThresholdContactDepth(620,55,550,depth));
        require(!doorThresholdContactDepth(590,55,550,depth));
        require(!doorThresholdContactDepth(605,0,550,depth));
        float height=0;require(doorThresholdHeight(550,550,550,height)&&height==550);
        require(!doorThresholdHeight(550,555,550,height));
        require(!doorThresholdHeight(0,0,550,height));
        require(!doorThresholdHeight(550,std::numeric_limits<float>::quiet_NaN(),550,height));
        require(!doorThresholdHeight(550,550,std::numeric_limits<float>::infinity(),height));
    }
    // An open leaf only removes its slab plane in a narrow upright aperture.
    // Floors, neighbouring walls, off-centre players and corrupt data fail closed.
    {
        const auto base=poseMatrix({});const Vec3 center{20,50,100};
        require(doorApertureAllows(base,center,{0,0,1},{20,100,100},{20,100,150}));
        require(!doorApertureAllows(base,center,{0,1,0},{20,50,100},{20,100,150}));
        require(!doorApertureAllows(base,center,{1,0,0},{20,100,100},{20,100,150}));
        require(!doorApertureAllows(base,center,{0,0,1},{20,100,160},{20,100,150}));
        require(!doorApertureAllows(base,center,{0,0,1},{20,100,100},{90,100,150}));
        require(!doorApertureAllows(base,center,{0,0,1},{20,100,100},{20,40,150}));
        require(!doorApertureAllows(base,center,{0,0,1},{20,100,100},{20,300,150}));
        auto broken=base;broken[0]=0;require(!doorApertureAllows(broken,center,{0,0,1},{20,100,100},{20,100,150}));
    }
    // Hose endpoints are attached exactly, ribs remain finite and roughly evenly
    // spaced, and mirrored attachments produce a mirrored curve without winding flips.
    {
        const std::array<Vec3,5> native{{{0,0,0},{0,-0.2f,0.2f},{0,-0.4f,0.4f},{0,-0.2f,0.6f},{0,0,0.8f}}};
        const auto pack=poseMatrix({});auto nozzle=poseMatrix({{0,0,0,1},{0.4f,0.2f,0.1f}});
        std::vector<Vec3> curve;require(retargetHose(native,pack,nozzle,curve));require(curve.size()==native.size());
        require(distance(curve.front(),native.front())<0.00001f&&distance(curve.back(),Vec3{0.4f,0.2f,0.9f})<0.00001f);
        float lo=100,hi=0;for(size_t i=1;i<curve.size();++i){const float d=distance(curve[i],curve[i-1]);lo=std::min(lo,d);hi=std::max(hi,d);}require(lo>0&&hi/lo<1.35f);
        auto reflected=pack;reflected[0]=-1;auto reflectedNozzle=nozzle;reflectedNozzle[0]=-1;reflectedNozzle[3]*=-1;
        std::vector<Vec3> left;require(retargetHose(native,reflected,reflectedNozzle,left));
        for(size_t i=0;i<curve.size();++i)require(distance(left[i],Vec3{-curve[i][0],curve[i][1],curve[i][2]})<0.00001f);
        ToolTarget connector;connector.origin={.4f,.2f,.9f};connector.direction={1,0,0};connector.up={0,1,0};
        const auto fitted=alignTool(native.back(),{0,.2f,.2f},{0,1,0},connector);
        require(retargetHose(native,pack,fitted,left));require(distance(left.back(),connector.origin)<.00001f);
        auto far=nozzle;far[3]=100;require(!retargetHose(native,pack,far,left));
        require(!retargetHose(std::span(native).first(2),pack,nozzle,left));
    }
    // Local sleeves end at the tracked wrist and point back along its grip,
    // regardless of the native shoulder or body position.
    {
        const Vec3 elbow{0,20,0},wrist{0,0,0};const auto target=poseMatrix({{0,0,0,1},{10,30,-20}});
        const auto skin=detachedForearmSkin(elbow,wrist,target,16);
        auto point=[&](Vec3 p){Vec3 out{};for(unsigned r=0;r<3;++r){out[r]=skin[r*4+3];for(unsigned k=0;k<3;++k)out[r]+=skin[r*4+k]*p[k];}return out;};
        require(distance(point(wrist),Vec3{10,30,-20})<0.0001f);
        require(distance(point(elbow),Vec3{10,30,-4})<0.0001f);
    }
    // Re-anchoring a tool preserves its authored rotation relative to the hand;
    // a controller's separate aim pose cannot rotate it out of the fingers.
    {
        const auto authored=poseMatrix({{0.70710678f,0,0,0.70710678f},{4,5,6}});const Vec3 handle{20,30,40};
        const auto inverse=reanchorGripInverse(authored,handle);
        for(unsigned r=0;r<3;++r){float v=inverse[r*4+3];for(unsigned k=0;k<3;++k){require(inverse[r*4+k]==authored[r*4+k]);v+=inverse[r*4+k]*handle[k];}require(std::fabs(v)<0.0001f);}
    }
    // Authored handle projection is invariant under the native model's world
    // translation/rotation/scale, and rejects a degenerate shaft.
    {
        auto mesh=poseMatrix({{0,0,0.70710678f,0.70710678f},{4,7,9}});
        for(unsigned r=0;r<3;++r)mesh[r*4+1]*=2;
        Vec3 contact{};for(unsigned r=0;r<3;++r)contact[r]=mesh[r*4+3]+42*mesh[r*4+1]+3*mesh[r*4+2];
        float y=0,residual=0;
        require(equipmentHandleProjection(mesh,contact,y,residual));
        require(std::fabs(y-42)<0.0001f&&std::fabs(residual-3)<0.0001f);
        mesh[1]=mesh[5]=mesh[9]=0;
        require(!equipmentHandleProjection(mesh,contact,y,residual));
    }
    // Contact is a glove-local point, including palm depth and lateral finger
    // position. Aim-space translation cannot drag the handle out of the grip.
    for(unsigned h=0;h<2;++h) {
        const auto grip=poseMatrix({{0,0,0,1},{0.2f,-0.4f,-0.3f}});
        const auto aim=poseMatrix({{0,0.70710678f,0,0.70710678f},{8,9,10}});
        const Vec3 contact{h?0.02f:-0.02f,-0.04f,-0.12f};
        const auto frame=equipmentContactFrame(grip,aim,h,0,contact);
        const auto glove=calibratedHand(grip,h,0);
        for(unsigned r=0;r<3;++r){float expected=grip[r*4+3];for(unsigned k=0;k<3;++k)expected+=glove[r*4+k]*contact[k];require(std::fabs(frame[r*4+3]-expected)<0.0001f);}
        require(std::fabs(glove[1]-(h?0.8729220773f:-0.8729220773f))<0.0001f);
        require(std::fabs(glove[5]-0.4878596591f)<0.0001f);
        for(unsigned r=0;r<3;++r)for(unsigned k=0;k<3;++k)require(frame[r*4+k]==glove[r*4+k]);
        // The actual attachment path must combine mesh-forward calibration
        // with glove contact, even when runtime aim is rotated 90 degrees.
        const auto mesh=poseMatrix({{0,0,0.70710678f,0.70710678f},{4,7,9}});
        const auto render=multiply(frame,multiply(equipmentMeshGripInverse(mesh,26),mesh));
        for(unsigned r=0;r<3;++r) {
            const float handle=render[r*4+3]+26*render[r*4+1];
            const float lip=render[r*4+3]+88*render[r*4+1];
            require(std::fabs(handle-frame[r*4+3])<0.0001f);
            require(std::fabs(lip-handle+62*grip[r*4+2])<0.0001f);
        }
    }
    // Held glove cylinder runs along the tool, with its grip centre fixed.
    for(unsigned h=0;h<2;++h) {
        const auto target=calibratedHand(poseMatrix({{0,0.38268343f,0,0.92387953f},{3,4,5}}),h,0.14f);
        const Vec3 centre{h?2.f:-2.f,-5,-15};
        const auto grasp=toolHoldingHand(target,centre,h);
        require(distance(point(grasp,centre),point(target,centre))<0.0001f);
        float alignment=0;for(unsigned r=0;r<3;++r)alignment+=grasp[r*4]*target[r*4+2];
        require(std::fabs(std::fabs(alignment)-1)<0.0001f);
    }
    // Independent live calibration supports zero and either roll direction
    // without translating the tracked wrist.
    for(unsigned h=0;h<2;++h)for(float angle:{-1.2f,0.f,0.7f}) {
        const auto controller=poseMatrix({{0,0,0,1},{1,2,3}});
        const auto glove=calibratedHand(controller,h,0,angle);
        require(distance(point(glove,{}),{1,2,3})<0.0001f);
        require(std::fabs(glove[1]+std::sin(angle))<0.0001f);
        require(std::fabs(glove[5]-std::cos(angle))<0.0001f);
        const Vec3 contact{2,-5,-15};
        const auto tool=equipmentContactFrame(controller,controller,h,0,contact,angle);
        require(distance(point(glove,contact),point(tool,{}))<0.0001f);
    }
    // A model-space translation translates the measured closed finger centre
    // equally; it does not change the finger's inferred opening/radius.
    {
        std::array<Matrix,3> chain;for(unsigned i=0;i<3;++i){chain[i]=poseMatrix({});chain[i][11]=-float(i)*0.04f;}
        const auto a=closedFingerGripCentre(chain,{0,1,0});
        for(auto& m:chain){m[3]+=0.2f;m[7]-=0.3f;m[11]+=0.5f;}
        const auto b=closedFingerGripCentre(chain,{0,1,0});
        require(std::fabs(b[0]-a[0]-0.2f)<0.0001f&&std::fabs(b[1]-a[1]+0.3f)<0.0001f&&std::fabs(b[2]-a[2]-0.5f)<0.0001f);
    }
    // A held grasp stays wrapped while the index still responds to activation.
    {HandInput hand;hand.valid=true;const auto rest=toolGripCurls(hand);hand.trigger=1;const auto firing=toolGripCurls(hand);
     require(rest[0]>0.6f&&rest[1]>0.7f&&rest[2]==0.8f);require(firing[1]>rest[1]);
     for(unsigned i:{0u,2u,3u,4u})require(rest[i]==firing[i]);}
    // Follow takes the shortest yaw path, has a dead zone/rate cap and never
    // snaps on a long render frame. No pitch or gameplay force enters this math.
    require(captureFollowStep(0,0.05f,0.02f)==0);
    require(captureFollowStep(0,1,0.02f)>0&&captureFollowStep(0,1,0.02f)<=2.09439510f*0.02f);
    require(captureFollowStep(3.1f,-3.f,0.02f)>0);
    require(std::fabs(captureFollowStep(0,3,3))<=2.09439510f*0.05f);
    // Physical motion preserves the player's direction, including wrong-way
    // movement. Head-relative conversion and neutral rearming do not auto-pull.
    require(counterPullStick({0,0,0},{0,0,0.03f},0)==std::array<float,2>{0,0});
    {const auto back=counterPullStick({0,0,0},{0,0,0.16f},0);require(std::fabs(back[0])<0.0001f&&back[1]<-0.99f);
     const auto wrong=counterPullStick({0,0,0},{0,0,-0.16f},0);require(wrong[1]>0.99f);
     const auto turned=counterPullStick({0,0,0},{0,0,0.16f},1.570796327f);require(turned[0]<-0.99f&&std::fabs(turned[1])<0.0001f);}
    // Only an active opposing stroke grants damage, not holding the hand at
    // an old displaced position, sideways motion, or motion with the escape.
    require(opposedPullDistance({0,0,0},0,0,0.02f)==0);
    require(opposedPullDistance({0,0,-0.01f},0,0,0.02f)==0);
    require(opposedPullDistance({0.01f,0,0},0,0,0.02f)==0);
    require(opposedPullDistance({0,0,0.001f},0,0,0.02f)==0);
    require(opposedPullDistance({0,0,0.01f},0,0,0.02f)>0.009f);
    require(opposedPullDistance({0,0,0.01f},1.570796327f,1.570796327f,0.02f)>0.009f);
    // Alternating native callbacks must not reset another ghost's stroke.
    CapturePulls captures;captures.observe(11,{0,0,0},{0,0,-100},1.0);captures.observe(22,{0,0,0},{-100,0,0},1.0);
    require(captures.focus(1.01).actor==11);
    captures.step({0,0,.02f},0,.05f,1.01,true);require(captures.allows(11,1.02));require(!captures.allows(22,1.02));
    captures.observe(22,{0,0,0},{-101,0,0},1.03);captures.observe(11,{0,0,0},{0,0,-101},1.03);
    require(captures.focus(1.03).actor==11);require(captures.allows(11,1.04));
    captures.step({.02f,0,0},0,.05f,1.05,true);require(!captures.allows(11,1.06));require(captures.allows(22,1.06));
    captures.observe(33,{0,0,0},{0,0,-90},1.07);require(captures.allows(22,1.08));require(captures.focus(1.08).actor==11);
    captures.step({0,0,0},0,.05f,1.22,true);require(!captures.allows(22,1.22));
    captures.observe(11,{0,0,0},{1,0,-101},1.23);captures.step({-.02f,0,0},0,.05f,1.24,true);require(captures.allows(11,1.25));
    captures.step({0,0,0},0,.05f,1.26,false);require(!captures.allows(11,1.27));
    captures.step({-.05f,0,0},0,.2f,1.28,true);require(!captures.allows(11,1.29));
    require(!captures.allows(22,1.5));captures.observe(33,{0,0,0},{0,0,-92},1.5);require(captures.focus(1.5).actor==33);
    captures.reset();require(captures.focus(1.51).actor==0);require(!captures.allows(11,1.51));
    // Blocked input must not produce zero HP even on the native final-HP tick;
    // allowed input retains the game's original one-HP drain/completion rule.
    for(int hp:{1,2,10,100}){require(captureHealthSource(hp,false)-1==hp);require(captureHealthSource(hp,true)-1==hp-1);}
    // Either equipped hand owns activation; the other trigger cannot turn on
    // its flashlight or vacuum. Lost tracking releases activation immediately.
    for(unsigned h=0;h<2;++h) {
        std::array<GrabState,2> equipped{};std::array<HandInput,2> input{};
        for(auto& v:input)v.valid=true;
        equipped[h].target=1;equipped[h].kind=GrabKind::Flashlight;
        input[1-h].trigger=1;require(equipmentTriggers(equipped,input)==std::array<float,3>{0,0,0});
        input[h].trigger=0.8f;require(equipmentTriggers(equipped,input)[2]==0.8f);
        equipped[h].kind=GrabKind::Poltergust;
        auto activation=equipmentTriggers(equipped,input);require(activation[0]==1&&activation[1]==0.8f&&activation[2]==0);
        input[h].valid=false;require(equipmentTriggers(equipped,input)==std::array<float,3>{0,0,0});
    }
    // Candle reveal remains a timed story event; only view/control ownership
    // commands are blanked. Other scripts fail closed without any changes.
    std::string encounter="<MENUOFF>\n<LUIGISTOP2>\n<GCAMERA>\"ima_demo.cmn\"\n<WAIT>(20)\n<SPEAK>(0)\n<FLAGON>(91)\n<END>";
    const auto originalSize=encounter.size();require(liveParlourEncounter(encounter));
    require(encounter.size()==originalSize&&encounter.find("<WAIT>(20)")!=std::string::npos&&encounter.find("<FLAGON>(91)")!=std::string::npos);
    require(encounter.find("<GCAMERA>")==std::string::npos&&encounter.find("<LUIGISTOP2>")==std::string::npos);
    std::string other="<LUIGISTOP2>\n<GCAMERA>\"story.cmn\"";const auto unchanged=other;
    require(!liveParlourEncounter(other)&&other==unchanged);
    // Belt and controllers share the same tracked-room coordinates after head
    // translation and a 90-degree turn; looking down does not move the belt.
    auto beltHead=poseMatrix({});beltHead[3]=0.3f;beltHead[7]=-0.1f;beltHead[11]=0.2f;
    auto belt=beltTargets(beltHead,1.570796327f);
    HandInput reach;reach.valid=true;reach.pose.position=belt[0].position;
    require(hoverTarget(reach,belt)==1);
    belt[0].available=false;require(hoverTarget(reach,belt)==0);
    reach.valid=false;require(hoverTarget(reach,belt)==0);
    require(std::fabs(belt[0].position[0]-0.22f)<0.0001f);
    require(std::fabs(belt[0].position[1]+0.75f)<0.0001f);
    require(std::fabs(belt[0].position[2]-0.44f)<0.0001f);
    require(belt[2].position[1]>belt[0].position[1]+0.4f);
    const auto standard=beltTargets(poseMatrix({}),0);
    const auto swapped=beltTargets(poseMatrix({}),0,false,true,true);
    require(standard[0].position[0]<0&&standard[2].position[0]>0&&standard[3].position[0]>0);
    require(swapped[0].position[0]>0&&swapped[2].position[0]<0&&swapped[3].position[0]<0);
    require(standard[2].position[2]>0&&standard[2].position[1]>-0.3f);
    GrabController device;HandInput deviceHand;deviceHand.valid=true;deviceHand.pose.position=standard[3].position;deviceHand.grip=1;
    require(device.update(deviceHand,standard).kind==GrabKind::GameBoy);
    // Independent tools can be held at once, while duplicate tool ownership remains excluded.
    rig.cancel();both={};both[0].valid=both[1].valid=true;targets=equipmentTargets();rig.update(both,targets);
    both[0].pose.position=targets[0].position;both[1].pose.position=targets[2].position;
    both[0].grip=both[1].grip=1;states=rig.update(both,targets);
    require(states[0].kind==GrabKind::Flashlight&&states[1].kind==GrabKind::Poltergust);
    both[0].trigger=0.7f;both[1].trigger=0.9f;
    auto dual=equipmentTriggers(states,both);require(dual[0]==0&&dual[1]==0.9f&&dual[2]==0.7f);
    both[0].valid=false;dual=equipmentTriggers(states,both);require(dual[1]==0.9f&&dual[2]==0);
    both[0].valid=true;std::swap(states[0],states[1]);dual=equipmentTriggers(states,both);require(dual[1]==0.7f&&dual[2]==0.9f);
    auto large=equipmentTargets();for(unsigned k=1;k<large.size();++k)large[k].available=false;
    HandInput nearHip;nearHip.valid=true;nearHip.pose.position=large[0].position;nearHip.pose.position[1]+=0.25f;nearHip.grip=1;
    require(hoverTarget(nearHip,large)==1);GrabController wide;require(wide.update(nearHip,large).grabbed);
    const auto toolbar=toolbarTargets(poseMatrix({}),0);
    require(toolbar[1].position[0]<toolbar[3].position[0]);require(toolbar[3].position[0]<toolbar[0].position[0]);
    require(std::fabs(toolbar[0].position[1]+0.50f)<0.001f);require(std::fabs(toolbar[0].position[2]+0.23f)<0.001f);
    for(unsigned i=0;i<4;++i){HandInput h;h.valid=true;h.pose.position=toolbar[i].position;require(hoverTarget(h,toolbar)==toolbar[i].id);
        for(unsigned j=i+1;j<4;++j){float d=0;for(unsigned k=0;k<3;++k)d+=std::pow(toolbar[i].position[k]-toolbar[j].position[k],2);require(std::sqrt(d)>toolbar[i].radius+toolbar[j].radius);}}
    const auto leftShoulder=toolbarTargets(poseMatrix({}),0,true);require(leftShoulder[2].position[0]<0);require(toolbar[2].position[0]>0);
    for(unsigned i:{0u,1u,3u})require(leftShoulder[i].position==toolbar[i].position);
    const auto adjustedBelt=toolbarTargets(poseMatrix({}),0,false,0.35f,0.15f);
    require(adjustedBelt[2].position==toolbar[2].position);
    require(std::fabs(adjustedBelt[0].position[1]+0.35f)<0.001f);require(std::fabs(adjustedBelt[0].position[2]+0.15f)<0.001f);

    WorldGesture gesture;WorldTarget chest{100,WorldAction::Chest,{0,0,0},{0,1,0},0.15f,true,true};
    std::array<HandInput,2> lifting{};for(auto& h:lifting){h.valid=true;h.grip=1;}lifting[1].grip=0;
    require(!gesture.update(chest,lifting).began);lifting[1].grip=1;require(gesture.update(chest,lifting).began);
    lifting[0].pose.position[1]=lifting[1].pose.position[1]=0.19f;require(gesture.update(chest,lifting).completed);require(!gesture.update(chest,lifting).completed);
    lifting[0].valid=false;require(gesture.update(chest,lifting).cancelled);require(!gesture.update(chest,lifting).began);
    for(auto& h:lifting){h.valid=true;h.grip=0;h.pose={};}gesture.update(chest,lifting);
    WorldTarget knob{101,WorldAction::Door,{0,0,0},{0,0,-1},0.12f,true,false};lifting[0].grip=1;require(gesture.update(knob,lifting).began);
    lifting[0].pose.position[2]=-0.2f;require(gesture.update(knob,lifting).amount==0);
    lifting[0].pose.orientation={0,0,-std::sin(0.4f),std::cos(0.4f)};require(gesture.update(knob,lifting).completed);
    knob.available=false;require(gesture.update(knob,lifting).cancelled);
    // Key insertion needs inward travel; merely gripping cannot unlock anything.
    for(auto& h:lifting){h.pose={};h.valid=true;h.grip=0;}gesture.update(knob,lifting);
    WorldTarget keyhole{102,WorldAction::KeyInsert,{0,0,0},{0,0,-1},0.08f,true,false};
    lifting[0].grip=1;require(gesture.update(keyhole,lifting).began);
    lifting[0].pose.position[2]=0.08f;require(gesture.update(keyhole,lifting).amount==0);
    lifting[0].pose.position[2]=-0.07f;require(gesture.update(keyhole,lifting).completed);
    for(auto& h:lifting){h.pose={};h.grip=0;}gesture.update(keyhole,lifting);
    WorldTarget drawer{103,WorldAction::Shake,{0,0,0},{1,0,0},0.12f,true,false};
    lifting[0].grip=1;require(gesture.update(drawer,lifting).began);
    lifting[0].pose.position[0]=0.12f;require(!gesture.update(drawer,lifting).completed);
    lifting[0].pose.position[0]=-0.12f;require(!gesture.update(drawer,lifting).completed);
    lifting[0].pose.position[0]=0.12f;require(gesture.update(drawer,lifting).completed);
    require(ordinaryDoorEligible(1,0x108,41,42,42));require(ordinaryDoorEligible(1,0x10B,41,42,42));
    require(ordinaryDoorCloseFlags(0x313)==0x383);require((ordinaryDoorCloseFlags(0x4713)&0x4400)==0x4400);
    for(unsigned blocked:{0x4408u,0x10008u,0x208u,0x48u})require(!ordinaryDoorEligible(1,blocked,41,42,42));
    require(!ordinaryDoorEligible(11,8,41,42,42));require(!ordinaryDoorEligible(1,8,41,42,2));
    for(unsigned flags:{0x10u,0x313u,0xB13u,0x8313u})require(ordinaryDoorClosable(1,flags,41,42,42));
    for(unsigned flags:{8u,0x340u,0x380u,0x4410u,0x10010u})require(!ordinaryDoorClosable(1,flags,41,42,42));
    require(!ordinaryDoorClosable(4,0x310,41,42,42));require(!ordinaryDoorClosable(1,0x310,41,42,12));
    require(!ordinaryDoorClosable(1,0x310,41,41,41));
    require(ordinaryDoorOpenFlags(0x108,false)==0x340);require(ordinaryDoorOpenFlags(0x108,true)==0xB40);
    // A held tool excludes that hand without excluding the free hand.
    gesture.cancel();for(auto& h:lifting){h.pose={};h.valid=true;h.grip=0;h.occupied=false;}
    lifting[0].occupied=true;lifting[0].grip=1;knob.available=true;gesture.update(knob,lifting);
    lifting[1].grip=1;require(gesture.update(knob,lifting).began);
    lifting[1].valid=false;require(gesture.update(knob,lifting).cancelled);
    lifting[1].valid=true;require(!gesture.update(knob,lifting).began);
    lifting[1].grip=0;gesture.update(knob,lifting);lifting[1].grip=1;require(gesture.update(knob,lifting).began);
    std::cout<<"Arm lengths, reach limits, grab hysteresis, tracking loss, inventory invalidation and door twist passed\n";
}


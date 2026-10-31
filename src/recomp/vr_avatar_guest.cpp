#include "recomp/vr_avatar_guest.h"
#include "recomp/vr_host.h"
#include "recomp/guest_util.h"
#include "recomp/interp.h"
#include "recomp/skip.h"
#include "recomp/version.h"
#include "recomp/rarc.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <cstdlib>
namespace recomp { u32 copyDispCount(); }
namespace recomp::vravatar {
namespace {
u32 sPlayer=0,sDemoFrame=0;
struct DoorBridge {u32 leaf;float height;};
std::vector<DoorBridge> sDoorBridges;
std::vector<u32> sOpenedDoors;u32 sDoorTable=0;unsigned sDoorCount=0;bool sDoorPlayerQuery=false;
struct NativeHoseBuild {u32 rope=0,up=0;float scale=0;};
NativeHoseBuild sHoseBuild;bool sHoseRebuild=false;
vr::PlayerAnchor sDemoAnchor;
bool sDoorTestWalk=false;vr::Vec3 sDoorTestGoal{},sDoorTestCenter{},sDoorTestTangent{};
bool sPickupActive=false,sReflectionPass=false,sReactionActive=false;
vr::Vec3 sNativeToolOrigin{},sNativeToolAxis{};
u32 sToolFrame=0,sSuctionVectors=0,sVisualToolVectors=0;bool sToolAxisValid=false;
struct ToolAttachment {vr::Matrix gripFromModel;vr::Vec3 tip,axis,rear;std::vector<vr::Matrix> gripViews,nativeGripViews;u32 model=0,clone=0;float palmReach=0;bool valid=false;};
std::array<ToolAttachment,2> sToolAttachments{};
std::array<vr::Matrix,2> sReflectedToolGrip{};std::array<bool,2> sReflectedToolValid{};u32 sReflectedToolFrame=0;
std::array<vr::Vec3,2> sGripContacts{};bool sGripContactsValid=false;float sGripHalfWidth=8;
std::array<u32,2> sToolCloneMemory{};
bool sDrawingOtherTool=false;
std::uint64_t sEquipmentContext=0;
unsigned toolIndex(){const u32 nozzle=gu::inMem1(sPlayer+0x7A7)?rd32(sPlayer+0x7A4):0;return gu::inMem1(nozzle+0x3F)&&rd32(nozzle+0x3C)==6?1:0;}
struct SleeveList {u32 source,size,target,targetSize;u64 signature;};
std::vector<SleeveList> sSleeveLists;
u32 filteredSleeveList(u32 source,u32 size,unsigned stride,const std::array<bool,10>& slots,const std::array<bool,10>& cuffs,const std::array<vr::Vec3,10>& axes,u32 model,u32& filteredSize) {
    if(!size||size>262144||!gu::inMem1(source)||!gu::inMem1(source+size-1))return 0;
    u64 hash=1469598103934665603ull;
    for(unsigned i=0;i<10;++i)hash=(hash^unsigned(slots[i]))*1099511628211ull;
    for(unsigned i=0;i<10;++i)hash=(hash^unsigned(cuffs[i]))*1099511628211ull;
    for(unsigned i=0;i<size;++i)hash=(hash^rd8(source+i))*1099511628211ull;
    for(const auto& cached:sSleeveLists)if(cached.source==source&&cached.size==size&&cached.signature==hash) {filteredSize=cached.targetSize;return cached.target;}
    if(sSleeveLists.size()>=96)return 0;
    std::vector<std::uint8_t> filtered;
    if(!vr::filterSleeveTriangles(std::span(hostp(source),size),stride,slots,filtered))return 0;
    std::vector<vr::Vec3> positions;
    if(std::any_of(cuffs.begin(),cuffs.end(),[](bool v){return v;})) {
        const u32 begin=rd32(model+0x48),end=rd32(model+0x4C);
        if(end>begin&&end-begin<1200000&&gu::inMem1(end-1))
            for(u32 v=begin;v+11<end;v+=12)positions.push_back({gu::rdf(v),gu::rdf(v+4),gu::rdf(v+8)});
    }
    std::vector<std::uint8_t> uncut;const std::array<bool,10> keepAll{};
    if(!positions.empty())vr::filterSleeveTriangles(std::span(hostp(source),size),stride,keepAll,uncut);
    const unsigned caps=vr::capCuffRims(filtered,stride,cuffs,positions,axes,uncut);
    if(const char* prefix=std::getenv("LM_VR_CUFF_DUMP");prefix&&!positions.empty()) {
        const std::string name=std::string(prefix)+"-"+std::to_string(source);
        std::ofstream(name+".positions",std::ios::binary).write(reinterpret_cast<const char*>(positions.data()),positions.size()*sizeof(vr::Vec3));
        std::ofstream(name+".source",std::ios::binary).write(reinterpret_cast<const char*>(hostp(source)),size);
        std::ofstream(name+".filtered",std::ios::binary).write(reinterpret_cast<const char*>(filtered.data()),filtered.size());
        std::ofstream meta(name+".txt");meta<<stride<<"\n";
        for(unsigned i=0;i<10;++i)meta<<slots[i]<<" "<<cuffs[i]<<" "<<axes[i][0]<<" "<<axes[i][1]<<" "<<axes[i][2]<<"\n";
    }
    if(caps){static unsigned reports=0;if(reports++<6)log("[vr cuffs] capped %u closed wrist boundary loops",caps);}
    const u32 memory=hleAlloc(u32(std::max<size_t>(32,filtered.size())),32);if(!memory)return 0;
    if(!filtered.empty())std::memcpy(hostp(memory),filtered.data(),filtered.size());
    else std::memset(hostp(memory),0,32);
    filteredSize=u32(filtered.size());sSleeveLists.push_back({source,size,memory,filteredSize,hash});
    log("[vr gloves] sleeve packet filtered: %u -> %u bytes",size,filteredSize);return memory;
}
vr::Matrix matrixAt(u32 address) {
    auto matrix=vr::poseMatrix({});for(int k=0;k<12;++k)matrix[k]=gu::rdf(address+4*k);return matrix;
}
vr::Vec3 position(const vr::Matrix& m) {return {m[3],m[7],m[11]};}
float distance(vr::Vec3 a,vr::Vec3 b) {float out=0;for(int k=0;k<3;++k)out+=(a[k]-b[k])*(a[k]-b[k]);return std::sqrt(out);}
void writeMatrix(u32 address,const vr::Matrix& matrix) {for(int k=0;k<12;++k)gu::wrf(address+k*4,matrix[k]);}
bool diagnosticTools() {const char* value=std::getenv("LM_VR_TOOL_TEST");return diagnosticEnabled()&&value&&std::strcmp(value,"1")==0;}
vr::Pose testToolPose() {
    const float phase=float(copyDispCount())*0.02f,yaw=0.45f*std::sin(phase);
    return {{0,std::sin(yaw/2),0,std::cos(yaw/2)},{0.25f+0.08f*std::sin(phase),-0.4f+0.06f*std::cos(phase),-0.3f}};
}
// Match the tool's original right-hand handle attachment to the same wrist,
// palm direction and calibration used by the tracked glove, including left use.
bool trackedToolGrip(vr::Matrix& target,vr::GrabKind kind=vr::GrabKind::None) {
    unsigned hand=1;float trigger=0;
    vr::Matrix grip,aim;float heading=0,height=0,forward=0;
    if(diagnosticTools()) {
        const auto anchor=captureAnchor();if(!anchor.valid)return false;
        auto pose=testToolPose();hand=kind==vr::GrabKind::Flashlight?0:1;
        if(!hand)pose={{0,0,0,1},{-0.25f,-0.4f,-0.3f}};
        grip=aim=vr::poseMatrix(pose);heading=anchor.yaw;height=1.8f;forward=0.2f;
    } else {
        if(!vrhost::heldEquipmentControl(hand,trigger,kind)||!vrhost::controllerToolPose(grip,aim,heading,height,forward,kind))return false;
    }
    if(sReflectionPass) {
        if(sReflectedToolFrame!=copyDispCount()||!sReflectedToolValid[hand])return false;
        target=sReflectedToolGrip[hand];return true;
    }
    const float units=vrhost::worldUnitsPerMetre();const auto fit=vrhost::handPositionAdjustment();
    const unsigned index=kind==vr::GrabKind::Flashlight?0:kind==vr::GrabKind::Poltergust?1:toolIndex();
    // The glove and handle use one calibrated grip frame. The tool retains
    // its authored rotation relative to the hand; emission follows that model.
    auto contact=sGripContactsValid?sGripContacts[hand]:vr::Vec3{0,0,-sToolAttachments[index].palmReach};
    for(float& v:contact)v/=units;
    target=vr::equipmentContactFrame(grip,aim,hand,vrhost::handOutwardYaw(),contact,vrhost::handRoll(hand));
    for(unsigned r=0;r<3;++r)target[r*4+3]*=units;
    target[7]+=fit[0]*units;target[11]-=(fit[1]+forward)*units;
    const auto anchor=captureAnchor();if(!anchor.valid)return false;
    auto body=vr::poseMatrix({{0,std::sin(heading/2),0,std::cos(heading/2)},anchor.position});body[7]+=height*units;
    target=vr::multiply(body,target);return true;
}
bool holsteredToolGrip(vr::Matrix& target,vr::GrabKind kind) {
    float heading=0,height=0,forward=0;
    if(!vrhost::holsterPose(kind,target,heading,height,forward))return false;
    const float units=vrhost::worldUnitsPerMetre();const auto anchor=captureAnchor();if(!anchor.valid)return false;
    for(unsigned r=0;r<3;++r)target[r*4+3]*=units;target[11]-=forward*units;
    auto body=vr::poseMatrix({{0,std::sin(heading/2),0,std::cos(heading/2)},anchor.position});body[7]+=height*units;
    target=vr::multiply(body,target);return true;
}
bool nativeGripInverse(vr::Matrix& result,const vr::Matrix& inverseCamera,float& palmReach) {
    const u32 object=gu::inMem1(sPlayer+7)?rd32(sPlayer+4):0;
    if(!gu::inMem1(object+0x1B))return false;
    const u32 model=rd32(object+4),views=rd32(object+0x18);
    if(!gu::inMem1(model+0xD)||rd32(model)!=0x04B40000u||rd16(model+8)!=65||!gu::inMem1(views+20*48-1))return false;
    if(!sGripContactsValid) {
        const u32 inverseBind=rd32(model+0x38);if(!gu::inMem1(inverseBind+65*48-1))return false;
        for(unsigned h=0;h<2;++h) {
            const unsigned w=h?18:35;vr::Matrix wristBind,palmBind;
            if(!vr::invertAffine(matrixAt(inverseBind+w*48),wristBind)||!vr::invertAffine(matrixAt(inverseBind+(w+1)*48),palmBind))return false;
            const auto frame=vr::handTransform(position(wristBind),position(palmBind),{(h?-1.f:1.f)*wristBind[1],(h?-1.f:1.f)*wristBind[5],(h?-1.f:1.f)*wristBind[9]},vr::poseMatrix({}));
            std::array<vr::Matrix,3> middle;
            for(unsigned k=0;k<3;++k){vr::Matrix bind;if(!vr::invertAffine(matrixAt(inverseBind+(w+5+k)*48),bind))return false;middle[k]=vr::multiply(frame,bind);}
            vr::Matrix indexBind,littleBind;
            if(vr::invertAffine(matrixAt(inverseBind+(w+8)*48),indexBind)&&vr::invertAffine(matrixAt(inverseBind+(w+2)*48),littleBind))
                sGripHalfWidth=std::max(sGripHalfWidth,0.5f*distance(position(indexBind),position(littleBind)));
            sGripContacts[h]=vr::closedFingerGripCentre(middle,{0,1,0});
            log("[vr grip anatomy] hand %u closed grip centre %.3f %.3f %.3f",h,sGripContacts[h][0],sGripContacts[h][1],sGripContacts[h][2]);
        }
        sGripContactsValid=true;
    }
    const auto wrist=vr::multiply(inverseCamera,matrixAt(views+18*48));
    const auto palm=vr::multiply(inverseCamera,matrixAt(views+19*48));
    const auto handle=position(palm);palmReach=distance(position(wrist),handle);
    // Use exactly the same wrist/palm/up frame as the rendered right glove.
    // The native equipment actor already contains the authored handle offset.
    result=vr::handTransform(position(wrist),handle,{-wrist[1],-wrist[5],-wrist[9]},vr::poseMatrix({}));
    if(diagnosticTools()){static unsigned reports=0;if(reports++<2)log("[vr handle fit] wrist %.2f %.2f %.2f; native tip %.2f %.2f %.2f",handle[0],handle[1],handle[2],sNativeToolOrigin[0],sNativeToolOrigin[1],sNativeToolOrigin[2]);}
    return true;
}
vr::Vec3 transformed(const vr::Matrix& matrix,vr::Vec3 value,bool point) {
    vr::Vec3 out{};for(unsigned r=0;r<3;++r){out[r]=point?matrix[r*4+3]:0;for(unsigned k=0;k<3;++k)out[r]+=matrix[r*4+k]*value[k];}return out;
}




}
namespace {
constexpr s32 kEncounterEntry=0x54000000;
rarc::Bytes sEncounterBytes,sKeyGhostBytes,sRescueBytes,sSwitchBytes;
constexpr s32 kKeyGhostEntry=kEncounterEntry+1;
constexpr s32 kRescueEntry=kEncounterEntry+2;
constexpr s32 kSwitchEntry=kEncounterEntry+3;
std::atomic<int> sPresentationEvent{-1};
std::atomic<bool> sDoorAnimationResource{false};
vr::KeyGhostVisibility sKeyGhostVisibility;
bool keyGhostRunning() {
    if(!vrhost::diorama() || rd32(VA(0x804D80C8))!=2 ||
       sPresentationEvent.load(std::memory_order_relaxed)!=7){sKeyGhostVisibility={};return false;}
    const u32 cursor=rd32(VA(0x803D2D40)+0x0C);
    const bool active=cursor!=0;
    const u32 wait=rd32(VA(0x803D2D40)+0x1C);
    sKeyGhostVisibility.update(active,cursor,wait);
    if(active && sKeyGhostVisibility.introDone && !sKeyGhostVisibility.handoff && wait==4 && gu::inMem1(cursor)) {
        // At the last pre-CAMERAEND wait, the script has just exposed the real
        // key. Its scene prop is now hidden in that same native FRAMESTART.
        const size_t limit=std::min<size_t>(16384,0x81800000u-cursor);
        const char* text=reinterpret_cast<const char*>(hostp(cursor));
        const std::string_view tail(text,strnlen(text,limit));
        const auto end=tail.find("<CAMERAEND>"),nextWait=tail.find("<WAIT>");
        const bool handoff=end!=std::string_view::npos && (nextWait==std::string_view::npos || nextWait>end);
        sKeyGhostVisibility.update(active,cursor,wait,handoff);
    }
    return active;
}
bool hideKeyGhostPickup() {
    const bool active=keyGhostRunning();
    return sKeyGhostVisibility.hidePickup(active);
}
bool sBoundedWasActive=false,sBoundedEnded=false;
}
void notePlayerSceneResource(const char* path) {
    if(!path)return;
    std::string name(path);for(auto& c:name)c=char(std::tolower(static_cast<unsigned char>(c)));
    if(!name.ends_with(".scd")&&!name.ends_with(".sco"))return;
    sDoorAnimationResource.store(vr::doorAnimationResource(name),std::memory_order_relaxed);
}
bool doorAnimationActive(){return sDoorAnimationResource.load(std::memory_order_relaxed);}
bool gameOverActive() {
    // Read the real player, never the separate cutscene Luigi used as a camera anchor.
    return gu::inMem1(sPlayer) && gu::inMem1(sPlayer+0xFC) && s16(rd16(sPlayer+0xFC))<=0;
}
bool searchInspectionEvent() {
    return sPresentationEvent.load(std::memory_order_relaxed)==77 &&
        rd32(VA(0x803D2D40)+0x0C)!=0;
}
bool wellSceneHudHidden() {
    // The Astral Hall candle sequence must hide ordinary HUD while scripted.
    if(sPresentationEvent.load(std::memory_order_relaxed)==76 &&
       rd32(VA(0x803D2D40)+0x0C)!=0 && skip::cinematicCamera())return true;
    // Event 46 is the well glimpse of King Boo and Mario; require the
    // active script and native MENUOFF so a cached archive cannot hide play HUD.
    return sPresentationEvent.load(std::memory_order_relaxed)==46 &&
        rd32(VA(0x803D2D40)+0x0C)!=0 && rd32(VA(0x804D8088))!=0;
}
bool endingRescueActive() {return sPresentationEvent.load(std::memory_order_relaxed)==52 && rd32(VA(0x803D2D40)+0x0C)!=0;}
bool hideKeyGhostPlayerDraw(u32 actor) {
    const bool active=keyGhostRunning();
    return actor && actor==sPlayer && sKeyGhostVisibility.hidePlayer(active) && skip::cinematicCamera();
}
u32 hiddenKeyGhostPlayer(){return hideKeyGhostPlayerDraw(sPlayer)?sPlayer:0;}
bool ownsGameBoyHorror(){return gu::inMem1(sPlayer+0x793) && (rd16(sPlayer+0x792)&8u)!=0;}
bool rescueEventCamera() {return sPresentationEvent.load(std::memory_order_relaxed)==1;}
bool boundedDioramaCamera() {
    const int event=sPresentationEvent.load(std::memory_order_relaxed);
    const u32 flags=VA(0x803D2D40)+0x659;
    const bool graveyardRevealed=(rd8(flags+59/8)&(1u<<(59%8)))!=0;
    if(event<0||!vr::boundedDioramaEvent(unsigned(event),graveyardRevealed))return false;
    // An archive can remain loaded after the shot ends. Never carry its framing
    // into an unrelated door animation before the next archive is requested.
    const bool running=rd32(VA(0x803D2D40)+0x0C)!=0;
    if(!running){if(sBoundedWasActive)sBoundedEnded=true;return false;}
    sBoundedWasActive=true;
    return !sBoundedEnded;
}
bool phoneEventCamera() {
    const int event=sPresentationEvent.load(std::memory_order_relaxed);
    // A later door may reuse the renderer while the phone archive is cached.
    // The native script must still be running to identify a phone lead-in.
    return event>=0 && vr::phoneDialogueEvent(unsigned(event)) && rd32(VA(0x803D2D40)+0x0C)!=0;
}
bool conversationEventCamera() {
    const int event=sPresentationEvent.load(std::memory_order_relaxed);
    // Boolossus defeat waits before warping back to the balcony; no authored
    // camera is introduced. Keep that pause in the existing diorama camera.
    if(event>=0 && vr::dioramaBossReturnEvent(unsigned(event),rd32(VA(0x804D80C8))) &&
       rd32(VA(0x803D2D40)+0x0C)!=0)return true;
    return event>=0 && vr::dioramaDialogueEvent(unsigned(event));
}
s32 encounterOverlayEntry(const char* path) {
    // Native callers use both absolute/relative archive names and mixed case.
    if(path){
        std::string name(path);for(char& c:name){if(c=='\\')c='/';else c=char(std::tolower(static_cast<unsigned char>(c)));}
        if(!name.empty()&&name.front()=='/')name.erase(0,1);
        if(name.size()==17 && name.compare(0,11,"event/event")==0 &&
           name[11]>='0'&&name[11]<='9'&&name[12]>='0'&&name[12]<='9'&&name.compare(13,4,".szp")==0){
            const int event=(name[11]-'0')*10+name[12]-'0';
            sBoundedWasActive=false;sBoundedEnded=false;
            if(event==7)sKeyGhostVisibility={};
            if(sPresentationEvent.exchange(event,std::memory_order_relaxed)!=event && vrhost::enabled())
                log("[vr event] archive %02d",event);
        }
    }
    if(vrhost::diorama() && path && std::strcmp(path,"/Event/event07.szp")==0) {
        if(sKeyGhostBytes.empty()) {
            const auto disc=rarc::readDisc(path);if(disc.empty())return -1;
            rarc::Archive archive;if(!archive.parse(rarc::decompress(disc)))return -1;
            auto* item=archive.find("text/event07.txt");if(!item)return -1;
            std::string script(item->data.begin(),item->data.end());
            if(!vr::idleKeyGhostLuigi(script)||!vr::handoffKeyGhostKeys(script))return -1;
            item->data.assign(script.begin(),script.end());sKeyGhostBytes=rarc::yay0Store(archive.build());
            log("[vr key ghost] native standing animation and earlier scene/pickup key handoff applied");
        }
        return kKeyGhostEntry;
    }
    if(vrhost::diorama() && path && std::strcmp(path,"/Event/event01.szp")==0) {
        if(sRescueBytes.empty()) {
            const auto disc=rarc::readDisc(path);if(disc.empty())return -1;
            rarc::Archive archive;if(!archive.parse(rarc::decompress(disc)))return -1;
            auto* item=archive.find("text/event01.txt");if(!item)return -1;
            std::string script(item->data.begin(),item->data.end());
            if(!vr::hideEarlyRescueProfessor(script))return -1;
            item->data.assign(script.begin(),script.end());sRescueBytes=rarc::yay0Store(archive.build());
            log("[vr rescue] professor and equipment revealed at native cleaner cue");
        }
        return kRescueEntry;
    }
    if(vrhost::diorama() && path && std::strcmp(path,"/Event/event35.szp")==0) {
        if(sSwitchBytes.empty()) {
            const auto disc=rarc::readDisc(path);if(disc.empty())return -1;
            rarc::Archive archive;if(!archive.parse(rarc::decompress(disc)))return -1;
            auto* item=archive.find("text/event35.txt");if(!item)return -1;
            std::string script(item->data.begin(),item->data.end());
            if(!vr::visibleSwitchSceneLuigi(script))return -1;
            item->data.assign(script.begin(),script.end());sSwitchBytes=rarc::yay0Store(archive.build());
            log("[vr switch scene] Luigi remains visible during moving-wall camera");
        }
        return kSwitchEntry;
    }
    if(!vrhost::firstPerson()||!path||std::strcmp(path,"/Event/event61.szp")!=0)return -1;
    if(sEncounterBytes.empty()) {
        const auto disc=rarc::readDisc(path);if(disc.empty())return -1;
        rarc::Archive archive;if(!archive.parse(rarc::decompress(disc)))return -1;
        auto* item=archive.find("text/event61.txt");if(!item)return -1;
        std::string script(item->data.begin(),item->data.end());
        if(!vr::liveParlourEncounter(script)){log("[vr encounter] candle script signature did not match; retaining retail event");return -1;}
        item->data.assign(script.begin(),script.end());sEncounterBytes=rarc::yay0Store(archive.build());
        log("[vr encounter] candle/portrait encounter keeps live movement and first-person camera; story flags/messages retained");
    }
    return kEncounterEntry;
}
bool isEncounterOverlay(s32 entry){return entry==kEncounterEntry || entry==kKeyGhostEntry || entry==kRescueEntry || entry==kSwitchEntry;}
u32 encounterOverlayLength(s32 entry){return !isEncounterOverlay(entry)?0:u32((entry==kSwitchEntry?sSwitchBytes:entry==kRescueEntry?sRescueBytes:entry==kKeyGhostEntry?sKeyGhostBytes:sEncounterBytes).size());}
s32 encounterOverlayRead(s32 entry,void* dst,s32 length,s32 offset) {
    if(!isEncounterOverlay(entry)||!dst||length<0||offset<0)return -1;
    const auto& bytes=entry==kSwitchEntry?sSwitchBytes:entry==kRescueEntry?sRescueBytes:entry==kKeyGhostEntry?sKeyGhostBytes:sEncounterBytes;
    if(size_t(offset)>bytes.size())return -1;
    const auto count=std::min(size_t(length),bytes.size()-size_t(offset));
    if(count)std::memcpy(dst,bytes.data()+offset,count);return s32(count);
}
ReflectionScope::ReflectionScope(unsigned flags):previous_(sReflectionPass) {sReflectionPass=previous_||vr::reflectedActorPass(flags)||(diagnosticEnabled()&&std::getenv("LM_VR_MIRROR_POSE_TEST")&&copyDispCount()>1200);}
ReflectionScope::~ReflectionScope(){sReflectionPass=previous_;}
bool diagnosticEquipment(){return diagnosticTools();}
void setPlayer(u32 actor) {if(actor!=sPlayer){sHoseBuild={};sToolAxisValid=false;sToolAttachments={};sReflectedToolValid={};sGripContactsValid=false;}sPlayer=actor;}
void setEquipmentContext(std::uint64_t context){if(context!=sEquipmentContext){sEquipmentContext=context;sHoseBuild={};sToolAttachments={};sReflectedToolValid={};sGripContactsValid=false;}}
bool toolModelReady(vr::GrabKind kind){return sToolAttachments[kind==vr::GrabKind::Poltergust?1:0].valid;}
void drawOtherTool(Ctx& c,u32 source,bool transparent) {
    if(sDrawingOtherTool||(!vrhost::beltMode()&&!diagnosticTools())||!gu::inMem1(sPlayer+0x7A7))return;
    const u32 nozzle=rd32(sPlayer+0x7A4);if(!gu::inMem1(nozzle+7)||rd32(nozzle+4)!=source)return;
    const unsigned other=1-toolIndex();const auto& a=sToolAttachments[other];vr::Matrix grip;
    if(transparent&&other==0&&!diagnosticTools()){unsigned hand=0;float trigger=0;if(!vrhost::heldEquipmentControl(hand,trigger,vr::GrabKind::Flashlight)||trigger<0.65f)return;}
    if(!a.valid||!a.clone)return;
    if(!trackedToolGrip(grip,other?vr::GrabKind::Poltergust:vr::GrabKind::Flashlight)&&
       (sReflectionPass||other!=0||!holsteredToolGrip(grip,vr::GrabKind::Flashlight)))return;
    const u32 view=interp::viewMatrixSource();if(!gu::inMem1(view+47))return;
    const u32 views=rd32(a.clone+0x18),normals=rd32(a.clone+0x1C);
    for(unsigned j=0;j<a.gripViews.size();++j) {
        const auto m=vr::multiply(matrixAt(view),vr::multiply(grip,sReflectionPass&&a.nativeGripViews.size()==a.gripViews.size()?a.nativeGripViews[j]:a.gripViews[j]));writeMatrix(views+j*48,m);
        vr::Matrix inverse;if(vr::invertAffine(m,inverse)){auto normal=vr::poseMatrix({});for(unsigned r=0;r<3;++r)for(unsigned k=0;k<3;++k)normal[r*4+k]=inverse[k*4+r];writeMatrix(normals+j*48,normal);}
    }
    static unsigned reports=0;if(reports++<4)log("[vr dual tools] drew independent %s model",other?"vacuum":"flashlight");
    sDrawingOtherTool=true;const Ctx saved=c;
    gu::call(c,transparent?GU_SYM("fn_80059F6C"):GU_SYM("fn_80059C54"),{a.clone});c=saved;sDrawingOtherTool=false;
}
bool toolAim(u32 owner,vr::ToolTarget& target,const vr::Vec3* sourceOrigin,const vr::Vec3* sourceAxis,vr::GrabKind kind) {
    target={};
    if(!owner || owner!=sPlayer || pickupPresentation() || reactionPresentation() || skip::choiceMenuActive() ||
       (skip::cinematicCamera()&&!skip::dialogueCamera()))return false;
    if(diagnosticTools()) {
        const auto anchor=captureAnchor();if(!anchor.valid)return false;
        const auto pose=vr::poseMatrix(testToolPose());
        target=vr::controllerToolTarget(pose,pose,anchor.position,anchor.yaw,1.8f,0.2f,100,{0,0});
        vr::Matrix held;const auto& attachment=sToolAttachments[kind==vr::GrabKind::Flashlight?0:kind==vr::GrabKind::Poltergust?1:toolIndex()];
        if(attachment.valid&&trackedToolGrip(held,kind)) {
            target.origin=transformed(held,attachment.tip,true);target.direction=transformed(held,attachment.axis,false);
            static unsigned reports=0;if(reports++<3)log("[vr handle test] shared held-model and emission attachment exercised");
        }
        return target.tracked;
    }
    vr::Matrix grip,aim;float heading=0,height=0,forward=0;
    if(vrhost::controllerToolPose(grip,aim,heading,height,forward,kind)) {
        const auto anchor=captureAnchor();if(!anchor.valid)return false;
        target=vr::controllerToolTarget(grip,aim,anchor.position,heading,height,forward,vrhost::worldUnitsPerMetre(),vrhost::handPositionAdjustment());
        vr::Matrix heldGrip;const auto& attachment=sToolAttachments[kind==vr::GrabKind::Flashlight?0:kind==vr::GrabKind::Poltergust?1:toolIndex()];
        if(target.tracked&&attachment.valid&&trackedToolGrip(heldGrip,kind)) {
            target.origin=transformed(heldGrip,attachment.tip,true);
            target.direction=transformed(heldGrip,attachment.axis,false);
            const float norm=std::sqrt(target.direction[0]*target.direction[0]+target.direction[1]*target.direction[1]+target.direction[2]*target.direction[2]);
            if(norm>0.001f)for(float& v:target.direction)v/=norm;
        }
        if(target.tracked)return true;
    }
    return vrhost::gazeDirection(target.direction);
}
bool toolVectorValid(u32 address) {
    // Owned HLE vectors are outside retail MEM1. Accept only our exact vectors,
    // not arbitrary addresses in the host's larger memory window.
    return (gu::inMem1(address)&&gu::inMem1(address+11))||
        (sSuctionVectors&&(address==sSuctionVectors||address==sSuctionVectors+16))||
        (sVisualToolVectors&&(address==sVisualToolVectors||address==sVisualToolVectors+16));
}
u32 heldToolVector(u32 source,bool direction,u32 original) {
    if((!vrhost::firstPerson()&&!diagnosticTools())||!gu::inMem1(sPlayer+0x7DB)||rd32(sPlayer+0x7D8)!=source)return original;
    // Warm both resident meshes from untouched native attachment matrices
    // before a source getter can influence their actor update/culling.
    if(!sToolAttachments[0].valid||!sToolAttachments[1].valid)return original;
    unsigned hand=0;float trigger=0;
    if(!diagnosticTools()&&!vrhost::heldEquipmentControl(hand,trigger,vr::GrabKind::Poltergust))return original;
    vr::ToolTarget target;if(!toolAim(sPlayer,target,nullptr,nullptr,vr::GrabKind::Poltergust)||!target.tracked)return original;
    if(!sVisualToolVectors)sVisualToolVectors=hleAlloc(32,16);
    if(!sVisualToolVectors)return original;
    for(unsigned k=0;k<3;++k){gu::wrf(sVisualToolVectors+4*k,target.origin[k]);gu::wrf(sVisualToolVectors+16+4*k,target.direction[k]);}
    if(diagnosticTools()){static unsigned reports=0;if(reports++<3)log("[vr tools test] native nozzle source getter redirected to held lip (%s)",direction?"axis":"position");}
    return sVisualToolVectors+(direction?16:0);
}
void aimGameBoyCamera(u32 camera) {
    if((!vrhost::firstPerson()&&!diagnosticTools())||!gu::inMem1(camera+0xA3))return;
    vr::Matrix grip,aim;float heading=0,height=0,forward=0;
    if(diagnosticTools()) {grip=aim=vr::poseMatrix(testToolPose());height=1.8f;forward=0.2f;heading=captureAnchor().yaw;}
    else if(!vrhost::controllerToolPose(grip,aim,heading,height,forward,vr::GrabKind::GameBoy))return;
    const auto anchor=captureAnchor();if(!anchor.valid)return;
    const auto target=vr::controllerToolTarget(grip,aim,anchor.position,heading,height,forward,vrhost::worldUnitsPerMetre(),vrhost::handPositionAdjustment(),0);
    if(!target.tracked)return;
    for(unsigned k=0;k<3;++k){gu::wrf(camera+0x8C+4*k,target.origin[k]);gu::wrf(camera+0x98+4*k,target.origin[k]+target.direction[k]*100);gu::wrf(camera+0x80+4*k,target.up[k]);}
    static unsigned reports=0;if(reports++<3)log("[vr camera] Game Boy Horror search camera follows held controller");
}
void aimSuction(u32 record) {
    vr::ToolTarget target;
    if((!vrhost::firstPerson()&&!diagnosticEnabled())||!gu::inMem1(record+0x5F))return;
    const u32 nativeOrigin=rd32(record+0x1C),nativeAxis=rd32(record+0x20);
    if(!toolVectorValid(nativeOrigin)||!toolVectorValid(nativeAxis))return;
    sNativeToolOrigin={gu::rdf(nativeOrigin),gu::rdf(nativeOrigin+4),gu::rdf(nativeOrigin+8)};
    sNativeToolAxis={gu::rdf(nativeAxis),gu::rdf(nativeAxis+4),gu::rdf(nativeAxis+8)};
    sToolFrame=copyDispCount();sToolAxisValid=true;
    if(!toolAim(sPlayer,target,&sNativeToolOrigin,&sNativeToolAxis,vr::GrabKind::Poltergust))return;
    const float reach=gu::rdf(record+0x40);
    if(!std::isfinite(reach))return;
    // The native record borrows the part's position/direction vectors. Give the
    // VR ray its own storage; writing those borrowed vectors feeds a previous
    // eye/controller override back into the next native attachment update.
    if(!sSuctionVectors)sSuctionVectors=hleAlloc(32,16);
    if(!sSuctionVectors)return;
    const u32 origin=sSuctionVectors,axis=sSuctionVectors+16;
    wr32(record+0x1C,origin);wr32(record+0x20,axis);
    for(unsigned k=0;k<3;++k) {
        gu::wrf(origin+4*k,sNativeToolOrigin[k]);
        gu::wrf(axis+4*k,target.direction[k]);
        if(target.tracked)gu::wrf(origin+4*k,target.origin[k]);
        gu::wrf(record+0x2C+4*k,gu::rdf(origin+4*k)+reach*target.direction[k]);
    }
    if(target.tracked) {
        const float height=gu::rdf(record+0x14)+target.origin[1]-sNativeToolOrigin[1];
        if(std::isfinite(height))gu::wrf(record+0x14,height);
        // Firing/caught-object callbacks use the part's own direction pointer.
        const u32 part=sPlayer+0x80C,fireAxis=gu::inMem1(part+0x703)?rd32(part+0x700):0;
        if(gu::inMem1(fireAxis+11))for(unsigned k=0;k<3;++k)gu::wrf(fireAxis+4*k,target.direction[k]);
        static unsigned reports=0;if(reports++<3)log("[vr tools] native suction origin/axis follow right controller");
        if(diagnosticTools()) {
            static unsigned reports=0;
            if(reports++<3) {
                float error=0;for(unsigned k=0;k<3;++k){error=std::max(error,std::fabs(gu::rdf(origin+4*k)-target.origin[k]));error=std::max(error,std::fabs(gu::rdf(axis+4*k)-target.direction[k]));error=std::max(error,std::fabs(gu::rdf(record+0x2C+4*k)-(target.origin[k]+reach*target.direction[k])));}
                log("[vr tools test] native suction ray/endpoint residual %.6f; source %.2f %.2f %.2f axis %.3f %.3f %.3f",error,sNativeToolOrigin[0],sNativeToolOrigin[1],sNativeToolOrigin[2],sNativeToolAxis[0],sNativeToolAxis[1],sNativeToolAxis[2]);
            }
        }
    }

}
bool captureDamageAllowed(u32 ghost) {return vrhost::captureDamageAllowed(ghost);}
void noteCapturedGhost(u32 ghost) {
    if(!vrhost::firstPerson()||!gu::inMem1(ghost+0xFD)||!gu::inMem1(sPlayer+0x4F)||s16(rd16(ghost+0xFC))<=0)return;
    unsigned hand=0;float trigger=0;
    if(!vrhost::heldEquipmentControl(hand,trigger,vr::GrabKind::Poltergust)||trigger<0.65f)return;
    vrhost::captureFocus({gu::rdf(sPlayer+0x44),gu::rdf(sPlayer+0x48),gu::rdf(sPlayer+0x4C)},
        {gu::rdf(ghost+0x44),gu::rdf(ghost+0x48),gu::rdf(ghost+0x4C)},ghost);
}
void notePickup() {
    if(vrhost::firstPerson()) log("[vr pickup] key-get effect (late marker only)");
}
bool pickupPresentation() {
    bool active=false;
    if(vrhost::firstPerson() && gu::inMem1(sPlayer) && gu::inMem1(sPlayer+0xF53)) {
        const u32 ctrl=rd32(sPlayer+0x7E4);
        if(gu::inMem1(ctrl) && gu::inMem1(ctrl+0x317))
            active=vr::pickupPoseState(rd32(ctrl+0x310),rd32(ctrl+0x314));
    }
    if(active!=sPickupActive) {
        sPickupActive=active;
        log("[vr pickup] %s",active?"pickup pose started (native controller)":"restored first person (pose ended)");
    }
    return active;
}
bool reactionPresentation() {
    bool active=false;
    if((vrhost::firstPerson()||diagnosticEnabled()) && gu::inMem1(sPlayer+0x7E7)) {
        const u32 ctrl=rd32(sPlayer+0x7E4);
        if(gu::inMem1(ctrl+0x333))active=vr::startlePoseState(rd32(ctrl+0x310),rd32(ctrl+0x314),rd32(ctrl+0x330));
    }
    if(active!=sReactionActive) {
        sReactionActive=active;
        log("[vr reaction] %s",active?"native startle: stereo screen":"startle finished: restore first person immediately");
    }
    return active;
}
vr::PlayerAnchor captureAnchor() {
    if(skip::cinematicCamera() && sDemoAnchor.valid && copyDispCount()>=sDemoFrame && copyDispCount()-sDemoFrame<=1) return sDemoAnchor;
    vr::PlayerAnchor anchor;anchor.actorId=sPlayer;
    if(!gu::inMem1(sPlayer)||!gu::inMem1(sPlayer+0xF53)) return anchor;
    anchor.position={gu::rdf(sPlayer+0x44),gu::rdf(sPlayer+0x48),gu::rdf(sPlayer+0x4C)};
    const float x=gu::rdf(sPlayer+0xF48),z=gu::rdf(sPlayer+0xF50);
    anchor.yaw=std::atan2(-x,-z);anchor.valid=std::isfinite(x)&&std::isfinite(z)&&x*x+z*z>0.01f;
    for(float value:anchor.position) anchor.valid=anchor.valid&&std::isfinite(value);
    return anchor;
}
bool diagnosticEnabled() {
    static const bool enabled=[] {const auto* value=std::getenv("LM_VR_AVATAR_TEST");return value && std::strcmp(value,"1")==0;}();
    return enabled;
}
bool testDoorMovement(std::int8_t& x,std::int8_t& y) {
    static const bool enabled=std::getenv("LM_VR_DOOR_WALK_TEST")!=nullptr;
    if(!enabled||!diagnosticEnabled()||!sDoorTestWalk||!gu::inMem1(sPlayer+0x4F))return false;
    float dx=sDoorTestGoal[0]-gu::rdf(sPlayer+0x44),dz=sDoorTestGoal[2]-gu::rdf(sPlayer+0x4C);
    const float gap=std::hypot(dx,dz);if(!std::isfinite(gap)||gap>600||gap<20){sDoorTestWalk=false;x=y=0;return true;}
    // Align with the middle of the aperture first. A tiny tangent component in
    // a diagonal stick can fall below the native axis dead zone at a wall.
    const float tangent=(sDoorTestCenter[0]-gu::rdf(sPlayer+0x44))*sDoorTestTangent[0]+(sDoorTestCenter[2]-gu::rdf(sPlayer+0x4C))*sDoorTestTangent[2];
    if(std::fabs(tangent)>25){dx=sDoorTestTangent[0]*tangent;dz=sDoorTestTangent[2]*tangent;}
    const u32 source=interp::viewMatrixSource();if(!gu::inMem1(source+47))return false;
    const auto mapping=vr::locomotionMapping(matrixAt(source),std::atan2(-dx,-dz));x=y=0;
    vr::routeControllerStick(x,y,0,0.65f,true,mapping);return true;
}
void noteNativeHoseBuild(u32 rope,u32 up,float scale) {
    if(!sHoseRebuild&&gu::inMem1(sPlayer+0x7DB)) {const u32 manager=rd32(sPlayer+0x7D8);if(gu::inMem1(manager+0x1B)&&rd32(manager+0x18)==rope)sHoseBuild={rope,up,scale};}
}
NativeHosePose::NativeHosePose(Ctx& c,u32 player,unsigned pass) {
    if(player!=sPlayer||(pass!=0&&pass!=1&&pass!=2&&pass!=5)||pickupPresentation()||reactionPresentation()||(skip::cinematicCamera()&&!skip::dialogueCamera()))return;
    if(!vrhost::firstPerson()&&!diagnosticTools())return;
    const u32 manager=rd32(player+0x7D8);if(!gu::inMem1(manager+0x1B))return;
    const u32 rope=rd32(manager+0x18);if(rope!=sHoseBuild.rope||!gu::inMem1(rope+0x63)||!gu::inMem1(sHoseBuild.up+11))return;
    const u32 model=rd32(rope+0x5C),points=rd32(rope+4),count=rd32(rope+0xC);
    if(!std::isfinite(sHoseBuild.scale)||sHoseBuild.scale<=0||sHoseBuild.scale>100||count<3||count>128||!gu::inMem1(points+count*0x1C-1)||!gu::inMem1(model+0x63))return;
    const u32 data=rd32(model+0xC);if(!gu::inMem1(data+0x8F))return;
    const unsigned jointCount=rd16(rope+0x60)+count,envCount=rd16(data+0x78);
    if(jointCount>256||envCount>256)return;
    const auto& attachment=sToolAttachments[1];vr::Matrix held;
    if(!attachment.valid||attachment.gripViews.size()<3||!trackedToolGrip(held,vr::GrabKind::Poltergust))return;
    const u32 nozzle=rd32(player+0x7A4),playerObject=rd32(player+4),view=interp::viewMatrixSource();
    if(!gu::inMem1(nozzle+0x3F)||(rd32(nozzle+0x3C)!=6&&rd32(nozzle+0x3C)!=7)||!gu::inMem1(playerObject+0x1B)||!gu::inMem1(view+47))return;
    const u32 nozzleObject=rd32(nozzle+4);if(!gu::inMem1(nozzleObject+0x1B))return;
    const u32 nativeViews=rd32(nozzleObject+0x18),playerViews=rd32(playerObject+0x18),playerModel=rd32(playerObject+4);
    if(!gu::inMem1(nativeViews+3*48-1)||!gu::inMem1(playerViews+47)||!gu::inMem1(playerModel+0x3B))return;
    const u32 bind=rd32(playerModel+0x38);if(!gu::inMem1(bind+47))return;
    vr::Matrix inverseCamera,inverseNozzle;
    if(!vr::invertAffine(matrixAt(view),inverseCamera)||!vr::invertAffine(vr::multiply(inverseCamera,matrixAt(nativeViews+2*48)),inverseNozzle))return;
    const auto root=vr::multiply(inverseCamera,vr::multiply(matrixAt(playerViews),matrixAt(bind)));
    const float rootYaw=std::atan2(-root[2],-root[10]);const auto anchor=captureAnchor();if(!anchor.valid)return;
    const auto packDelta=vr::avatarBodyAlignment(vr::poseMatrix({}),anchor.position,rootYaw,diagnosticTools()?anchor.yaw:vrhost::bodyHeading());
    auto nozzleDelta=vr::multiply(held,vr::multiply(attachment.gripViews[2],inverseNozzle));
    if(sReflectionPass){vr::Matrix nativeGrip;float reach=0;if(nativeGripInverse(nativeGrip,inverseCamera,reach))nozzleDelta=vr::multiply(held,nativeGrip);}
    std::vector<vr::Vec3> native(count),curve;
    for(unsigned i=0;i<count;++i)native[i]={gu::rdf(points+i*0x1C),gu::rdf(points+i*0x1C+4),gu::rdf(points+i*0x1C+8)};
    if(!sReflectionPass) {
        vr::ToolTarget connector;connector.origin=transformed(held,attachment.rear,true);
        connector.direction=transformed(held,attachment.axis,false);connector.up={held[1],held[5],held[9]};connector.tracked=true;
        vr::Vec3 tangent{};for(unsigned k=0;k<3;++k)tangent[k]=native.back()[k]-native[native.size()-2][k];
        nozzleDelta=vr::alignTool(native.back(),tangent,{0,1,0},connector);
    }
    if(!vr::retargetHose(native,packDelta,nozzleDelta,curve))return;
    auto save=[&](u32 address,unsigned bytes){if(!bytes)return true;if(!gu::inMem1(address)||!gu::inMem1(address+bytes-1))return false;saved_.push_back({address,std::vector<u8>(hostp(address),hostp(address)+bytes)});return true;};
    if(!save(points,count*0x1C)||!save(rd32(model+0x54),jointCount*48)||!save(rd32(model+0x58),envCount*48)||!save(rd32(model+0x50),envCount))return;
    for(unsigned i=0;i<count;++i)for(unsigned k=0;k<3;++k)gu::wrf(points+i*0x1C+4*k,curve[i][k]);
    sHoseRebuild=true;gu::call(c,GU_SYM("fn_8008A504"),{rope,sHoseBuild.up},{sHoseBuild.scale});sHoseRebuild=false;
    static unsigned reports=0;if(reports++<3)log("[vr hose] rebuilt %u native ribs between pack and tracked nozzle; reflection/physics restored after draw",count);
}
NativeHosePose::~NativeHosePose() {for(auto i=saved_.rbegin();i!=saved_.rend();++i)std::memcpy(hostp(i->address),i->bytes.data(),i->bytes.size());}
bool openDoorCollision(u32 query,u32 sphere,u32 triangle) {
    if(!sDoorPlayerQuery||sOpenedDoors.empty()||!gu::inMem1(query+3)||!gu::inMem1(sphere+0x1B)||!gu::inMem1(triangle+0x17))return false;
    const u32 object=rd32(query);if(!gu::inMem1(object+7))return false;const u32 data=rd32(object+4);if(!gu::inMem1(data+0x2F))return false;
    const u32 normal=data+rd32(data+0x28)+rd16(triangle+6)*12,vertex=data+rd32(data+0x24)+rd16(triangle)*12;
    if(!gu::inMem1(normal+11)||!gu::inMem1(vertex+11))return false;
    const vr::Vec3 n{gu::rdf(normal),gu::rdf(normal+4),gu::rdf(normal+8)},v{gu::rdf(vertex),gu::rdf(vertex+4),gu::rdf(vertex+8)};
    if(!std::isfinite(n[1])||std::fabs(n[1])>0.05f)return false;
    const unsigned room=rd32(sPlayer+0xB4)>>24;
    for(u32 leaf:sOpenedDoors) {
        if(leaf<sDoorTable||leaf>=sDoorTable+sDoorCount*0x68||(leaf-sDoorTable)%0x68||!vr::ordinaryDoorClosable(rd8(leaf+1),rd32(leaf+12),rd8(leaf+0x2A),rd8(leaf+0x2B),room))continue;
        if(std::none_of(sDoorBridges.begin(),sDoorBridges.end(),[&](const DoorBridge& b){return b.leaf==leaf;}))continue;
        const auto base=matrixAt(leaf+0x2C);const vr::Vec3 center{gu::rdf(leaf+0x10),gu::rdf(leaf+0x14),gu::rdf(leaf+0x18)};
        const vr::Vec3 player{gu::rdf(sphere),gu::rdf(sphere+4),gu::rdf(sphere+8)};
        if(diagnosticEnabled()&&std::getenv("LM_VR_DOOR_TRACE")&&copyDispCount()%120==0&&std::fabs(player[0]-center[0])<250&&std::fabs(player[2]-center[2])<250) {
            static u32 last=0;if(last!=copyDispCount()){last=copyDispCount();log("[vr door surface] normal %.2f %.2f %.2f vertex %.2f %.2f %.2f sphere %.2f %.2f %.2f radius %.2f",n[0],n[1],n[2],v[0],v[1],v[2],player[0],player[1],player[2],gu::rdf(sphere+12));}
        }
        if(!vr::doorApertureAllows(base,center,n,v,player))continue;
        static unsigned reports=0;if(reports++<6)log("[vr door aperture] leaf %u admitted player collision query through open slab plane",rd8(leaf));
        return true;
    }
    return false;
}
bool doorThresholdContact(u32 query,u32 sphere,u32 triangle,u32 correction) {
    if(!sDoorPlayerQuery||sDoorBridges.empty()||!gu::inMem1(query+0x2B)||!gu::inMem1(sphere+0x1B)||!gu::inMem1(triangle+0x17)||!gu::inMem1(correction+11))return false;
    const u32 object=rd32(query);if(!gu::inMem1(object+7))return false;const u32 data=rd32(object+4);if(!gu::inMem1(data+0x2F))return false;
    const u32 normal=data+rd32(data+0x28)+rd16(triangle+6)*12,vertex=data+rd32(data+0x24)+rd16(triangle)*12;
    if(!gu::inMem1(normal+11)||!gu::inMem1(vertex+11)||!std::isfinite(gu::rdf(normal))||!std::isfinite(gu::rdf(normal+4))||!std::isfinite(gu::rdf(normal+8))||!std::isfinite(gu::rdf(vertex+4))||std::fabs(gu::rdf(normal))>0.01f||std::fabs(gu::rdf(normal+4)-1)>0.01f||std::fabs(gu::rdf(normal+8))>0.01f)return false;
    const vr::Vec3 p{gu::rdf(sphere),gu::rdf(sphere+4),gu::rdf(sphere+8)};const float radius=gu::rdf(sphere+12);const unsigned room=rd32(sPlayer+0xB4)>>24;
    for(const auto& bridge:sDoorBridges) {
        const u32 leaf=bridge.leaf;
        if(leaf<sDoorTable||leaf>=sDoorTable+sDoorCount*0x68||(leaf-sDoorTable)%0x68||!vr::ordinaryDoorClosable(rd8(leaf+1),rd32(leaf+12),rd8(leaf+0x2A),rd8(leaf+0x2B),room))continue;
        const auto base=matrixAt(leaf+0x2C);const vr::Vec3 center{gu::rdf(leaf+0x10),gu::rdf(leaf+0x14),gu::rdf(leaf+0x18)},axis{base[2],base[6],base[10]};
        if(!vr::doorApertureAllows(base,center,axis,center,p)||std::fabs(gu::rdf(vertex+4)-bridge.height)>2)continue;
        if(std::fabs((p[0]-center[0])*base[2]+(p[2]-center[2])*base[10])>65)continue;
        float penetration=0;if(!vr::doorThresholdContactDepth(p[1],radius,bridge.height,penetration))continue;
        // Supply one small virtual threshold contact through the native query
        // interface. Native actor grounding and collision response still run.
        wr32(query+0xC,normal);wr32(query+4,normal);
        gu::wrf(correction,0);gu::wrf(correction+4,penetration);gu::wrf(correction+8,0);
        static unsigned reports=0;if(reports++<4)log("[vr door threshold] native ground contact on door %u height %.2f",rd8(leaf),bridge.height);
        return true;
    }
    return false;
}
bool beginDoorPlayerQuery(u32 actor) {const bool old=sDoorPlayerQuery;sDoorPlayerQuery=actor==sPlayer&&gu::inMem1(actor+0xF53);return old;}
void endDoorPlayerQuery(bool old) {sDoorPlayerQuery=old;}
void traceNativeHose(u32 player,unsigned pass) {
    if(!std::getenv("LM_VR_MODEL_TRACE")||copyDispCount()!=2000||!gu::inMem1(player+0x7DB))return;
    const u32 manager=rd32(player+0x7D8);
    if(!gu::inMem1(manager+0x20))return;
    const u32 rope=rd32(manager+0x18);
    if(!gu::inMem1(rope+0xA0))return;
    const u32 draw=rd32(rope+0x5C),points=rd32(rope+4),count=rd32(rope+0xC);
    log("[vr hose trace] pass %u manager %08X rope %08X points %08X count %u draw %08X",pass,manager,rope,points,count,draw);
    if(gu::inMem1(draw+0x63))for(unsigned o=0;o<0x64;o+=4)log("[vr hose draw] +%02X %08X",o,rd32(draw+o));
    if(count<=128&&count&&gu::inMem1(points+count*0x1C-1))for(unsigned i:{0u,count/2,count-1})log("[vr hose point] %u %.2f %.2f %.2f",i,gu::rdf(points+i*0x1C),gu::rdf(points+i*0x1C+4),gu::rdf(points+i*0x1C+8));
}
float nativeFloorProbe(Ctx& c,vr::Vec3 point) {
    static u32 scratch=0;if(!scratch)scratch=hleAlloc(16,16);if(!scratch)return NAN;
    for(unsigned k=0;k<3;++k)gu::wrf(scratch+4*k,point[k]);
    const Ctx saved=c;c.r[3]=scratch;c.call(GU_SYM("fn_8005E9B0"));const float floor=float(c.f[1]);c=saved;return floor;
}
void validateDoorBridge(Ctx& c,u32 leaf) {
    sDoorBridges.erase(std::remove_if(sDoorBridges.begin(),sDoorBridges.end(),[&](const DoorBridge& b){return b.leaf==leaf;}),sDoorBridges.end());
    const auto base=matrixAt(leaf+0x2C);const vr::Vec3 center{gu::rdf(leaf+0x10),gu::rdf(leaf+0x14),gu::rdf(leaf+0x18)};
    auto a=center,b=center;for(unsigned k=0;k<3;++k){a[k]+=base[k*4+2]*120;b[k]-=base[k*4+2]*120;}a[1]+=100;b[1]+=100;
    const float left=nativeFloorProbe(c,a),right=nativeFloorProbe(c,b);
    float height=0;
    if(vr::doorThresholdHeight(left,right,center[1],height)) {
        sDoorBridges.push_back({leaf,height});log("[vr door threshold] leaf %u native floor samples %.2f %.2f agree",rd8(leaf),left,right);
    } else log("[vr door threshold] leaf %u rejected floor samples %.2f %.2f at expected %.2f",rd8(leaf),left,right,center[1]);
}
void worldFrame(Ctx& c) {
    static const bool trace=std::getenv("LM_VR_DOOR_TRACE")!=nullptr;
    static const int test=std::getenv("LM_VR_DOOR_TEST")?std::atoi(std::getenv("LM_VR_DOOR_TEST")):-1;
    struct Passage {u32 leaf;unsigned other;vr::DoorPassage crossing;};
    static std::vector<Passage> passages;
    static vr::WorldGesture gesture;auto& opened=sOpenedDoors;static u32 active=0,lastTable=0,lastMap=~0u,observed=0;static unsigned phase=0,expectedRoom=255;static bool closing=false,testClosing=false;
    static const bool returnTest=std::getenv("LM_VR_DOOR_RETURN_TEST")!=nullptr;
    static unsigned returnStage=0,entryTestRoom=255;static u32 journeyLeaf=0;
    static const bool cycleTest=std::getenv("LM_VR_DOOR_CYCLE_TEST")!=nullptr;
    static const bool closeTest=std::getenv("LM_VR_DOOR_CLOSE_TEST")!=nullptr;
    const bool synthetic=test>=0&&diagnosticEnabled();
    if(!gu::inMem1(sPlayer+0xF53)){opened.clear();sDoorBridges.clear();passages.clear();lastTable=0;lastMap=~0u;observed=0;expectedRoom=255;sDoorTestWalk=false;gesture.cancel();active=0;vrhost::worldContact({},false,0,false,false,false);return;}
    if(!trace&&!synthetic&&!vrhost::physicalDoors()&&opened.empty()){gesture.cancel();active=0;vrhost::worldContact({},false,0,false,false,false);return;}
    const u32 table=rd32(c.r[13]+VOFF(0x2A4,0x2B4));const unsigned count=rd16(c.r[13]+VOFF(0x2B0,0x2C0));
    if(!count||count>256||!gu::inMem1(table+count*0x68-1)){opened.clear();sDoorBridges.clear();passages.clear();lastTable=0;gesture.cancel();active=0;vrhost::worldContact({},false,0,false,false,false);return;}
    sDoorTable=table;sDoorCount=count;
    const u32 map=rd32(VA(0x804D80C8));
    if(table!=lastTable||map!=lastMap){opened.clear();sDoorBridges.clear();passages.clear();lastMap=map;gesture.cancel();active=0;observed=0;phase=0;expectedRoom=255;closing=false;testClosing=false;returnStage=0;entryTestRoom=255;journeyLeaf=0;sDoorTestWalk=false;lastTable=table;if(trace)log("[vr door trace] table %08X count %u room %u",table,count,rd32(sPlayer+0xB4)>>24);}
    if(synthetic&&expectedRoom<255&&copyDispCount()%120==0)log("[vr door test] walking room %u position %.1f %.1f %.1f expected %u",rd32(sPlayer+0xB4)>>24,gu::rdf(sPlayer+0x44),gu::rdf(sPlayer+0x48),gu::rdf(sPlayer+0x4C),expectedRoom);
    if(synthetic&&expectedRoom<255&&(rd32(sPlayer+0xB4)>>24)==expectedRoom) {
        if(returnTest&&returnStage==0) {
            log("[vr door test] outward native crossing reached room %u; walking back through the same open door",expectedRoom);
            for(unsigned k=0;k<3;++k)sDoorTestGoal[k]=2*sDoorTestCenter[k]-sDoorTestGoal[k];
            expectedRoom=entryTestRoom;returnStage=1;
        } else {
            log("[vr door test] %s native portal into room %u",returnTest?"returned across":"walked across",expectedRoom);
            expectedRoom=255;sDoorTestWalk=false;
            if(returnTest&&gu::inMem1(journeyLeaf+0x67)){wr32(journeyLeaf+12,vr::ordinaryDoorCloseFlags(rd32(journeyLeaf+12)));observed=journeyLeaf;closing=true;returnStage=2;}
        }
    }
    if(!synthetic&&!vrhost::physicalDoors()){
        // Return only doors opened by this adapter to their native closed pose.
        // Table/map identity is checked before touching any retained leaf.
        for(u32 d:opened)if(d>=table&&d<table+count*0x68&&(d-table)%0x68==0){const auto flags=rd32(d+12);
            if(!(flags&8)&&(flags&(0x10|0x40)))wr32(d+12,vr::ordinaryDoorCloseFlags(flags));}
        opened.clear();sDoorBridges.clear();passages.clear();gesture.cancel();active=0;vrhost::worldContact({},false,0,false,false,false);return;}
    if(!synthetic&&(skip::cinematicCamera()||skip::choiceMenuActive()||pickupPresentation()||reactionPresentation())){gesture.cancel();active=0;vrhost::worldContact({},false,0,false,false,false);return;}
    if(observed&&gu::inMem1(observed+0x67)){
        static unsigned samples=0;if(samples++<8||copyDispCount()%30==0)log("[vr door test] native leaf flags %08X animation %u frame %.2f",rd32(observed+12),rd16(observed+0x5C),gu::rdf(observed+0x60));
        if(closing&&(rd32(observed+12)&8)){log("[vr door test] native recovery closing completed");if(returnTest&&returnStage==2){log("[vr door test] round trip and native closing completed");returnStage=3;}observed=0;closing=false;}
        else if(!closing&&(rd32(observed+12)&0x10)&&!(rd32(observed+12)&0x40)){
            log("[vr door test] native open animation completed");
            if(cycleTest){observed=0;testClosing=true;phase=0;gesture.cancel();active=0;}
            else if(closeTest){wr32(observed+12,vr::ordinaryDoorCloseFlags(rd32(observed+12)));closing=true;}
            else {const unsigned room=rd32(sPlayer+0xB4)>>24;expectedRoom=room==rd8(observed+0x2A)?rd8(observed+0x2B):rd8(observed+0x2A);observed=0;sDoorTestWalk=true;}
        }
    }
    std::array<vr::HandInput,2> hands{};float heading=0,height=1.8f,forward=0.2f;
    const bool tracked=vrhost::worldHands(hands,heading,height,forward);
    const auto fit=vrhost::handPositionAdjustment();
    for(auto& hand:hands){hand.pose.position[1]+=fit[0];hand.pose.position[2]-=fit[1];}
    if(!tracked&&!synthetic){gesture.cancel();active=0;vrhost::worldContact({},false,0,false,false,false);return;}
    const auto anchor=captureAnchor();const float units=vrhost::worldUnitsPerMetre();
    if(!anchor.valid||!std::isfinite(units)||units<1)return;
    vr::Pose body{{0,std::sin(heading/2),0,std::cos(heading/2)},anchor.position};
    body.position[1]+=height*units;body.position[0]-=std::sin(heading)*forward*units;body.position[2]-=std::cos(heading)*forward*units;
    const auto referenceFromWorld=vr::inversePose(body);
    if(tracked&&!synthetic)for(auto& passage:passages) {
        const u32 d=passage.leaf;const unsigned room=rd32(sPlayer+0xB4)>>24;
        if(d<table||d>=table+count*0x68||(d-table)%0x68||!vr::ordinaryDoorClosable(rd8(d+1),rd32(d+12),rd8(d+0x2A),rd8(d+0x2B),room))continue;
        const auto base=matrixAt(d+0x2C);const float cx=gu::rdf(d+0x10),cz=gu::rdf(d+0x18);
        auto side=[&](vr::Vec3 p){return (p[0]-cx)*base[2]+(p[2]-cz)*base[10];};
        const auto world=vr::poseMatrix(body);std::array<float,2> clearance{};
        for(unsigned h=0;h<2;++h){auto p=hands[h].pose.position;for(float& v:p)v*=units;clearance[h]=hands[h].valid?side(transformed(world,p,true)):NAN;}
        if(passage.crossing.step(room,passage.other,side(anchor.position),clearance,1.f/30)) {
            wr32(d+12,vr::ordinaryDoorCloseFlags(rd32(d+12)));
            log("[vr door] closed door %u after player and both hands cleared the passage",rd8(d));
        }
    }
    vr::WorldTarget best;u32 leaf=0;unsigned nearHand=0;float nearest=1e9f;bool reverse=false;
    for(unsigned i=0;i<count;++i){const u32 d=table+i*0x68;const unsigned room=rd32(sPlayer+0xB4)>>24;
        const bool owned=std::find(opened.begin(),opened.end(),d)!=opened.end();
        const bool canClose=(!synthetic||testClosing)&&owned&&vr::ordinaryDoorClosable(rd8(d+1),rd32(d+12),rd8(d+0x2A),rd8(d+0x2B),room);
        if((!vr::ordinaryDoorEligible(rd8(d+1),rd32(d+12),rd8(d+0x2A),rd8(d+0x2B),room)&&!canClose)||(synthetic&&rd8(d)!=unsigned(test)))continue;
        const u32 anim=rd32(d+0x64);if(!gu::inMem1(anim+0x317)||rd32(anim+0x30C)!=3)continue;
        const u32 matrices=rd32(anim+0x128);if(!gu::inMem1(matrices+3*48-1))continue;
        const auto knob=matrixAt(matrices+2*48);bool sane=true;for(float v:knob)sane&=std::isfinite(v);
        if(!sane||distance(position(knob),{gu::rdf(d+0x10),gu::rdf(d+0x14),gu::rdf(d+0x18)})>500)continue;
        const auto local=vr::multiply(referenceFromWorld,knob);
        vr::Vec3 axis{local[2],local[6],local[10]};const float norm=std::sqrt(axis[0]*axis[0]+axis[1]*axis[1]+axis[2]*axis[2]);if(norm<0.5f||norm>2)continue;for(float& v:axis)v/=norm;
        for(unsigned side=0;side<2;++side){const u32 id=0x10000+i*2+side;if(active&&active!=id)continue;
            const float tip=side?32.f:-32.f;vr::Vec3 point{(local[3]+local[2]*tip)/units,(local[7]+local[6]*tip)/units,(local[11]+local[10]*tip)/units};
            if(distance(point,{0,0,0})>2.5f)continue;
            for(unsigned h=0;h<2;++h){if(!synthetic&&(!hands[h].valid||hands[h].occupied))continue;const float gap=synthetic?distance(point,{0,-0.6f,0}):distance(point,hands[h].pose.position);
                if(gap<nearest){nearest=gap;leaf=d;nearHand=h;reverse=side!=0;best={id,vr::WorldAction::Door,point,axis,0.20f,true,false};}}
        }
    }
    if(synthetic&&best.id&&copyDispCount()>900){
        for(auto& hand:hands){hand.valid=true;hand.grip=0;}hands[0].pose.position=hands[1].pose.position=best.contact;
        if(phase>=1)hands[0].grip=1;
        if(phase>=2){const float sn=std::sin(0.4f);hands[0].pose.orientation={best.axis[0]*sn,best.axis[1]*sn,best.axis[2]*sn,std::cos(0.4f)};}++phase;nearHand=0;
    }
    if(!best.id||(!synthetic&&!active&&nearest>0.45f)){gesture.cancel();active=0;vrhost::worldContact({},false,0,false,false,false);return;}
    const auto intent=gesture.update(best,hands);
    if(intent.began)active=best.id;
    if(intent.cancelled)active=0;
    const bool hover=nearest<=best.reach;vrhost::worldContact(best.contact,true,nearHand,hover,intent.began,intent.completed);
    const bool closingOwned=leaf&&std::find(opened.begin(),opened.end(),leaf)!=opened.end()&&vr::ordinaryDoorClosable(rd8(leaf+1),rd32(leaf+12),rd8(leaf+0x2A),rd8(leaf+0x2B),rd32(sPlayer+0xB4)>>24);
    if(intent.completed&&closingOwned) {
        // Reverse the existing native animation at its current frame. Retain
        // ownership until closed; no story/native-opened leaf can be closed here.
        wr32(leaf+12,vr::ordinaryDoorCloseFlags(rd32(leaf+12)));
        log("[vr door] hand %u closed physical wooden door %u",nearHand,rd8(leaf));
        if(synthetic){observed=leaf;closing=true;}
        gesture.cancel();active=0;
    }
    else if(intent.completed&&leaf&&vr::ordinaryDoorEligible(rd8(leaf+1),rd32(leaf+12),rd8(leaf+0x2A),rd8(leaf+0x2B),rd32(sPlayer+0xB4)>>24)){
        if(std::find(opened.begin(),opened.end(),leaf)==opened.end())opened.push_back(leaf);
        validateDoorBridge(c,leaf);
        const unsigned entry=rd32(sPlayer+0xB4)>>24;const auto placement=matrixAt(leaf+0x2C);
        const float signedDistance=(gu::rdf(sPlayer+0x44)-gu::rdf(leaf+0x10))*placement[2]+(gu::rdf(sPlayer+0x4C)-gu::rdf(leaf+0x18))*placement[10];
        passages.erase(std::remove_if(passages.begin(),passages.end(),[&](const Passage& p){return p.leaf==leaf;}),passages.end());
        Passage passage{leaf,entry==rd8(leaf+0x2A)?rd8(leaf+0x2B):rd8(leaf+0x2A),{}};passage.crossing.opened(entry,signedDistance);passages.push_back(passage);
        gu::wrf(leaf+0x60,0);wr32(leaf+12,vr::ordinaryDoorOpenFlags(rd32(leaf+12),reverse));
        log("[vr door] hand %u twisted native wooden door %u leaf %u; no cinema or button injection",nearHand,rd8(leaf),(leaf-table)/0x68);
        if(synthetic){observed=leaf;journeyLeaf=leaf;entryTestRoom=rd32(sPlayer+0xB4)>>24;const auto base=matrixAt(leaf+0x2C);
            vr::Vec3 center{gu::rdf(leaf+0x10),gu::rdf(leaf+0x14),gu::rdf(leaf+0x18)};
            const float dot=(gu::rdf(sPlayer+0x44)-center[0])*base[2]+(gu::rdf(sPlayer+0x4C)-center[2])*base[10];const float side=dot>0?-160.f:160.f;
            sDoorTestTangent={base[0],0,base[8]};
            // Test steering can sample the aperture without moving the actor.
            if(const char* offset=std::getenv("LM_VR_DOOR_TANGENT_TEST")) {
                const float amount=std::strtof(offset,nullptr);
                if(std::isfinite(amount)&&std::fabs(amount)<=200)for(unsigned k=0;k<3;++k)center[k]+=amount*sDoorTestTangent[k];
            }
            sDoorTestCenter=center;
            sDoorTestGoal={center[0]+side*base[2],center[1],center[2]+side*base[10]};
            log("[vr door test] ordinary walk goal %.1f %.1f %.1f",sDoorTestGoal[0],sDoorTestGoal[1],sDoorTestGoal[2]);}gesture.cancel();active=0;
    }
}
namespace {
u32 keyGhostPickupModel(Ctx& c) {
    if(!hideKeyGhostPickup())return 0;
    static u32 name=0,frame=~0u,model=0;
    if(!name){name=hleAlloc(16,16);if(!name)return 0;std::memcpy(hostp(name),"demo_key2",10);}
    const u32 now=copyDispCount();if(frame!=now) {
        frame=now;
        // Native EnManager lookup by CodeName returns the key's MoveObj.
        const u32 actor=gu::call(c,VOFF(0x800E97DC,0x800ECC08),{name});
        model=gu::inMem1(actor+7)?rd32(actor+4):0;
        if(model){static unsigned reports=0;if(reports++<3)log("[vr key cutaway] pickup model %08X hidden",model);}
    }
    return model;
}
}
DrawPose::DrawPose(Ctx& c,u32 object,bool shadowPass) {
    if(object && object==keyGhostPickupModel(c)){hidden_=true;return;}
    static const bool modelTrace=std::getenv("LM_VR_MODEL_TRACE")!=nullptr;
    if(modelTrace&&gu::inMem1(object+0x1F)) {
        if(copyDispCount()==2000)log("[vr model draw] object %08X caller %08X model %08X joints %08X views %08X shadow %u",object,c.lr,rd32(object+4),rd32(object+0x14),rd32(object+0x18),unsigned(shadowPass));
        static std::vector<u32> seen;const u32 data=rd32(object+4);
        if(gu::inMem1(data+0x73)&&seen.size()<128&&std::find(seen.begin(),seen.end(),data)==seen.end()){seen.push_back(data);
            log("[vr model trace] object %08X model %08X joints %u env %u shapes %u mats %u player %08X",object,data,rd16(data+8),rd16(data+12),rd16(data+0x26),rd16(data+0x28),sPlayer);
            for(unsigned i=0;i<256;++i){const u32 actor=rd32(VA(0x803D48A0)+i*4);if(gu::inMem1(actor+0x3F)&&rd32(actor+4)==object)log("[vr model trace] actor %08X type %u slot %u",actor,rd32(actor+0x3C),i);}
            if(gu::inMem1(sPlayer+0x7EF))for(unsigned off=0x794;off<=0x7EC;off+=4)log("[vr model trace] player +%03X = %08X",off,rd32(sPlayer+off));
        }
    }
    const bool fullReflection=sReflectionPass;
    // Equipment and authored fingers must share the same native mirror pose.
    // Mixing native tools with tracked wrists produced crossed, floating grips.
    const bool mirrorMotion=fullReflection&&vrhost::mirrorAvatarTracking();
    std::array<vr::HandInput,2> hands;float heading=0,height=0,forward=0;
    if(!gu::inMem1(sPlayer+0xF53) || !gu::inMem1(object+0x1F)) return;
    const bool isPlayer=rd32(sPlayer+4)==object;
    // Event 7's key-ghost cutaway parks Luigi but leaves his walk animation
    // running outside the authored frustum. Hide only his render model while
    // that scripted camera is active; native animation, script and input remain.
    if(isPlayer && hideKeyGhostPlayerDraw(sPlayer)) {
        hidden_=true;return;
    }
    const bool authoredPose=pickupPresentation() || reactionPresentation() || (skip::cinematicCamera() && !skip::dialogueCamera());
    const bool firstPersonView=vrhost::avatarTargets(hands,heading,height,forward) && !authoredPose;
    if(!firstPersonView && diagnosticEnabled()) {
        height=1.8f;forward=0.08f;
        heading=std::atan2(-gu::rdf(sPlayer+0xF48),-gu::rdf(sPlayer+0xF50));
        for(unsigned h=0;h<2;++h) {hands[h].valid=true;hands[h].pose.position={h?0.25f:-0.25f,-0.4f,-0.3f};hands[h].grip=h?0.9f:0.0f;hands[h].trigger=h?0.0f:0.9f;hands[h].fingerValid=31;hands[h].fingerCurl=h?std::array<float,5>{0.4f,0.1f,0.9f,0.2f,0.1f}:std::array<float,5>{0.2f,0.9f,0.1f,0.7f,0.3f};}
    }
    if(diagnosticTools() && !firstPersonView)hands[1].pose=testToolPose();
    const u32 model=rd32(object+4),joints=rd32(object+0x14),views=rd32(object+0x18),normals=rd32(object+0x1C),view=interp::viewMatrixSource();
    // Attached tools have their own model objects, outside Luigi's skeleton.
    bool attachment=false;
    for(unsigned offset:{0x7A4u,0x7A8u,0x7ACu}) {
        const u32 actor=rd32(sPlayer+offset);
        attachment|=gu::inMem1(actor+7)&&rd32(actor+4)==object;
    }
    if(attachment && hideKeyGhostPlayerDraw(sPlayer)) {hidden_=true;return;}
    const u32 device=rd32(sPlayer+0x7AC);
    if(firstPersonView && !shadowPass && !fullReflection && !vrhost::gameBoyGrabbable() &&
       gu::inMem1(device+7) && rd32(device+4)==object) {hidden_=true;return;}
    if(attachment && (firstPersonView || (diagnosticEnabled()&&!authoredPose))) {
        const u32 playerObject=rd32(sPlayer+4);
        if(!gu::inMem1(playerObject+0x1F)||!gu::inMem1(model+0x3B)||rd32(model)!=0x04B40000u||!gu::inMem1(view+47))return;
        const unsigned count=unsigned(rd16(model+8))+rd16(model+0xC);
        if(!count||count>512)return;
        const unsigned bytes=count*48;
        if(!gu::inMem1(joints+bytes-1)||!gu::inMem1(views+bytes-1)||!gu::inMem1(normals+bytes-1))return;
        const u32 playerModel=rd32(playerObject+4),playerViews=rd32(playerObject+0x18);
        if(!gu::inMem1(playerModel+0x3B)||!gu::inMem1(playerViews+47))return;
        const u32 bind=rd32(playerModel+0x38);
        vr::Matrix cameraInverse;
        if(!gu::inMem1(bind+47)||!vr::invertAffine(matrixAt(view),cameraInverse))return;
        const auto root=vr::multiply(cameraInverse,vr::multiply(matrixAt(playerViews),matrixAt(bind)));
        const float rootYaw=std::atan2(-root[2],-root[10]);
        const auto anchor=captureAnchor();if(!anchor.valid)return;
        const auto delta=vr::avatarBodyAlignment(vr::poseMatrix({}),anchor.position,rootYaw,diagnosticEnabled()?heading:vrhost::bodyHeading());
        auto equipmentDelta=delta;
        const u32 nozzle=rd32(sPlayer+0x7A4);
        vr::ToolTarget target;
        if(!fullReflection&&gu::inMem1(nozzle+7)&&rd32(nozzle+4)==object && sToolAxisValid &&
           copyDispCount()>=sToolFrame && copyDispCount()-sToolFrame<=1 && toolAim(sPlayer,target) && (!fullReflection || target.tracked)) {
            vr::Vec3 origin{},axis{},up{};
            const auto nativeWorld=vr::multiply(cameraInverse,matrixAt(views));
            for(unsigned r=0;r<3;++r) {
                origin[r]=delta[r*4+3];
                for(unsigned col=0;col<3;++col){origin[r]+=delta[r*4+col]*sNativeToolOrigin[col];axis[r]+=delta[r*4+col]*sNativeToolAxis[col];up[r]+=delta[r*4+col]*nativeWorld[col*4+1];}
            }
            if(!target.tracked){target.origin=origin;target.up=up;}
            equipmentDelta=vr::multiply(vr::alignTool(origin,axis,up,target),delta);
        }
        vr::Matrix heldGrip,inverseJoint;
        const ToolAttachment* heldAttachment=nullptr;
        if(gu::inMem1(nozzle+7)&&rd32(nozzle+4)==object&&sToolAxisValid&&
           copyDispCount()>=sToolFrame&&copyDispCount()-sToolFrame<=1&&
           vr::invertAffine(matrixAt(joints),inverseJoint)) {
            const auto worldFromModel=vr::multiply(cameraInverse,vr::multiply(matrixAt(views),inverseJoint));
            auto& attachment=sToolAttachments[toolIndex()];vr::Matrix gripFromWorld;
            if(attachment.model!=model){attachment={};attachment.model=model;}
            // Calibrate once from untouched authored matrices. Reusing it for
            // both gameplay and drawing avoids animation/update-order drift.
            if(!attachment.valid&&!shadowPass&&!fullReflection&&nativeGripInverse(gripFromWorld,cameraInverse,attachment.palmReach)) {
                auto nativeTip=sNativeToolOrigin,nativeAxis=sNativeToolAxis,nativeRear=sNativeToolOrigin;
                const auto nativeGripFromWorld=gripFromWorld;
                // Measure the narrow handle and mouth in the actual drawn mesh.
                // Preserve the original barrel rotation relative to the hand.
                const u32 vertices=rd32(model+0x48),end=rd32(model+0x4C);
                if(rd16(model+8)==3&&end>vertices&&end-vertices<1048576&&gu::inMem1(vertices)&&gu::inMem1(end-1)) {
                    float low=100,high=60;
                    for(u32 at=vertices;at+11<end;at+=12){const float x=gu::rdf(at),y=gu::rdf(at+4),z=gu::rdf(at+8);if(std::isfinite(x)&&std::isfinite(y)&&std::isfinite(z)&&std::fabs(x)<64&&std::fabs(z)<64&&y>=60&&y<=100){low=std::min(low,y);high=std::max(high,y);}}
                    if(high-low>5) {
                        // Both retail tool draw packets explicitly load matrix 2.
                        // Matrix 1 is an animation parent, not the visible mesh.
                        const auto mesh=vr::multiply(cameraInverse,matrixAt(views+2*48));
                        // The narrow rear ring and next ring bound this tool's
                        // handle; keep that contact separate from the front lip.
                        float next=high;
                        for(u32 at=vertices;at+11<end;at+=12){const float y=gu::rdf(at+4);if(std::isfinite(y)&&y>low+0.25f&&y<next)next=y;}
                        float stemBack=high,stemFront=-1,stemRadius=0;
                        for(u32 at=vertices;at+11<end;at+=12) {
                            const float x=gu::rdf(at),y=gu::rdf(at+4),z=gu::rdf(at+8),radius=std::sqrt(x*x+z*z);
                            if(std::isfinite(y)&&std::isfinite(radius)&&y>=-1&&y<high-5&&radius>=3&&radius<=6.5f) {
                                stemBack=std::min(stemBack,y);stemFront=std::max(stemFront,y);stemRadius=std::max(stemRadius,radius);
                            }
                        }
                        // The long narrow handle starts near Y=0, not Y=60.
                        // Hold behind its front shoulder by half a glove width
                        // plus the shaft radius, leaving the bell clear of fingers.
                        float handleY=stemFront-stemBack>10?std::clamp(stemFront-sGripHalfWidth-stemRadius,stemBack+2,stemFront-2):(low+next)*0.5f;
                        // Measure the contact in the untouched authored holding
                        // pose before choosing an attachment on the mesh shaft.
                        vr::Matrix authoredHand;
                        if(sGripContactsValid&&vr::invertAffine(gripFromWorld,authoredHand)) {
                            const auto contact=transformed(authoredHand,sGripContacts[1],true);
                            float projected=0,residual=0;
                            if(vr::equipmentHandleProjection(mesh,contact,projected,residual)) {
                                if(diagnosticTools())log("[vr authored handle] tool %u shaft %.2f..%.2f contact %.2f residual %.2f previous %.2f",toolIndex(),stemBack,stemFront,projected,residual,handleY);
                                // Reject animation poses whose contact is outside
                                // the real shaft instead of snapping to the bell.
                                if(stemFront-stemBack>10&&projected>=stemBack&&projected<=stemFront&&residual<=2*sGripHalfWidth+stemRadius)
                                    handleY=std::clamp(projected,stemBack+2,stemFront-2);
                            }
                        }
                        nativeRear=transformed(mesh,{0,stemBack,0},true);
                        nativeTip=transformed(mesh,{0,high,0},true);nativeAxis={mesh[1],mesh[5],mesh[9]};
                        // Keep Luigi's authored barrel-to-hand rotation. Only
                        // move the source contact to the measured handle center.
                        // Runtime aim axes differ from grip axes on real controllers.
                        // Native animation may currently be stowing the nozzle.
                        // Orient the measured mesh barrel toward calibrated grip
                        // forward instead of inheriting that animation's rotation.
                        gripFromWorld=vr::equipmentMeshGripInverse(mesh,handleY);
                        if(diagnosticTools())log("[vr handle axis] native %.3f %.3f %.3f mesh %.3f %.3f %.3f; lip distance %.3f",sNativeToolAxis[0],sNativeToolAxis[1],sNativeToolAxis[2],nativeAxis[0],nativeAxis[1],nativeAxis[2],distance(nativeTip,sNativeToolOrigin));
                        log("[vr handle] mesh-2 handle %.2f / emission lip %.2f; authored hand orientation",handleY,high);
                    }
                }
                attachment.gripFromModel=vr::multiply(gripFromWorld,worldFromModel);
                attachment.tip=transformed(gripFromWorld,nativeTip,true);
                attachment.rear=transformed(gripFromWorld,nativeRear,true);
                attachment.axis=transformed(gripFromWorld,nativeAxis,false);
                attachment.nativeGripViews.resize(count);
                for(unsigned j=0;j<count;++j)attachment.nativeGripViews[j]=vr::multiply(nativeGripFromWorld,vr::multiply(cameraInverse,matrixAt(views+j*48)));
                attachment.gripViews.resize(count);
                for(unsigned j=0;j<count;++j)attachment.gripViews[j]=vr::multiply(gripFromWorld,vr::multiply(cameraInverse,matrixAt(views+j*48)));
                if(count<=64) {
                    auto& memory=sToolCloneMemory[toolIndex()];if(!memory)memory=hleAlloc(0x40+3*64*48+64*16,32);
                    if(memory) {
                        attachment.clone=memory;std::memcpy(hostp(memory),hostp(object),0x2C);
                        const u32 cursor=memory+0x40+3*64*48;std::memset(hostp(cursor),0,64*16);wr32(memory+0x28,cursor);
                        for(unsigned array=0;array<3;++array){const u32 dest=memory+0x40+array*64*48;wr32(memory+0x14+array*4,dest);const u32 source=array==0?joints:array==1?views:normals;std::memcpy(hostp(dest),hostp(source),count*48);}
                    }
                }
                attachment.valid=true;
                log("[vr handle] captured native %s grip/model/muzzle attachment",toolIndex()?"vacuum":"flashlight");
            }
            vr::Matrix modelFromWorld;
            const auto kind=toolIndex()?vr::GrabKind::Poltergust:vr::GrabKind::Flashlight;
            const bool heldPose=trackedToolGrip(heldGrip,kind)||(!fullReflection&&!toolIndex()&&holsteredToolGrip(heldGrip,kind));
            if(attachment.valid&&attachment.gripViews.size()==count&&heldPose&&vr::invertAffine(worldFromModel,modelFromWorld)) {
                equipmentDelta=vr::multiply(heldGrip,vr::multiply(attachment.gripFromModel,modelFromWorld));
                heldAttachment=&attachment;
                if(fullReflection){static unsigned reports=0;if(reports++<3)log("[vr mirror tools] held model follows connected reflected glove contact");}
            }
        }
        const u32 gameBoy=rd32(sPlayer+0x7AC);vr::Matrix deviceMount;
        if(gu::inMem1(gameBoy+7)&&rd32(gameBoy+4)==object&&count>=3&&holsteredToolGrip(deviceMount,vr::GrabKind::GameBoy)) {
            const u32 vertices=rd32(model+0x48),end=rd32(model+0x4C);
            if(end>vertices&&end-vertices<1048576&&gu::inMem1(vertices)&&gu::inMem1(end-1)) {
                vr::Vec3 low{1e6f,1e6f,1e6f},high{-1e6f,-1e6f,-1e6f};bool sane=true;
                for(u32 at=vertices;at+11<end;at+=12)for(unsigned k=0;k<3;++k){const float v=gu::rdf(at+4*k);sane&=std::isfinite(v)&&std::fabs(v)<500;low[k]=std::min(low[k],v);high[k]=std::max(high[k],v);}
                if(sane){vr::Vec3 center{};for(unsigned k=0;k<3;++k)center[k]=(low[k]+high[k])*0.5f;
                    const auto current=transformed(equipmentDelta,transformed(vr::multiply(cameraInverse,matrixAt(views+2*48)),center,true),true);
                    for(unsigned k=0;k<3;++k)equipmentDelta[k*4+3]+=deviceMount[k*4+3]-current[k];}
            }
        }
        std::vector<vr::Matrix> mirrorHeldViews;
        if(fullReflection&&heldAttachment) {
            vr::Matrix nativeGrip;float reach=0;
            if(nativeGripInverse(nativeGrip,cameraInverse,reach))for(unsigned j=0;j<count;++j)mirrorHeldViews.push_back(vr::multiply(nativeGrip,vr::multiply(cameraInverse,matrixAt(views+j*48))));
        }
        const auto viewDelta=vr::multiply(matrixAt(view),vr::multiply(equipmentDelta,cameraInverse));
        object_=object;joints_=joints;views_=views;normals_=normals;
        saved_.assign(hostp(joints),hostp(joints)+bytes);
        savedViews_.assign(hostp(views),hostp(views)+bytes);
        savedNormals_.assign(hostp(normals),hostp(normals)+bytes);
        for(unsigned j=0;j<count;++j) {
            const auto transformed=heldAttachment?vr::multiply(matrixAt(view),vr::multiply(heldGrip,mirrorHeldViews.size()==count?mirrorHeldViews[j]:heldAttachment->gripViews[j])):vr::multiply(viewDelta,matrixAt(views+j*48));
            writeMatrix(views+j*48,transformed);
            vr::Matrix inverse;
            if(vr::invertAffine(transformed,inverse)) {
                auto normal=vr::poseMatrix({});
                for(unsigned r=0;r<3;++r)for(unsigned col=0;col<3;++col)normal[r*4+col]=inverse[col*4+r];
                writeMatrix(normals+j*48,normal);
            }
        }
        static unsigned reports=0;
        if(reports++<6)log("[vr equipment] aligned attached model %08X (%u matrices)",object,count);
        return;
    }
    // Exact gameplay rig. Other demo skeletons, menus and model replacements fail closed.
    if(!gu::inMem1(model+0x3C)||rd32(model)!=0x04B40000u||rd16(model+8)!=65||rd16(model+0xC)!=55||!gu::inMem1(joints)||!gu::inMem1(joints+120*48-1)||!gu::inMem1(view+47)) return;
    if(!isPlayer) {
        if(!skip::cinematicCamera()) return;
        const u32 playerObject=rd32(sPlayer+4);
        if(!gu::inMem1(playerObject+7)) return;
        const u32 playerModel=rd32(playerObject+4);
        if(!gu::inMem1(playerModel+0x3B)) return;
        const u32 bind=rd32(model+0x38),playerBind=rd32(playerModel+0x38);
        if(!gu::inMem1(bind)||!gu::inMem1(bind+65*48-1)||!gu::inMem1(playerBind)||!gu::inMem1(playerBind+65*48-1)) return;
        if(std::memcmp(hostp(bind),hostp(playerBind),65*48)!=0) return;
    }
    if(!gu::inMem1(views)||!gu::inMem1(views+120*48-1)||!gu::inMem1(normals)||!gu::inMem1(normals+120*48-1)) return;
    vr::Matrix inverseJoint,inverseCamera,modelFromWorld;
    if(!vr::invertAffine(matrixAt(joints),inverseJoint)||!vr::invertAffine(matrixAt(view),inverseCamera)) return;
    // V_j = cameraFromWorld * worldFromModel * J_j. Recover the owner transform
    // from its untouched root matrices rather than assuming joints are world-space.
    const auto modelView=vr::multiply(matrixAt(views),inverseJoint);
    const auto worldFromModel=vr::multiply(inverseCamera,modelView);
    // Validate the recovered owner transform against another untouched joint.
    const auto check=vr::multiply(modelView,matrixAt(joints+16*48));
    const auto actual=matrixAt(views+16*48);
    float residual=0;for(int k=0;k<12;++k) residual=std::max(residual,std::fabs(check[k]-actual[k]));
    if(!std::isfinite(residual)||residual>0.1f) return;
    if(!vr::invertAffine(worldFromModel,modelFromWorld)) return;
    // A mirror can reflect the camera or the model owner. Neither draw is
    // the local eye view: retain the complete native avatar in that pass.
    // This occurs before face materials, sleeve packets or joints are touched.
    if(!shadowPass && !fullReflection && (firstPersonView||diagnosticEnabled()) &&
       (vr::mirroredTransform(matrixAt(view)) || vr::mirroredTransform(worldFromModel))) {
        static unsigned reports=0;
        if(reports++<4)log("[vr reflection] retained full native Luigi head and arms for reflected draw %08X",object);
        return;
    }

    if(!isPlayer) {
        const auto rootWorld=vr::multiply(inverseCamera,matrixAt(views));
        sDemoAnchor.position=position(rootWorld);sDemoAnchor.yaw=std::atan2(-rootWorld[2],-rootWorld[10]);
        sDemoAnchor.valid=true;sDemoAnchor.actorId=object;sDemoFrame=copyDispCount();
        for(float v:rootWorld) sDemoAnchor.valid=sDemoAnchor.valid&&std::isfinite(v);
        static u32 previousDemo=0;
        if(previousDemo!=object) {previousDemo=object;log("[vr avatar] scripted Luigi anchor %08X at %.1f %.1f %.1f",object,sDemoAnchor.position[0],sDemoAnchor.position[1],sDemoAnchor.position[2]);}
    }
    if(!firstPersonView && !diagnosticEnabled()) return;
    const char* faceTest=std::getenv("LM_VR_FACE_TEST");
    const bool hideFace=!shadowPass && !fullReflection && (firstPersonView || (diagnosticEnabled() && faceTest && std::strcmp(faceTest,"1")==0));
    if(hideFace && rd16(model+0x28)==16 && rd16(model+0x26)==19) {
        const u32 materials=rd32(model+0x68);
        if(gu::inMem1(materials)&&gu::inMem1(materials+16*0x120-1)) {
            // Verified retail shapes: these materials are used only by face,
            // nose, eyes, ears and hat. Torso, sleeves and gloves stay visible.
            for(unsigned material:{2u,3u,4u,5u,6u,8u,9u,10u,11u,12u,14u}) {
                const u32 address=materials+material*0x120+8;
                materialFlags_.push_back({address,rd8(address)});wr8(address,rd8(address)|2);
            }
        }
    }
    static unsigned faceReports=0;
    if(!materialFlags_.empty() && faceReports++<2) log("[vr avatar] hid %u face-only materials; body and gloves retained",unsigned(materialFlags_.size()));
    // White gloves have separate shapes; retain the body mesh while filtering
    // triangles whose skin weights belong to the connecting sleeves.
    const bool shortForearms=vrhost::shortForearms();
    std::array<int,120> forearmOwner;forearmOwner.fill(-1);
    forearmOwner[17]=1;forearmOwner[34]=0;
    const u32 shapes=rd32(model+0x70),packetTable=rd32(model+0x34),counts=rd32(model+0x44),indices=rd32(model+0x40),weights=rd32(model+0x3C);
    if(!fullReflection && gu::inMem1(shapes+19*8-1)&&gu::inMem1(counts+54)&&gu::inMem1(indices)&&gu::inMem1(weights)) {
        std::array<bool,120> sleeve{};
        for(unsigned j:{15u,16u,32u,33u})sleeve[j]=true;
        sleeve[17]=!shortForearms||!hands[1].valid;sleeve[34]=!shortForearms||!hands[0].valid;
        unsigned pair=0;bool valid=true;
        for(unsigned env=0;env<55;++env) {
            const unsigned count=rd8(counts+env);float armWeight=0;std::array<float,2> lowerWeight{};
            if(count>65||pair+count>4096||!gu::inMem1(indices+(pair+count)*2)||!gu::inMem1(weights+(pair+count)*4)) {valid=false;break;}
            for(unsigned k=0;k<count;++k) {
                const auto joint=rd16(indices+(pair+k)*2);const float weight=gu::rdf(weights+(pair+k)*4);
                if(joint>=65||!std::isfinite(weight)||weight<0||weight>1) {valid=false;break;}
                if(sleeve[joint])armWeight+=weight;
                if(joint==17)lowerWeight[1]+=weight;
                if(joint==34)lowerWeight[0]+=weight;
            }
            sleeve[65+env]=armWeight>=0.499f;
            if(lowerWeight[0]>=0.499f)forearmOwner[65+env]=0;
            else if(lowerWeight[1]>=0.499f)forearmOwner[65+env]=1;
            pair+=count;
        }
        // Keep the rigid white cuff envelopes when green forearms are removed.
        if(!shortForearms){if(hands[1].valid)sleeve[68]=false;if(hands[0].valid)sleeve[72]=false;}
        const unsigned count=rd16(shapes+4*8+4),first=rd16(shapes+4*8+6);
        const u32 flags=rd32(shapes+4*8);
        const unsigned stride=3+2+(rd16(model+0x14)?2:0)+(rd16(model+0x16)?2:0)+((flags&0x02000000)?6:(rd16(model+0x12)?2:0));
        std::array<bool,10> slots{},cuffs{};std::array<vr::Vec3,10> cuffAxes{};
        if(valid&&count<=64&&first+count<=256&&gu::inMem1(packetTable+(first+count)*32-1))for(unsigned p=first;p<first+count;++p) {
            const u32 packet=packetTable+p*32;
            for(unsigned slot=0;slot<10;++slot) {const unsigned joint=rd16(packet+12+slot*2);if(joint!=0xFFFF) {if(joint>=120){valid=false;break;}slots[slot]=sleeve[joint];cuffs[slot]=!shortForearms&&((joint==68&&hands[1].valid)||(joint==72&&hands[0].valid));
                if(cuffs[slot]){const unsigned w=joint==68?18:35;vr::Matrix elbow,wrist;const u32 ib=rd32(model+0x38);if(vr::invertAffine(matrixAt(ib+(w-1)*48),elbow)&&vr::invertAffine(matrixAt(ib+w*48),wrist))for(unsigned k=0;k<3;++k)cuffAxes[slot][k]=wrist[k*4+3]-elbow[k*4+3];}}}
            if(!valid)break;
            if(diagnosticEnabled()){static unsigned reports=0;if(reports++<16){log("[vr cuff slots] packet %u stride %u cuff %d%d%d%d%d%d%d%d%d%d",p,stride,int(cuffs[0]),int(cuffs[1]),int(cuffs[2]),int(cuffs[3]),int(cuffs[4]),int(cuffs[5]),int(cuffs[6]),int(cuffs[7]),int(cuffs[8]),int(cuffs[9]));}}
            if(std::none_of(slots.begin(),slots.end(),[](bool v){return v;})&&std::none_of(cuffs.begin(),cuffs.end(),[](bool v){return v;}))continue;
            u32 size=0;const u32 dl=rd32(packet),originalSize=rd32(packet+4);
            if(const u32 filtered=filteredSleeveList(model+dl,originalSize,stride,slots,cuffs,cuffAxes,model,size)) {packets_.push_back({packet,dl,originalSize});wr32(packet,filtered-model);wr32(packet+4,size);}
        }
    }
    static unsigned layoutReports=0;
    if(diagnosticEnabled() && layoutReports++<2) log("[vr avatar] model-space check %.6f; pools %08X %08X %08X; actor %.1f %.1f %.1f",residual,joints,views,normals,gu::rdf(sPlayer+0x44),gu::rdf(sPlayer+0x48),gu::rdf(sPlayer+0x4C));
    const float units=vrhost::worldUnitsPerMetre();
    const auto anchor=captureAnchor();
    vr::Pose body{{0,std::sin(heading*0.5f),0,std::cos(heading*0.5f)},anchor.position};
    body.position[1]+=height*units;
    body.position[0]-=std::sin(heading)*forward*units;body.position[2]-=std::cos(heading)*forward*units;
    auto worldFromBody=vr::poseMatrix(body);
    object_=object;joints_=joints;views_=views;normals_=normals;
    saved_.resize(120*48);savedViews_.resize(120*48);savedNormals_.resize(120*48);
    std::memcpy(saved_.data(),hostp(joints),saved_.size());
    std::memcpy(savedViews_.data(),hostp(views),savedViews_.size());
    std::memcpy(savedNormals_.data(),hostp(normals),savedNormals_.size());
    // The original player can turn while walking independently of the VR view.
    // Rotate the whole avatar around its actor before solving arms in that frame.
    const u32 inverseBind=rd32(model+0x38);
    const auto rootSkin=vr::multiply(worldFromModel,vr::multiply(matrixAt(joints),matrixAt(inverseBind)));
    const float rootYaw=std::atan2(-rootSkin[2],-rootSkin[10]);
    const auto bodyAlignment=vr::avatarBodyAlignment(worldFromModel,anchor.position,rootYaw,diagnosticEnabled()?heading:vrhost::bodyHeading());
    for(unsigned j=0;j<65;++j) writeMatrix(joints+j*48,vr::multiply(bodyAlignment,matrixAt(joints+j*48)));
    if(mirrorMotion) {
        // The virtual eye can sit above Luigi's authored head. Map tracked
        // hand offsets to that visible head in reflections, not camera height.
        const auto headWorld=transformed(worldFromModel,position(matrixAt(joints+52*48)),true);
        if(std::isfinite(headWorld[1]))worldFromBody[7]=headWorld[1];
    }
    if(mirrorMotion) {
        vr::Matrix tilt;
        bool valid=vrhost::avatarHeadTilt(tilt);
        if(!valid && diagnosticEnabled()){tilt=vr::poseMatrix({{std::sin(0.125f),0,0,std::cos(0.125f)}, {}});valid=true;}
        if(valid) {
            const float yaw=diagnosticEnabled()?heading:vrhost::bodyHeading();
            const auto bodyRotation=vr::poseMatrix({{0,std::sin(yaw/2),0,std::cos(yaw/2)}, {}});
            vr::Matrix inverseBody;vr::invertAffine(bodyRotation,inverseBody);
            auto delta=vr::multiply(modelFromWorld,vr::multiply(bodyRotation,vr::multiply(tilt,vr::multiply(inverseBody,worldFromModel))));
            delta=vr::rotationAboutPoint(position(matrixAt(joints+51*48)),delta);
            // Verified native head branch: neck 51, skull/face/hat descendants 52..58.
            for(unsigned j=51;j<=58;++j)writeMatrix(joints+j*48,vr::multiply(delta,matrixAt(joints+j*48)));
        }
    }
    if(mirrorMotion&&sReflectedToolFrame!=copyDispCount()){sReflectedToolFrame=copyDispCount();sReflectedToolValid={};}
    std::array<vr::Matrix,2> sleeveSkin{},detachedSleeveSkin{};std::array<bool,2> sleeveValid{};

    std::array<std::array<vr::Matrix,3>,2> ringSkin{};std::array<bool,2> ringValid{};
    for(unsigned h=0;h<2;++h) {
        if((fullReflection&&!mirrorMotion) || !hands[h].valid)continue;
        auto hand=vr::calibratedHand(vr::poseMatrix(hands[h].pose),h,vrhost::handOutwardYaw(),vrhost::handRoll(h));hand[3]*=units;hand[7]*=units;hand[11]*=units;
        const auto adjustment=vrhost::handPositionAdjustment();
        hand[7]+=(adjustment[0]+(mirrorMotion?vrhost::mirrorHandHeight():0.f))*units;hand[11]-=adjustment[1]*units;
        auto target=vr::multiply(modelFromWorld,vr::multiply(worldFromBody,hand));
        const unsigned wrist=h?18:35,last=h?31:48;
        if(mirrorMotion) {
            const unsigned collar=wrist-3,upper=wrist-2,lower=wrist-1;
            const auto cm=matrixAt(joints+collar*48),um=matrixAt(joints+upper*48),lm=matrixAt(joints+lower*48);
            const auto root=position(cm),shoulder=position(um),elbow=position(lm),oldWrist=position(matrixAt(joints+wrist*48));
            const float yaw=diagnosticEnabled()?heading:vrhost::bodyHeading();
            const auto frame=vr::multiply(modelFromWorld,vr::poseMatrix({{0,std::sin(yaw/2),0,std::cos(yaw/2)}, {}}));
            const vr::Vec3 hint{h?0.6f:-0.6f,-0.7f,0.25f};vr::Vec3 bend{};
            for(unsigned r=0;r<3;++r)for(unsigned col=0;col<3;++col)bend[r]+=frame[r*4+col]*hint[col];
            const auto arm=vr::solveReflectedArm(root,shoulder,elbow,oldWrist,position(target),bend);
            if(arm.valid) {
                const auto plane=[](vr::Vec3 a,vr::Vec3 b,vr::Vec3 c){
                    vr::Vec3 u{},v{};for(unsigned k=0;k<3;++k){u[k]=b[k]-a[k];v[k]=c[k]-b[k];}
                    return vr::Vec3{u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]};
                };
                const auto oldPlane=plane(shoulder,elbow,oldWrist),newPlane=plane(arm.shoulder,arm.elbow,arm.wrist);
                writeMatrix(joints+collar*48,vr::multiply(vr::boneTransform(root,shoulder,root,arm.shoulder),cm));
                writeMatrix(joints+upper*48,vr::multiply(vr::limbTransform(shoulder,elbow,oldPlane,arm.shoulder,arm.elbow,newPlane),um));
                writeMatrix(joints+lower*48,vr::multiply(vr::limbTransform(elbow,oldWrist,oldPlane,arm.elbow,arm.wrist,newPlane),lm));
                // Reflected anatomy stays connected at extreme reaches. Local-eye
                // gloves retain their exact accepted tracking positions.
                static unsigned reports=0;
                if(diagnosticEnabled() && reports++<4)log("[vr mirror IK] %s wrist/controller distance %.6f; shoulder shift %.3f",h?"right":"left",distance(arm.wrist,position(target)),distance(arm.shoulder,shoulder));
                target[3]=arm.wrist[0];target[7]=arm.wrist[1];target[11]=arm.wrist[2];
            }
        }
        bool held=diagnosticTools();
        for(auto kind:{vr::GrabKind::Flashlight,vr::GrabKind::Poltergust}){unsigned holding=0;float trigger=0;if(vrhost::heldEquipmentControl(holding,trigger,kind)&&holding==h)held=true;}
        if(mirrorMotion&&held) {
            const auto original=matrixAt(joints+wrist*48);vr::Matrix authored;
            const auto fromNative=vr::handTransform(position(original),position(matrixAt(joints+(wrist+1)*48)),{(h?-1.f:1.f)*original[1],(h?-1.f:1.f)*original[5],(h?-1.f:1.f)*original[9]},vr::poseMatrix({}));
            if(vr::invertAffine(fromNative,authored))for(unsigned r=0;r<3;++r)for(unsigned k=0;k<3;++k)target[r*4+k]=authored[r*4+k];
        }
        if(mirrorMotion){static unsigned reports=0;if(reports++<8)log("[vr mirror fit] hand %u input Y %.3f target world Y %.2f head world Y %.2f; held %d",h,hands[h].pose.position[1],transformed(worldFromModel,position(target),true)[1],worldFromBody[7],int(held));}
        if(mirrorMotion&&sGripContactsValid) {
            sReflectedToolGrip[h]=vr::multiply(worldFromModel,target);

            sReflectedToolValid[h]=true;
        }
        const auto forearmDirection=target; // controller frame before the held-only grasp turn
        if(held&&!fullReflection&&sGripContactsValid)target=vr::toolHoldingHand(target,sGripContacts[h],h);
        const auto wm=matrixAt(joints+wrist*48);
        const auto gloveDelta=vr::handTransform(position(wm),position(matrixAt(joints+(wrist+1)*48)),
            {(h?-1.0f:1.0f)*wm[1],(h?-1.0f:1.0f)*wm[5],(h?-1.0f:1.0f)*wm[9]},target);
        writeMatrix(joints+wrist*48,vr::multiply(gloveDelta,wm));
        // Rebuild open fingers from the user's own bind matrices, avoiding
        // retail animation curls being added a second time to sensed curls.
        const auto wristFromBind=vr::multiply(matrixAt(joints+wrist*48),matrixAt(inverseBind+wrist*48));
        sleeveSkin[h]=wristFromBind;sleeveValid[h]=true;
        detachedSleeveSkin[h]=wristFromBind;
        vr::Matrix elbowBind,wristBind;
        if(!fullReflection&&shortForearms&&vr::invertAffine(matrixAt(inverseBind+(wrist-1)*48),elbowBind)&&vr::invertAffine(matrixAt(inverseBind+wrist*48),wristBind))
            {
            auto sleeveTarget=forearmDirection;
            for(unsigned r=0;r<3;++r)sleeveTarget[r*4+3]=target[r*4+3];
            detachedSleeveSkin[h]=vr::detachedForearmSkin(position(elbowBind),position(wristBind),sleeveTarget,0.16f*units);
        }
        // Retail cuffs blend 50/50 between forearm and wrist. Rebase the hidden
        // forearm influence into the same rigid glove frame so it cannot tug
        // cuff vertices back toward the body. Its sleeve triangles are hidden.
        vr::Matrix forearmBind;
        if(!mirrorMotion && vr::invertAffine(matrixAt(inverseBind+(wrist-1)*48),forearmBind))
            writeMatrix(joints+(wrist-1)*48,vr::multiply(shortForearms&&!fullReflection?detachedSleeveSkin[h]:wristFromBind,forearmBind));
        if(mirrorMotion&&held) {
            for(unsigned j=wrist+1;j<=last;++j)writeMatrix(joints+j*48,vr::multiply(gloveDelta,matrixAt(joints+j*48)));
            continue; // retain native authored finger pose around the native handle
        }
        for(unsigned j=wrist+1;j<=last;++j) {
            vr::Matrix bind;
            if(vr::invertAffine(matrixAt(inverseBind+j*48),bind))writeMatrix(joints+j*48,vr::multiply(wristFromBind,bind));
        }
        auto curls=vr::gloveCurls(hands[h]);
        if(held)curls=vr::toolGripCurls(hands[h]);
        const auto moved=matrixAt(joints+wrist*48);
        const vr::Vec3 back{(h?-1.0f:1.0f)*moved[1],(h?-1.0f:1.0f)*moved[5],(h?-1.0f:1.0f)*moved[9]};
        const unsigned bases[4]{wrist+11,wrist+8,wrist+5,wrist+2};
        const unsigned curlIndex[4]{0,1,2,4};
        for(unsigned digit=0;digit<4;++digit) {
            std::array<vr::Matrix,3> chain;
            for(unsigned j=0;j<3;++j)chain[j]=matrixAt(joints+(bases[digit]+j)*48);
            vr::curlFinger(chain,back,curls[curlIndex[digit]],digit==0);
            for(unsigned j=0;j<3;++j)writeMatrix(joints+(bases[digit]+j)*48,chain[j]);
        }
        // The visible ring finger is skinned by blended neighboring chains,
        // not a separate skeleton chain. Give its envelopes an independent pose.
        std::array<vr::Matrix,3> bindRing,posedRing;
        bool valid=true;
        for(unsigned j=0;j<3;++j) {
            vr::Matrix outer,middle;
            valid=valid&&vr::invertAffine(matrixAt(inverseBind+(wrist+2+j)*48),outer)&&vr::invertAffine(matrixAt(inverseBind+(wrist+5+j)*48),middle);
            if(!valid)break;
            bindRing[j]=middle;
            for(unsigned r=0;r<3;++r)bindRing[j][r*4+3]=(outer[r*4+3]+middle[r*4+3])*0.5f;
            posedRing[j]=vr::multiply(wristFromBind,bindRing[j]);
        }
        if(valid) {
            vr::curlFinger(posedRing,back,curls[3]);
            for(unsigned j=0;j<3;++j) {vr::Matrix inverse;valid=valid&&vr::invertAffine(bindRing[j],inverse);if(valid)ringSkin[h][j]=vr::multiply(posedRing[j],inverse);}
            ringValid[h]=valid;
        }
    }
    gu::call(c,GU_SYM("fn_80058850"),{object_}); // original guest skinning, unchanged
    // Mixed elbow/forearm envelopes must use the rigid wrist frame too;
    // otherwise retained sleeve vertices can still stretch toward the shoulder.
    if(shortForearms && !fullReflection) {
        unsigned rigid=0;
        for(unsigned j=65;j<120;++j) {
            const int h=forearmOwner[j];
            if(h>=0 && sleeveValid[unsigned(h)]){writeMatrix(joints+j*48,detachedSleeveSkin[unsigned(h)]);++rigid;}
        }
        static unsigned reports=0;
        if(rigid && reports++<4)log("[vr sleeves] short native forearms enabled; %u envelopes rigidly follow gloves",rigid);
    }
    for(unsigned h=0;h<2;++h)if(sleeveValid[h])
        writeMatrix(joints+(h?68:72)*48,sleeveSkin[h]); // rigid white cuff, full green forearm IK retained
    for(unsigned h=0;h<2;++h)if(ringValid[h]) {
        if(h) {writeMatrix(joints+71*48,ringSkin[h][1]);writeMatrix(joints+70*48,ringSkin[h][2]);}
        else {writeMatrix(joints+75*48,ringSkin[h][0]);writeMatrix(joints+76*48,ringSkin[h][1]);writeMatrix(joints+74*48,ringSkin[h][2]);}
    }
    static unsigned cuffReports=0;
    if(diagnosticEnabled() && !fullReflection && cuffReports++<2)for(unsigned h=0;h<2;++h)if(hands[h].valid) {
        const unsigned wrist=h?18:35,envelope=h?68:72;
        const auto expected=vr::multiply(matrixAt(joints+wrist*48),matrixAt(inverseBind+wrist*48));
        const auto actualCuff=matrixAt(joints+envelope*48);
        float error=0;for(unsigned k=0;k<12;++k)error=std::max(error,std::fabs(actualCuff[k]-expected[k]));
        log("[vr gloves] cuff skinning residual %.6f (%s)",error,h?"right":"left");
    }
    for(unsigned j=0;j<120;++j) {
        const auto transformed=vr::multiply(modelView,matrixAt(joints+j*48));
        writeMatrix(views+j*48,transformed);
        vr::Matrix inverse;
        if(vr::invertAffine(transformed,inverse)) {
            auto normal=vr::poseMatrix({});
            for(int r=0;r<3;++r) for(int col=0;col<3;++col) normal[r*4+col]=inverse[col*4+r];
            writeMatrix(normals+j*48,normal);
        }
    }
    static unsigned reflectionReports=0;
    if(fullReflection && reflectionReports++<4)log("[vr reflection] full head/arms retained; %s",mirrorMotion?"tracked gloves/fingers, arm IK and headset head tilt":"authored arms with VR body heading");
    static unsigned shadowReports=0;
    if(shadowPass && shadowReports++<2)log("[vr avatar] tracked glove pose applied to native shadow caster");
    static unsigned reports=0;
    if(reports++<4) log("[vr avatar] applied %s pose to player %08X model %08X; left wrist %.1f %.1f %.1f, right wrist %.1f %.1f %.1f",diagnosticEnabled()?"synthetic":"tracked",sPlayer,object_,gu::rdf(joints+18*48+12),gu::rdf(joints+18*48+28),gu::rdf(joints+18*48+44),gu::rdf(joints+35*48+12),gu::rdf(joints+35*48+28),gu::rdf(joints+35*48+44));
}
DrawPose::~DrawPose() {
    for(const auto& saved:packets_) {wr32(saved.address,saved.pointer);wr32(saved.address+4,saved.size);}
    for(const auto& [address,flags]:materialFlags_) wr8(address,flags);
    if(!object_) return;
    std::memcpy(hostp(joints_),saved_.data(),saved_.size());
    std::memcpy(hostp(views_),savedViews_.data(),savedViews_.size());
    std::memcpy(hostp(normals_),savedNormals_.data(),savedNormals_.size());
    static unsigned reports=0;
    if(diagnosticEnabled() && reports++<4) log("[vr avatar] restored joint/view/normal arrays byte-for-byte");
}
}



namespace recomp {
HLE(fn_8005E7FC) {
    const u32 actor=c.r[3];
    const bool trace=vravatar::diagnosticEnabled()&&std::getenv("LM_VR_DOOR_TRACE")&&gu::inMem1(actor+0xF53)&&rd32(actor+0x3C)==0;
    const float x=trace?gu::rdf(actor+0x44):0,z=trace?gu::rdf(actor+0x4C):0;
    const bool previous=vravatar::beginDoorPlayerQuery(actor);
    callOriginal(c,"fn_8005E7FC");
    vravatar::endDoorPlayerQuery(previous);
    if(trace&&copyDispCount()%120==0)log("[vr door collision] attempted %.2f %.2f resolved %.2f %.2f floor %u flags %08X",x,z,gu::rdf(actor+0x44),gu::rdf(actor+0x4C),rd16(actor+0xB2),rd32(actor+0x40));
}
}

namespace recomp {
HLE(fn_8008A504) {vravatar::noteNativeHoseBuild(c.r[3],c.r[4],float(c.f[1]));callOriginal(c,"fn_8008A504");}
HLE(fn_800BD870) {
    const u32 query=c.r[3],sphere=c.r[4],triangle=c.r[5],correction=c.r[6];
    if(vravatar::openDoorCollision(query,sphere,triangle)){c.r[3]=0;return;}
    callOriginal(c,"fn_800BD870");
    if(!c.r[3]&&vravatar::doorThresholdContact(query,sphere,triangle,correction))c.r[3]=1;
}
}

namespace recomp {
HLE(fn_8005E9B0) {callOriginal(c,"fn_8005E9B0");}
}

// Processing machines are original animated actors. Alter only temporary draw
// visibility, never scripts, motion, update flags outside the render pass or room geometry.
#include "recomp/processing_visibility.h"
#include "recomp/guest_util.h"
#include "recomp/version.h"
#include "recomp/vr_host.h"
#include "recomp/vr_avatar_guest.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <array>
#include <cmath>
namespace recomp::processing {
namespace {
std::string_view actorAction(u32 actor,u32 type) {
    // Native action-name lookup table, verified in both disc builds. The actor
    // stores its selected action at +0xC4; each 32-byte entry ends in a name.
    const u32 table=VOFF(0x8034A6C4,0x8033B70C)+type*8;
    const u32 action=rd32(actor+0xC4),count=rd32(table),entries=rd32(table+4);
    if(action>=count || count>1024 || !gu::inMem1(entries+action*32+31))return {};
    const u32 text=rd32(entries+action*32+28);
    if(!gu::inMem1(text+31))return {};
    const char* name=reinterpret_cast<const char*>(hostp(text));
    const void* end=std::memchr(name,0,32);
    return end?std::string_view(name,static_cast<const char*>(end)-name):std::string_view{};
}
}
MachineDrawScope::MachineDrawScope(unsigned pass) {
    static const bool test=std::getenv("LM_PROCESSING_VISIBILITY_TEST")!=nullptr;
    if((!vrhost::diorama()&&!test) || !vr::processingMachineMap(rd32(VA(0x804D80C8))) || (pass&0x30)==0x30)return;
    const bool ending=rd32(VA(0x804D80C8))==12 && vravatar::endingRescueActive();
    const unsigned count=std::min(rd32(VA(0x804D8750)),128u);
    const u32 table=VA(0x803D48A0),types=VA(0x8033B590);
    bool marioEmerging=false;
    if(ending)for(unsigned i=0;i<count;++i){
        const u32 actor=rd32(table+4*i);
        if(!gu::inMem1(actor+0xC7))continue;
        const u32 type=rd32(actor+0x3C);if(type>=512)continue;
        const u32 text=rd32(types+type*0x1C+0x18);
        if(gu::inMem1(text+7) && std::memcmp(hostp(text),"dmario",7)==0 &&
           vr::endingMarioHasEmerged(actorAction(actor,type)))marioEmerging=true;
    }
    static unsigned reports=0;
    static std::array<u32,128> seenActors{},seenFlags{},seenActions{};
    for(unsigned i=0;i<count;++i){
        const u32 actor=rd32(table+4*i);
        if(!gu::inMem1(actor+0xC7))continue;
        const u32 model=rd32(actor+4);if(!gu::inMem1(model+0x3F))continue;
        const u32 type=rd32(actor+0x3C);if(type>=512)continue;
        const u32 entry=types+type*0x1C;
        if(!gu::inMem1(entry+0x1B))continue;
        const u32 text=rd32(entry+0x18);if(!gu::inMem1(text+31))continue;
        const char* name=reinterpret_cast<const char*>(hostp(text));
        const void* end=std::memchr(name,0,32);if(!end)continue;
        const std::string_view actorName(name,static_cast<const char*>(end)-name);
        const u32 flags=rd32(actor+0x40);
        const auto action=ending?actorAction(actor,type):std::string_view{};
        const bool grate=ending && actorName=="edfuta";
        const bool parked=(ending && vr::endingParkedActor(actorName,action)) ||
            (grate && !marioEmerging);
        const bool revealed=ending && vr::endingRetainedActor(actorName,action);
        if(!vr::processingMachine(actorName) && !revealed && !parked)continue;
        // A parked NORMAL model may be visible outside the original camera's
        // frustum. Never expose it in the expanded diorama. Mario and his separate frame actor's EDLM still
        // obeys native concealment; continuity starts with his actual EDHS reveal.
        const u32 visible=parked ? flags|0x4000u :
            vr::processingMachineDrawFlags(flags,actorName=="edfuta");
        saved_.push_back({actor,flags,type,rd32(actor)});
        wr32(actor+0x40,visible);
        if(reports<70 && (seenActors[i]!=actor || seenFlags[i]!=(flags&0x6300u) || seenActions[i]!=rd32(actor+0xC4))){seenActors[i]=actor;seenFlags[i]=flags&0x6300u;seenActions[i]=rd32(actor+0xC4);++reports;log("[vr processing actor] %s actor %08X draw flags %08X -> %08X action %.*s",name,actor,flags,visible,int(action.size()),action.data()?action.data():"");}
    }
}
MachineDrawScope::~MachineDrawScope() {
    constexpr u32 mask=0x6300;
    for(auto it=saved_.rbegin();it!=saved_.rend();++it)
        if(rd32(it->actor)==it->vtable && rd32(it->actor+0x3C)==it->type)
            wr32(it->actor+0x40,(rd32(it->actor+0x40)&~mask)|(it->flags&mask));
}
}

namespace recomp::processing {
void MachineDrawScope::verifyCurrentView() const {
    static const bool test=std::getenv("LM_PROCESSING_VISIBILITY_TEST")!=nullptr;
    if(!test)return;
    static std::array<bool,512> reported{};
    const auto read=[](u32 address){auto m=vr::poseMatrix({});for(unsigned i=0;i<12;++i)m[i]=gu::rdf(address+i*4);return m;};
    const auto camera=read(VA(0x803A3820)+VOFF(0x18C,0x198));
    for(const auto& saved:saved_){
        // Intentionally concealed parked props do not calculate a current
        // model-view matrix in the native renderer and must not be validated.
        if(rd32(saved.actor+0x40)&0x6000u)continue;
        const u32 model=rd32(saved.actor+4),joints=rd32(model+0x14),views=rd32(model+0x18);
        if(!gu::inMem1(joints+47)||!gu::inMem1(views+47))continue;
        const auto expected=vr::multiply(vr::multiply(camera,read(saved.actor+8)),read(joints));
        const auto actual=read(views);float residual=0;
        for(unsigned i=0;i<12;++i)residual=std::max(residual,std::fabs(expected[i]-actual[i]));
        if(residual>0.1f)log("[vr processing view ERROR] type %u residual %.6f",saved.type,residual);
        else if(!reported[saved.type]){reported[saved.type]=true;log("[vr processing view] type %u current-camera/world root residual %.6f",saved.type,residual);}
    }
}
HLE(fn_80060C88) {
    MachineDrawScope visibility(0);
    callOriginal(c,"fn_80060C88");
    visibility.verifyCurrentView();
}
HLE(fn_80060D44) {
    MachineDrawScope visibility(0);
    callOriginal(c,"fn_80060D44");
}
}

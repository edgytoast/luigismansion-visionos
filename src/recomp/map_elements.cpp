// Optional map-only source annotations. All location data comes from the mounted disc.
#include "recomp/map_elements.h"
#include "recomp/rarc.h"
#include "recomp/guest_util.h"
#include "recomp/interp.h"
#include "recomp/hidden_mansion.h"
#include "recomp/version.h"
#include "port/settings.h"
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>
namespace recomp::mapelements {
namespace {
struct Source { int room,kind; std::array<float,3> position; };
std::vector<Source> sources;
int loadedVersion=-1;
struct Icon { alignas(32) std::array<u8,2048> pixels{};GXTexObj texture{};bool ready=false; };
std::array<Icon,3> icons;
void loadIcons() {
    rarc::Archive data,list;
    if(!data.parse(rarc::decompress(rarc::readDisc(ver::pal()?"/Game/sdata_e.arc":"/Game/sdata.arc"))))return;
    const auto* resource=data.find("res_list.szp");
    if(!resource || !list.parse(rarc::decompress(resource->data)))return;
    const char* names[]{"timg/list_item_0a.bti","timg/list_item_0b.bti","timg/list_item_0c.bti"};
    for(unsigned i=0;i<3;++i){
        const auto* image=list.find(names[i]);if(!image)continue;const auto& d=image->data;
        if(d.size()<32 || d[0]!=5 || d[2]!=0 || d[3]!=32 || d[4]!=0 || d[5]!=32)continue;
        const u32 offset=(u32(d[28])<<24)|(u32(d[29])<<16)|(u32(d[30])<<8)|d[31];
        if(u64(offset)+2048>d.size())continue;
        std::memcpy(icons[i].pixels.data(),d.data()+offset,2048);
        GXInitTexObj(&icons[i].texture,icons[i].pixels.data(),32,32,GX_TF_RGB5A3,GX_CLAMP,GX_CLAMP,GX_FALSE);
        GXInitTexObjLOD(&icons[i].texture,GX_LINEAR,GX_LINEAR,0,0,0,GX_FALSE,GX_FALSE,GX_ANISO_1);
        icons[i].ready=true;
    }
}
u32 word(const rarc::Bytes& d,size_t p) {
    if(p+4>d.size())return 0;
    return (u32(d[p])<<24)|(u32(d[p+1])<<16)|(u32(d[p+2])<<8)|d[p+3];
}
u32 hash(const char* s){u32 h=0;for(;*s;++s)h=(u8(*s)+(h<<8))%0x1ffffd9;return h;}
struct Field {u32 offset=~0u,mask=0;unsigned shift=0,type=0;};
Field field(const rarc::Bytes& d,const char* name) {
    const auto h=hash(name);const auto n=word(d,4);
    if(n>128||16+size_t(n)*12>d.size())return {};
    for(unsigned i=0;i<n;++i){size_t p=16+i*12;if(word(d,p)==h)
        return {word(d,p+8)>>16,word(d,p+4),d[p+10],d[p+11]};}
    return {};
}
void load() {
    const int version=ver::pal()?1:0;if(loadedVersion==version)return;
    loadedVersion=version;sources.clear();for(auto& icon:icons)icon.ready=false;loadIcons();
    rarc::Archive archive;
    if(!archive.parse(rarc::decompress(rarc::readDisc("/Map/map2.szp"))))return;
    const auto* item=archive.find("jmp/generatorinfo");if(!item)return;
    const auto& d=item->data;
    const auto type=field(d,"type"),room=field(d,"room_no");
    const std::array<Field,3> pos{field(d,"pos_x"),field(d,"pos_y"),field(d,"pos_z")};
    const u32 count=word(d,0),start=word(d,8),stride=word(d,12);
    if(count>10000||!stride||start+u64(count)*stride>d.size()||type.offset==~0u||room.offset==~0u)return;
    std::map<std::pair<int,int>,Source> rooms;
    for(unsigned i=0;i<count;++i){size_t row=start+size_t(i)*stride;
        if(type.offset+32>stride||room.offset+4>stride)continue;
        std::string name;for(unsigned j=0;j<32&&d[row+type.offset+j];++j)name+=char(d[row+type.offset+j]);
        int kind=name=="elfire"?0:name=="elwater"?1:name=="elice"?2:-1;if(kind<0)continue;
        int r=int((word(d,row+room.offset)&room.mask)>>room.shift);if(r<0||r>=80)continue;
        Source source{r,kind,{}};bool valid=true;
        for(unsigned axis=0;axis<3;++axis){if(pos[axis].offset==~0u||pos[axis].offset+4>stride){valid=false;break;}
            source.position[axis]=std::bit_cast<float>(word(d,row+pos[axis].offset));
            valid&=std::isfinite(source.position[axis])&&std::fabs(source.position[axis])<100000;}
        if(valid)rooms.try_emplace({r,kind},source);
    }
    for(const auto& [id,source]:rooms)sources.push_back(source);
    log("[map elements] loaded %zu source room/type markers from mounted disc",sources.size());
}
void vertex(float x,float y,float z,float u,float v){GXPosition3f32(x,y,z);GXTexCoord2f32(u,v);}
}
void draw() {
    if(!port::settings().enhancements.mapElementSources || rd32(VA(0x804D80C8))!=2)return;
    load();if(sources.empty())return;
    const unsigned count=std::min(rd32(VOFF(0x804D8310,0x804C59E0)),80u);
    const int floor=s8(rd8(VOFF(0x804D8314,0x804C59E4)));
    const u32 rooms=VOFF(0x803CCD38,0x803BA098),view=VA(0x803A3820)+VOFF(0x18C,0x198);
    float matrix[12];for(unsigned i=0;i<12;++i)matrix[i]=gu::rdf(view+i*4);
    // Slot 15 is a temporary state-only slot, outside both eye history rings.
    // Direct GX state writes leave the guest HLE's native state tracking intact.
    GXSaveStateAurora(15);
    GXSetCurrentMtx(GX_PNMTX0);
    const float identity[3][4]={{1,0,0,0},{0,1,0,0},{0,0,1,0}};
    interp::TagScope camera(interp::TagCat::SubView,0x454c4d50);
    const u32 at=interp::mark();GXLoadPosMtxImm(identity,GX_PNMTX0);
    interp::note(at,interp::Kind::PosMtx,0,0,12);
    GXClearVtxDesc();GXSetVtxDesc(GX_VA_POS,GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT7,GX_VA_POS,GX_POS_XYZ,GX_F32,0);
    GXSetVtxDesc(GX_VA_TEX0,GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT7,GX_VA_TEX0,GX_TEX_ST,GX_F32,0);
    GXSetNumChans(1);GXSetChanCtrl(GX_COLOR0A0,GX_DISABLE,GX_SRC_REG,GX_SRC_REG,0,GX_DF_NONE,GX_AF_NONE);
    GXSetNumTexGens(1);GXSetTexCoordGen(GX_TEXCOORD0,GX_TG_MTX2x4,GX_TG_TEX0,GX_IDENTITY);GXSetNumIndStages(0);GXSetNumTevStages(1);
    GXSetTevDirect(GX_TEVSTAGE0);GXSetTevOrder(GX_TEVSTAGE0,GX_TEXCOORD0,GX_TEXMAP0,GX_COLOR_NULL);
    GXSetTevOp(GX_TEVSTAGE0,GX_REPLACE);GXSetCullMode(GX_CULL_NONE);
    GXSetZMode(GX_DISABLE,GX_ALWAYS,GX_DISABLE);GXSetBlendMode(GX_BM_BLEND,GX_BL_SRCALPHA,GX_BL_INVSRCALPHA,GX_LO_CLEAR);
    GXSetAlphaCompare(GX_ALWAYS,0,GX_AOP_AND,GX_ALWAYS,0);GXSetColorUpdate(GX_ENABLE);
    GXSetFog(GX_FOG_NONE,0,0,0,1,GXColor{0,0,0,0});
    unsigned drawn=0;
    for(const auto& source:sources) {
        if(!icons[source.kind].ready)continue;
        if(unsigned(source.room)>=count || s8(rd8(rooms+source.room*4))!=floor)continue;
        float p[3]{};auto world=source.position;
        // Map meshes use 1/10 world units, including the mirrored mansion's X axis.
        world[0]*=hm::mirrorOn()?-0.1f:0.1f;world[1]*=0.1f;world[2]*=0.1f;
        for(unsigned axis=0;axis<3;++axis)p[axis]=matrix[axis*4]*world[0]+matrix[axis*4+1]*world[1]+matrix[axis*4+2]*world[2]+matrix[axis*4+3];
        if(!std::isfinite(p[2])||p[2]>=-1)continue;
        const float size=std::clamp(-p[2]*0.055f,2.f,40.f);
        unsigned roomCount=0,rank=0;
        for(const auto& other:sources)if(other.room==source.room){++roomCount;if(other.kind<source.kind)++rank;}
        p[0]+=(float(rank)-(float(roomCount)-1)*0.5f)*size*1.12f;
        GXLoadTexObj(&icons[source.kind].texture,GX_TEXMAP0);
        const float l=p[0]-size*.5f,r=p[0]+size*.5f,t=p[1]+size*.5f,b=p[1]-size*.5f;
        GXBegin(GX_QUADS,GX_VTXFMT7,4);vertex(l,t,p[2],0,0);vertex(l,b,p[2],0,1);
        vertex(r,b,p[2],1,1);vertex(r,t,p[2],1,0);GXEnd();++drawn;
    }
    static int lastFloor=-999;
    if(lastFloor!=floor){lastFloor=floor;log("[map elements] floor %d, room records %u, medal markers drawn %u",floor,count,drawn);}

    GXRestoreStateAurora(15);
}
}

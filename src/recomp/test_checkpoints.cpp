// Room testing checkpoints use original save packing and native map setup.
// No memory image, native stack, GPU state or actor pointers are restored.
#include "recomp/test_checkpoints.h"
#include "recomp/test_checkpoint_input.h"
#include "recomp/test_checkpoint_element.h"
#include "recomp/guest_util.h"
#include "recomp/version.h"
#include "recomp/dev_tools.h"
#include "recomp/menu.h"
#include "recomp/vr_host.h"
#include "port/install.h"
#include <imgui.h>
#include <SDL3/SDL.h>
#include <array>
#include <atomic>
#include <mutex>
#include <fstream>
#include <cstring>
#include <cmath>
#include <ctime>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
namespace recomp::checkpoints {
namespace {
constexpr unsigned kSize=0x3B0,kFormat=1;
struct Slot {
    unsigned magic=0x4C4D4350,format=kFormat,pal=0,map=0,room=0,appear=0;
    float position[3]{},yaw=0;
    unsigned long long time=0;
    std::array<unsigned char,kSize> progress{};
    std::array<unsigned char,32> eventFlags{};
};
std::array<ElementState,4> elements{};ElementState restoringElement;
std::array<Slot,4> slots{};std::array<bool,4> valid{};
std::mutex mutex;std::atomic<bool> enabled{false};
int selected=0,request=0; // 1 capture, 2 reload
bool initialized=false,armed=false,spawn=false,yawApplied=false;
unsigned toolId=0;int entryId=-1;Slot restoring;
std::string status="Capture while standing safely in a room, before starting the event.";
unsigned scratch=0;
int mode(){return !vrhost::enabled()?3:vrhost::firstPerson()?2:vrhost::diorama()?0:1;}
int lastMode=-1;
auto legacyDir(){return port::configDir()/"test-checkpoints";}
auto dir(){
    // Isolated automated runs must never write into the playable installation.
    if(std::getenv("LM_OFFSCREEN") || std::getenv("LM_CHECKPOINT_TEST"))return port::configDir()/"statesaves";
    auto root=port::exeDir();if(root.filename()=="bin")root=root.parent_path();
    return root/"statesaves";
}
void migrateSlots(){
    std::error_code ec;std::filesystem::create_directories(dir(),ec);
    if(ec){status="Could not create statesaves in the game folder. Check folder write permissions.";return;}
    for(const char* region:{"PAL-","USA-"})for(int i=1;i<=4;++i){
        const auto name=std::string(region)+std::to_string(i)+".bin";
        const auto source=legacyDir()/name,target=dir()/name;
        if(std::filesystem::exists(source,ec) && !std::filesystem::exists(target,ec)){
            std::filesystem::copy_file(source,target,std::filesystem::copy_options::none,ec);
            if(ec)status="Could not copy an older checkpoint to statesaves; the original is still available.";
        }
    }
}
auto file(int slot){return dir()/(std::string(ver::pal()?"PAL-":"USA-")+std::to_string(slot+1)+".bin");}
unsigned hash(const Slot& slot){unsigned h=2166136261u;const auto* bytes=reinterpret_cast<const unsigned char*>(&slot);for(unsigned i=0;i<sizeof(slot);++i)h=(h^bytes[i])*16777619u;return h;}
bool sane(const Slot& s){return s.magic==0x4C4D4350&&s.format==kFormat&&s.pal==unsigned(ver::pal())&&s.map<=13&&s.room<=255&&s.appear<=255&&std::isfinite(s.yaw)&&std::all_of(std::begin(s.position),std::end(s.position),[](float v){return std::isfinite(v)&&std::fabs(v)<10000000;});}
void init(){
    if(!initialized){initialized=true;migrateSlots();for(int i=0;i<4;++i){Slot s{};unsigned checksum=0;std::ifstream f(file(i),std::ios::binary);if(f.read(reinterpret_cast<char*>(&s),sizeof(s))&&f.read(reinterpret_cast<char*>(&checksum),4)&&sane(s)&&checksum==hash(s)){
        if(f.peek()!=EOF){ElementTail tail{};if(!f.read(reinterpret_cast<char*>(tail.data()),sizeof(tail))||f.peek()!=EOF)continue;
            elements[i]=decodeElement(tail,checksum);if(!elements[i].available)continue;}
        slots[i]=s;valid[i]=true;if(s.time>slots[selected].time)selected=i;}}}
    if(lastMode!=mode()){lastMode=mode();int value=0;std::ifstream f(legacyDir()/("shortcuts-mode-"+std::to_string(lastMode)+".txt"));f>>value;enabled=value==1;}
}
void saveSlot(int i){std::error_code ec;std::filesystem::create_directories(dir(),ec);auto tmp=file(i);tmp+=".tmp";std::ofstream f(tmp,std::ios::binary|std::ios::trunc);const unsigned h=hash(slots[i]);f.write(reinterpret_cast<const char*>(&slots[i]),sizeof(Slot));f.write(reinterpret_cast<const char*>(&h),4);
    if(elements[i].available){const auto tail=elementTail(elements[i],h);f.write(reinterpret_cast<const char*>(tail.data()),sizeof(tail));}
    f.close();if(!f){status="Checkpoint captured in memory; could not write its file.";return;}std::filesystem::rename(tmp,file(i),ec);if(ec){std::filesystem::remove(file(i),ec);std::filesystem::rename(tmp,file(i),ec);}if(ec)status="Checkpoint captured in memory; file replacement failed.";}
void queue(int action){if(armed||spawn){status="Room reload is already in progress.";return;}if(action==2&&!valid[selected]){status="Selected checkpoint slot is empty.";return;}request=action;status=action==1?"Capture requested: waiting for normal gameplay.":"Reload requested: waiting for the current event/menu to finish.";}
unsigned buffer(){if(!scratch)scratch=hleAlloc(kSize);return scratch;}
}
bool shortcuts(){return enabled.load();}
bool loading(){std::lock_guard lock(mutex);return armed;}
void sourceClicks(unsigned source,bool left,bool right,bool menuOpen){
    std::lock_guard lock(mutex);init();
    static std::array<ClickState,2> state;
    const auto action=state[source].update(left,right,enabled,menuOpen);
    if(action==ClickAction::Capture)queue(1);
    if(action==ClickAction::Reload)queue(2);
}
void clicks(bool l,bool r,bool menuOpen){sourceClicks(0,l,r,menuOpen);}
void pollGamepads(){
    // The tracked controllers are sampled in vr_host.cpp. A physical gamepad
    // uses the same optional shortcuts, including when VR is running.
    int n=0;auto* ids=SDL_GetGamepads(&n);static SDL_JoystickID device=0;
    SDL_Gamepad* pad=nullptr;for(int i=0;i<n;++i){auto* p=SDL_GetGamepadFromID(ids[i]);if(p&&SDL_GamepadConnected(p)){pad=p;device=ids[i];break;}}SDL_free(ids);
    if(pad)sourceClicks(1,SDL_GetGamepadButton(pad,SDL_GAMEPAD_BUTTON_LEFT_STICK),SDL_GetGamepadButton(pad,SDL_GAMEPAD_BUTTON_RIGHT_STICK),menu::isOpen());
    else sourceClicks(1,false,false,menu::isOpen());
    (void)device;
}
void drawMenu(){std::lock_guard lock(mutex);init();bool on=enabled;
    if(ImGui::Checkbox("Enable room checkpoint stick shortcuts",&on)){enabled=on;std::error_code ec;std::filesystem::create_directories(legacyDir(),ec);std::ofstream f(legacyDir()/("shortcuts-mode-"+std::to_string(mode())+".txt"));f<<int(on)<<'\n';}
    ImGui::TextWrapped("Left stick click: capture selected slot. Right stick click: reload selected slot. Both sticks: F1. F6: recenter. Shortcuts save separately per view mode; disabling keeps all slots.");
    for(int i=0;i<4;++i){ImGui::PushID(i);char label[100];if(valid[i])std::snprintf(label,sizeof(label),"Slot %d: map %u, room %u",i+1,slots[i].map,slots[i].room);else std::snprintf(label,sizeof(label),"Slot %d: empty",i+1);ImGui::RadioButton(label,&selected,i);ImGui::PopID();}
    if(ImGui::Button("Capture selected room"))queue(1);ImGui::SameLine();if(ImGui::Button("Reload selected room"))queue(2);
    ImGui::TextWrapped("%s",status.c_str());
    ImGui::TextWrapped("Shareable files: %s",port::displayPath(dir()).c_str());
    ImGui::TextWrapped("Share PAL-1.bin through PAL-4.bin (or USA files for USA games). Close the game before replacing a slot, then restart. Use the same game region and preferably the same VR build. Include the build number and steps to reproduce the problem.");
    ImGui::TextWrapped("Testing checkpoints reload the room with the game's saved progress. Capture before an event. New captures also restore the current element and supply. Older captures remain compatible; recapture once to include the element. They do not resume animations or live ghost movement. Ordinary memory-card files are not written by these controls. Saving normally afterwards saves your restored progress.");
}
void frame(Ctx& c,bool ready,unsigned luigi,int map,int room){std::lock_guard lock(mutex);
    static int probeStage=0,probeFrames=0;
    static const bool probe=std::getenv("LM_CHECKPOINT_TEST")!=nullptr;
    if(probe&&ready){
        ++probeFrames;
        if(probeStage==0&&probeFrames==10){dev::requestWarp(2,7);probeStage=10;probeFrames=0;}
        if(probeStage==10&&room!=2&&probeFrames>=60){request=1;probeStage=1;probeFrames=60;}
        if(probeStage==1&&valid[selected]&&probeFrames==150){
            wr8(VA(0x803D2D40)+0x659+31,rd8(VA(0x803D2D40)+0x659+31)^0x80);
            dev::requestWarp(2,0);probeStage=2;probeFrames=0;
        }
        if(probeStage==2 && room!=int(slots[selected].room) && probeFrames>=90){request=2;probeStage=3;}
    }
    if(spawn&&!yawApplied&&gu::inMem1(luigi)&&rd32(VA(0x803D2D40)+0x0C)==0){gu::wrf(luigi+0xF48,restoring.yaw);yawApplied=true;}
    if(spawn&&ready&&map==int(restoring.map)){
        if(restoringElement.available && gu::inMem1(luigi)) {
            wr32(luigi+0x1184,restoringElement.type);gu::wrf(luigi+0x1188,restoringElement.supply);
            // Use the original synchronization path to refresh the native HUD record.
            gu::call(c,GU_SYM("fn_800AEEA8"),{luigi,59,0});
            log("[checkpoint] restored element %u supply %.3f",restoringElement.type,restoringElement.supply);
        }
        spawn=false;status=restoringElement.available?"Room checkpoint loaded (element restored).":"Room checkpoint loaded (legacy capture has no element snapshot).";
        if(probe&&probeStage==3){
            bool flags=std::memcmp(hostp(VA(0x803D2D40)+0x659),restoring.eventFlags.data(),32)==0;
            float distance=0;for(unsigned i=0;i<3;++i)distance+=std::fabs(gu::rdf(luigi+0x44+4*i)-restoring.position[i]);
            const auto health=u16(restoring.progress[0x234])<<8|restoring.progress[0x235];
            const bool hp=rd16(luigi+0xFC)==health;
            log("[checkpoint test] restored flags %d map %d room %d health %d position error %.2f",flags,map==int(restoring.map),room==int(restoring.room),hp,distance);probeStage=4;
        }
        log("[checkpoint] loaded map %d room %d",map,room);}
    if(request!=1||!ready||!gu::inMem1(luigi)||map<0||map>13||room<0||room>255)return;
    Slot s{};s.pal=ver::pal();s.map=map;s.room=room;s.appear=rd32(VA(0x804D872C));s.time=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    for(unsigned i=0;i<3;++i)s.position[i]=gu::rdf(luigi+0x44+4*i);s.yaw=gu::rdf(luigi+0xF48);
    if(!sane(s)){request=0;status="Capture rejected: invalid room position.";return;}
    // Original message 59 synchronizes live health, equipment, treasure and
    // captured ghosts into the player record, as at normal map changes.
    gu::call(c,GU_SYM("fn_800AEEA8"),{luigi,59,0});
    std::memset(hostp(buffer()),0,kSize);
    gu::call(c,ver::pal()?0x80069E48u:0x80067108u,{buffer()});
    std::memcpy(s.progress.data(),hostp(buffer()),kSize);
    std::memcpy(s.eventFlags.data(),hostp(VA(0x803D2D40)+0x659),32);
    ElementState element{rd32(luigi+0x1184),gu::rdf(luigi+0x1188),true};
    if(element.type<2||element.type>4){element.type=1;element.supply=0;}
    if(!validElement(element)){request=0;status="Capture rejected: invalid element supply.";return;}
    elements[selected]=element;
    slots[selected]=s;valid[selected]=true;request=0;status="Room checkpoint captured.";saveSlot(selected);
    log("[checkpoint] captured slot %d map %d room %d",selected+1,map,room);
}
void beforeScene(bool ready){std::lock_guard lock(mutex);if(request!=2||!ready)return;
    if(!valid[selected]||!sane(slots[selected])){request=0;status="Checkpoint is empty or incompatible.";return;}
    restoring=slots[selected];restoringElement=elements[selected];request=0;armed=true;dev::requestWarp(restoring.map,restoring.appear);
}
void mapSetup(Ctx& c){std::lock_guard lock(mutex);if(!armed)return;
    // Called after outgoing map teardown, so its record update cannot overwrite
    // the checkpoint. The retail deserializer restores doors/event flags too.
    std::memcpy(hostp(buffer()),restoring.progress.data(),kSize);
    gu::call(c,ver::pal()?0x80069D90u:0x80067050u,{buffer()});
    std::memcpy(hostp(VA(0x803D2D40)+0x659),restoring.eventFlags.data(),32);
    armed=false;spawn=true;yawApplied=false;toolId=0;entryId=-1;
}
void spawnEntry(unsigned tool,int entry){std::lock_guard lock(mutex);if(spawn){toolId=tool;entryId=entry;}}
void spawnValue(unsigned tool,int entry,const char* name,unsigned out){std::lock_guard lock(mutex);if(!spawn||tool!=toolId||entry!=entryId)return;
    int axis=std::strcmp(name,"pos_x")==0?0:std::strcmp(name,"pos_y")==0?1:std::strcmp(name,"pos_z")==0?2:-1;
    if(axis>=0)gu::wrf(out,restoring.position[axis]);
}
}

namespace recomp::checkpoints {
void spawnRoom(unsigned tool,int entry,const char* name,unsigned out){std::lock_guard lock(mutex);if(spawn&&tool==toolId&&entry==entryId&&std::strcmp(name,"room_no")==0)wr32(out,restoring.room);}
HLE(getValue__Q24Koga8ToolDataCFiPCcPUl) {
    const u32 tool=c.r[3],entry=c.r[4],name=c.r[5],out=c.r[6];
    callOriginal(c,"getValue__Q24Koga8ToolDataCFiPCcPUl");
    spawnRoom(tool,s32(entry),gstr(name),out);
}
HLE(getValue__Q24Koga8ToolDataCFiPCcPl) {
    const u32 tool=c.r[3],entry=c.r[4],name=c.r[5],out=c.r[6];
    callOriginal(c,"getValue__Q24Koga8ToolDataCFiPCcPl");
    spawnRoom(tool,s32(entry),gstr(name),out);
}
}

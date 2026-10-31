#pragma once
#include "recomp/ctx.h"
namespace recomp::checkpoints {
void drawMenu();
bool shortcuts();
void clicks(bool left,bool right,bool menuOpen); // release-triggered; chord cancels both
void pollGamepads();
void frame(Ctx& c,bool ready,unsigned luigi,int map,int room);
void beforeScene(bool ready);
void mapSetup(Ctx& c);
void spawnEntry(unsigned tool,int entry);
void spawnValue(unsigned tool,int entry,const char* name,unsigned out);
bool loading();
}

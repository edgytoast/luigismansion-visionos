// Leftover room models visited for one warp (see room_swap.h).
#include "recomp/room_swap.h"
#include "recomp/test_checkpoints.h"

#include "recomp/dev_tools.h"
#include "recomp/guest_util.h"
#include "recomp/hle.h"

#include <dolphin/dvd.h>

#include <atomic>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace recomp::rooms {
namespace {

// Rooms are matched by the retail file the game loads for them (map descriptors lbl_802FF218:
// map 1's single room is /Iwamoto/map1/hakase.arc, map 4's /Iwamoto/map4/h_02.bin, map 2's
// rooms /Iwamoto/map2/room_NN.arc with the model in "room.bin"). Spawn points are floor
// positions inside the room (world coordinates, checked in game with the F3 overlay).
// Map 2's descriptor (lbl_8030377C +0xC, lbl_80301360) lists alternate room models the room
// loader (fn_80011FDC) uses when a room's slot +0x45 selects one: room 28 -> room_28A.arc
// (switched by game code, fn_80037550 / fn_800376A4) and room 1 -> room01A.bin, which nothing
// ever selects. The visit selects it with the game's own fn_800129FC(room, 1).
const LeftoverRoom kRooms[] = {
    {"Laboratory, E3 2001 version", "extra stand for a robotic arm; /h_01.bin/1.3/Fri Apr 27 2001", 1, -1, {},
     "/Iwamoto/map1/hakase.arc", "room.bin", "/Iwamoto/map1/h_01.bin", nullptr, 0},
    {"Portrificationizer room, pre-E3", "door bump; dirt ball where the Gallery door is", 4, -1, {},
     "/Iwamoto/map4/h_02.bin", nullptr, "/Iwamoto/map1/h_02.bin", nullptr, 0},
    {"Breaker room, older version", "door on the left wall; retail collision and door kept", 2, 67,
     {3350.0f, -550.0f, -1810.0f}, "/Iwamoto/map2/room_67.arc", "room.bin", "/Iwamoto/map2/room_67.arc", "b1_c_67.bin", 0},
    {"1F hallway, duplicate", "room_07.arc \"room..bin\"", 2, 7, {-1250.0f, 0.0f, -5000.0f},
     "/Iwamoto/map2/room_07.arc", "room.bin", "/Iwamoto/map2/room_07.arc", "room..bin", 0},
    {"Foyer hallway (2F), duplicate", "better-aligned beam UVs; room_30.arc \"room..bin\"", 2, 30,
     {250.0f, 550.0f, -1000.0f}, "/Iwamoto/map2/room_30.arc", "room.bin", "/Iwamoto/map2/room_30.arc", "room..bin", 0},
    {"Hidden Room, bare alternate model (room01A)", "map 2's unused alternate model for room 1: no curtains, rug or portraits; furniture not drawn", 2, 1,
     {-1800.0f, 0.0f, 100.0f}, nullptr, nullptr, nullptr, nullptr, 1},
};

constexpr s32 kEntryBase = 0x48000000;  // virtual FST entries (hidden_mansion.cpp uses 0x40000000)

std::mutex sM;
std::atomic<int> sActive{-1};
bool sArrived = false;
int sStartRoom = -1;  // the game's current-room id where Luigi was placed (not the archive index)
bool sAltShown = false;  // alternate model selected (alternate visits)
std::vector<u8> sBytes;  // served file for the active swap
int sBuilt = -1;
bool sSpawnPending = false;
float sSpawn[3] = {};
s32 sLuigiEntry = -1;
u32 sLuigiTool = 0;

u32 be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }
void put32(u8* p, u32 v) {
    for (int i = 0; i < 4; i++) p[i] = u8(v >> (24 - 8 * i));
}

std::vector<u8> readDisc(const char* path) {
    DVDFileInfo fi{};
    if (!DVDOpen(path, &fi)) return {};
    std::vector<u8> buf((fi.length + 31) & ~31u);
    const s32 got = DVDReadPrio(&fi, buf.data(), s32(buf.size()), 0, 2);
    DVDClose(&fi);
    if (got < 0) return {};
    buf.resize(fi.length);
    return buf;
}

// Finds a file entry of an uncompressed RARC by name: returns the entry's offset, or 0.
u32 findEntry(const std::vector<u8>& d, const char* name) {
    if (d.size() < 0x40 || std::memcmp(d.data(), "RARC", 4) != 0) return 0;
    const u32 numEntries = be32(&d[0x28]), entryOff = be32(&d[0x2C]) + 0x20, strOff = be32(&d[0x34]) + 0x20;
    for (u32 i = 0; i < numEntries; i++) {
        const u32 e = entryOff + i * 20;
        if (e + 20 > d.size()) break;
        const u32 flagsName = be32(&d[e + 4]);
        if ((flagsName >> 24) & 0x02) continue;  // directory
        const u32 n = strOff + (flagsName & 0xFFFFFF);
        if (n < d.size() && std::strcmp(reinterpret_cast<const char*>(&d[n]), name) == 0) return e;
    }
    return 0;
}

std::vector<u8> member(const std::vector<u8>& arc, const char* name) {
    const u32 e = findEntry(arc, name);
    if (!e) return {};
    const u32 off = be32(&arc[0x0C]) + 0x20 + be32(&arc[e + 8]), size = be32(&arc[e + 12]);
    if (size_t(off) + size > arc.size()) return {};
    return {arc.begin() + off, arc.begin() + off + size};
}

// The archive with `name`'s data replaced. Data that fits in the member's own space is written
// there, so the archive keeps its size: map 2 streams room archives through fixed-size buffers,
// and a grown archive overruns them (the breaker room's archive + the old model appended came
// to 485 KB and the room's node tree was overwritten: endless recursion in the room draw
// fn_80015844). Otherwise the new data is appended (32-byte aligned) and the entry, file size,
// data size and main-RAM size are updated.
std::vector<u8> replaceMember(std::vector<u8> arc, const char* name, const std::vector<u8>& data) {
    const u32 e = findEntry(arc, name);
    if (!e) return {};
    const u32 dataStart = be32(&arc[0x0C]) + 0x20;
    const u32 oldOff = dataStart + be32(&arc[e + 8]), oldSize = be32(&arc[e + 12]);
    if (data.size() <= oldSize && size_t(oldOff) + oldSize <= arc.size()) {
        std::memcpy(&arc[oldOff], data.data(), data.size());
        std::memset(&arc[oldOff + data.size()], 0, oldSize - data.size());
        put32(&arc[e + 12], u32(data.size()));
        return arc;
    }
    arc.resize((arc.size() + 31) & ~size_t(31), 0);
    const u32 at = u32(arc.size());
    arc.insert(arc.end(), data.begin(), data.end());
    arc.resize((arc.size() + 31) & ~size_t(31), 0);
    put32(&arc[e + 8], at - dataStart);
    put32(&arc[e + 12], u32(data.size()));
    const u32 dataLen = u32(arc.size()) - dataStart;
    put32(&arc[0x04], u32(arc.size()));
    put32(&arc[0x10], dataLen);
    put32(&arc[0x14], dataLen);
    return arc;
}

bool build(int i) {
    if (sBuilt == i) return !sBytes.empty();
    sBuilt = i;
    const LeftoverRoom& r = kRooms[i];
    std::vector<u8> src = readDisc(r.source);
    if (r.sourceMember) src = member(src, r.sourceMember);
    sBytes.clear();
    if (src.empty()) {
        log("[rooms] %s: source %s%s%s not found", r.name, r.source, r.sourceMember ? "/" : "",
            r.sourceMember ? r.sourceMember : "");
        return false;
    }
    sBytes = r.targetMember ? replaceMember(readDisc(r.target), r.targetMember, src) : src;
    if (sBytes.empty()) log("[rooms] %s: could not rebuild %s", r.name, r.target);
    else log("[rooms] %s: %s served with the leftover model (%zu bytes)", r.name, r.target, sBytes.size());
    return !sBytes.empty();
}

void end(const char* why) {
    const int i = sActive.exchange(-1);
    if (i >= 0) log("[rooms] %s: back to the retail model (%s)", kRooms[i].name, why);
}

// fn_800129FC(room, n): selects alternate model n of a map-2 room (0 = retail) and reloads it
// if it is loaded.
// Test aid: LM_DEV_ROOM_RETAIL=1 makes visits place Luigi without swapping the model (for
// comparing the leftover with the retail room).
const bool sRetailOnly = [] {
    const char* v = std::getenv("LM_DEV_ROOM_RETAIL");
    return v && *v == '1';
}();

void selectAlternate(Ctx& c, int room, int alt) { gu::call(c, GU_SYM("fn_800129FC"), {u32(room), u32(alt)}); }

}  // namespace

const LeftoverRoom* list(int* count) {
    *count = int(std::size(kRooms));
    return kRooms;
}

void visit(int index) {
    if (index < 0 || index >= int(std::size(kRooms))) return;
    const LeftoverRoom& r = kRooms[index];
    dev::requestWarp(r.map, 0);  // ends any other visit first
    {
        std::lock_guard lk(sM);
        sArrived = sAltShown = false;
        sStartRoom = -1;
        sSpawnPending = r.spawn[0] != 0.0f || r.spawn[1] != 0.0f || r.spawn[2] != 0.0f;
        std::memcpy(sSpawn, r.spawn, sizeof(sSpawn));
    }
    sActive = index;
    log("[rooms] visit: %s (map %d room %d)", r.name, r.map, r.room);
}

void cancel() {
    end("another warp");
    std::lock_guard lk(sM);
    sSpawnPending = false;
}

int active() { return sActive.load(); }

// The warp's map setup: from here on the visit is in its map (the spawn override stays armed
// until Luigi has been placed).
void onMapSetup() {
    if (sActive.load() < 0) return;
    if (sArrived) return end("map changed");
    sArrived = true;
}

void onFrame(Ctx& c, bool inGame, bool luigi, int map, int room) {
    const int i = sActive.load();
    if (i < 0 || !inGame || !sArrived) return;
    const LeftoverRoom& r = kRooms[i];
    if (luigi) {
        std::lock_guard lk(sM);
        sSpawnPending = false;
        sLuigiEntry = -1;
    }
    if (map != r.map) return end("left the map");
    if (r.alternate > 0 && !sAltShown && luigi && !sRetailOnly) {
        sAltShown = true;
        selectAlternate(c, r.room, r.alternate);
        log("[rooms] %s: room %d alternate model %d selected", r.name, r.room, r.alternate);
    }
    // The current-room id (0x803A3CA0 +0xC) is not the archive index (the breaker room, archive
    // 67, reads 69), so "left the room" is a change from the room Luigi was placed in.
    if (r.room >= 0 && luigi) {
        if (sStartRoom < 0) sStartRoom = room;
        else if (room != sStartRoom) {
            if (sAltShown) selectAlternate(c, r.room, 0);
            end("left the room");
        }
    }
}

s32 overlayEntry(const char* guestPath) {
    const int i = sActive.load();
    if (i < 0 || sRetailOnly || !kRooms[i].target || std::strcmp(guestPath, kRooms[i].target) != 0) return -1;
    std::lock_guard lk(sM);
    return build(i) ? kEntryBase + i : -1;
}

bool isOverlayEntry(s32 entry) { return entry >= kEntryBase && entry < kEntryBase + s32(std::size(kRooms)); }

u32 overlayLength(s32 entry) {
    std::lock_guard lk(sM);
    return sBuilt == entry - kEntryBase ? u32(sBytes.size()) : 0;
}

s32 overlayRead(s32 entry, void* dst, s32 length, s32 offset) {
    std::lock_guard lk(sM);
    if (sBuilt != entry - kEntryBase || offset < 0 || length < 0 || size_t(offset) > sBytes.size()) return -1;
    const size_t n = std::min(size_t(length), sBytes.size() - size_t(offset));
    std::memcpy(dst, sBytes.data() + offset, n);
    if (n < size_t(length)) std::memset(static_cast<u8*>(dst) + n, 0, size_t(length) - n);
    return s32(n);
}

// Luigi's start position for a visit. The map setup picks Luigi's characterinfo entry for the
// appear point (findLuigiAppearIndex) and reads its pos_x/pos_y/pos_z with ToolData::getValue
// (fn_800E52BC for the start slot, and again where Luigi is placed). While a visit's spawn is
// pending, those three reads of that entry return the visit's position.
HLE(findLuigiAppearIndex__Q24Koga9EnManagerFPQ24Koga8ToolDataUl) {
    const u32 tool = c.r[4];
    callOriginal(c, "findLuigiAppearIndex__Q24Koga9EnManagerFPQ24Koga8ToolDataUl");
    checkpoints::spawnEntry(tool,s32(c.r[3]));
    std::lock_guard lk(sM);
    if (sSpawnPending && sActive.load() >= 0) {
        sLuigiEntry = s32(c.r[3]);
        sLuigiTool = tool;
    }
}

HLE(getValue__Q24Koga8ToolDataCFiPCcPf) {
    const u32 tool = c.r[3], entry = c.r[4], name = c.r[5], out = c.r[6];
    callOriginal(c, "getValue__Q24Koga8ToolDataCFiPCcPf");
    checkpoints::spawnValue(tool,s32(entry),gstr(name),out);
    std::lock_guard lk(sM);
    if (!sSpawnPending || sActive.load() < 0 || tool != sLuigiTool || s32(entry) != sLuigiEntry) return;
    const char* n = gstr(name);
    const int axis = std::strcmp(n, "pos_x") == 0 ? 0 : std::strcmp(n, "pos_y") == 0 ? 1 : std::strcmp(n, "pos_z") == 0 ? 2 : -1;
    if (axis >= 0) wr32(out, std::bit_cast<u32>(sSpawn[axis]));
}

}  // namespace recomp::rooms

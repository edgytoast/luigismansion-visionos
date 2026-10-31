#pragma once
// High-level emulation of the Dolphin SDK. Recompiled calls into SDK functions
// resolve by name to the handlers registered with HLE(name). Handlers read
// their arguments from the guest registers (CodeWarrior PPC EABI: r3-r10,
// f1-f8, extra integer arguments at r1+8; structs are passed by pointer) and
// write the result to r3 (or r3:r4 for 64-bit values, f1 for floats).
#include "recomp/ctx.h"

namespace recomp {

// HleFn, hleLookup and hleMissing are declared in ctx.h (the generated code uses them).

struct HleReg {
    HleReg(const char* name, HleFn fn);
};

#define HLE(name)                                    \
    static void hle_##name(::recomp::Ctx& c);        \
    static ::recomp::HleReg hleReg_##name(#name, &hle_##name); \
    static void hle_##name([[maybe_unused]] ::recomp::Ctx& c)

// Address of a guest function by symbol name (from the recompiler's symbol table).
u32 guestSymbol(const char* name);

inline const char* gstr(u32 a) { return reinterpret_cast<const char*>(hostp(a)); }
inline u32 stackArg(const Ctx& c, int n) { return rd32(c.r[1] + 8 + 4 * n); }  // n-th argument past r10

// From inside a hook (tools/recomp/hooks.txt): runs the hooked function's original
// translated body with the current registers, so a hook can wrap it.
void callOriginal(Ctx& c, const char* name);

// Calls a guest function with up to 4 integer arguments; returns r3.
u32 guestCall(Ctx& c, u32 fn, u32 a0 = 0, u32 a1 = 0, u32 a2 = 0, u32 a3 = 0);

// Bump allocator for HLE-owned guest objects (thread structs, stacks, FIFO objects),
// placed outside MEM1 in the host address window (runtime.h: kHleHeapBegin).
u32 hleAlloc(u32 size, u32 align = 32);

void log(const char* fmt, ...);

}  // namespace recomp

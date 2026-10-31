#pragma once
// Guest CPU state and memory access for code produced by the recompiler (src/recompiler).
//
// Guest memory: MEM1 (24 MiB, physical 0x00000000) is reachable through the
// cached (0x8...) and uncached (0xC...) mirrors. The 16 KiB locked cache at
// 0xE0000000 lives just past MEM1. Hardware registers (0xCC......) go through
// mmio_*; the write-gather pipe (0xCC008000) feeds Aurora's GX FIFO.
//
// FREESTANDING: this header is shipped with the game and compiled into the game module
// on the user's machine by the bundled clang, with no C/C++ library headers available
// (docs/SETUP.md). Use compiler builtins only; no #include of system headers.
// Everything the generated code calls that is not inline here is provided by the runtime
// (statically in lmrecomp, through module_rt.cpp in the game module DLL).

namespace recomp {

using u8 = __UINT8_TYPE__;
using u16 = __UINT16_TYPE__;
using u32 = __UINT32_TYPE__;
using u64 = __UINT64_TYPE__;
using s8 = __INT8_TYPE__;
using s16 = __INT16_TYPE__;
using s32 = __INT32_TYPE__;
using s64 = __INT64_TYPE__;

constexpr u32 kMem1Size = 0x01800000;
constexpr u32 kLockedCacheOffset = 0x01800000;  // host offset of the 0xE0000000 locked cache
// Host window for guest addresses (masked): MEM1 (24 MiB), the locked cache, then memory that
// only host tools use (viewer scratch at 0x81810000, HLE objects at 0x81900000,
// viewer heap at 0x82000000). Must stay a power of two.
constexpr u32 kHostMemSize = 0x04000000;
constexpr u32 kHostMemMask = kHostMemSize - 1;

inline u32 bswap32(u32 v) { return __builtin_bswap32(v); }
inline u16 bswap16(u16 v) { return __builtin_bswap16(v); }

extern u8* g_mem;
extern bool* g_irqPending;  // set (atomically) by the runtime when an interrupt is queued
extern bool g_traceOn;      // LM_TRACE: log/record guest function entries
extern u8* g_patches;       // runtime-switchable instruction patches ("patch" lines in tools/recomp/hooks.txt)
extern bool g_watchOn;      // LM_WATCH: report guest stores to an address range
void watch_store(u32 a, u32 v, int size);

inline u8* hostp(u32 a) {
    return g_mem + ((a >> 28) == 0xE ? (kLockedCacheOffset | (a & 0x3FFF)) : (a & kHostMemMask));
}
inline bool is_mmio(u32 a) { return (a >> 24) == 0xCC; }

u32 mmio_rd(u32 a, int size);
void mmio_wr(u32 a, u32 v, int size);

// Bit casts and math, as builtins (the module has no C library).
inline u64 bits64(double v) { return __builtin_bit_cast(u64, v); }
inline double dbl(u64 v) { return __builtin_bit_cast(double, v); }
inline u32 bits32(float v) { return __builtin_bit_cast(u32, v); }
inline float flt(u32 v) { return __builtin_bit_cast(float, v); }
inline double fma_(double a, double b, double c) { return __builtin_fma(a, b, c); }
inline double sqrt_(double v) { return __builtin_sqrt(v); }
inline double fabs_(double v) { return __builtin_fabs(v); }
inline bool isnan_(double v) { return __builtin_isnan(v); }

inline u32 rd32(u32 a) {
    if (is_mmio(a)) [[unlikely]] return mmio_rd(a, 4);
    u32 v;
    __builtin_memcpy(&v, hostp(a), 4);
    return bswap32(v);
}
inline u32 rd16(u32 a) {
    if (is_mmio(a)) [[unlikely]] return mmio_rd(a, 2);
    u16 v;
    __builtin_memcpy(&v, hostp(a), 2);
    return bswap16(v);
}
inline u32 rd8(u32 a) {
    if (is_mmio(a)) [[unlikely]] return mmio_rd(a, 1);
    return *hostp(a);
}
inline u32 rd32r(u32 a) {
    u32 v;
    __builtin_memcpy(&v, hostp(a), 4);
    return v;
}
inline u32 rd16r(u32 a) {
    u16 v;
    __builtin_memcpy(&v, hostp(a), 2);
    return v;
}
inline void wr32(u32 a, u32 v) {
    if (g_watchOn) [[unlikely]] watch_store(a, v, 4);
    if (is_mmio(a)) [[unlikely]] return mmio_wr(a, v, 4);
    v = bswap32(v);
    __builtin_memcpy(hostp(a), &v, 4);
}
inline void wr16(u32 a, u32 v) {
    if (g_watchOn) [[unlikely]] watch_store(a, v, 2);
    if (is_mmio(a)) [[unlikely]] return mmio_wr(a, v & 0xFFFF, 2);
    const u16 s = bswap16(u16(v));
    __builtin_memcpy(hostp(a), &s, 2);
}
inline void wr8(u32 a, u32 v) {
    if (g_watchOn) [[unlikely]] watch_store(a, v, 1);
    if (is_mmio(a)) [[unlikely]] return mmio_wr(a, v & 0xFF, 1);
    *hostp(a) = u8(v);
}
inline void wr32r(u32 a, u32 v) { __builtin_memcpy(hostp(a), &v, 4); }
inline void wr16r(u32 a, u32 v) {
    const u16 s = u16(v);
    __builtin_memcpy(hostp(a), &s, 2);
}
inline float rdf32(u32 a) { return flt(rd32(a)); }
inline double rdf64(u32 a) { return dbl(u64(rd32(a)) << 32 | rd32(a + 4)); }
inline void wrf32(u32 a, float v) { wr32(a, bits32(v)); }
inline void wrf64(u32 a, double v) {
    const u64 b = bits64(v);
    wr32(a, u32(b >> 32));
    wr32(a + 4, u32(b));
}

struct GuestThread;
struct Ctx;
using GuestFn = void (*)(Ctx&);
struct FuncEntry {
    u32 addr;
    GuestFn fn;
};
struct SymEntry {
    const char* name;
    u32 addr;
};

// The recompiled game: tables emitted by the recompiler (func_table.cpp). Linked into
// lmrecomp directly, or exported by the game module DLL (module_abi.h).
constexpr u32 kModuleAbiVersion = 3;
constexpr u32 kMaxPatches = 64;
struct GameModule {
    u32 abiVersion;
    const char* gameId;  // "GLME01" / "GLMP01": the DOL it was recompiled from
    const FuncEntry* funcs;
    u32 funcCount;
    const SymEntry* syms;  // canonical names (hooks and HLE use US names on every version)
    u32 symCount;
    const FuncEntry* orig;  // translated bodies of hooked functions (tools/recomp/hooks.txt)
    u32 origCount;
};

// HLE (hle.h): host replacements registered by name.
using HleFn = void (*)(Ctx&);
HleFn hleLookup(const char* name);
void hleMissing(const char* name, u32 addr, Ctx& c);

void service_irq(Ctx& c);
void trace_enter(u32 addr, Ctx& c);
[[noreturn]] void fatal(const char* fmt, ...);

struct Ctx {
    u32 r[32]{};
    double f[32]{};
    double ps1[32]{};
    u8 cr[8]{};
    u32 lr = 0, ctr = 0;
    u32 ca = 0, so = 0, ov = 0;
    u32 fpscr = 0;
    u32 gqr[8]{};
    u32 msr = 0x8000;  // external interrupts enabled
    GuestThread* thread = nullptr;
    u32 lastFn = 0;  // last guest function entered (only maintained while tracing)
    u32 ring[32]{};  // recent function entries (tracing only)
    u32 ringPos = 0;

    // --- condition register ---
    bool crbit(int n) const { return (cr[n >> 2] >> (3 - (n & 3))) & 1; }
    void setCrbit(int n, bool v) {
        const u8 m = u8(1 << (3 - (n & 3)));
        cr[n >> 2] = v ? (cr[n >> 2] | m) : (cr[n >> 2] & ~m);
    }
    void cmps(int f, s32 a, s32 b) { cr[f] = u8((a < b ? 8 : a > b ? 4 : 2) | so); }
    void cmpu(int f, u32 a, u32 b) { cr[f] = u8((a < b ? 8 : a > b ? 4 : 2) | so); }
    void setCr0(s32 v) { cmps(0, v, 0); }
    void setCr1() { cr[1] = u8(fpscr >> 28); }
    void fcmp(int f, double a, double b, bool) {
        cr[f] = isnan_(a) || isnan_(b) ? 1 : a < b ? 8 : a > b ? 4 : 2;
        fpscr = (fpscr & ~0xF000u) | (u32(cr[f]) << 12);
    }
    u32 getCr() const {
        u32 v = 0;
        for (int i = 0; i < 8; i++) v |= u32(cr[i] & 0xF) << (28 - 4 * i);
        return v;
    }
    void mtcrf(u32 crm, u32 v) {
        for (int i = 0; i < 8; i++)
            if (crm & (0x80 >> i)) cr[i] = (v >> (28 - 4 * i)) & 0xF;
    }
    u32 getXer() const { return (so << 31) | (ov << 30) | (ca << 29); }
    void setXer(u32 v) {
        so = (v >> 31) & 1;
        ov = (v >> 30) & 1;
        ca = (v >> 29) & 1;
    }

    // --- integer helpers ---
    static u32 rotl(u32 v, u32 n) { return __builtin_rotateleft32(v, n & 31); }
    static u32 cntlzw(u32 v) { return v ? u32(__builtin_clz(v)) : 32u; }
    static u32 divw(u32 a, u32 b) {
        const s32 x = s32(a), y = s32(b);
        if (y == 0 || (x == s32(0x80000000u) && y == -1)) return x < 0 && y == 0 ? 0xFFFFFFFFu : 0;
        return u32(x / y);
    }
    static u32 divwu(u32 a, u32 b) { return b == 0 ? 0 : a / b; }
    void sraw(int dst, u32 s, u32 sh) {
        const s32 v = s32(s);
        if (sh >= 32) {
            r[dst] = v < 0 ? 0xFFFFFFFFu : 0;
            ca = v < 0;
            return;
        }
        r[dst] = u32(v >> sh);
        ca = v < 0 && sh != 0 && (u32(v) & ((1u << sh) - 1)) != 0;
    }

    // --- floating point ---
    void setSingle(int d, float v) {
        f[d] = v;
        ps1[d] = v;
    }
    static double fctiw(double v, int truncate) {
        s32 i;
        if (isnan_(v)) i = s32(0x80000000u);
        else if (v >= 2147483647.0) i = 0x7FFFFFFF;
        else if (v <= -2147483648.0) i = s32(0x80000000u);
        else i = s32(truncate ? __builtin_trunc(v) : __builtin_nearbyint(v));
        return dbl(0xFFF8000000000000ull | u32(i));
    }

    // --- paired-single quantized load/store (GQR type/scale) ---
    static double qscale(int scale) { return dbl(u64(1023 - scale) << 52); }  // 2^-scale
    static int sext6(u32 v) { return (v & 0x20) ? int(v) - 64 : int(v); }
    static double dequant(u32 ea, int type, double s, u32* size) {
        switch (type) {
        case 4: *size = 1; return double(u8(rd8(ea))) * s;
        case 5: *size = 2; return double(u16(rd16(ea))) * s;
        case 6: *size = 1; return double(s8(rd8(ea))) * s;
        case 7: *size = 2; return double(s16(rd16(ea))) * s;
        default: *size = 4; return double(rdf32(ea));
        }
    }
    static u32 saturate(double v, double lo, double hi) {
        if (isnan_(v)) return 0;
        v = v < lo ? lo : v > hi ? hi : v;
        return u32(s32(v));
    }
    static u32 quant(u32 ea, double v, int type, double s) {
        const double q = v * s;
        switch (type) {
        case 4: wr8(ea, saturate(q, 0.0, 255.0)); return 1;
        case 5: wr16(ea, saturate(q, 0.0, 65535.0)); return 2;
        case 6: wr8(ea, saturate(q, -128.0, 127.0)); return 1;
        case 7: wr16(ea, saturate(q, -32768.0, 32767.0)); return 2;
        default: wrf32(ea, float(v)); return 4;
        }
    }
    void psqLoad(int d, u32 ea, int w, int i) {
        const u32 g = gqr[i];
        const int type = (g >> 16) & 7;
        const double s = qscale(sext6((g >> 24) & 0x3F));
        u32 size;
        f[d] = dequant(ea, type, s, &size);
        ps1[d] = w ? 1.0 : dequant(ea + size, type, s, &size);
    }
    void psqStore(int d, u32 ea, int w, int i) {
        const u32 g = gqr[i];
        const int type = g & 7;
        const double s = qscale(-sext6((g >> 8) & 0x3F));
        const u32 size = quant(ea, f[d], type, s);
        if (!w) quant(ea + size, ps1[d], type, s);
    }

    // --- system ---
    u32 mfspr(u32 spr);
    void mtspr(u32 spr, u32 v);
    u32 mftb(u32 spr);
    static void dcbz(u32 ea) { __builtin_memset(hostp(ea & ~31u), 0, 32); }

    void enter(u32 addr) {
        if (g_traceOn) [[unlikely]] trace_enter(addr, *this);
    }
    void poll() {
        if (__atomic_load_n(g_irqPending, __ATOMIC_RELAXED) && (msr & 0x8000)) [[unlikely]]
            service_irq(*this);
    }
    void call(u32 addr);
    [[noreturn]] void unimplemented(u32 pc, u32 insn);
};

}  // namespace recomp

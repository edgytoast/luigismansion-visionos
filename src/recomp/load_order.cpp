// The order of a map's loads: the map's models before the game's finished asynchronous loads.
//
// The map's setup runs on the game's second DVD thread (sDvdThread2, priority 17). It builds the
// map's models with their own solid heap current (fn_801713CC: a 512 KB JKRSolidHeap made the
// current heap, every model, then the main heap again and the solid heap trimmed to fit).
//
// The first DVD thread (sDvdThread1, priority 10) finishes the game's asynchronous loads: it
// closes the file (LMDvdFile::closeFile), switches the heap group, decompresses the file into
// memory taken from the current heap (the load's callback at +0x78) and calls the load's own
// callback (+0x74; the sky's, fn_8002F4DC, builds the sky's model). Being the higher priority, it
// does so in the middle of whatever the second thread is doing. A load finishing while the second
// thread is among the map's models takes its memory from the models' solid heap: the sky's
// archive left too little for the rest of the models, their materials' blocks came back null and
// the map's load crashed in the model loader (call to null or garbage, lr 801C0B34, 801C0B7C,
// 801C0BE8, 801C0CE4 or 801C6F04, right after "[unused] sky load"). On the console the sky's read
// came in after the models; here, with the host busy, the models took longer and about half the
// Simulator's loads crashed (c460600 and later alike).
//
// A load the first DVD thread finishes while the map's models are being built waits for them, a
// retrace at a time (ten seconds at most, by the clock: fast-forward makes retraces many times
// faster), before its heap group, memory and callback.
#include "recomp/frame.h"
#include "recomp/hle.h"
#include "recomp/sched.h"
#include "recomp/version.h"

#include <chrono>

namespace recomp {
namespace {
u32 sModelsThread = 0;  // the guest thread building the map's models (fn_801713CC), 0 if none
constexpr auto kMaxWait = std::chrono::seconds(10);
}  // namespace

// The map's models in their solid heap (the second DVD thread, under the map's setup).
HLE(fn_801713CC) {
    const u32 outer = sModelsThread;
    sModelsThread = sched::currentGuest();
    callOriginal(c, "fn_801713CC");
    sModelsThread = outer;
}

// LMDvdFile::closeFile(void*, LMDvdFileInfo*): on the first DVD thread, a read done, before its
// heap group, its memory and its callback.
HLE(closeFile__9LMDvdFileFPvP13LMDvdFileInfo) {
    static const u32 sDvdThread1 = VA(0x8039EE60);
    const u32 self = sched::currentGuest();
    if (sModelsThread && sModelsThread != self && self == sDvdThread1) {
        const auto deadline = std::chrono::steady_clock::now() + kMaxWait;
        int retraces = 0;
        while (sModelsThread && std::chrono::steady_clock::now() < deadline) {
            frame::waitRetrace();
            ++retraces;
        }
        log("[load] a load finished while the map's models were being built: it waited %d retraces%s", retraces,
            sModelsThread ? " (and went ahead)" : "");
    }
    callOriginal(c, "closeFile__9LMDvdFileFPvP13LMDvdFileInfo");
}

}  // namespace recomp

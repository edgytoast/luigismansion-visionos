// Apple Vision Pro: no updater. Upstream's self-updater (src/port/update.cpp) checks GitHub
// Releases, downloads a Windows/Linux package and swaps the program's files: none of which an app
// on visionOS may or can do (it can't rewrite its own bundle or start processes), and this port's
// builds are made from source with the player's own disc. This replaces it with the same API that
// never checks, never downloads and reports nothing, so the F1 menu shows no update notice.
// Written for luigismansion-visionos (Trevorbilt).
#include "port/update.h"

#include <cstdlib>
#include <string>

#ifndef LM_VERSION
#define LM_VERSION "0"
#endif

namespace port::update {

const char* currentVersion() { return LM_VERSION; }
const char* buildVersion() { return LM_VERSION; }

State state() {
    State s;
    s.phase = Phase::Idle;
    s.packaged = false;
    s.installWritable = false;
    return s;
}

void startupCheck() {}
void checkNow() {}
void download() {}
void cancel() {}
void installAndRestart() {}
bool shouldNotify() { return false; }
void skipThisVersion() {}
void later() {}
bool dismissed() { return true; }
void cleanupAfterUpdate() {}

int compareVersions(const std::string& a, const std::string& b) {
    // "v1.1" == "1.1.0" < "1.1.1", as upstream's.
    auto next = [](const std::string& s, size_t& i) -> long {
        while (i < s.size() && (s[i] < '0' || s[i] > '9')) ++i;
        long v = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') v = v * 10 + (s[i++] - '0');
        return v;
    };
    size_t i = 0, j = 0;
    for (int part = 0; part < 4; ++part) {
        const long x = next(a, i), y = next(b, j);
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

}  // namespace port::update

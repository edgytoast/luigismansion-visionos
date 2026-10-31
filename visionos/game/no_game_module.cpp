// Stands in for the recompiled game module until the player's disc is there: the framework links
// and the app launches, and the host reports that the game wasn't built in. The real module is
// generated at build time from the player's own main.dol (visionos/scripts/build-visionos.sh) and
// is never committed. Written for luigismansion-visionos (Trevorbilt).
#include "recomp/ctx.h"

namespace recomp {
extern const GameModule kGameModule = {kModuleAbiVersion, "NONE", nullptr, 0u, nullptr, 0u, nullptr, 0u};
}  // namespace recomp

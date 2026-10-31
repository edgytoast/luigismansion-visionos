#include <aurora/main.h>
#undef main

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_VISION
// Apple Vision Pro: main() belongs to the SwiftUI app around the game. The
// game's host bridge calls aurora_main() on a thread of its own, after
// SDL_SetMainReady(), once the immersive space exists; SDL_main.h would
// otherwise bring SDL's own main(), which starts a UIApplication.
#else
#include <SDL3/SDL_main.h>

int main(int argc, char** argv) { return aurora_main(argc, argv); }
#endif

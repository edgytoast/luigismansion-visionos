# Apple Vision Pro: SDL gates its virtual-joystick driver behind HIDAPI, which
# visionOS does not have, so the driver disappears there with it. The driver
# itself uses no HIDAPI, and the VR menu gamepad (dusk::vr, vr_menu_gamepad.hpp)
# attaches one, so keep it on visionOS.
if (NOT DEFINED SDL_SOURCE_DIR)
  message(FATAL_ERROR "SDL_SOURCE_DIR is required")
endif ()

set(_cmakelists "${SDL_SOURCE_DIR}/CMakeLists.txt")
file(READ "${_cmakelists}" _source)
set(_old [==[dep_option(SDL_VIRTUAL_JOYSTICK    "Enable the virtual-joystick driver" ON SDL_HIDAPI OFF)]==])
set(_new [==[dep_option(SDL_VIRTUAL_JOYSTICK    "Enable the virtual-joystick driver" ON "SDL_HIDAPI OR VISIONOS" OFF)]==])

string(FIND "${_source}" "${_new}" _already_patched)
if (NOT _already_patched EQUAL -1)
  message(STATUS "SDL3 visionOS virtual-joystick patch already applied")
  return()
endif ()

string(FIND "${_source}" "${_old}" _patch_site)
if (_patch_site EQUAL -1)
  message(FATAL_ERROR "Failed to apply SDL3 visionOS virtual-joystick patch: expected dep_option not found")
endif ()

string(REPLACE "${_old}" "${_new}" _patched_source "${_source}")
file(WRITE "${_cmakelists}" "${_patched_source}")
message(STATUS "Applied SDL3 visionOS virtual-joystick patch")

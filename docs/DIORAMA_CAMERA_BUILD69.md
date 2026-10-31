Build 69: authored diorama tour framing

The roomDrawn override previously promoted every main-scene camera to World,
including wide scripted shots. A wider/head-tracked diorama view exposes the
room shell beyond the authored camera frustum. Diorama main-scene cinematic shots
without an own-camera inset or dialogue now retain StereoPanel framing (except
training map 3). NPC lead-ins with insets, dialogue, menus and gameplay keep their
prior presentation. No room-wall visibility or first-person camera rule changed.
Camera/avatar/HLE regression tests pass. Visual acceptance on PCVR is pending,
especially the room tour, key ghost and Toad dialogue lead-in.
Current was updated with a build68 rollback; private profiles retained. Sharing
ZIP was generated from clean staging without ROMs or compiled game modules.

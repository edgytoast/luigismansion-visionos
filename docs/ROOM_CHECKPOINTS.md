# Room testing checkpoints (VR build 75)

F1 > Testing has four selectable checkpoint slots, Capture and Reload buttons,
and an optional stick-click shortcut checkbox, disabled by default. Shortcut
preference is stored separately for diorama, life-size, first person and flat.
Disabling it retains slots and menu buttons. With shortcuts enabled, left-stick
release captures the selected slot; right-stick release reloads it. Both sticks
still toggle F1 and cancel single-stick actions. Right-click recenter in diorama
is suppressed only while checkpoint shortcuts are enabled; F6 still recenters.
Tracked controllers and a connected physical gamepad are sampled separately.

Capture while in normal gameplay before an event. Requests defer through events,
Game Boy interfaces and transitions. Restore uses native map teardown/setup;
progress is restored after outgoing teardown so its player-record update cannot
overwrite the checkpoint. Original save pack fn_80067108 / restore fn_80067050
(PAL 80069E48 / 80069D90) preserve the retail progress payload (0x3B0 bytes).
Original Luigi message 59 synchronizes live inventory/treasure/health first. The
256-bit event flag array is also captured. Native ToolData reads override Luigi's
spawn position and room; facing is applied once outside a scripted event.

These are room reloads, not exact-moment states. Native collision may adjust the
spawn; ghosts, moving objects, animations and entry events are recreated normally.
Unlocked/story door state follows progress; an open door may close on room reload.
Checkpoint controls do not issue memory-card writes. A normal in-game save made
later saves the restored progress, as expected.

Slots are private save data under configDir/test-checkpoints, separated by PAL/USA.
They use a format tag, region check, bounded metadata, finite position checks and
a checksum. Complete temporary files replace slot files; corrupt/incompatible
slots are ignored. A restart defaults to the most recently captured valid slot.
They store no actor pointers, executable code, GPU objects or disc assets. Keep
this folder out of distributable packages, like other private profile save data.

Validation: camera/avatar/HLE uniqueness and checkpoint chord/disable tests pass.
Isolated PAL native test captured progress, changed an event flag, visited room37,
and returned to captured foyer with matching flags, map, room and health. Position
was adjusted approximately 93 native units by the native entry/collision path.
The corrected second test captured room37, visited the foyer and restored room37
with matching flags, map, room and health; position error was 0.00 native units.
The first attempt caught missing PAL spawn hook tags; the four original matched
hooks are now enabled in both regions and the private PAL test exited normally.
Headset selection and real controller clicks still need user verification.

Build 118 appends a checksummed ELM1 extension to new captures with Luigi's active element type and remaining supply. The original checkpoint payload and checksum remain unchanged, and legacy files still load. The extension is restored after actor initialization, then native message 59 refreshes the player record. Recapture legacy slots once to include element state.

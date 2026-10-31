# Native door adapter notes â€” checkpoint 34

These findings came from the user's locally owned retail discs. No game assets are stored in this document.

## Verified state layout

- Runtime leaf table: USA r13 + 0x2A4, PAL + 0x2B4. Leaf count u16: USA +0x2B0, PAL +0x2C0. Leaf stride 0x68. Both layouts verified from native code.
- Leaf +0 logical door ID; +1 model type; +0xC transient flags; +0x10/14/18 placement; +0x28 placement yaw; +0x2A/2B room links; +0x2C placement matrix; +0x5C animation index; +0x60 animation frame; +0x64 native animator.
- Animator +0x30C node count, +0x128 node matrix array. Retail door_01 has three nodes: root, slab, knob. Use the animated node matrices, not raw BIN node translations (the rest animation changes them).
- Only model type 1 is enabled in the prototype. Knob contacts are its animated node 2, local Z +/-32 units; contact radius 0.20 metres. Software tests do not prove the physical fit feels right in a headset.
- Closed flag 8; opening 0x40 with animation-active 0x200; open 0x10; reverse closing 0x80. Direction selection uses 0x800. Idle 0x100 is orientation. Observed visibility bits 1 and 2 are allowed. Unknown bits fail closed.
- Lock flags 0x4400 and seal flag 0x10000 are excluded. No persistent key/save flag is changed by this prototype.

## Native behavior and limits

fn_8001B888 updates door animation. fn_8001BDD8 updates active leaves and poses the animator with fn_8001EC34. The adapter writes the native opening state after a validated physical gesture; it does not translate those functions or synthesize A.

The original traversal strategy does more than start the model animation. Its native routines include fn_8017FF50 and fn_8017FD48; Koga collision enable/disable calls are fn_800BA21C / fn_800BA278, with a query fn_800BA210 used by fn_8007999C. Those roles need further tracing before adding a traversal override. Do not infer that visually open automatically means the room boundary can be walked through.

## Testing

Ignored work/test-physical-door33.py simulates an empty controller at the real knob, then rotates its orientation. Both PAL and USA tests completed native opening and recovery closing, with normal exits (PAL 61.5 seconds; USA result in its private profile). work/test-door-recovery33.py additionally drives the same native reverse-close state used by turning the option off. See the per-version result.json and stdout.log under work/door-recovery-smoke-*.

work/test-door-crossing33.py uses native PAD input after opening, without setting actor position to cross the doorway. It has not completed a room transition. At door ID 43, leaf 22, starting in room 42, it stopped near x -925, y 550, z -3354 while aiming beyond the door plane at x -1025. Steering had to be aligned with the aperture first because small tangent input was lost at a wall; even the revised run did not reach room 41. This is evidence of an unresolved traversal path, not proof that every possible manual path is blocked.

The test-only PAD helper requires LM_VR_DOOR_WALK_TEST plus the diagnostic avatar flag and an active synthetic door test. Normal launchers clear these flags. It never changes actor position directly.

## Next implementation

Trace the original door strategy's complete lifecycle, room streaming/handoff and collision changes. Add a narrow, validated open-door corridor crossing adapter, preserving floor collision and real walls. Test both sides of a door and room return, before enabling physical traversal by default. Then extend model coverage and physical key ownership/insertion.

## Checkpoint 34 — reversible physical gesture

An owned, fully open type-1 leaf now accepts a second release/grip/twist gesture to reverse the native animation. Only leaves opened by this adapter qualify; native/story-opened leaves, locks, seals, unsupported models and animating leaves are excluded. The open knob contact follows the live animated matrix. No A input or progression mutation is introduced.

The PAL native cycle fixture passed opening, release/rearm, a second physical twist and native closing, exiting normally in 79.6 seconds. Pure eligibility tests cover valid orientation/direction/visibility flags and rejection of closed, opening, closing, locked, sealed, wrong-room and unsupported leaves. The USA native cycle also passed with a normal exit in 81.6 seconds.

Additional isolated crossing experiments did not pass: clearing the verified native per-leaf collision byte (USA r13+0xCF4/count+0xCF8; PAL +0xD1C/count+0xD20) still stopped at x -925. Steering 100 units along the door tangent also remained blocked. The collision-byte experiment was removed from source; it is not a shipping collision override. Room crossing remains unfinished and the feature remains off by default.

LM_VR_DOOR_CYCLE_TEST and LM_VR_DOOR_TANGENT_TEST are diagnostic-only companion flags requiring the existing synthetic-door/avatar fixture. The normal launcher clears them.

## Confirmed first-person traversal target

The requested interaction is physical knob grip/twist, in-world door opening, and player-controlled walking through the aperture. Crossing must not invoke the flat stereo door presentation or move the player automatically. Close the leaf behind the player only after the player and tracked hands have cleared its sweep; do not close on a player who stops in the doorway or reaches back through it. A successful crossing must preserve native room visibility, streaming, floor/wall collision and return traversal.

Locked doors retain their existing flat stereo key/unlock presentation until physical key ownership and insertion are implemented. Story seals and unsupported doors retain native behavior. The current open/close prototype does not yet satisfy manual crossing; it remains disabled by default. No headset test is requested until offline crossing and return fixtures pass.

## Checkpoint 39 - narrow native doorway aperture

Native collision tracing found that the first blocking planes in the corridor fixture are the wall faces at X=-980 and X=-1070, 45 units either side of the leaf placement X=-1025. The earlier 20-unit plane tolerance missed both. The actor +0xB2 value reported as a wall in earlier diagnostics is actually a floor polygon index (7167 is a horizontal floor); diagnostic wording was corrected.

The adapter scopes a collision-query exception to Luigi's native fn_8005E7FC call and adapter-owned, fully open type-1 doors. fn_800BD870 keeps its original implementation for every other query. Only a near-horizontal wall normal parallel to the leaf's normal, within 50 native units of its slab plane, can qualify. The player query center must be inside a narrow aperture (55 tangent, 180 normal, 0..240 vertical units from placement). Unknown flags, locks, seals, floors, transverse walls and other actors fail closed. Geometry and persistent progression data are never changed. Native movement and room membership/handoff still run.

The USA one-way fixture passed a simulated grip/twist followed by native PAD walking into room 41 from room 42, with no crossing teleport, normal exit in 29.4 seconds. Round-trip PAL/USA verification is recorded below when complete.

A DoorPassage tracker schedules native closing only after the linked-room crossing, 200 units of player clearance, both tracked hands at least 30 units through, and 0.5 seconds of continuous clearance. Reaching back, returning, invalid tracking, unrelated rooms or a long update gap reset the dwell. Only adapter-owned leaves can auto-close. This behavior is still experimental and requires a headset reach/clearance check.

The initial one-way result was insufficient: the first round-trip fixtures exposed a floor gap at the threshold, allowing a fall into lower room 12. Traversal remains experimental while corrected. The collision aperture now requires a validated threshold: native floor probes 120 units on either side must agree within 2 units and both lie within 4 units of placement height. Failed probes keep collision intact. An initial floor-query fallback was insufficient and removed. The final adapter supplies a small virtual floor contact through the existing native collision-query interface, only inside the 65-unit threshold strip, against an upward native floor normal at the validated height. The sphere must touch that height (within 8 units of penetration); airborne players and unrelated floors are excluded. The game's original grounding code performs the constraint. No actor-position store or blanket collision disable is introduced. Stepped/sloping thresholds fail closed.

Final native round-trip fixtures passed with normal exits: USA 30.5 seconds; PAL 30.4 seconds. Both used a simulated grip/twist, native PAD walking 42 -> 41 -> 42 and the native reverse-close animation. No A inputs were supplied after frame 605, before the hand gesture begins after frame 900. No actor-position writes cross the portal; LM_TELEPORT only establishes the fixture's initial position. The return remained on the upper floor instead of falling into room 12. This validates one supported doorway in each version, not every door in the mansion. Physical knob reach and hands-clear auto-close still require headset testing.

The added LM_VR_DOOR_RETURN_TEST is diagnostic-only and also requires LM_VR_DOOR_TEST plus the synthetic-avatar fixture. Portable launchers clear it.

# Physical interaction migration â€” checkpoint 34

Scope: isolated first-person build only. Frozen base-game and accepted diorama output stay untouched.

## Available in this build

- Inventory toolbar is the default first-person selection layout. Four separated waist-level slots use original vector symbols. Owned flashlight, vacuum and Game Boy Horror can be selected by hand hover then grip, with gold highlighting and haptics. Release returns equipment. F1 > VR can restore the earlier hip/shoulder layout. Keys remain unavailable until native key ownership and consumption are integrated; no key is invented.
- Ghost follow uses the captured actor directly, with bounded horizontal turning while the left stick is held backward and the vacuum is activated. Headset looking remains tracked; manual snap turn pauses assistance.
- Backward stick supplies native resistance away from the hooked ghost. Physical controller movement opposite its escape direction opens a short native damage window. Sideways/forward stick input cannot substitute for the motion stroke. This capture path needs a real hooked-ghost validation; the automated encounter did not reach that state.
- The existing authored frame-piece removal option now also applies in first person. It removes only nodes marked as fadeable outer frames, and restores their batch count after each draw. Native closed-door visibility checks remain intact. It is not a blanket removal of room walls, and does not guarantee all black surfaces are gone.

## Ordinary wooden door prototype

F1 > VR now has an off-by-default physical wooden door prototype. It locates the actual knob through the native door animator's node matrices, projects a near-contact marker in stereo, gives hover/grip/completion haptics, and accepts a twist from an empty tracked hand. A tool in the other hand does not prevent the interaction.

Only the verified ordinary wooden door model (type 1, three animated nodes) is accepted. The native room link must include Luigi's current room, the door must be closed and unlocked, and unknown/busy/seal flags fail closed. The adapter starts the original in-room opening animation by setting its transient animation state, without injecting A, starting a cinema, changing key ownership, or bypassing locks.

Turning the option off schedules native closing for leaves opened by this adapter. Retained leaf pointers are checked against the current table and map before reuse; changing maps discards them.

Both PAL and USA native fixtures have completed the opening animation from a simulated contact and wrist twist and the native recovery closing animation. Manual room crossing remains under investigation; the prototype stays off by default. This is not a completed replacement for the normal door interaction.

WorldGesture also handles upward chest-lid lift, inward key insertion, and repeated directional shake, with two-hand requirements and tracking-loss/release safeguards. Those gestures are not yet connected to chests, keys or furniture. No A-button substitution is used.

A disc-local audit tool inspected 14 map archives per version (856 PAL / 855 USA furniture/item metadata records). Reports live under ignored work/. Counts are metadata rows, not a claim that every row is an interactable object. Native live-object identity, key ownership, hinge/knob contacts, large-object hand requirements and progression commits still need adapters.

## Remaining implementation

1. Extend the ordinary door prototype: manual swing/latch behavior, collision, portal visibility, and room transition on actual crossing. Add other verified door models and physical key integration. Keep fake/trap doors and boss/story seals working. Do not just suppress the door cinema: the original sequence also performs game-state changes.
2. Key inventory: enumerate owned keys, show the correct belt item, insert into matching lock, consume only after native unlock success. Retain unrelated item-get presentation.
3. Chests: connect lid angle to native animation and trigger contents/rewards exactly once, with state persisted normally.
4. Furniture: bind grab/shake intents to native furniture actor/state and its existing ghost/item/event dispatch. Catalog exceptions such as telephones, switches, traps, doors disguised as furniture and boss objects.
5. Game Boy Horror: actual held model with a render target on its screen; map/camera mode selection by touching device controls; aim inspection/photo ray from the physical device. Current grab still opens the native panel and is not this completed feature.
6. Local-only hose suppression: identify its actual retail draw geometry and scope the removal to the local eye pass. Preserve mirror, reflection and shadow geometry. Investigation ongoing; do not hide the entire backpack or an unidentified effect.
7. Remaining camera-authored occluders: inspect individual black surfaces, distinguish authored cutaways from real walls and portal culling.

## Optional combat ideas (not enabled)

- Precisely aimed flashlight cone: require a brief continuous exposure before the stun; do not stun from an unfocused peripheral beam.
- Brief recovery after a successful stun, rather than permanently disabling either tool. Consider a stronger charge/shorter pulse to reward timing.
- A stunned ghost becomes temporarily resistant to repeated flashlight hits while hooked, so holding both triggers cannot endlessly reset the struggle.
- Keep native escape changes, damage windows and release rules. The motion hand supplies counter-pull direction; stick-back only braces. Require a new opposing stroke when the ghost changes direction.
- Give clear haptic/tension feedback for correct vs wrong pull. Prefer readable feedback to arbitrarily multiplying ghost health.
- Optional advanced profile: sustained simultaneous use builds heat, which cools when either trigger is released; tune after baseline physical combat works. Avoid forcing rapid shoulder regrabs.

Recommendation: begin with aimed exposure plus short stun recovery and motion-gated damage, then test the native PAL balance before adding health/heat modifiers.

## Checkpoint 34 addition

Grip/twist can also close an ordinary wooden door opened by this prototype: release the first grip, reach its moved knob, grip and twist again. Native closing plays in-room. Story-opened leaves cannot be closed by this adapter. This extends the off-by-default prototype; manual room crossing is still unresolved.

### Next headset checkpoint

Before expanding hand-contact interactions, check the real controller/glove/tool alignment, knob hover reach and a captured ghost's camera follow/opposing pull. Native synthetic poses prove dispatch and animation, not the physical hand fit. The door option remains off by default and does not yet allow manual room crossing; ordinary door controls are still required.

Hose tracing captured a complete PAL frame with all active S3D model instances, including mirror/shadow passes. It has not isolated the visible hose geometry. A live gameplay state with the hose clearly visible near the uncovered mirror is needed to compare the actual draw; no guessed mesh suppression has been enabled.

## Checkpoint 35 repair

Fixed the missing headset toolbar: per-eye UI now reaches the XR output instead of only the desktop. The flashlight slot is on the right and the Game Boy Horror slot on the left; grab zones widened without overlap. This addresses invisible target locations, not a loss of controller tracking. Native hip/shoulder models can still appear while equipment is stowed; use the highlighted toolbar slot when toolbar mode is enabled.

## Checkpoint 36 — belt reach, order and stowed models

Moved the toolbar from 58 cm to 70 cm below the recentered head and from 38 cm to 23 cm forward. Slots from left to right are item/key, Game Boy Horror, flashlight, Poltergust; identities and ownership remain unchanged. Adjacent reach volumes remain non-overlapping. The item slot remains reserved and unavailable until the native key/item adapter is complete.

Replaced generic glyphs with colored equipment silhouettes: a lens/barrel flashlight, Poltergust pack/hose/nozzle, and the Game Boy Horror case/screen/buttons. Highlight and haptics remain tied to the same grab targets.

During active local first-person equipment mode, native stowed nozzle, backpack and Game Boy Horror mesh draws are omitted. Model calibration/update continues so later held and dual-tool rendering still works. Held tools remain visible. Explicit reflection passes, reflected camera bases, shadow passes and authored pickup/startle/cinema presentation retain native draws. No equipment ownership or saved progression is changed. Headset reach and mirror visibility need user confirmation.

## Checkpoint 37 — three-slot belt and shoulder vacuum

Raised the default belt by 20 cm from checkpoint 36, to 50 cm below the tracked head, with 23 cm front reach. Only item, Game Boy Horror and flashlight remain on the belt (left to right); Poltergust uses a 34 cm shoulder zone. F1 > VR's existing left-shoulder option applies in toolbar mode too. Added persisted F1 belt height/front-distance controls and reset. The item slot remains reserved until its native adapter is ready.

Removed the toolbar's stacked marker boxes. Each equipment silhouette and its highlighted card now share exactly one projected center, tied to its grab contact.

Reflection equipment uses native model poses plus the same body-heading correction as Luigi. Controller aiming/contact offsets and dual-tool clones are excluded from reflection passes. With equipment mode enabled, reflected arms/fingers retain the native animation instead of tracked wrist IK, so native held tools and hands share an authored pose. First-person held tool motion is unchanged. Mirrors show the game's native active tool, not a second VR-only clone when both tools are held. Native mirror fit and shoulder/belt reach await headset confirmation. Camera/avatar/hook checks pass.

## Checkpoint 38 — model holsters and independent ghost pulls

Removed the toolbar, belt markers, reserved key box and toolbar controls. Existing ini layout fields remain readable for compatibility but toolbar mode is retired. New defaults are flashlight left hip, Game Boy Horror right hip and Poltergust right shoulder. F1 > VR retains a side checkbox for each tool. An ini schema marker migrates old side defaults once while preserving subsequently saved choices.

Native stowed equipment meshes are visible again. Flashlight uses its cached model at the matching hip mount, including when the native nozzle has switched to vacuum. Game Boy Horror's mesh center follows the selected hip. Mirrors retain the checkpoint 37 native arm/equipment pose. First-person tracked hand/tool aiming remains active while held.

Replaced the single shared ghost pull state with sixteen independent capture tracks. Alternating native capture callbacks no longer erase each other's stroke or repeatedly change camera focus. The focus stays on a fresh primary ghost until it releases/expires. Each ghost retains its own observed escape direction, stroke distance and 160 ms native-tick permission window. A 1.5 cm opposing stroke at over 6 cm/s inside a 60-degree opposition cone opens that ghost's window; wrong-way/sideways travel, stale capture, tracking loss or a long frame gap deny it. Holding still expires the window. Direction changes greater than 60 degrees reset that ghost's accumulated stroke/window.

Only the hand holding and activating Poltergust contributes controller movement, relative to tracked head translation. Stick-back is separate resistance/camera assistance, not a second prerequisite for damage permission. Native damage cadence, native ghost behavior and final-HP handling remain in place. A queued pulse in the Poltergust hand confirms actual native HP drain, rate limited to 90 ms. Multiple ghosts moving in different directions require different opposing strokes; they do not receive blanket damage from one permitted target.

Software regressions cover interleaved two/three-ghost updates, stable focus, independent directions, wrong-way motion, stationary timeout, direction changes, release, stale tracks, long frame gaps and reset, plus the new default/swapped holster sides. Native multi-ghost gameplay still needs headset confirmation.

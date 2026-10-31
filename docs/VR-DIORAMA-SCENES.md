Build84: forcing machines visible only in the actor draw loop exposes stale
camera-space model matrices. Native view-update loops fn_80060C88/80060D44 skip
hidden actors using the same flags. Scope the same seven-machine visibility
over those original loops too. The native code then calculates current view x
actor-world x animated-joint matrices each frame; no machine placement or joint
animation is frozen/translated. Test-only matrix checks compare each model root
against that expected composition. Results presentation stays accepted build81.
Isolated PAL matrix-validation smoke checked all seven machine roots: each
reported 0.000000 residual against current view x world x joint composition,
no matrix errors, normal exit after116.9s. Four regression suites pass.
Headset world-anchoring/animation review pending.

Build83: build82 room-node change did not retain machines and introduced
proximity pop-in; that change is removed. PAL map4 metadata has no furniture
entries: the machines are seven animated actors (kiarm, kiashuk, kifuta,
kisentak, kitv, kiusiro, kibet; native types77..83). During Diorama map4 actor
render passes, scope only their draw-hide flags (clear0x6100, set0x200) and restore
those exact bits after the original actor renderer returns. Leave animation,
AI, physics, character models and room-node behavior native. Guard initialized
model pointers and actor identity. This lasts for the map4 visit, including the
results phase, and ends on map change; accepted ACCOUNT presentation unchanged.
Tests cover exact machine-name selection, unrelated flag preservation/restoration.
Isolated PAL native processing smoke reached all seven machine actors, observed
native hide-flag transitions and exited normally after 111.8s. Four regression
suites passed. Headset visibility/animation confirmation still pending.

Build82 candidate: processing-room static machinery disappears while the
Diorama camera changes. Native event02 does not script-hide machines (DISPOFF
only targets gameplay Luigi). During native scripted map4 static-room draws,
exclude nodes from the VR frame cutaway and scope the graph renderer's existing
mode1 (as used by the model viewer) over native node calls, restoring its previous
mode afterwards. This retains authored geometry instead of flat-camera fades.
Silhouette passes and dynamic actor geometry keep native handling. No global
wall/culling change. Pure policy tests cover map/mode/script/actor isolation.
Area results keep accepted build81 ACCOUNT flat presentation. Headset machine
visibility review pending; this does not prove a missing native draw is retained.

Build81 correction: actual area-clear ACCOUNT results draw in fn_80050524
(PAL80052378), already hooked for native widescreen layout. The build80 marker
covered gallery plaques only, so it never fired on the reported screen. Mark
ACCOUNT draws explicitly and present their primarily 2D background/portrait/text
layout as a normal flat panel; do not apply room/eye transforms. Preserve gallery
plaque stereo presentation separately. ACCOUNT takes precedence over cached room,
dialogue and GBH flags. Tests cover those combinations. Actual Area1 headset
verification pending. Prior accepted camera rules unchanged.

Build80: area-clear boss-portrait results inherited tracked room presentation,
producing an enlarged/doubled display. Mark actual native fn_8018721C gallery/
area-results renderer each capture and give its screen layout stereo-panel
precedence over room/dialogue classification. Keep authored projections and
HUD sizes through existing cinematic replay. First-person dialogue override
must not promote a results screen back into world view. Flat-screen rendering
is unchanged. Added results/presentation regression cases; headset Area1 review
pending. Phone, parents-door, tour/key ghost and rescue rules unchanged.

Build79 correction: event15 is the professor call after the key, not a key-door
unlock. Its PDEMO resource is gb_call01. All fourteen retail gb_call01/gb_endcall/
gb_minigame1 phone events now count as dialogue before their first text/call-mode
frame. Actual non-phone door renderer precedence remains. Before marking a draw
as a dedicated player demo, exclude an active native phone script as well as the
existing call-anchored state. A stale archive alone does not exclude a door.
Tests cover every phone event lead-in, phone draw marker exclusion and later
actual-door precedence. Accepted tour/key ghost/rescue rules unchanged.
Headset post-key professor-call and parents-door verification pending.

Build 78: event-list exclusion did not fix parents-room key doors. The last
NPC archive can remain cached while a door draws through its own native PDEMO
renderer. Mark actual fn_80114E18 draws when not phone-anchored, snapshot that
marker in each tape, and prioritize it over cached NPC conversation identity.
Retain phone overlays, training and accepted bounded tour/key-ghost policy.
Tests cover a dedicated door camera with stale NPC identity and stale dialogue,
plus phone/training exclusions. Per-demo stereo diagnostics now record later
doors instead of exhausting the report limit on the opening. Headset check of
parents-room doors and nursery door pending.

Build 77: user accepted balcony Toad, but key-unlock event15 regressed. Text
commands alone are insufficient: native PDEMO events require dedicated player
camera presentation. Exclude ten newly promoted player-demo events (10,11,12,
15,29,42,54,70,80,91); retain the previously accepted training09 exception.
Ordinary dialogue classification now covers 41 events. Regression coverage
checks key-unlock cinema precedence and keeps balcony/courtyard Toad classified
as world dialogue. No camera transform/depth changes; headset key-door check pending.

Build 76: replace the initial five-event NPC allowlist with the retail dialogue
classification (SPEAK + WINDOW, no GCAMERA and no .cmn member). PAL scan found
51 dialogue events, including balcony Toad32, courtyard Toad63 and professor26.
This changes presentation before the first text draw, not scripts or controls.
Rescue01 and entrance02 remain authored; key07/tour48 retain accepted build74
forward-shot diorama framing. Menu/map precedence remains intact. Automated
classification/presentation checks cover later NPCs and movie exclusions.
Headset confirmation of the balcony encounter and other later dialogue pending.
Only event IDs/metadata were added; no retail scripts or dialogue were copied.

Build 74 candidate: tour/key-ghost retain World presentation with no aperture.
Selected active events use a fixed -0.05m viewingDistance (5cm forward), bypassing
the scripted 1.05m comfort setback. Scale and tracked stereo are unchanged.
Other cameras, rescue and conversations retain prior behavior. Headset acceptance pending.

Status: build 73 rejected in headset testing: narrower view, same geometry issue.
Tracked-world override disabled; returned preview to build 72.
Boundary helpers retained inactive for reference, not an accepted solution.

# Diorama authored scenes

Build 73 prototype: PAL event48 (foyer tour) and event07 (key ghost) use a tracked
world view, not a stereo texture panel. Their per-eye original-camera frustum
projects a 90% inset viewing opening at the scripted focus distance. Host masks
cover geometry outside that opening without altering retail room geometry.
The existing scripted-camera setback and animation eye separation still apply.
Masks reset each replay and are not enabled for ordinary gameplay, other modes,
NPC conversations, or event01 (the accepted rescue cinema treatment).
The event scope ends when the native script cursor clears, even if its archive
remains resident. No original event scripts or gameplay controls are changed.

The aperture math is reusable for future authored camera paths. Add individual
scene classifications only after the tour/key-ghost prototype passes headset
review. Avoid blanket conversion of every cinematic: some scenes need their
existing presentation. Check for exposed geometry inside the opening, clipping
of important action, unstable size, uncomfortable depth, and return to gameplay.
The aperture covers exterior geometry; it does not reconstruct missing geometry
or guarantee that disocclusion inside the original frame is resolved.

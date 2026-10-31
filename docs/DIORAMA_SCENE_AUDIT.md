# Diorama event-camera audit (build 126)

Generated from the locally mounted PAL disc. This is camera-command metadata, not retail script content.

General policy: rendered story/gameplay and dedicated item/door cameras use tracked diorama 3D when the new option is enabled. The classification does not depend on individual event IDs. Explicit menus, map/search, results, and Game Over retain their baseline rules. Pre-rendered movies remain 2D because the disc contains no 3D scene for them.

Existing accepted per-event framing is retained as the fallback when the option is disabled. Native control locks, event flags, warps, ghost AI, damage, and rewards are unchanged.

| Event | Camera commands | Warp | Dialogue | Save/choice commands |
|---|---|---|---|---|
| 01 | GCAMERA | Yes | Yes |  |
| 02 | CAMERA, CAMTARGET |  | Yes |  |
| 03 | CAMTARGET2 |  | Yes | Yes |
| 04 | CAMTARGET2 |  | Yes | Yes |
| 07 | GCAMERA |  |  |  |
| 08 | CAMTARGET | Yes | Yes | Yes |
| 09 | CAMTARGET, PDEMO | Yes | Yes | Yes |
| 10 | CAMTARGET, PDEMO | Yes | Yes |  |
| 11 | CAMTARGET, PDEMO | Yes | Yes |  |
| 12 | CAMTARGET, PDEMO |  | Yes |  |
| 13 | None |  | Yes |  |
| 14 | None |  | Yes |  |
| 15 | CAMTARGET, PDEMO |  | Yes |  |
| 16 | None |  |  |  |
| 17 | CAMTARGET2 |  | Yes | Yes |
| 18 | None |  | Yes |  |
| 19 | None |  | Yes |  |
| 20 | None |  | Yes |  |
| 21 | None |  | Yes |  |
| 22 | None |  | Yes |  |
| 23 | None |  | Yes |  |
| 24 | CAMTARGET |  | Yes | Yes |
| 25 | CAMTARGET |  |  |  |
| 26 | CAMTARGET | Yes | Yes | Yes |
| 27 | None |  |  |  |
| 28 | CAMTARGET2 | Yes | Yes | Yes |
| 29 | CAMTARGET, PDEMO |  | Yes |  |
| 30 | GCAMERA |  |  |  |
| 31 | GCAMERA |  |  |  |
| 32 | None |  | Yes | Yes |
| 33 | CAMTARGET |  | Yes | Yes |
| 34 | None |  | Yes |  |
| 35 | GCAMERA |  |  |  |
| 36 | CAMTARGET2 |  | Yes | Yes |
| 37 | None |  |  |  |
| 38 | GCAMERA |  | Yes |  |
| 39 | None |  | Yes | Yes |
| 40 | GCAMERA |  |  |  |
| 41 | None |  | Yes |  |
| 42 | CAMTARGET, PDEMO | Yes | Yes |  |
| 43 | None |  |  |  |
| 44 | GCAMERA, PDEMO |  | Yes |  |
| 45 | CAMTARGET |  |  |  |
| 46 | GCAMERA |  | Yes |  |
| 47 | None |  | Yes |  |
| 48 | GCAMERA |  |  |  |
| 49 | None |  | Yes |  |
| 50 | CAMTARGET2 | Yes | Yes |  |
| 51 | None |  |  |  |
| 52 | GCAMERA |  |  |  |
| 53 | None |  | Yes |  |
| 54 | CAMTARGET, PDEMO |  | Yes |  |
| 55 | None |  | Yes | Yes |
| 56 | None |  | Yes | Yes |
| 57 | None |  | Yes | Yes |
| 58 | None |  | Yes | Yes |
| 59 | None |  | Yes | Yes |
| 60 | None |  | Yes | Yes |
| 61 | GCAMERA |  | Yes |  |
| 62 | None |  |  |  |
| 63 | CAMTARGET2 |  | Yes | Yes |
| 64 | CAMERA |  |  |  |
| 65 | GCAMERA | Yes |  |  |
| 66 | GCAMERA |  |  |  |
| 67 | None | Yes |  |  |
| 68 | CAMTARGET, GCAMERA |  |  |  |
| 69 | CAMTARGET, GCAMERA, PDEMO | Yes | Yes |  |
| 70 | CAMTARGET, PDEMO |  | Yes |  |
| 71 | GCAMERA | Yes | Yes |  |
| 72 | GCAMERA |  |  |  |
| 73 | GCAMERA |  | Yes |  |
| 74 | GCAMERA | Yes | Yes |  |
| 75 | GCAMERA |  |  |  |
| 76 | GCAMERA |  |  |  |
| 77 | None |  | Yes |  |
| 78 | None |  | Yes |  |
| 79 | None |  |  |  |
| 80 | CAMTARGET, PDEMO | Yes | Yes |  |
| 81 | None |  | Yes |  |
| 82 | CAMTARGET2 |  | Yes |  |
| 83 | CAMTARGET |  |  |  |
| 84 | GCAMERA |  |  |  |
| 85 | GCAMERA |  | Yes |  |
| 86 | None |  | Yes |  |
| 87 | GCAMERA |  | Yes |  |
| 88 | CAMTARGET, GCAMERA, PDEMO | Yes | Yes |  |
| 89 | CAMTARGET2 |  | Yes |  |
| 90 | GCAMERA | Yes |  |  |
| 91 | CAMTARGET, PDEMO |  | Yes |  |
| 92 | None |  |  |  |
| 93 | CAMTARGET |  | Yes | Yes |
| 94 | CAMTARGET |  | Yes |  |
| 95 | None |  |  |  |
| 96 | None |  |  |  |
| 97 | None |  |  |  |
| 98 | None |  |  |  |
| 99 | None |  |  |  |
| 100 | None |  |  |  |
| 101 | None |  |  |  |

Audited 99 event scripts. Native item/door rigs are covered separately through the existing player-demo draw marker and primary perspective-subview selection. Opening, staff-roll and ending scenes expand only when real perspective geometry was captured.

Validation covers the complete presentation state matrix and the existing camera/avatar/checkpoint/HLE regression checks. This audit does not mean every camera has been visually verified in a headset.

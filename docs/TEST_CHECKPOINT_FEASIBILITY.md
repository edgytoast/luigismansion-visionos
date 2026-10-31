Implemented room checkpoints in build75; see ROOM_CHECKPOINTS.md.
Exact-moment states remain unsupported for the reasons below.

# Test save states: feasibility and requested behavior

Requested: F1 opt-in testing shortcuts; four selectable slots; left stick click
captures selected slot; right stick click restores selected/latest slot. Disabling
shortcuts retains slots and returns the clicks to normal behavior. Both-stick
F1 chord must never accidentally capture, restore or recenter. F6 remains recenter.
Slots must record ROM hash, module ABI/build compatibility, timestamp and scene.
Normal memory-card saves must not be overwritten by test-state restoration.

No save-state implementation currently exists. Current runtime constraints:
- sched.cpp GuestThread uses std::thread; generated guest functions return via
  native C++ call stacks, not a serializable program-counter/continuation loop.
- Copying Ctx and MEM1 cannot rewind those native call stacks or HLE waits.
- frame.cpp has live host/guest turns, condition variables and retrace work.
- hle_dvd.cpp stores file/overlay maps on the host and queues asynchronous work.
- Audio/DSP, interrupt timing, file/card handles, GX state, EFB copies and
  interpolation history also require explicit capture/rebuild contracts.
- OpenXR sessions and GPU objects must remain live and be rebuilt/rebound safely;
  do not serialize their pointers or restore an entire process memory image.

Exact-moment support needs resumable generated execution, versioned state
serialization, a device-quiescence barrier and host subsystem snapshot adapters.
A scene checkpoint would instead request the native room/map load path and apply
validated progress at a stable point; it cannot claim to resume a cutscene frame.
No unsafe memory-only restore was added. Build75 implements the selected room
checkpoint alternative; build74 remains the preserved visual fallback.

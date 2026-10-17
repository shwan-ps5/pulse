# PS VR2 Sense controllers on the jailbroken PS5

Verified on hardware (firmware 13.60, ps5-opengl title, Pulse). Code: `platform/vr_ps5.cpp` (`openHands`, `hand`),
`platform/vr.h` (`vr::Hand`). Copy those two plus `vr_eyes.*` into a new game.

## Setup (once, after the headset tracker is initialised)
1. `scePadInit()`, then per hand `scePadOpen(userId, type, 0, NULL)`: **type 3 = left, 4 = right**. Returns a
   handle (e.g. 0x3840701 / 0x3850702).
2. Register each handle with the tracker: `sceVrTracker2RegisterDevice(deviceId, padHandle)`:
   **device 2 for the left (pad type 3), device 1 for the right (type 4)**; device 0 is the headset.
   Both return 0 when it works. Without this, no pose.
3. Every frame (or at least once per frame, after `sceVrTracker2` has the headset registered), read the pose
   and the buttons separately, below.

## Pose
`sceVrTracker2GetResult(in, out)`:
- `in` (0x20 bytes, zeroed): `+0x00` u32 = 0x20, `+0x04` i32 = the **pad handle**, `+0x10` u64 = the frame time
  used for the headset prediction.
- `out` (0x400 bytes, zeroed): `+0x04` u32 non-zero = pose valid, `+0x60` three floats = position in metres,
  `+0x70` four floats = quaternion (x y z w). Same tracker space as the headset's own pose, so convert hand and
  eyes with one shared origin (Pulse: recentre point + yaw, `toScene()` in `game/main.cpp`).
- Quality: tracking is good and steady. Hidden behind the back stays tracked; on a table the pose is **exactly
  constant** (bit-identical for minutes, gyro ~0.001): that is a controller at rest, not a stale or frozen
  tracker. The very first sample after tracking starts can be an outlier (one read -1.76 m): skip a couple of
  frames at start.
- Pulse's blade runs along the controller's **-z** axis from the controller position: correct, sabers sit
  where the hands are.

## Buttons, sticks, triggers: use `scePadReadState`
**`scePadVrControllerReadState` does not work from a title.** `sceKernelDlsym` finds it in the system
libScePad (rc 0), but calling it returns -1 (0xffffffff) and leaves the buffer zero. The PS5VR reference
describes it, but it was never confirmed; don't use it.

**Use the ordinary `scePadReadState(handle, buf)` on the Sense handle** (rc 0). It fills a normal ScePadData:

| Offset | Content |
|---|---|
| 0 | u32 buttons (bits below) |
| 4,5 | left stick x, y (byte, 0x80 centre, 0 = up/left) |
| 6,7 | right stick x, y |
| 8, 9 | L2, R2 analog (0-255) |
| 12-27 | IMU orientation quaternion (see below) |
| 28-39 | accelerometer, 40-51 gyro (rad/s) |

Button bits (same as the DualSense PS bits):

| Hand | Control | Bit |
|---|---|---|
| Left | Trigger | 0x100 (L2) plus analog byte 8 |
| Left | Grip | 0x400 (L1) |
| Left | Stick click | 0x2 (L3) |
| Left | Upper face button | 0x1000 (Triangle) |
| Left | Lower face button | 0x8000 (Square) |
| Left | Create | not seen (0x1 expected, unconfirmed) |
| Right | Trigger | 0x200 (R2) plus analog byte 9 |
| Right | Grip | 0x800 (R1) |
| Right | Stick click | 0x4 (R3) |
| Right | Options | 0x8 |
| Right | Face buttons | 0x2000 (Circle), 0x4000 (Cross) |
| Both | PS button | 0x80000000 (the system menu also reacts) |

### The right stick garbage (read this)
The left stick is always in bytes 4-5. The **right controller's stick alternates between bytes 4-5 and 6-7
from one read to the next**, and the other pair holds filler: `80 80` or `00 00`. Reading a fixed pair gives
`00 00` = "stick fully up" on about every other frame, which made the menu cursor jump whenever the controller
moved. Rule that works: for the right hand use bytes 6-7 unless they are `00 00`, otherwise 4-5
(`hand()` in `vr_ps5.cpp`). The real value rests near `82 7c` (small bias, so use a dead zone of about 0.1).
The left controller's 6-7 also hold filler (`00 00` / `80 80`): ignore them.

### IMU orientation: do not use
Offsets 12-27 sometimes return unnormalised or garbage quaternions (one sample read -1.7e38, others had
w = 1.08), and it sits in a different frame from the tracker's quaternion (differs by tens of degrees). Use the
tracker pose for orientation; the gyro (bytes 40-51) looks sane if a swing speed is wanted.

## Things not done or not known
- Haptics on the Sense controllers: `vr::haptic` starts a buzz with `scePadSetVibration` and stops it 60 ms later; not yet
  confirmed to be felt. Adaptive triggers: not tried. `vr::vibrate` drives the headset's rumble motor (`sceHmd2SetVibration`, 0-25).
- Finger touch / proximity sensing: not decoded (probably in the touch fields after offset 52).
- Left Create bit.
- Latency of pose versus the display: not measured.

## Debug aid
`vr_ps5.cpp` logs one line per hand per second (and on button changes): pad rc, tracker rc, valid flag,
buttons, trigger, stick, position, tracker quaternion, IMU quaternion, gyro. With the live log
(`tools/logsrv.py`, UDP 9023) this is how all of the above was found.

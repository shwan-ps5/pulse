# PS5 + PS VR2 homebrew: hardware and platform notes

What building Pulse taught us about the jailbroken PS5 as a platform for PS VR2 games. Written so a developer (or an AI assistant)
starting another game does not have to rediscover it. Tags: **[V]** verified on hardware (a Pro, firmware 13.60 unless noted),
**[U]** implemented or inferred but not verified, **[?]** open. Code: `platform/`, `ps5/`. Controller details: `SENSE-CONTROLLERS.md`.

## Stack
- Native title built with the ps5-opengl SDK (Mesa OpenGL 4.6 core via EGL on the console's GPU), the native app boilerplate and the PS5
  Payload SDK. `ps5/build.sh` stages and builds; the output is an app folder (`eboot.bin`, `sce_module/`, `sce_sys/`, `assets/`). [V]
- The app folder is mounted from USB (ShadowMountPlus) and appears on the home screen. FTP to the console is on port 2121 and only works
  right after the jailbreak is loaded. You cannot overwrite files of a running title (FTP 550): close the game first. [V]
- A title that returns from `main()` or calls `exit()` is killed with a crash report. Quit by `sceSystemServiceLoadExec("exit", nullptr)`.
  Closing from the PS menu still shows "something went wrong" [?]; the in-game Quit does not.
- The PS button belongs to the system. `platform::poll()` quits on Create + Options held for 1 s. [V]

## Sandbox and storage
- `opendir()` fails with `EPERM` on `/mnt`, `/mnt/usb0`, `/data` and `/download0`. `fopen()` of files in the app folder (`/app0`) and writes to
  `/download0` work. Listing `/app0/assets/...` is not verified [U]; the game falls back to an `index.txt` file list. [V for the failures]
- `/download0` is the title's persistent writable folder (settings, scores, cache). From FTP it appears at
  `/mnt/sandbox/<TITLEID>_000/download0/` only while the title runs. [V]
- The console keeps its own copies of `sce_sys` (icon, backgrounds, `param.json`) made when the title is first registered, in
  `/user/appmeta/<TITLEID>/` and `/user/app/<TITLEID>/sce_sys/`. Changing art in the app folder does not update them: copy the files there too. [V]
- Memory: the module heap is fixed (the build patches it to 256 MiB); ps5-opengl's own app heap defaults to 128 MiB and a title can ask for
  more with a strong `ps5_opengl_heap_size` (Pulse asks 1 GiB; the runtime falls back by halving). A 10-minute song used 143 MiB in total. [V]
  Free large buffers before allocating the next (loading a second song on top of the first crashed). [V]
- Time to decode and analyse on the console: about 1 s decode, 2-7 s analysis for 3.5-10 minute tracks. [V]
- Release scan (catalog): reports whether an app can leave the sandbox. Pulse's release has no network code and no loader/payload use and scores
  "no sign of leaving the sandbox". [V]

## Graphics
- EGL window surface from ps5-opengl (native handle 0); display mode and refresh are set before `eglInitialize`. 4.6 core, compute shaders,
  SSBOs, persistent mapped buffers, RGBA16F render targets all work. [V]
- The GL does not clip triangles at the eye plane: a large triangle with a vertex behind the camera disappears (menu slab vanished in one eye when the
  head turned). Tile big flat geometry or clip yourself. [V]
- On the console the bloom/post-process path was only used in the flat view; the headset path draws straight to the eye framebuffer. Drawing the
  flat view in VR mode crashed the title [V]; the loading screen is drawn through the eye path instead.
- Additive glow without depth write is hidden by any opaque geometry drawn after it: order passes accordingly. [V]
- Frame cost of the neon background in VR is unmeasured; Pulse ships it off by default on the console. [?]

## PS VR2 headset
- Libraries: `libSceHmd2` and `libSceVrTracker2` loaded with `sceSysmoduleLoadModule` + `sceKernelLoadStartModule` from the system library
  folder; functions by `sceKernelDlsym`, else by fixed export offsets that are firmware 13.60 only. Structures follow the PS5VR project. [V]
- Pipeline: eye buffers of 2048 x 2048 per eye (both eyes side by side in one 4096 x 2048 texture), rendered at 60 Hz and reprojected to 120 Hz;
  `waitSlot` paces the loop to the headset, `begin` gives the predicted eye poses, `submit` hands the buffer over. [V]
- The headset reads memory in the GPU's 64 KiB-block tiled layout. Copy the GL texture with a compute shader into a persistently mapped GL buffer
  (a mapped buffer is real direct memory with one address for CPU and GPU); a linear layout came out shifted. Three buffers rotate. [V]
- The TV mirrors the headset image once mirroring is enabled; the title must not present its own frames during VR. [V]
- Recentre: keep only the heading about the vertical axis. The tracker's first poses are stale; wait ~3 s of tracking before the automatic
  recentre, and give the user R3. A head-locked "press R3" hint helps when the menu starts off to one side. [V]
- Headset rumble: `sceHmd2SetVibration` level 0-25. [U]

## Controllers and input
- DualSense: `scePadOpen(user, 0, 0)`, `scePadReadState`. Sticks are bytes (0x80 centre); triggers 0-255.
- Sense controllers: pad types 3 (left) / 4 (right) registered with the tracker as devices 2 / 1; pose from `sceVrTracker2GetResult`, buttons and
  sticks from the ordinary `scePadReadState` (the VR-specific read does not work). Right-stick bytes alternate between two positions. [V]
  Full detail in `SENSE-CONTROLLERS.md`.
- Haptics on the Sense controllers: `scePadSetVibration` start + timed stop is implemented; whether it buzzes is unverified. [U]
- Finger touch, Create button on the left controller: not decoded. [?]

## Audio
`sceAudioOutInit`, `sceAudioOutOpen(0xFF, 0, 0, 256, 48000, FLOAT_STEREO)`; `sceAudioOutOutput` blocks until the port wants the next 256 frames, so
a thread that loops fill + output is paced by it. Keep the callback lock-free (try_lock, skip rather than wait). [V]

## Logging and debugging
- stderr goes to `/download0/ps5-opengl.log` (the runtime does this). Before the first frame it is out of reach, so early errors use
  `sceKernelSendNotificationRequest` pop-ups. Lines containing "error", "failed" or "no such" pop up automatically in `platform::log`. [V]
- Development builds also send each line as a UDP datagram to the PC in `loghost.txt` (port 9023); `tools/logsrv.py` prints them. Release builds
  compile this out. [V]
- Screenshots taken on the console are HDR JXR files under `/user/av_contents/photo/NPXS40087/<TITLEID>/`. `JxrDecApp -i in.jxr -o out.tif`
  then ffmpeg makes a PNG. [V]
- `test/hands.cpp` is a small title that shows the head and both sabers and probes which storage paths the sandbox allows.

## Open questions
Standard PS5 (not Pro) behaviour and speed; frame cost of the heavier backgrounds in VR; Sense haptics and finger touch; the "something went
wrong" on closing from the PS menu; the flat TV view flashing when the title presents its own frames; other firmware versions (the headset code
only has 13.60 offsets).

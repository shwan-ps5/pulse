# Pulse

A Beat Saber-style rhythm game for the jailbroken PS5 with PS VR2 and Sense controllers. Give it any MP3 and it
analyses the song (tempo, beats, build-ups and drops) and cuts Easy, Medium, Hard and Extreme note maps from it,
on the console, in a few seconds. The neon backgrounds and bloom follow the music.

Title ID `PPSA99993`. Needs a jailbroken PS5 running ShadowMountPlus (or another way to mount native app folders).

## Install

1. Copy the `PPSA99993` folder from the release zip to your mounted app location (for example `/mnt/usb0/PPSA99993`).
2. Start the jailbreak and mount it as usual, then launch Pulse from the home screen.

The release carries one generated demo song, "Neon Run".

## Adding your own music

Copy MP3s (in any folders) into `PPSA99993/assets/songs/`, then choose **Options, Rescan Music** in the game: new
songs appear in the list without restarting. Songs are read by their ID3 tags (artist, title, cover picture) and
fall back to the file name. Tracks over about 17 minutes are skipped. Maps are cached in `/download0/cache` the
first time a song is played.

If the console will not let the title list a folder, Rescan finds nothing. In that case list the files in
`assets/songs/index.txt`, one relative path per line: `python3 tools/make-index.py <path to assets/songs>` writes it.

## Playing

Stick or D-pad and the trigger navigate the menus. If the menu starts off to one side, look straight ahead and press
R3 to centre it (a hint shows until you do). **How to play** in the title menu explains the rules; in short:

- **Easy**: dots, cut in any direction, or just touched with the right saber.
- **Medium**: lines. Cut along a line either way for full points, or across it for half. The combo is never broken
  by direction.
- **Hard**: arrows. Cut the way the arrow points; the wrong way scores nothing and breaks the combo.
- **Extreme**: Hard with more blocks, pairs and faster runs.

A cut scores up to 115 times the multiplier (x2 at 2 in a row, x4 at 6, x8 at 14); a miss or a bad cut resets it.
Triangle on the song list picks a new pattern for a song. Options has colour-blind and high-contrast palettes, a
hand swap, and the background: off, flat or 3D.

## Building

Needs the PS5 OpenGL SDK, the native app boilerplate and the payload SDK checked out as `ps5/build.sh` describes.

- `bash ps5/build.sh` builds `build/ps5/dist/PPSA99993` (a development build also sends its log to this PC, see `tools/logsrv.py`).
- `bash ps5/release.sh` builds the shareable zip, with the demo song and no log address.
- `make play` runs the desktop version (SDL, keyboard or gamepad), `make test` the unit tests.
- Art: `tools/makeart.py`; demo song: `tools/makedemo.py` (both need a venv with numpy and pillow).

## Thanks

Pulse stands on a lot of other people's work:

- [ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl) and the [native app boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate), by BlackBearReloaded: OpenGL 4.6 on the PS5 and the app packaging. Without them there is no picture.
- The [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk) and everyone behind the PS5 jailbreak, for the toolchain and the way in.
- [PS5VR](https://github.com/theghostonline/PS5VR), by theghostonline: the PS VR2 headset, Sense controller and audio output handling that `platform/vr_ps5.cpp` follows.
- [Mesa](https://www.mesa3d.org/) and the OpenGNM projects, which ps5-opengl is built on.
- ShadowMountPlus, for mounting the app from a USB stick.
- The [PS5 homebrew catalog](https://github.com/blackbearreloaded/ps5-homebrew-catalog), also by BlackBearReloaded, for somewhere to list it.
- [minimp3](https://github.com/lieff/minimp3) by lieff, [stb_image](https://github.com/nothings/stb) by Sean Barrett, and the DejaVu fonts (Bitstream Vera).

## Licence

GPL-3.0-or-later, because the PS5 OpenGL runtime it links is. See `LICENSE` and `THIRD_PARTY_NOTICES.md`.

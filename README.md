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

Put MP3s (any folders) in `PPSA99993/assets/songs/`. The title may not be allowed to list folders on the console,
so also list them in `assets/songs/index.txt`: run `python3 tools/make-index.py <path to assets/songs>` on a PC,
or write one relative path per line by hand. Songs are read by their ID3 tags (artist, title, cover picture) and
fall back to the file name. Tracks over about 17 minutes are skipped. Maps are cached in `/download0/cache`
the first time a song is played.

## Playing

Triggers and sticks navigate the menus; Triangle picks a new pattern for a song, Options opens the settings
(saber colours with colour-blind and high-contrast palettes, hand swap, and the background: off, flat or 3D).
Dots can be cut any way (Easy); lines take any cut, across them for half points (Medium); arrows need the right
direction and a wrong one breaks the combo (Hard, Extreme).

## Building

Needs the PS5 OpenGL SDK, the native app boilerplate and the payload SDK checked out as `ps5/build.sh` describes.

- `bash ps5/build.sh` builds `build/ps5/dist/PPSA99993` (a development build also sends its log to this PC, see `tools/logsrv.py`).
- `bash ps5/release.sh` builds the shareable zip, with the demo song and no log address.
- `make play` runs the desktop version (SDL, keyboard or gamepad), `make test` the unit tests.
- Art: `tools/makeart.py`; demo song: `tools/makedemo.py` (both need a venv with numpy and pillow).

## Licence

GPL-3.0-or-later, because the PS5 OpenGL runtime it links is. See `LICENSE` and `THIRD_PARTY_NOTICES.md`.

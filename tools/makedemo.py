#!/usr/bin/env python3
# Synthesises the demo song shipped with Pulse: a 128 BPM house track, all generated here, so it carries no licence.
# Usage: .venv/bin/python tools/makedemo.py out.mp3
import subprocess, sys
import numpy as np

SR, BPM = 44100, 128
beat = 60.0 / BPM
bar = 4 * beat
plan = [("intro", 8), ("build", 8), ("drop", 16), ("break", 8), ("build", 8), ("drop", 16)]
bars = sum(n for _, n in plan)
N = int(bars * bar * SR) + SR
rng = np.random.default_rng(7)
bus = {k: [np.zeros(N, np.float32), np.zeros(N, np.float32)] for k in "dm"}


def add(buf, start, sig, gain=1.0):
    i = int(start * SR)
    end = min(N, i + len(sig))
    if i < N:
        buf[i:end] += sig[: end - i] * gain


def stereo(start, sig, gain=1.0, pan=0.0, to="m"):
    add(bus[to][0], start, sig, gain * (1 - max(0, pan)))
    add(bus[to][1], start, sig, gain * (1 + min(0, pan)))


def kick(vel=1.0):
    t = np.arange(int(0.32 * SR)) / SR
    f = 45 + 110 * np.exp(-t * 28)
    ph = 2 * np.pi * np.cumsum(f) / SR
    return (np.sin(ph) * np.exp(-t * 9) * 1.1 + np.sin(ph * 0.5) * 0.0) .astype(np.float32) * vel


def clap(vel=1.0):
    t = np.arange(int(0.22 * SR)) / SR
    n = rng.standard_normal(len(t)).astype(np.float32)
    n = n - np.concatenate([[0], n[:-1]]) * 0.9
    env = np.exp(-t * 22) + 0.6 * np.exp(-((t - 0.012) ** 2) * 9000) + 0.5 * np.exp(-((t - 0.024) ** 2) * 9000)
    return (n * env * 0.35 * vel).astype(np.float32)


def hat(open_=False, vel=1.0):
    t = np.arange(int((0.28 if open_ else 0.05) * SR)) / SR
    n = rng.standard_normal(len(t)).astype(np.float32)
    n = n - np.concatenate([[0], n[:-1]])
    return (n * np.exp(-t * (14 if open_ else 90)) * 0.22 * vel).astype(np.float32)


def tone(freq, dur, kind="saw", harm=8):
    t = np.arange(int(dur * SR)) / SR
    s = np.zeros_like(t)
    for h in range(1, harm + 1):
        if kind == "square" and h % 2 == 0:
            continue
        s += np.sin(2 * np.pi * freq * h * t) / h
    return s.astype(np.float32) * 0.5


def env(n, a=0.005, d=0.1, s=0.6, r=0.05):
    e = np.ones(n, np.float32)
    ai, di, ri = int(a * SR), int(d * SR), int(r * SR)
    e[:ai] = np.linspace(0, 1, ai, endpoint=False)
    e[ai : ai + di] = np.linspace(1, s, di)
    e[ai + di :] = s
    e[-ri:] *= np.linspace(1, 0, ri)
    return e


roots = [57, 53, 48, 55]  # Am F C G, as MIDI-ish roots an octave down below
mid = lambda m: 440.0 * 2 ** ((m - 69) / 12)
chords = [[57, 60, 64], [53, 57, 60], [48, 52, 55], [55, 59, 62]]
arp_notes = [[69, 72, 76, 72], [65, 69, 72, 69], [72, 76, 79, 76], [67, 71, 74, 71]]

pump = np.ones(N, np.float32)
bar_i = 0
for name, count in plan:
    for b in range(count):
        t0 = bar_i * bar
        ch = (bar_i // 1) % 4
        k_on = name in ("intro", "drop") or (name == "build" and b >= 2)
        if name == "intro":
            k_on = b >= 2
        for q in range(4):
            tb = t0 + q * beat
            if k_on:
                stereo(tb, kick(), 0.95, to="d")
                pi = int(tb * SR)
                m = min(N, pi + int(0.35 * SR))
                pump[pi:m] = np.minimum(pump[pi:m], 0.25 + 0.75 * np.linspace(0, 1, m - pi) ** 0.8)
        # hats: offbeats, sixteenths in the drop
        if name in ("intro", "build", "drop"):
            for q in range(4):
                stereo(t0 + q * beat + beat / 2, hat(True), 0.7, 0.2, "d")
                if name != "intro" or b >= 4:
                    for s16 in (0.25, 0.75):
                        stereo(t0 + (q + s16) * beat, hat(False, 0.6), 0.6, -0.2, "d")
        if name == "drop":
            for q in (1, 3):
                stereo(t0 + q * beat, clap(), 0.9, to="d")
        if name == "build":
            fr = b / 8
            reps = 2 if b < 4 else 4 if b < 6 else 8
            for rr in range(reps * 2):
                stereo(t0 + rr * bar / (reps * 2), clap(0.35 + 0.5 * fr), 0.7, to="d")
            # riser
            dur = bar
            tt = np.arange(int(dur * SR)) / SR
            f = 300 + (b + tt / bar) / 8 * 3000
            ph = 2 * np.pi * np.cumsum(f) / SR
            stereo(t0, (np.sin(ph) * 0.12 * (0.3 + fr)).astype(np.float32), 1.0)
        # bass
        if name in ("drop", "build") and not (name == "build" and b < 4):
            r = roots[ch] - 12
            for step in range(8):
                ts = t0 + step * beat / 2 + beat / 4
                d = beat / 2 * 0.8
                s = tone(mid(r + (12 if step % 4 == 3 else 0)), d, "saw", 6) * env(int(d * SR), 0.003, 0.05, 0.7, 0.02)
                stereo(ts, s, 0.55)
        # pad
        if name in ("break", "intro", "build", "drop"):
            gain = {"break": 0.34, "intro": 0.18, "build": 0.22, "drop": 0.2}[name]
            for note in chords[ch]:
                for det in (-0.25, 0.25):
                    s = tone(mid(note) * 2 ** (det / 12 / 4), bar, "saw", 5) * env(int(bar * SR), 0.25, 0.3, 0.8, 0.3)
                    stereo(t0, s, gain / 6, det * 2)
        # lead arp
        if name == "drop" and (b >= 4 or bar_i >= 40 + 8):
            notes = arp_notes[ch]
            for step in range(16):
                ts = t0 + step * beat / 4
                d = beat / 4 * 0.9
                s = tone(mid(notes[step % 4] + (12 if (b // 4) % 2 and step % 8 > 5 else 0)), d, "square", 5) * env(int(d * SR), 0.002, 0.04, 0.5, 0.02)
                stereo(ts, s, 0.20, 0.3 if step % 2 else -0.3)
        bar_i += 1

out = np.stack([bus["d"][0] + bus["m"][0] * pump, bus["d"][1] + bus["m"][1] * pump], 1)
out = out[: int(bars * bar * SR) + SR // 2]
fade = int(1.5 * SR)
out[-fade:] *= np.linspace(1, 0, fade)[:, None]
out = np.tanh(out * 1.2) * 0.85
pcm = (out * 32767).astype("<i2").tobytes()
subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "s16le", "-ar", str(SR), "-ac", "2", "-i", "-", "-c:a", "libmp3lame", "-b:a", "192k",
                "-id3v2_version", "3", "-metadata", "title=Neon Run", "-metadata", "artist=Pulse Demo", sys.argv[1]], input=pcm, check=True)
print(len(out) / SR, "s")

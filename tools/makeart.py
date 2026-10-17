#!/usr/bin/env python3
# Draws Pulse's launcher art: ps5/art/icon0.png (512x512), pic0.dds and pic1.dds (3840x2160, BC7 mode 6).
import math, sys, colorsys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
out = sys.argv[1] if len(sys.argv) > 1 else "ps5/art"


def hue(h, s=0.85, v=1.0):
    return tuple(int(255 * c) for c in colorsys.hsv_to_rgb(h % 1, s, v))


def glow(layer, radii=(3, 10, 28), gains=(1.0, 0.8, 0.9)):
    a = np.asarray(layer, np.float32)
    acc = a.copy()
    for r, g in zip(radii, gains):
        acc += np.asarray(layer.filter(ImageFilter.GaussianBlur(r)), np.float32) * g
    return acc


def scene(w, h, wordmark, seed_rot=0.0):
    s = h / 1080.0
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    cx, cy = w * (0.5 if wordmark is None else 0.5), h * 0.46
    d = np.hypot((xx - cx) / w, (yy - cy) / h * 1.2)
    base = np.zeros((h, w, 3), np.float32)
    base[..., 0] = 6 + 14 * np.exp(-d * 5)
    base[..., 1] = 8 + 10 * np.exp(-d * 5)
    base[..., 2] = 22 + 38 * np.exp(-d * 4)
    acc = base

    rings = Image.new("RGB", (w, h), 0)
    dr = ImageDraw.Draw(rings)
    n = 11
    for i in range(n):
        r = 70 * s * (1.34 ** i)
        sides = 6 if i % 2 == 0 else 5
        rot = seed_rot + i * 0.21
        pts = [(cx + r * math.cos(rot + k * 2 * math.pi / sides), cy + r * math.sin(rot + k * 2 * math.pi / sides) * 0.92) for k in range(sides)]
        col = hue(0.52 + i * 0.045, 0.9, 1.0)
        fade = max(0.18, 1.0 - i * 0.075)
        col = tuple(int(c * fade) for c in col)
        dr.line(pts + [pts[0]], fill=col, width=max(2, int(3 * s)), joint="curve")
    acc = acc + glow(rings, (2 * s, 8 * s, 26 * s), (0.9, 0.7, 0.8))

    stuff = Image.new("RGB", (w, h), 0)
    ds = ImageDraw.Draw(stuff)
    sab = [((-0.30, 0.62), (0.08, -0.12), (255, 40, 80)), ((0.30, 0.62), (-0.08, -0.12), (40, 140, 255))]
    for (bx, by), (tx, ty), col in sab:
        p0 = (cx + bx * h * 1.0, cy + by * h)
        p1 = (cx + tx * h * 1.0, cy + ty * h)
        ds.line([p0, p1], fill=col, width=int(26 * s))
    acc = acc + glow(stuff, (4 * s, 14 * s, 40 * s), (0.8, 0.9, 1.0))
    core = Image.new("RGB", (w, h), 0)
    dc = ImageDraw.Draw(core)
    for (bx, by), (tx, ty), col in sab:
        p0 = (cx + bx * h, cy + by * h)
        p1 = (cx + tx * h, cy + ty * h)
        dc.line([p0, p1], fill=(255, 235, 240) if col[0] > 200 else (230, 240, 255), width=int(10 * s))
    acc = np.maximum(acc, np.asarray(core, np.float32))

    cube = Image.new("RGB", (w, h), 0)
    dq = ImageDraw.Draw(cube)
    k = 130 * s
    ccx, ccy = cx, cy - 0.26 * h
    dq.rounded_rectangle([ccx - k, ccy - k, ccx + k, ccy + k], radius=int(26 * s), fill=(18, 20, 42), outline=(255, 70, 120), width=int(8 * s))
    dq.polygon([(ccx - k * 0.55, ccy - k * 0.15), (ccx, ccy + k * 0.5), (ccx + k * 0.55, ccy - k * 0.15), (ccx + k * 0.3, ccy - k * 0.15), (ccx, ccy + k * 0.1), (ccx - k * 0.3, ccy - k * 0.15)], fill=(255, 255, 255))
    ca = np.asarray(cube, np.float32)
    acc = np.where(ca.sum(2, keepdims=True) > 0, ca, acc) + glow(Image.fromarray(np.clip(ca, 0, 255).astype(np.uint8)), (6 * s, 20 * s), (0.6, 0.6)) * 0.35

    if wordmark:
        txt = Image.new("RGB", (w, h), 0)
        dt = ImageDraw.Draw(txt)
        f = ImageFont.truetype(FONT, int(190 * s))
        dt.text((w / 2, h * 0.86), wordmark, font=f, fill=(255, 255, 255), anchor="mm")
        acc = acc + glow(txt, (8 * s, 30 * s), (0.5, 0.7)) * 0.6
        acc = np.maximum(acc, np.asarray(txt, np.float32))
    return Image.fromarray(np.clip(acc * 0.92 / (1 + 0.0006 * acc), 0, 255).astype(np.uint8))


def bc7_mode6(img):
    a = np.asarray(img.convert("RGBA"), np.uint8)
    h, w = a.shape[:2]
    b = a.reshape(h // 4, 4, w // 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4).astype(np.float32)
    mean = b.mean(1, keepdims=True)
    c = b - mean
    axis = np.ones((b.shape[0], 4), np.float32)
    for _ in range(4):
        cov = np.einsum("bpi,bpj->bij", c, c)
        axis = np.einsum("bij,bj->bi", cov, axis)
        axis /= np.linalg.norm(axis, axis=1, keepdims=True) + 1e-6
    proj = np.einsum("bpi,bi->bp", c, axis)
    e0 = b[np.arange(len(b)), proj.argmin(1)]
    e1 = b[np.arange(len(b)), proj.argmax(1)]

    def quant(e):
        p = np.round((e % 2).mean(1)).astype(np.int64)
        q = np.clip(np.round((e - p[:, None]) / 2), 0, 127).astype(np.int64)
        return q, p
    q0, p0 = quant(e0)
    q1, p1 = quant(e1)

    def idx_for(q0, p0, q1, p1):
        d0 = (q0 * 2 + p0[:, None]).astype(np.float32)
        d1 = (q1 * 2 + p1[:, None]).astype(np.float32)
        dv = d1 - d0
        t = np.einsum("bpi,bi->bp", b - d0[:, None, :], dv) / (np.einsum("bi,bi->b", dv, dv)[:, None] + 1e-6)
        return np.clip(np.round(t * 15), 0, 15).astype(np.int64)
    idx = idx_for(q0, p0, q1, p1)
    swap = idx[:, 0] >= 8
    q0s, q1s = np.where(swap[:, None], q1, q0), np.where(swap[:, None], q0, q1)
    p0s, p1s = np.where(swap, p1, p0), np.where(swap, p0, p1)
    idx = np.where(swap[:, None], 15 - idx, idx)

    n = len(b)
    lo = np.zeros(n, np.uint64)
    hi = np.zeros(n, np.uint64)
    bit = 0

    def put(val, nbits):
        nonlocal bit
        val = val.astype(np.uint64) & np.uint64((1 << nbits) - 1)
        for k in range(nbits):
            pos = bit + k
            if pos < 64:
                lo[:] |= ((val >> np.uint64(k)) & np.uint64(1)) << np.uint64(pos)
            else:
                hi[:] |= ((val >> np.uint64(k)) & np.uint64(1)) << np.uint64(pos - 64)
        bit += nbits
    put(np.full(n, 64), 7)
    for ch in range(4):
        put(q0s[:, ch], 7)
        put(q1s[:, ch], 7)
    put(p0s, 1)
    put(p1s, 1)
    for p in range(16):
        put(idx[:, p], 3 if p == 0 else 4)
    assert bit == 128
    return np.stack([lo, hi], 1).astype("<u8").tobytes()


def write_dds(path, img, header_from):
    hdr = open(header_from, "rb").read()[:148]
    open(path, "wb").write(hdr + bc7_mode6(img))


if __name__ == "__main__":
    hdr = sys.argv[2]
    scene(512, 512, None).save(out + "/icon0.png")
    big = scene(3840, 2160, "PULSE")
    write_dds(out + "/pic0.dds", big, hdr)
    write_dds(out + "/pic1.dds", scene(3840, 2160, None, 0.4), hdr)
    big.resize((1280, 720)).save(out + "/pic0-preview.png")

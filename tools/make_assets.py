#!/usr/bin/env python3
"""
Tao tai nguyen giao dien tu anh mau UI/Page_N.png (1536x1024):
  - cat/thu nho ve 320x240 (khong meo hinh), doi sang RGB565 big-endian co dither
  - ghi assets/*.bin (nhung vao flash qua EMBED_FILES) va src/ui_assets.h (toa do, kich thuoc)

Chay:  python tools/make_assets.py
Can:   pip install pillow numpy
"""
import math
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI_DIR = os.path.join(ROOT, "UI")
OUT_DIR = os.path.join(ROOT, "assets")
HDR_PATH = os.path.join(ROOT, "src", "ui_assets.h")
TMP_DIR = os.path.join(ROOT, "tmp")

SW, SH = 320, 240
X0 = 150.0  # cot trai cua vung cat (toa do anh goc)
S = SH / 1024.0  # ty le thu nho (giu nguyen ti le)
FONT_PATH = "C:/Windows/Fonts/bahnschrift.ttf"

# khung phai (panel) trong anh goc: x0, y0, x1, y1 (tam duong vien) + ban kinh goc
PANEL = (843, 46, 1495, 970)
PANEL_R = 50
# popup Page 3 (toa do man hinh): x0, y0, x1, y1 + ban kinh goc
POP = (16, 10, 304, 230)
POP_R = 12


def fx(x):
    return (x - X0) * S


def fy(y):
    return y * S


def scr_rect(x0, y0, x1, y1, pad=0.0):
    """Hinh chu nhat anh goc -> hinh chu nhat man hinh (x, y, w, h) nguyen."""
    a = max(0, int(math.floor(fx(x0) - pad)))
    b = max(0, int(math.floor(fy(y0) - pad)))
    c = min(SW, int(math.ceil(fx(x1) + pad)))
    d = min(SH, int(math.ceil(fy(y1) + pad)))
    return (a, b, c - a, d - b)


# ----------------------------------------------------------------- co ban
def load(i):
    return np.asarray(Image.open(os.path.join(UI_DIR, f"Page_{i}.png")).convert("RGB")).astype(np.float32)


def to_pil(a):
    return Image.fromarray(np.clip(a + 0.5, 0, 255).astype(np.uint8))


def to_arr(im):
    return np.asarray(im.convert("RGB")).astype(np.float32)


def bayer(n):
    m = np.array([[0]])
    while m.shape[0] < n:
        m = np.block([[4 * m, 4 * m + 2], [4 * m + 3, 4 * m + 1]])
    return m


B8 = (bayer(8) + 0.5) / 64.0


def to_screen565(a):
    """anh goc float -> mang uint16 RGB565 240x320 (cat + thu nho + dither co dinh theo vi tri)."""
    im = to_pil(a).resize((SW, SH), Image.LANCZOS, box=(X0, 0, X0 + SW / S, 1024))
    s = np.asarray(im).astype(np.float32)
    t = np.tile(B8, (SH // 8 + 1, SW // 8 + 1))[:SH, :SW]
    r = np.clip(np.floor(s[:, :, 0] * 31 / 255 + t), 0, 31).astype(np.uint16)
    g = np.clip(np.floor(s[:, :, 1] * 63 / 255 + t), 0, 63).astype(np.uint16)
    b = np.clip(np.floor(s[:, :, 2] * 31 / 255 + t), 0, 31).astype(np.uint16)
    return (r << 11) | (g << 5) | b


def region_bytes(v, rect):
    x, y, w, h = rect
    return v[y:y + h, x:x + w].astype(">u2").tobytes()


def decode565(v):
    r = (v >> 11) & 31
    g = (v >> 5) & 63
    b = v & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], axis=2).astype(np.uint8)


def inpaint_h(a, x0, x1, y0, y1, w=8):
    """Xoa vung [x0,x1)x[y0,y1) bang noi suy ngang giua cot tham chieu 2 ben (giu gradient doc)."""
    left = a[y0:y1, x0 - w:x0].mean(axis=1)
    right = a[y0:y1, x1:x1 + w].mean(axis=1)
    t = ((np.arange(x0, x1) - x0 + 0.5) / (x1 - x0))[None, :, None]
    a[y0:y1, x0:x1] = left[:, None, :] * (1 - t) + right[:, None, :] * t


def rr_sdf(x0, y0, x1, y1, r, wx0, wy0, wx1, wy1):
    """Khoang cach co dau toi hinh chu nhat bo goc, tinh tren cua so [wx0,wx1)x[wy0,wy1)."""
    ys, xs = np.mgrid[wy0:wy1, wx0:wx1].astype(np.float32)
    xs += 0.5
    ys += 0.5
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    hw, hh = (x1 - x0) / 2, (y1 - y0) / 2
    qx = np.abs(xs - cx) - (hw - r)
    qy = np.abs(ys - cy) - (hh - r)
    return np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - r


# ----------------------------------------------------------------- font
def font(size, wght):
    f = ImageFont.truetype(FONT_PATH, size)
    f.set_variation_by_axes([wght, 100])
    return f


def font_for_cap(cap, wght, ref="H"):
    ratio = -font(100, wght).getbbox(ref, anchor="ls")[1] / 100.0
    return font(max(1, round(cap / ratio)), wght)


def text_width(f, s, track):
    return sum(f.getlength(c) for c in s) + track * (len(s) - 1)


def draw_text(a, x, base, s, f, fill, track=0.0):
    im = to_pil(a)
    d = ImageDraw.Draw(im)
    for c in s:
        d.text((x, base), c, font=f, fill=tuple(int(round(v)) for v in fill), anchor="ls")
        x += f.getlength(c) + track
    a[:] = to_arr(im)


# ----------------------------------------------------------------- nut kinh (LOG OUT / EXIT)
def glass_button(a, rect, radius, top, bot, border, glow, label, f, track, label_col, bw=3):
    x0, y0, x1, y1 = rect
    w, h = x1 - x0, y1 - y0
    pad = 40
    g = Image.new("L", (w + 2 * pad, h + 2 * pad), 0)
    ImageDraw.Draw(g).rounded_rectangle((pad, pad, pad + w, pad + h), radius, fill=255)
    ga = np.asarray(g.filter(ImageFilter.GaussianBlur(11))).astype(np.float32) / 255 * 0.6
    reg = a[y0 - pad:y1 + pad, x0 - pad:x1 + pad]
    reg[:] = reg * (1 - ga[..., None]) + np.array(glow, np.float32) * ga[..., None]

    m = Image.new("L", (w, h), 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, w - 1, h - 1), radius, fill=255)
    ma = np.asarray(m).astype(np.float32) / 255
    mi = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mi).rounded_rectangle((bw, bw, w - 1 - bw, h - 1 - bw), radius - bw, fill=255)
    ring = np.clip(ma - np.asarray(mi).astype(np.float32) / 255, 0, 1)
    t = np.linspace(0, 1, h, dtype=np.float32)[:, None, None]
    col = np.array(top, np.float32) * (1 - t) + np.array(bot, np.float32) * t
    body = a[y0:y1, x0:x1]
    body[:] = body * (1 - ma[..., None]) + col * ma[..., None]
    body[:] = body * (1 - ring[..., None]) + np.array(border, np.float32) * ring[..., None]

    tw = text_width(f, label, track)
    cap = -f.getbbox("H", anchor="ls")[1]
    draw_text(a, (x0 + x1) / 2 - tw / 2, (y0 + y1) / 2 + cap / 2, label, f, label_col, track)


# ----------------------------------------------------------------- vong dem nguoc
def draw_ring(a, cx, cy, r_in, r_out, frac):
    n = int(r_out) + 4
    x_lo, y_lo = int(cx) - n, int(cy) - n
    ys, xs = np.mgrid[y_lo:y_lo + 2 * n, x_lo:x_lo + 2 * n].astype(np.float32)
    dx, dy = xs + 0.5 - cx, ys + 0.5 - cy
    rho = np.hypot(dx, dy)
    theta = (np.degrees(np.arctan2(dx, -dy)) + 360.0) % 360.0
    band = np.clip(np.minimum(rho - r_in + 0.5, r_out - rho + 0.5), 0, 1)
    sweep = 340.0 * frac
    arc = np.clip((sweep - theta) * rho * math.pi / 180.0 + 0.5, 0, 1) if frac > 0 else np.zeros_like(rho)
    cyan = np.array([2, 204, 252], np.float32)
    white = np.array([185, 243, 253], np.float32)
    wgt = np.clip(1 - np.abs(theta - 262) / 75.0, 0, 1) ** 1.3
    acol = cyan[None, None, :] + wgt[..., None] * (white - cyan)[None, None, :]
    track = np.array([8, 52, 104], np.float32)
    reg = a[y_lo:y_lo + 2 * n, x_lo:x_lo + 2 * n]
    ta = (band * (1 - arc))[..., None]
    reg[:] = reg * (1 - ta) + track * ta
    aa = (band * arc)[..., None]
    reg[:] = reg * (1 - aa) + acol * aa


# ----------------------------------------------------------------- nen dong nhat giua cac trang
def panel_blend(bg, pg):
    """Giu khung phai (panel) cua pg; phan con lai (xe, chu, mach dien) lay y het tu bg.
    Ba anh mau co xe/nen ve hoi khac nhau -> dung chung 1 nen thi chuyen trang khong bi doi."""
    h, w = bg.shape[:2]
    d = rr_sdf(PANEL[0], PANEL[1], PANEL[2], PANEL[3], PANEL_R, 0, 0, w, h)
    k = np.clip((8.0 - d) / 22.0, 0, 1)[..., None]
    return pg * k + bg * (1 - k)


# ----------------------------------------------------------------- popup lon (Page 3)
def sx(x):
    """toa do man hinh -> toa do anh goc"""
    return X0 + x / S


def sy(y):
    return y / S


def fill_rrect(a, rect, radius, top, bot, border, bw):
    """Hinh chu nhat bo goc, nen gradient doc, vien bw px (toa do anh goc)."""
    x0, y0, x1, y1 = rect
    wx0, wy0, wx1, wy1 = int(x0) - 6, int(y0) - 6, int(x1) + 7, int(y1) + 7
    d = rr_sdf(x0, y0, x1, y1, radius, wx0, wy0, wx1, wy1)
    ma = np.clip(0.5 - d, 0, 1)[..., None]
    ys = np.arange(wy0, wy1, dtype=np.float32)
    t = np.clip((ys + 0.5 - y0) / (y1 - y0), 0, 1)[:, None, None]
    col = np.array(top, np.float32) * (1 - t) + np.array(bot, np.float32) * t
    ring = np.clip(0.5 - (np.abs(d + bw / 2) - bw / 2), 0, 1)[..., None]
    win = a[wy0:wy1, wx0:wx1]
    win[:] = win * (1 - ma) + col * ma
    win[:] = win * (1 - ring) + np.array(border, np.float32) * ring


def draw_popup(a, bg_ref):
    """Popup kinh toi mo (Dark Frosted Cyberpunk Glass) hoa quyen hoan hao voi man hinh chinh.
    - Nen toan man hinh duoc phu lop toi mo (backdrop dimming), xe va cac chi tiet van hien mo ao.
    - Hop popup la kinh ban trong suot tong mau xanh den / sapphire toi [10, 32, 64] -> [3, 14, 32],
      co vien neon cyan phat sang [0, 220, 255] va bong do mem mai [0, 2, 8].
    """
    x0, y0, x1, y1 = sx(POP[0]), sy(POP[1]), sx(POP[2]), sy(POP[3])
    r = POP_R / S
    h, w = a.shape[:2]
    d = rr_sdf(x0, y0, x1, y1, r, 0, 0, w, h)

    # 1. Lop phu lam toi toan bo nen phia sau de tao chieu sau (backdrop dimming)
    a[:] = a * 0.32 + np.array([2, 10, 22], np.float32) * 0.68

    # 2. Bong do mem ben ngoai vien popup (drop shadow)
    dsh = rr_sdf(x0, y0 + 12, x1, y1 + 12, r, 0, 0, w, h)
    sh = (np.clip(1 - dsh / 44.0, 0, 1) ** 2 * 0.70)[..., None]
    a[:] = a * (1 - sh) + np.array([0, 2, 8], np.float32) * sh

    # 3. Quang sang xanh neon ben ngoai vien (outer cyan glow)
    gl = np.where(d > 0, 0.45 * np.exp(-np.maximum(d, 0) / 14.0), 0)[..., None]
    a[:] = a * (1 - gl) + np.array([0, 180, 255], np.float32) * gl

    # 4. Than kinh mo toi mau (frosted dark glass):
    # Gradient tu xanh sapphire toi [10, 32, 64] o tren xuong xanh dem midnight [3, 14, 32] o duoi
    t = np.clip((np.arange(h, dtype=np.float32) - y0) / (y1 - y0), 0, 1)[:, None, None]
    tint = np.array([10, 32, 64], np.float32) * (1 - t) + np.array([3, 14, 32], np.float32) * t
    # Hieu ung phan xa anh sang nhe phia mep tren cua kinh
    inner = np.where(d < 0, 0.25 * np.exp(np.minimum(d, 0) / 22.0), 0)[..., None]
    tint = tint + (np.array([30, 140, 230], np.float32) - tint) * inner

    # Kinh ban trong suot: ket hop 12% anh goc bg_ref voi 88% mau kinh toi
    glass = bg_ref * 0.12 + tint * 0.88
    ma = np.clip(0.5 - d, 0, 1)[..., None]
    a[:] = a * (1 - ma) + glass * ma

    # 5. Duong vien neon cyan phat sang
    bw = 6.0
    ring = np.clip(0.5 - (np.abs(d + bw / 2) - bw / 2), 0, 1)[..., None]
    a[:] = a * (1 - ring) + np.array([0, 220, 255], np.float32) * ring


def mask_blend(a, mask_img, color, alpha=1.0, sigma=0.0):
    m = mask_img.filter(ImageFilter.GaussianBlur(sigma)) if sigma > 0 else mask_img
    g = np.asarray(m).astype(np.float32)[..., None] / 255.0 * alpha
    a[:] = a * (1 - g) + np.array(color, np.float32) * g


def draw_lock_icon(a, cx, cy, rad):
    """Vong tron sang + o khoa dang mo."""
    h, w = a.shape[:2]
    bb = (cx - rad, cy - rad, cx + rad, cy + rad)
    ring = Image.new("L", (w, h), 0)
    ImageDraw.Draw(ring).ellipse(bb, outline=255, width=10)
    disc = Image.new("L", (w, h), 0)
    ImageDraw.Draw(disc).ellipse(bb, fill=255)
    mask_blend(a, ring, (70, 200, 255), 0.9, 16)
    mask_blend(a, disc, (6, 36, 92), 0.55)
    mask_blend(a, ring, (150, 228, 255))

    u = 0.85 * rad
    gy = cy + 0.08 * u
    sw = int(0.13 * u)
    gm = Image.new("L", (w, h), 0)
    g = ImageDraw.Draw(gm)
    by0 = gy - 0.04 * u
    g.rounded_rectangle((cx - 0.38 * u, by0, cx + 0.38 * u, gy + 0.46 * u), radius=0.09 * u, fill=255)
    lx, rx, ay, rr_ = cx - 0.23 * u, cx + 0.23 * u, gy - 0.32 * u, 0.23 * u
    g.line((lx, by0 + 2, lx, ay), fill=255, width=sw)
    rb = rr_ + sw / 2
    g.arc((cx - rb, ay - rb, cx + rb, ay + rb), 180, 360, fill=255, width=sw)
    ry = gy - 0.20 * u
    g.line((rx, ay, rx, ry), fill=255, width=sw)
    for px, py in ((rx, ry), (lx, by0 + 2)):
        g.ellipse((px - sw / 2, py - sw / 2, px + sw / 2, py + sw / 2), fill=255)
    mask_blend(a, gm, (240, 250, 255))
    km = Image.new("L", (w, h), 0)
    k = ImageDraw.Draw(km)
    k.ellipse((cx - 0.065 * u, gy + 0.10 * u, cx + 0.065 * u, gy + 0.23 * u), fill=255)
    k.rectangle((cx - 0.025 * u, gy + 0.20 * u, cx + 0.025 * u, gy + 0.36 * u), fill=255)
    mask_blend(a, km, (14, 52, 112))


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    os.makedirs(TMP_DIR, exist_ok=True)
    p0, p1, p2, p3 = load(0), load(1), load(2), load(3)

    # ---------------- Page 1: bo 4 cham mau, de ve dong theo so ky tu da nhap
    inpaint_h(p1, 1050, 1290, 188, 244)

    # ---------------- font chu theo mau (FAN ON cao 36px, rong 180px)
    f_btn = font_for_cap(36, 700)
    track_btn = (180 - text_width(f_btn, "FAN ON", 0)) / 5

    # ---------------- Page 2: them nut LOG OUT
    f_small = font_for_cap(28, 700)
    glass_button(p2, (989, 826, 1349, 910), 24, (170, 38, 60), (100, 16, 36), (255, 118, 132), (255, 60, 80),
                 "LOG OUT", f_small, track_btn * 0.8, (245, 250, 255))
    # mau chu "FAN ON" khi mo di (lay tu Page 3 mau goc vi phan nen mo)
    reg = p3[350:390, 1186:1352].reshape(-1, 3)
    order = np.argsort(reg.sum(1))
    dim_text = reg[order[-max(8, len(order) // 12):]].mean(0)

    # ---------------- NEN DONG NHAT: ngoai khung phai (xe, chu, mach dien) lay y het Page 1
    p2 = panel_blend(p1, p2)

    # ---------------- Page 2: bien the FAN OFF
    p2off = p2.copy()
    inpaint_h(p2off, 1135, 1380, 410, 470)
    wx0, wy0, wx1, wy1 = 860, 310, 1480, 570
    d = rr_sdf(892, 341, 1444, 534, 45, wx0, wy0, wx1, wy1)
    mask = np.clip(1 - d / 16.0, 0, 1)[..., None]
    win = p2off[wy0:wy1, wx0:wx1]
    win[:] = win * (1 - mask) + win * 0.46 * mask
    draw_text(p2off, 1151, 457, "FAN OFF", f_btn, dim_text, track_btn)

    # ---------------- Page 3: popup lon "Push The Door To Open" tren nen kinh toi
    p3 = p2.copy()
    draw_popup(p3, p2)
    draw_lock_icon(p3, sx(160), sy(50), 27 / S)

    msg = "Push The Door To Open"
    cap = 16 / S
    while True:
        f_pop = font_for_cap(cap, 600)
        tw = text_width(f_pop, msg, 2.0)
        if tw <= 260 / S:
            break
        cap -= 2
    draw_text(p3, sx(160) - tw / 2, sy(108), msg, f_pop, (240, 250, 255), 2.0)

    # vien nang dem nguoc
    fill_rrect(p3, (sx(96), sy(120), sx(224), sy(164)), 10 / S, (4, 18, 42), (2, 10, 26), (0, 170, 240), 4)

    # nut EXIT lon trong popup
    exit_scr = (104, 178, 112, 36)
    f_exit = font_for_cap(11 / S, 700)
    glass_button(p3, (int(round(sx(104))), int(round(sy(178))), int(round(sx(216))), int(round(sy(214)))),
                 int(10 / S), (160, 32, 50), (90, 12, 26), (255, 90, 110), (255, 50, 70),
                 "EXIT", f_exit, 10, (245, 250, 255), bw=5)

    # ---------------- Page 3: 11 bien the dem nguoc (k = so giay con lai 0..10; 10 = nen Page 3)
    cd_rect = (112, 123, 96, 38)
    f_num = font_for_cap(16 / S, 400, "0")
    r_out, r_in, gap = 15 / S, 11 / S, 10 / S
    start = sx(160) - (2 * r_out + gap + text_width(f_num, "10s", 0)) / 2
    ring_cx, ring_cy = start + r_out, sy(142)
    txt_x, txt_base = start + 2 * r_out + gap, sy(142) + (16 / S) / 2
    cd_arrays = []
    for k in range(11):
        a = p3.copy()
        draw_ring(a, ring_cx, ring_cy, r_in, r_out, k / 10.0)
        draw_text(a, txt_x, txt_base, f"{k}s", f_num, (232, 244, 255), 0.0)
        cd_arrays.append(a)
    p3 = cd_arrays[10]
    ring_left = (ring_cx - r_out - X0) * S
    txt_right = (txt_x + text_width(f_num, "10s", 0) - X0) * S
    assert ring_left >= cd_rect[0] + 3 and txt_right <= cd_rect[0] + cd_rect[2] - 3, (ring_left, txt_right)

    # ---------------- doi sang man hinh + ghi file
    v0 = to_screen565(p0)
    v1, v2, v3 = to_screen565(p1), to_screen565(p2), to_screen565(p3)
    v2off = to_screen565(p2off)
    fan2 = scr_rect(892, 341, 1444, 534, 4.0)

    for name, v in (("page0", v0), ("page1", v1), ("page2", v2), ("page3", v3)):
        with open(os.path.join(OUT_DIR, name + ".bin"), "wb") as fh:
            fh.write(v.astype(">u2").tobytes())
    with open(os.path.join(OUT_DIR, "fan2_off.bin"), "wb") as fh:
        fh.write(region_bytes(v2off, fan2))
    cd_v = [to_screen565(a) for a in cd_arrays]
    with open(os.path.join(OUT_DIR, "cd.bin"), "wb") as fh:
        for v in cd_v:
            fh.write(region_bytes(v, cd_rect))

    # ---------------- sinh src/ui_images.c (mang C trong flash)
    names = [("img_page0", "page0"), ("img_page1", "page1"), ("img_page2", "page2"), ("img_page3", "page3"),
             ("img_fan2_off", "fan2_off"), ("img_cd", "cd")]
    with open(os.path.join(ROOT, "src", "ui_images.c"), "w", newline="\n") as c:
        c.write("/* Tu dong sinh boi tools/make_assets.py - khong sua tay */\n")
        c.write('#include <stdint.h>\n#include "ui_assets.h"\n\n')
        for sym, fn in names:
            with open(os.path.join(OUT_DIR, fn + ".bin"), "rb") as fh:
                data = fh.read()
            c.write("const uint8_t %s[%d] __attribute__((aligned(4))) = {\n" % (sym, len(data)))
            for i in range(0, len(data), 32):
                c.write(",".join(str(b) for b in data[i:i + 32]) + ",\n")
            c.write("};\n\n")

    # ---------------- vung cham (hit-box) + thong so cham PIN
    pad_key = 10
    key_cols = [(883, 1059), (1080, 1256), (1278, 1459)]
    key_rows = [(298, 400), (419, 522), (542, 646), (664, 771)]
    keys = []
    for r0, r1 in key_rows:
        for c0, c1 in key_cols:
            keys.append(scr_rect(c0 - pad_key, r0 - pad_key, c1 + pad_key, r1 + pad_key))
    login = scr_rect(882 - pad_key, 797 - pad_key, 1459 + pad_key, 914 + pad_key)
    hit_fan = scr_rect(892 - 8, 341 - 8, 1444 + 8, 534 + 8)
    hit_unlock = scr_rect(892 - 8, 573 - 8, 1444 + 8, 767 + 8)
    hit_logout = scr_rect(989 - 25, 826 - 25, 1349 + 25, 910 + 25)
    hit_exit = (exit_scr[0] - 8, exit_scr[1] - 8, exit_scr[2] + 16, exit_scr[3] + 12)
    dot_cx = fx((1087.5 + 1251.5) / 2)
    dot_cy = fy(215.5)
    band = (int(round(dot_cx)) - 56, int(math.floor(dot_cy)) - 8, 112, 16)

    def cr(r):
        return "{%d, %d, %d, %d}" % r

    with open(HDR_PATH, "w", newline="\n") as h:
        h.write("/* Tu dong sinh boi tools/make_assets.py - khong sua tay */\n#pragma once\n#include <stdint.h>\n\n")
        for sym, fn in names:
            h.write("extern const uint8_t %s[];\n" % sym)
        h.write("\n")
        h.write("#define UI_KEY_RECTS { \\\n    " + ", \\\n    ".join(cr(k) for k in keys) + " }\n")
        h.write("#define UI_LOGIN_RECT  %s\n" % cr(login))
        h.write("#define UI_FAN_RECT    %s\n" % cr(hit_fan))
        h.write("#define UI_UNLOCK_RECT %s\n" % cr(hit_unlock))
        h.write("#define UI_LOGOUT_RECT %s\n" % cr(hit_logout))
        h.write("#define UI_EXIT_RECT   %s\n\n" % cr(hit_exit))
        h.write("#define UI_DOT_CX    %.2ff\n#define UI_DOT_CY    %.2ff\n" % (dot_cx, dot_cy))
        h.write("#define UI_DOT_PITCH %.2ff\n#define UI_DOT_R     %.2ff\n" % (54.5 * S, 11.5 * S))
        h.write("#define UI_DOT_BAND  %s\n\n" % cr(band))
        h.write("#define UI_FAN2_RECT %s /* sprite FAN OFF Page 2 */\n" % cr(fan2))
        h.write("#define UI_CD_RECT   %s /* 11 sprite dem nguoc 0..10 */\n" % cr(cd_rect))

    # ---------------- anh xem thu (kem hit-box) de kiem tra bang mat
    def preview(v, name, rects):
        im = Image.fromarray(decode565(v)).resize((SW * 3, SH * 3), Image.NEAREST)
        dr = ImageDraw.Draw(im)
        for (x, y, w, h) in rects:
            dr.rectangle((x * 3, y * 3, (x + w) * 3, (y + h) * 3), outline=(255, 255, 0))
        im.save(os.path.join(TMP_DIR, name))

    def to_rect(r):
        return r
    preview(v0, "prev0.png", [])
    preview(v1, "prev1.png", keys + [login, band])
    preview(v2, "prev2.png", [hit_fan, hit_unlock, hit_logout, fan2])
    preview(v3, "prev3.png", [hit_exit, cd_rect])
    preview(v2off, "prev2off.png", [fan2])
    preview(cd_v[7], "prev3_k7.png", [cd_rect])

    # kiem tra: vung nen ben trai khung phai phai giong het nhau giua Page 1 va Page 2 (so pixel khac = 0)
    xl = int(fx(PANEL[0] - 30))
    print("nen Page1 vs Page2 (x<%d) pixel khac:" % xl, int((v1[:, :xl] != v2[:, :xl]).sum()))

    total = sum(os.path.getsize(os.path.join(OUT_DIR, f)) for f in os.listdir(OUT_DIR))
    print("assets:", total, "bytes", sorted(os.listdir(OUT_DIR)))
    print("keys:", keys)
    print("login", login, "fan", hit_fan, "unlock", hit_unlock, "logout", hit_logout, "exit", hit_exit)
    print("dot", dot_cx, dot_cy, "band", band, "fan2", fan2, "cd", cd_rect, "ring/text", ring_left, txt_right)


if __name__ == "__main__":
    main()

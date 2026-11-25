#!/usr/bin/env python3
"""Title artwork for the PS5 home screen from any images: sce_sys/icon0.png (512x512 PNG) and pic0.dds / pic1.dds
(3840x2160 BC7, the selection and launch backgrounds).

    python3 tools/ps5/make_title_art.py --icon ICON_SOURCE --background BACKGROUND [--launch LAUNCH_BACKGROUND]
                                        [--icon-box X0 Y0 X1 Y1] [--out build/ps5-art]

The images are cropped to the target's aspect ratio (centred, or to --icon-box for the icon) and scaled.
tools/ps5/build_title.sh copies build/ps5-art into the title when it exists. Screenshots of the game are Valve's
artwork: keep the results out of the repository.

BC7 is written in mode 6 only (one colour line per 4x4 block, 16 levels, opaque), encoded with numpy; the header is
the profile the console's shell accepts (single level, DX10, BC7_UNORM, straight alpha), as deps/ps5-vulkan's
tools/prepare-assets.sh writes it. The encoder decodes its own output and reports the error.
"""
import argparse
import struct
from pathlib import Path

import numpy as np
from PIL import Image

WEIGHTS = np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64], dtype=np.int32)


def fit(image, width, height, box=None):
    """Crops to the width:height ratio (inside `box` when given), then scales with a Lanczos filter."""
    image = image.convert('RGB')
    if box:
        image = image.crop(box)
    ratio = width / height
    w, h = image.size
    if w / h > ratio:
        cut = int(round(h * ratio))
        left = (w - cut) // 2
        image = image.crop((left, 0, left + cut, h))
    else:
        cut = int(round(w / ratio))
        top = (h - cut) // 2
        image = image.crop((0, top, w, top + cut))
    return image.resize((width, height), Image.LANCZOS)


def bc7_mode6(rgb):
    """BC7 blocks (mode 6, opaque) for an RGB image whose sides are multiples of 4: bytes, row-major blocks."""
    h, w, _ = rgb.shape
    blocks = rgb.reshape(h // 4, 4, w // 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 3).astype(np.int32)
    low, high = blocks.min(axis=1), blocks.max(axis=1)
    # Endpoints are 7 bits plus a shared low bit (the p-bit), here 1 for both: values 2q+1.
    q0 = np.clip((low - 1) // 2, 0, 127)
    q1 = np.clip(high // 2, 0, 127)
    v0, v1 = 2 * q0 + 1, 2 * q1 + 1
    span = (v1 - v0).astype(np.float64)
    length = (span * span).sum(axis=1)
    t = ((blocks - v0[:, None, :]) * span[:, None, :]).sum(axis=2) / np.where(length > 0, length, 1)[:, None]
    t = np.clip(t, 0.0, 1.0) * 64
    index = np.abs(t[:, :, None] - WEIGHTS[None, None, :]).argmin(axis=2).astype(np.uint64)
    # The first pixel's index has an implicit zero top bit: swap the endpoints where it is set.
    swap = index[:, 0] >= 8
    index[swap] = 15 - index[swap]
    q0[swap], q1[swap] = q1[swap].copy(), q0[swap].copy()
    q0, q1 = q0.astype(np.uint64), q1.astype(np.uint64)
    alpha = np.uint64(127)
    low_word = (np.uint64(1) << np.uint64(6))
    for shift, value in ((7, q0[:, 0]), (14, q1[:, 0]), (21, q0[:, 1]), (28, q1[:, 1]), (35, q0[:, 2]), (42, q1[:, 2])):
        low_word = low_word | (value << np.uint64(shift))
    low_word = low_word | (alpha << np.uint64(49)) | (alpha << np.uint64(56)) | (np.uint64(1) << np.uint64(63))
    high_word = np.uint64(1) | (index[:, 0] << np.uint64(1))
    for pixel in range(1, 16):
        high_word = high_word | (index[:, pixel] << np.uint64(4 + (pixel - 1) * 4))
    out = np.empty((blocks.shape[0], 2), dtype='<u8')
    out[:, 0], out[:, 1] = low_word, high_word
    return out.tobytes()


def bc7_mode6_decode(data, width, height):
    """Decodes what bc7_mode6 writes (mode 6 only), for checking the encoder."""
    words = np.frombuffer(data, dtype='<u8').reshape(-1, 2)
    low, high = words[:, 0], words[:, 1]
    field = lambda word, shift, bits: ((word >> np.uint64(shift)) & np.uint64((1 << bits) - 1)).astype(np.int32)
    e0 = np.stack([field(low, 7, 7), field(low, 21, 7), field(low, 35, 7)], axis=1) * 2 + field(low, 63, 1)[:, None]
    e1 = np.stack([field(low, 14, 7), field(low, 28, 7), field(low, 42, 7)], axis=1) * 2 + field(high, 0, 1)[:, None]
    index = np.stack([field(high, 1, 3)] + [field(high, 4 + (p - 1) * 4, 4) for p in range(1, 16)], axis=1)
    weight = WEIGHTS[index][:, :, None]
    pixels = ((64 - weight) * e0[:, None, :] + weight * e1[:, None, :] + 32) >> 6
    return pixels.reshape(height // 4, width // 4, 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(height, width, 3).astype(np.uint8)


def write_dds(path, rgb):
    height, width, _ = rgb.shape
    payload = bc7_mode6(rgb)
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000   # caps, height, width, pixel format, mipmap count, linear size
    header = struct.pack('<4s7I44x', b'DDS ', 124, flags, height, width, len(payload), 0, 1)
    pixel_format = struct.pack('<2I4s5I', 32, 0x4, b'DX10', 0, 0, 0, 0, 0)   # DDPF_FOURCC
    caps = struct.pack('<5I', 0x1000, 0, 0, 0, 0)   # DDSCAPS_TEXTURE
    dx10 = struct.pack('<5I', 98, 3, 0, 1, 1)       # BC7_UNORM, 2D, no flags, one surface, straight alpha
    data = header + pixel_format + caps + dx10 + payload
    assert len(data) == 148 + (width // 4) * (height // 4) * 16
    Path(path).write_bytes(data)
    decoded = bc7_mode6_decode(payload, width, height)
    error = np.mean((decoded.astype(np.float64) - rgb) ** 2)
    psnr = 10 * np.log10(255 ** 2 / error) if error else float('inf')
    print(f'{path}: {width}x{height} BC7, {len(data)} bytes, PSNR {psnr:.1f} dB')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--icon', type=Path, required=True)
    parser.add_argument('--background', type=Path, required=True)
    parser.add_argument('--launch', type=Path, help='launch background (pic1); the background by default')
    parser.add_argument('--icon-box', type=int, nargs=4, metavar=('X0', 'Y0', 'X1', 'Y1'))
    parser.add_argument('--out', type=Path, default=Path(__file__).resolve().parents[2] / 'build/ps5-art')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    icon = fit(Image.open(args.icon), 512, 512, tuple(args.icon_box) if args.icon_box else None)
    icon.save(args.out / 'icon0.png', optimize=True)
    print(f'{args.out / "icon0.png"}: 512x512 PNG')
    write_dds(args.out / 'pic0.dds', np.asarray(fit(Image.open(args.background), 3840, 2160)))
    write_dds(args.out / 'pic1.dds', np.asarray(fit(Image.open(args.launch or args.background), 3840, 2160)))


if __name__ == '__main__':
    main()

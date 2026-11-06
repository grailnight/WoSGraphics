"""Turns gpu_test.exe's <scene>_{new,old,gt}.raw into PNGs: each scene as a
row of old | new | ground truth, plus an amplified |new - truth| strip."""
from pathlib import Path
import numpy as np
from PIL import Image

W, H = 1920, 1080
HERE = Path(__file__).resolve().parent


def load(name):
    return np.fromfile(HERE / name, dtype=np.float32).reshape(H, W, 3)


def display(rgb):
    mapped = rgb / (1.0 + rgb)
    return (np.clip(mapped, 0, 1) ** (1 / 2.2) * 255 + 0.5).astype(np.uint8)


for scene in ('static_half', 'pan_half', 'static_quarter', 'inside_half'):
    if not (HERE / f'{scene}_new.raw').exists():
        continue
    old, new, gt = load(f'{scene}_old.raw'), load(f'{scene}_new.raw'), load(f'{scene}_gt.raw')
    diff = np.abs(new - gt) * 8.0
    row = np.concatenate([display(old), display(new), display(gt), display(diff)], axis=1)
    Image.fromarray(row).resize((row.shape[1] // 3, row.shape[0] // 3), Image.LANCZOS).save(HERE / f'{scene}.png')
    # Full-resolution crop of the left tower edge and the sky beside it.
    crop = (slice(int(H * 0.30), int(H * 0.62)), slice(int(W * 0.20), int(W * 0.40)))
    detail = np.concatenate([display(new)[crop], display(gt)[crop], display(diff)[crop]], axis=1)
    Image.fromarray(detail).save(HERE / f'{scene}_detail.png')
    print(scene, 'written')

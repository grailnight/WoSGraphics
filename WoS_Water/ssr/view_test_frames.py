"""Tone-map the GPU test's before/after frames side by side into test_frames.png."""
from pathlib import Path
import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
W, H = 800, 450
data = np.fromfile(HERE / 'test_frames.raw', dtype=np.float32).reshape(2, H, W, 4)[..., :3]
mapped = np.clip(data / (1.0 + data), 0, 1) ** (1 / 2.2)
image = np.concatenate([mapped[0], np.ones((H, 8, 3)), mapped[1]], axis=1)
Image.fromarray((image * 255).astype(np.uint8)).save(HERE / 'test_frames.png')
print(HERE / 'test_frames.png')

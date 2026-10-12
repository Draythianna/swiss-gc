# Crop a disc image to the circle its opaque pixels fit and shrink it to 256x256,
# the size the jewel case view draws discs at: python3 fitdisc.py in.png GAMEID.png
import sys
import numpy as np
from PIL import Image
src, dst = sys.argv[1], sys.argv[2]
im = Image.open(src).convert('RGBA')
a = np.asarray(im)[..., 3] > 128
edge = []
H, W = a.shape
for y in range(0, H, 3):
    xx = np.nonzero(a[y])[0]
    if len(xx): edge += [(xx.min(), y), (xx.max() + 1, y)]
for x in range(0, W, 3):
    yy = np.nonzero(a[:, x])[0]
    if len(yy): edge += [(x, yy.min()), (x, yy.max() + 1)]
E = np.array(edge, float)
for _ in range(3):
    A = np.c_[2 * E[:, 0], 2 * E[:, 1], np.ones(len(E))]
    sol = np.linalg.lstsq(A, (E ** 2).sum(1), rcond=None)[0]
    cx, cy = sol[0], sol[1]
    r = np.sqrt(sol[2] + cx * cx + cy * cy)
    # drop the points that don't sit on the circle (clipped or shadowed edges) and fit again
    d = np.abs(np.hypot(E[:, 0] - cx, E[:, 1] - cy) - r)
    E = E[d < max(2.0, np.percentile(d, 80))]
print('%s: centre %.1f %.1f radius %.1f (image %dx%d)' % (src.split('/')[-1], cx, cy, r, W, H))
box = (cx - r, cy - r, cx + r, cy + r)
out = im.crop(tuple(int(round(v)) for v in box)).resize((256, 256), Image.LANCZOS)
out.save(dst, optimize=True)

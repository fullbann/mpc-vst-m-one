"""Cut the keybed out of the owner's keyboard photo (art/src/keys-photo.jpg -> art/src/keys.png): black and white, only the
keys -- no panel, lights, logo or room. The polygon follows the keybed's edges (photo pixels, 2212x1661); edges feathered."""
import os
from PIL import Image, ImageDraw, ImageFilter
D = os.path.join(os.path.dirname(os.path.abspath(__file__)), "src")
im = Image.open(os.path.join(D, "keys-photo.jpg")).convert("L")
POLY = [(40, 900), (2212, 428), (2212, 566), (560, 1661), (40, 1661)]   # back of the keys (below the lit strip) .. front edge
m = Image.new("L", im.size, 0)
ImageDraw.Draw(m).polygon(POLY, fill=255)
m = m.filter(ImageFilter.GaussianBlur(22))
im = Image.eval(im, lambda v: int(min(255, v * 1.05)))
out = Image.merge("LA", (im, m))
out.save(os.path.join(D, "keys.png"))
print(out.size)

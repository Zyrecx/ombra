# Renders the setup wizard's side-panel art (installer/wizard-*.png): the brand mark from
# ombra-icongen, enlarged and bleeding off the bottom-left, under the Inter wordmark, on the overlay's
# near-black. Rerun after changing the mark; build first (build.ps1).
#   py scripts/wizard-art.py
import os
import subprocess
import tempfile

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICONGEN = os.path.join(ROOT, "build", "Release", "ombra-icongen.exe")
FONT = os.path.join(ROOT, "third_party", "fonts", "inter", "Inter-SemiBold.ttf")
W, H = 328, 628  # 2x Inno's modern wizard image (164x314)

with tempfile.TemporaryDirectory() as tmp:
    mark_path = os.path.join(tmp, "mark.png")
    subprocess.run([ICONGEN, "--png", "620", mark_path], check=True)
    mark = Image.open(mark_path).convert("RGBA")

art = Image.new("RGBA", (W, H))
draw = ImageDraw.Draw(art)
top, bottom = (14, 14, 17), (5, 5, 6)
for y in range(H):
    t = y / (H - 1)
    draw.line([(0, y), (W, y)], fill=tuple(round(top[i] + (bottom[i] - top[i]) * t) for i in range(3)))
art.alpha_composite(mark, (-250, 250))
font = ImageFont.truetype(FONT, 40)
draw.text(((W - draw.textlength("Ombra", font=font)) / 2, 70), "Ombra", font=font, fill=(230, 232, 238))

art = art.convert("RGB")
art.save(os.path.join(ROOT, "installer", "wizard-328.png"), optimize=True)
art.resize((W // 2, H // 2), Image.LANCZOS).save(os.path.join(ROOT, "installer", "wizard-164.png"), optimize=True)

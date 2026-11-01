"""Renders DejaVu Sans Mono into the rts menu's font: ASCII 0x20-0x7E, 8x16 cells,
2 bits per pixel (0 = background .. 3 = full ink), row-major, low bits = leftmost.
Usage: python mkfont.py DejaVuSansMono.ttf font.bin [preview.txt]"""
import sys
from PIL import Image, ImageDraw, ImageFont

W, H = 8, 16
font = ImageFont.truetype(sys.argv[1], 13)
out = bytearray(); preview = []
for c in range(0x20, 0x7F):
    img = Image.new('L', (W, H), 0)
    ImageDraw.Draw(img).text((0, 0), chr(c), font=font, fill=255)
    px = img.load()
    rows = []
    for y in range(H):
        word = 0; line = ''
        for x in range(W):
            v = min(3, (px[x, y] + 32) // 64)
            word |= v << (2 * x); line += ' .+#'[v]
        out += word.to_bytes(2, 'little'); rows.append(line)
    preview.append(f'{chr(c)!r}\n' + '\n'.join(rows))
open(sys.argv[2], 'wb').write(out)
if len(sys.argv) > 3: open(sys.argv[3], 'w').write('\n'.join(preview))
print(len(out), 'bytes')

"""draws the blox7 icon (roblox tilt, win7 flag colors, a bit of gloss) into a multi-size .ico
usage: blox7-icon.py <out.ico> [<preview.png>]"""
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter

SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
LIGHT = ((247, 134, 92), (180, 223, 94), (90, 191, 243), (255, 219, 104))
DEEP = ((216, 57, 26), (91, 165, 38), (14, 108, 195), (238, 157, 14))


def vertical(size, top, bottom, y0, y1):
    strip = Image.new('RGBA', (1, size))
    for y in range(size):
        t = min(1.0, max(0.0, (y - y0) / max(1.0, y1 - y0)))
        strip.putpixel((0, y), tuple(int(round(a + (b - a) * t)) for a, b in zip(top, bottom)) + (255,))
    return strip.resize((size, size))


def tile(target):
    scale = 8
    big = target * scale
    side = big * 0.74
    half = side / 2
    gap = max(side * 0.034, 1.15 * scale)
    counter = side * 0.14
    corner = side * 0.07
    c = big / 2

    outer = Image.new('L', (big, big), 0)
    ImageDraw.Draw(outer).rounded_rectangle((c - half, c - half, c + half, c + half), radius=corner, fill=255)
    hole = Image.new('L', (big, big), 255)
    ImageDraw.Draw(hole).rectangle((c - counter, c - counter, c + counter, c + counter), fill=0)

    panes = ((c - half, c - half, c - gap / 2, c - gap / 2), (c + gap / 2, c - half, c + half, c - gap / 2),
             (c - half, c + gap / 2, c - gap / 2, c + half), (c + gap / 2, c + gap / 2, c + half, c + half))
    art = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    union = Image.new('L', (big, big), 0)
    edge_width = max(1, int(round(scale * 0.9)))
    for index, box in enumerate(panes):
        mask = Image.new('L', (big, big), 0)
        ImageDraw.Draw(mask).rectangle(box, fill=255)
        mask = ImageChops.multiply(ImageChops.multiply(mask, outer), hole)
        art.paste(vertical(big, LIGHT[index], DEEP[index], box[1], box[3]), (0, 0), mask)
        inner = mask.filter(ImageFilter.MinFilter(edge_width * 2 + 1))
        rim = ImageChops.subtract(mask, inner).point(lambda v: v * 0.6)
        art.paste(Image.new('RGBA', (big, big), DEEP[index] + (255,)), (0, 0), rim)
        union = ImageChops.lighter(union, mask)

    gloss = Image.new('L', (big, big), 0)
    for y in range(int(c - half), int(c)):
        t = (y - (c - half)) / half
        ImageDraw.Draw(gloss).line(((0, y), (big, y)), fill=int(255 * (0.36 - 0.28 * t)))
    gloss = ImageChops.multiply(gloss, union)
    art = Image.alpha_composite(art, Image.merge('RGBA', (Image.new('L', (big, big), 255),) * 3 + (gloss,)))

    art = art.rotate(-15, resample=Image.BICUBIC, center=(c, c))
    alpha = art.getchannel('A')
    shadow = Image.new('RGBA', (big, big), (10, 30, 60, 0))
    shadow.putalpha(alpha.point(lambda v: int(v * 0.28)).filter(ImageFilter.GaussianBlur(big * 0.018)))
    canvas = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    canvas.alpha_composite(shadow, (0, int(round(big * 0.03))))
    canvas.alpha_composite(art)
    return canvas.resize((target, target), Image.LANCZOS)


def main():
    out = sys.argv[1]
    images = [tile(size) for size in SIZES]
    images[-1].save(out, format='ICO', sizes=[(s, s) for s in SIZES], append_images=images[:-1])
    if len(sys.argv) > 2:
        images[-1].save(sys.argv[2])
    print('wrote', out)


if __name__ == '__main__':
    main()

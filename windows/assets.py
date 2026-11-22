#!/usr/bin/env python3
# Renders the Windows icon (evict.ico) and the installer's side panel (installer.bmp) from the logo's geometry, so each
# icon size is drawn on its own pixel grid instead of shrunk from a big image: crisp edges down to 16 px.
# Usage: python3 windows/assets.py   (needs Pillow)
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).parent
# The logo on a 20×20 grid, as in the page: (x0, y0, x1, y1, color)
BLOCKS = [(0, 0, 11, 20, '#e0473d'), (12, 0, 20, 12, '#e0921f'), (12, 13, 20, 20, '#3d7fe6')]
SS = 8  # supersampling for the rounded corners only; edges fall on whole pixels


def logo(size, margin):
    scale = (size - 2 * margin) / 20
    px = lambda v: margin + round(v * scale)
    radius = max(1, round(2.5 * scale))
    big = Image.new('RGBA', (size * SS, size * SS), (0, 0, 0, 0))
    draw = ImageDraw.Draw(big)
    for x0, y0, x1, y1, color in BLOCKS:
        draw.rounded_rectangle([px(x0) * SS, px(y0) * SS, px(x1) * SS - 1, px(y1) * SS - 1], radius * SS, fill=color)
    return big.resize((size, size), Image.LANCZOS)


sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
icons = [logo(s, max(1, round(s * 0.06))) for s in sizes]
icons[-1].save(HERE / 'evict.ico', sizes=[(s, s) for s in sizes], append_images=icons[:-1])

# The welcome and finish pages' side panel: 164×314, the page's background, the logo and the name
panel = Image.new('RGB', (164, 314), '#0c0e12')
mark = logo(64, 0)
panel.paste(mark, (50, 96), mark)
font = next((ImageFont.truetype(f, 26) for f in ['/System/Library/Fonts/SFNS.ttf', '/System/Library/Fonts/Helvetica.ttc',
                                                  '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'] if Path(f).exists()), None)
small = font.font_variant(size=11) if font else None
try:
    font.set_variation_by_name('Bold')  # SF is a variable font
except (AttributeError, OSError, ValueError):
    pass
draw = ImageDraw.Draw(panel)
draw.text((82, 196), 'Evict', fill='#e8eaf0', font=font, anchor='mm')
draw.text((82, 226), 'The bouncer for your disk', fill='#8b93a1', font=small, anchor='mm')
panel.save(HERE / 'installer.bmp')

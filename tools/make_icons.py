#!/usr/bin/env python3
"""Renders assets/logo/mark.svg into AppIcon.iconset PNGs and Sreon.icns (needs playwright+Pillow, run on any OS)."""
import os, struct, sys
from playwright.sync_api import sync_playwright
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
svg = open(os.path.join(root, 'assets/logo/mark.svg')).read()
out = os.path.join(root, 'assets/logo'); os.makedirs(out + '/AppIcon.iconset', exist_ok=True)
sizes = {16: 'icon_16x16', 32: 'icon_16x16@2x', 33: 'icon_32x32', 64: 'icon_32x32@2x', 128: 'icon_128x128', 256: 'icon_128x128@2x', 257: 'icon_256x256', 512: 'icon_256x256@2x', 513: 'icon_512x512', 1024: 'icon_512x512@2x'}
pngs = {}
with sync_playwright() as p:
    b = p.chromium.launch(); pg = b.new_page(viewport={'width': 1024, 'height': 1024})
    for px in (16, 32, 64, 128, 256, 512, 1024):
        pg.set_viewport_size({'width': px, 'height': px})
        pg.set_content('<body style="margin:0;background:transparent">' + svg.replace('width="1024" height="1024"', f'width="{px}" height="{px}"') + '</body>')
        path = f'{out}/_{px}.png'; pg.screenshot(path=path, omit_background=True); pngs[px] = open(path, 'rb').read(); os.remove(path)
    b.close()
names = {16: ['icon_16x16'], 32: ['icon_16x16@2x', 'icon_32x32'], 64: ['icon_32x32@2x'], 128: ['icon_128x128'], 256: ['icon_128x128@2x', 'icon_256x256'], 512: ['icon_256x256@2x', 'icon_512x512'], 1024: ['icon_512x512@2x']}
for px, ns in names.items():
    for n in ns: open(f'{out}/AppIcon.iconset/{n}.png', 'wb').write(pngs[px])
# .icns written directly so it works without macOS tools
types = [(b'icp4', 16), (b'icp5', 32), (b'icp6', 64), (b'ic07', 128), (b'ic08', 256), (b'ic09', 512), (b'ic10', 1024)]
body = b''
for t, px in types: body += t + struct.pack('>I', len(pngs[px]) + 8) + pngs[px]
open(f'{out}/Sreon.icns', 'wb').write(b'icns' + struct.pack('>I', len(body) + 8) + body)
open(f'{out}/Sreon-1024.png', 'wb').write(pngs[1024])
print('icons written to', out)

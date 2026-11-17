"""Zipshot icon set: 24px-grid SVGs, rendered to 64px PNGs with headless Chrome.

Style: rounded 1.8px light strokes for the dark ShareX theme, one colour accent per icon.
Run:  python icons.py   (writes ../res/*.png and preview.png)
"""
import os
import subprocess
import sys
import tempfile

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, '..', 'res')
CHROME = r'C:\Program Files\Google\Chrome\Application\chrome.exe'

L = '#E8EAED'      # line
D = '#1E1F22'      # dark detail on filled shapes
BLUE, GREEN, RED, YELLOW = '#4DA3FF', '#36C77B', '#FF5A5F', '#FFC531'
PURPLE, ORANGE, TEAL, PINK = '#A78BFA', '#FF9F43', '#2DD4BF', '#FF7AB6'

S = f'fill="none" stroke="{L}" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"'


def st(color=L, w=1.8, extra=''):
    return f'fill="none" stroke="{color}" stroke-width="{w}" stroke-linecap="round" stroke-linejoin="round" {extra}'


DASH = 'stroke-dasharray="2.6 2.4"'

ICONS = {
    # ---------------- editor / region toolbar ----------------
    'tick': f'<circle cx="12" cy="12" r="9.5" fill="{GREEN}"/><path d="M7.5 12.3l3 3 6-6.3" {st("#fff", 2.2)}/>',
    'disk-black': f'<path d="M5 4h11l3.5 3.5V19a1 1 0 0 1-1 1h-13a1 1 0 0 1-1-1V5a1 1 0 0 1 1-1z" {S}/>'
                  f'<rect x="8" y="4.2" width="7" height="4.6" rx=".8" fill="{BLUE}"/><rect x="7.5" y="13" width="9" height="7" rx="1" {st()}/>',
    'disks-black': f'<path d="M4 4h10l3 3v5" {S}/><path d="M4 4v14a1 1 0 0 0 1 1h6" {S}/>'
                   f'<rect x="7" y="4.2" width="6" height="4" rx=".8" fill="{BLUE}"/>'
                   f'<path d="M14.5 20.5l.6-3 5-5a1.4 1.4 0 0 1 2 2l-5 5z" fill="{YELLOW}" stroke="{YELLOW}" stroke-width="1" stroke-linejoin="round"/>',
    'clipboard': f'<rect x="8.5" y="8.5" width="11" height="12" rx="2" fill="{BLUE}" fill-opacity=".25" {st(BLUE)}/>'
                 f'<path d="M15.5 8.5V5.5a2 2 0 0 0-2-2h-7a2 2 0 0 0-2 2v9a2 2 0 0 0 2 2h2" {S}/>',
    'cross': f'<path d="M6.5 6.5l11 11M17.5 6.5l-11 11" {st(RED, 2.4)}/>',
    'cursor': f'<path d="M6 3.5l12 7.6-5.3 1.3 3.2 5.8-2.4 1.3-3.2-5.8L6.5 17.5z" fill="{L}" stroke="{L}" stroke-width="1.2" stroke-linejoin="round"/>',
    'layer-shape': f'<rect x="3.5" y="5.5" width="17" height="13" rx="2.5" {S}/>',
    'layer-shape-ellipse': f'<ellipse cx="12" cy="12" rx="8.8" ry="6.8" {S}/>',
    'pencil': f'<path d="M4 20l1-4.2L15.6 5.2a2 2 0 0 1 2.9 0l.3.3a2 2 0 0 1 0 2.9L8.2 19z" {S}/>'
              f'<path d="M14 6.8l3.2 3.2" {st()}/><path d="M4 20l1-4.2 3.2 3.2z" fill="{YELLOW}" stroke="{YELLOW}" stroke-width="1.2" stroke-linejoin="round"/>',
    'pencil--arrow': f'<path d="M3.5 17c2.5-6 5-6 6.5-2s4 3.5 7-4" {S}/><path d="M13.2 9.2l4-.4.6 4" {st(BLUE, 2)}/>',
    'layer-shape-line-white': f'<path d="M5.5 18.5l13-13" {S}/><circle cx="5.5" cy="18.5" r="1.9" fill="{BLUE}"/><circle cx="18.5" cy="5.5" r="1.9" fill="{BLUE}"/>',
    'layer-shape-arrow-white': f'<path d="M5 19L18 6" {st(L, 2)}/><path d="M10.5 5.5H18.5V13.5" {st(L, 2)}/>',
    'edit-outline-white': f'<text x="12" y="19.2" text-anchor="middle" font-family="Segoe UI" font-weight="800" font-size="19" fill="none" stroke="{L}" stroke-width="1.5" stroke-linejoin="round">A</text>',
    'edit-shade-white': f'<rect x="3" y="4" width="18" height="16" rx="3.5" fill="{RED}"/>'
                        f'<text x="12" y="17.3" text-anchor="middle" font-family="Segoe UI" font-weight="700" font-size="14" fill="#fff">T</text>',
    'counter-reset': f'<circle cx="12" cy="12" r="9" fill="{RED}"/>'
                     f'<text x="12" y="16.6" text-anchor="middle" font-family="Segoe UI" font-weight="700" font-size="13" fill="#fff">1</text>',
    'magnifier-zoom': f'<circle cx="10.5" cy="10.5" r="6.5" {S}/><path d="M15.4 15.4l5 5" {st(L, 2.4)}/><path d="M10.5 7.8v5.4M7.8 10.5h5.4" {st(BLUE, 2)}/>',
    'eraser': f'<path d="M8.5 19.5L3.8 14.8a1.6 1.6 0 0 1 0-2.3L12.6 3.7a1.6 1.6 0 0 1 2.3 0l5.4 5.4a1.6 1.6 0 0 1 0 2.3L12.2 19.5z" {S}/>'
              f'<path d="M3.8 14.8a1.6 1.6 0 0 1 0-2.3l4.4-4.4 7.7 7.7-4 3.7H8.5z" fill="{PINK}"/><path d="M12.5 20h8" {S}/>',
    'layer-shade-white': f'<path d="M12 3.2c3.4 4.2 6 7.5 6 10.6a6 6 0 0 1-12 0c0-3.1 2.6-6.4 6-10.6z" fill="{BLUE}" fill-opacity=".35" {st(BLUE)}/>'
                         f'<path d="M9.2 14.2a3 3 0 0 0 2.6 2.7" {st("#fff", 1.6)}/>',
    'grid-white': ''.join(
        f'<rect x="{4 + c * 5.6:.1f}" y="{4 + r * 5.6:.1f}" width="4.6" height="4.6" rx="1" fill="{L}" fill-opacity="{o}"/>'
        for (r, c), o in zip([(r, c) for r in range(3) for c in range(3)], [.95, .45, .8, .3, .9, .5, .7, .35, .95])),
    'highlighter-text': f'<path d="M14.5 4.5l5 5-7.8 7.8H6.7v-5z" {S}/><path d="M6.7 17.3l-2.2 2.2h4.4" {S}/>'
                        f'<rect x="3" y="20.2" width="18" height="2.3" rx="1.1" fill="{YELLOW}"/><path d="M11.5 7.5l5 5" {st(YELLOW, 2)}/>',
    'image-crop': f'<path d="M6.5 2.5v13a2 2 0 0 0 2 2h13" {S}/><path d="M2.5 6.5h13a2 2 0 0 1 2 2v13" {st(BLUE)}/>',
    'layer--pencil': f'<path d="M4 7h9M17 7h3M4 17h3M11 17h9" {S}/><circle cx="15" cy="7" r="2.2" fill="{BLUE}"/><circle cx="9" cy="17" r="2.2" fill="{BLUE}"/>',
    'wrench-screwdriver': f'<path d="M4.5 4.5l9 9" {st(L, 1.8)}/><path d="M3.6 3.6l1.8 1.8" {st(L, 2.6)}/>'
                          f'<path d="M14.2 14.2l5.6 5.6" {st(BLUE, 3.6)}/>'
                          f'<path d="M5.2 18.8l8.3-8.3" {st(L, 2.2)}/>'
                          f'<path d="M16.4 4.1a3.4 3.4 0 1 0 3.5 3.5l-2.2.6-1.9-1.9z" fill="{L}"/>',
    'arrow-circle-225-left': f'<path d="M9 14L4 9l5-5" {S}/><path d="M4 9h10.5a5.5 5.5 0 0 1 0 11H11" {S}/>',
    'arrow-circle-315': f'<path d="M15 14l5-5-5-5" {S}/><path d="M20 9H9.5a5.5 5.5 0 0 0 0 11H13" {S}/>',
    'layer--minus': f'<path d="M4 6.5h16M9.5 6.5V4.5a1 1 0 0 1 1-1h3a1 1 0 0 1 1 1v2" {S}/>'
                    f'<path d="M6 6.5l1 13a1.5 1.5 0 0 0 1.5 1.4h7a1.5 1.5 0 0 0 1.5-1.4l1-13" {st(RED)}/><path d="M10 10.5v6M14 10.5v6" {st(RED)}/>',
    'layer-shape-region': f'<rect x="3.5" y="5.5" width="17" height="13" rx="1.5" {st(L, 1.6, DASH)}/>'
                          f'<path d="M3.5 9V5.5H7M17 5.5h3.5V9M20.5 15v3.5H17M7 18.5H3.5V15" {st(BLUE, 2.2)}/>',
    'layer-shape-ellipse-region': f'<ellipse cx="12" cy="12" rx="8.8" ry="6.8" {st(L, 1.6, DASH)}/><circle cx="12" cy="12" r="1.7" fill="{BLUE}"/>',
    'layer-shape-polygon': f'<path d="M6 5c3-2 9-1.5 12 1.5s1.5 8-2.5 9.5-6 .5-8.5 3-4-1-3.5-5S3 7 6 5z" {st(L, 1.6, DASH)}/><circle cx="18.2" cy="6.7" r="1.8" fill="{BLUE}"/>',
    'camera': f'<path d="M3 8.5a2 2 0 0 1 2-2h2.4l1.5-2.2h6.2l1.5 2.2H19a2 2 0 0 1 2 2V18a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z" {S}/>'
              f'<circle cx="12" cy="13" r="4" fill="{BLUE}" fill-opacity=".35" {st(BLUE)}/><circle cx="17.6" cy="9.4" r=".9" fill="{L}"/>',
    'gear': f'<path d="M10.3 3.2h3.4l.5 2.3 1.6.9 2.2-.8 1.7 3-1.7 1.6v1.8l1.7 1.6-1.7 3-2.2-.8-1.6.9-.5 2.3h-3.4l-.5-2.3-1.6-.9-2.2.8-1.7-3 1.7-1.6v-1.8L4.3 8.6l1.7-3 2.2.8 1.6-.9z" {S}/>'
            f'<circle cx="12" cy="12" r="2.8" fill="{BLUE}"/>',
    'layer_fullscreen': f'<path d="M3.5 8.5v-4h4M16.5 4.5h4v4M20.5 15.5v4h-4M7.5 19.5h-4v-4" {S}/><rect x="7.5" y="8.5" width="9" height="7" rx="1.5" fill="{BLUE}"/>',
    'monitor': f'<rect x="2.5" y="4" width="19" height="13" rx="2" {S}/><path d="M9 21h6M12 17v4" {S}/><rect x="5" y="6.5" width="14" height="8" rx="1" fill="{BLUE}" fill-opacity=".3"/>',
    # ---------------- annotation tools added for ShareX parity (ids 140+) ----------------
    'balloon-box-left': f'<path d="M5 4.5h14a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2h-8.5l-4.8 3.6v-3.6H5a2 2 0 0 1-2-2v-8a2 2 0 0 1 2-2z" fill="{BLUE}" fill-opacity=".25" {st(BLUE)}/>'
                        f'<path d="M7 8.8h10M7 12.2h6.5" {st(L, 1.6)}/>',
    # 'folder-open-image' (Image (File) tool, id 141) is shared with the Image menu entry below.
    'monitor-image': f'<rect x="2.5" y="4" width="19" height="13" rx="2" {S}/><path d="M9 21h6M12 17v4" {S}/>'
                     f'<path d="M5 14.5l4.3-4.3 3.2 3 2.4-2 4.1 3.3" {st(GREEN, 1.7)}/><circle cx="15.8" cy="7.9" r="1.4" fill="{YELLOW}"/>',
    'smiley-cool': f'<circle cx="12" cy="12" r="9.3" fill="{YELLOW}"/>'
                   f'<path d="M4.6 9.6h14.8" stroke="{D}" stroke-width="1.4" stroke-linecap="round"/>'
                   f'<path d="M5.6 9.4h5.2l-.5 2.6a1.8 1.8 0 0 1-1.8 1.4H7.9a1.8 1.8 0 0 1-1.8-1.4zM13.2 9.4h5.2l-.5 2.6a1.8 1.8 0 0 1-1.8 1.4h-.6a1.8 1.8 0 0 1-1.8-1.4z" fill="{D}"/>'
                   f'<path d="M8.6 16a4.6 4.6 0 0 0 6.8 0" {st(D, 1.6)}/>',
    'stamp-cursor': f'<rect x="2.5" y="15.5" width="12" height="4.5" rx="1.3" fill="{ORANGE}"/><path d="M6 15.5v-3.2h5v3.2" {st(L, 1.6)}/>'
                    f'<circle cx="8.5" cy="8.8" r="2.6" {st(L, 1.6)}/>'
                    f'<path d="M13.5 9.5l7 4.4-3 .8 1.9 3.4-1.4.8-1.9-3.4-2.2 2.3z" fill="{L}" stroke="{L}" stroke-width="1" stroke-linejoin="round"/>',
    'table-delete-column': f'<rect x="3" y="4" width="18" height="16" rx="2" {S}/><path d="M3 8.5h18" {S}/>'
                           f'<rect x="9.4" y="4" width="5.2" height="16" fill="{RED}" fill-opacity=".3"/><path d="M9.4 4v16M14.6 4v16" {st(RED, 1.8)}/>',
    'document-copy': f'<path d="M8.5 7.5V5a1.5 1.5 0 0 1 1.5-1.5h6l4 4V17a1.5 1.5 0 0 1-1.5 1.5H16" {st(L, 1.6)}/>'
                     f'<path d="M4.5 9a1.5 1.5 0 0 1 1.5-1.5h6l3.5 3.5v8.5A1.5 1.5 0 0 1 14 21H6a1.5 1.5 0 0 1-1.5-1.5z" fill="{BLUE}" fill-opacity=".25" {st(BLUE)}/>',
    'layers-stack-arrange': f'<rect x="10.5" y="2.5" width="11" height="11" rx="2" {st(L, 1.4)}/><rect x="6.5" y="6.5" width="11" height="11" rx="2" fill="{D}" {st(L, 1.4)}/>'
                            f'<rect x="2.5" y="10.5" width="11" height="11" rx="2" fill="{BLUE}"/>',
    'layers-arrange': f'<rect x="9.5" y="3.5" width="11" height="11" rx="2" {st(L, 1.6)}/><rect x="3.5" y="9.5" width="11" height="11" rx="2" fill="{BLUE}"/>',
    'layers-arrange-back': f'<rect x="9.5" y="3.5" width="11" height="11" rx="2" fill="{BLUE}"/><rect x="3.5" y="9.5" width="11" height="11" rx="2" fill="{D}" {st(L, 1.6)}/>',
    'layers-stack-arrange-back': f'<rect x="10.5" y="2.5" width="11" height="11" rx="2" fill="{BLUE}"/><rect x="6.5" y="6.5" width="11" height="11" rx="2" fill="{D}" {st(L, 1.4)}/>'
                                 f'<rect x="2.5" y="10.5" width="11" height="11" rx="2" fill="{D}" {st(L, 1.4)}/>',

    # ---------------- editor "Image" menu (ShareX tsddbImage) ----------------
    'image--pencil': f'<rect x="2.5" y="4" width="15" height="12" rx="2" {S}/><path d="M2.5 13l4-3.5 3 2.5 2.5-2" {st()}/>'
                     f'<circle cx="13" cy="7.8" r="1.3" fill="{BLUE}"/>'
                     f'<path d="M12.5 21.5l.6-2.8 6.4-6.4a1.4 1.4 0 0 1 2 2l-6.4 6.4z" fill="{YELLOW}" stroke="{YELLOW}" stroke-width="1" stroke-linejoin="round"/>',
    'image-empty': f'<path d="M6 3h8.5L19 7.5V20a1 1 0 0 1-1 1H6a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1z" {S}/><path d="M14 3v4.5h5" {S}/>'
                   f'<path d="M12 11v6M9 14h6" {st(BLUE, 2)}/>',
    'folder-open-image': f'<path d="M2.5 7a2 2 0 0 1 2-2h4l2 2.2h8a2 2 0 0 1 2 2V17a2 2 0 0 1-2 2h-14a2 2 0 0 1-2-2z" fill="{YELLOW}" fill-opacity=".9"/>'
                         f'<rect x="9.5" y="10" width="10" height="7.5" rx="1.2" fill="{D}" stroke="{L}" stroke-width="1.4"/>'
                         f'<path d="M10.2 16.6l2.6-2.3 1.8 1.4 1.8-1.3 2.4 2" {st(BLUE, 1.3)}/>',
    'image-select': f'<rect x="3.5" y="8.5" width="12" height="12" rx="1.8" {S}/><path d="M3.5 17.5l3.5-3 2.5 2 2-1.6 4 3.2" {st(BLUE, 1.6)}/>'
                    f'<path d="M14 3.5h6.5V10M20.5 3.5L14.5 9.5" {st(BLUE, 2)}/>',
    'image-resize': f'<rect x="2.5" y="2.5" width="19" height="19" rx="2" {st(L, 1.5, DASH)}/>'
                    f'<rect x="7" y="7" width="10" height="10" rx="1.5" fill="{BLUE}" fill-opacity=".35" {st(BLUE)}/>',
    'image-resize-actual': f'<path d="M6.5 2.5v13a2 2 0 0 0 2 2h13" {S}/><path d="M2.5 6.5h13a2 2 0 0 1 2 2v13" {S}/>'
                           f'<path d="M19.5 1.5l.8 2.2 2.2.8-2.2.8-.8 2.2-.8-2.2-2.2-.8 2.2-.8z" fill="{YELLOW}"/>'
                           f'<rect x="9.5" y="9.5" width="5" height="5" rx="1" fill="{BLUE}"/>',
    'arrow-circle': f'<path d="M5 12a7 7 0 0 1 14 0" {S}/><path d="M16.3 9.8l2.7 2.7 2.7-2.7" {st(BLUE, 2)}/>'
                    f'<rect x="8" y="14.5" width="8" height="6" rx="1" fill="{BLUE}" fill-opacity=".35" {st(BLUE, 1.4)}/>',
    'arrow-circle-135-left': f'<path d="M19 12a7 7 0 0 0-14 0" {S}/><path d="M2.3 9.8l2.7 2.7 2.7-2.7" {st(BLUE, 2)}/>'
                             f'<rect x="8" y="14.5" width="8" height="6" rx="1" fill="{BLUE}" fill-opacity=".35" {st(BLUE, 1.4)}/>',
    'arrow-circle-double': f'<path d="M5 11a7 7 0 0 1 13.3-3" {S}/><path d="M19 4.3v3.9h-3.9" {st(BLUE, 2)}/>'
                           f'<path d="M19 13a7 7 0 0 1-13.3 3" {S}/><path d="M5 19.7v-3.9h3.9" {st(BLUE, 2)}/>',
    'layer-flip': f'<path d="M12 2.5v19" {st(L, 1.5, DASH)}/><path d="M9.5 6.5v11l-6.5-5.5z" fill="{BLUE}" stroke="{BLUE}" stroke-width="1.2" stroke-linejoin="round"/>'
                  f'<path d="M14.5 6.5v11l6.5-5.5z" {S}/>',
    'layer-flip-vertical': f'<path d="M2.5 12h19" {st(L, 1.5, DASH)}/><path d="M6.5 9.5h11L12 3z" fill="{BLUE}" stroke="{BLUE}" stroke-width="1.2" stroke-linejoin="round"/>'
                           f'<path d="M6.5 14.5h11L12 21z" {S}/>',
    'image-saturation': f'<circle cx="9" cy="9.5" r="5.2" fill="{RED}" fill-opacity=".8"/><circle cx="15" cy="9.5" r="5.2" fill="{GREEN}" fill-opacity=".7"/>'
                        f'<circle cx="12" cy="14.8" r="5.2" fill="{BLUE}" fill-opacity=".7"/>',

    'app': '<defs><linearGradient id="g" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#5AB0FF"/><stop offset="1" stop-color="#2F6FE0"/></linearGradient></defs>'
           '<rect x="1" y="1" width="22" height="22" rx="5.5" fill="url(#g)"/>'
           '<path d="M5 9.2a1.8 1.8 0 0 1 1.8-1.8h1.9l1.3-1.9h4l1.3 1.9h1.9A1.8 1.8 0 0 1 19 9.2v7.1a1.8 1.8 0 0 1-1.8 1.8H6.8A1.8 1.8 0 0 1 5 16.3z" fill="#fff"/>'
           '<circle cx="12" cy="12.6" r="3.3" fill="#2F6FE0"/><circle cx="12" cy="12.6" r="1.6" fill="#9CD0FF"/>',
    # ---------------- main window ----------------
    'mw-camera': None,  # = camera
    'mw-upload': f'<path d="M7 18.5h-.5A4.5 4.5 0 0 1 6 9.6a6 6 0 0 1 11.6 1.4h.4a3.8 3.8 0 0 1 0 7.5H17" {S}/><path d="M12 20v-8M8.8 15l3.2-3.2 3.2 3.2" {st(BLUE, 2)}/>',
    'mw-workflows': f'<rect x="3.5" y="3.5" width="7" height="7" rx="2" fill="{BLUE}"/><rect x="13.5" y="3.5" width="7" height="7" rx="2" fill="{GREEN}"/>'
                    f'<rect x="3.5" y="13.5" width="7" height="7" rx="2" fill="{ORANGE}"/><rect x="13.5" y="13.5" width="7" height="7" rx="2" fill="{PURPLE}"/>',
    'mw-wrench': f'<path d="M14.7 6.3a4 4 0 0 0-5.3 5.3L3.8 17.2a1.9 1.9 0 0 0 2.7 2.7l5.6-5.6a4 4 0 0 0 5.3-5.3l-2.5 2.5-2.4-.3-.3-2.4z" {S}/><circle cx="5.2" cy="18.6" r="1" fill="{ORANGE}"/>',
    'mw-after-capture-tasks': f'<rect x="3" y="4.5" width="15" height="12" rx="2" {S}/><path d="M3 13.5l4-3.5 3 2.5 3-2 5 4" {st()}/>'
                              f'<circle cx="17.5" cy="17" r="4.5" fill="{GREEN}"/><path d="M15.4 17.1l1.4 1.4 2.6-2.8" {st("#fff", 1.6)}/>',
    'mw-after-upload-tasks': f'<path d="M7 17h-.5A4.5 4.5 0 0 1 6 8.1a6 6 0 0 1 11.6 1.4" {S}/><circle cx="16.5" cy="16" r="5" fill="{GREEN}"/><path d="M14.2 16.1l1.6 1.6 2.9-3.1" {st("#fff", 1.7)}/>',
    'mw-destinations': f'<circle cx="12" cy="12" r="9" {S}/><path d="M3 12h18M12 3c2.6 2.6 3.8 5.6 3.8 9s-1.2 6.4-3.8 9c-2.6-2.6-3.8-5.6-3.8-9S9.4 5.6 12 3z" {st(BLUE)}/>',
    'mw-settings': None,  # = wrench-screwdriver
    'mw-sliders': None,   # = gear
    'mw-keyboard': f'<rect x="2.5" y="6" width="19" height="12.5" rx="2.5" {S}/>'
                   + ''.join(f'<rect x="{5 + i * 3.2:.1f}" y="9" width="1.9" height="1.9" rx=".5" fill="{L}"/>' for i in range(5))
                   + ''.join(f'<rect x="{6.6 + i * 3.2:.1f}" y="12" width="1.9" height="1.9" rx=".5" fill="{L}"/>' for i in range(4))
                   + f'<rect x="8" y="15.1" width="8" height="1.8" rx=".9" fill="{BLUE}"/>',
    'mw-destination-settings': f'<circle cx="11" cy="11" r="8" {S}/><path d="M3 11h16M11 3c2.3 2.3 3.4 5 3.4 8s-1.1 5.7-3.4 8c-2.3-2.3-3.4-5-3.4-8S8.7 5.3 11 3z" {st(BLUE)}/>'
                               f'<circle cx="18" cy="18" r="4.2" fill="{D}" stroke="{L}" stroke-width="1.6"/><circle cx="18" cy="18" r="1.5" fill="{L}"/>',
    'mw-custom-uploader-settings': f'<path d="M7 17h-.5A4.5 4.5 0 0 1 6 8.1a6 6 0 0 1 11.6 1.4h.4a3.8 3.8 0 0 1 2 .6" {S}/><path d="M12.5 15l-2.5 2.5 2.5 2.5M17.5 15l2.5 2.5-2.5 2.5" {st(PURPLE, 1.9)}/>',
    'mw-screenshots-folder': f'<path d="M3 7a2 2 0 0 1 2-2h4l2 2.2h8a2 2 0 0 1 2 2V17a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z" fill="{YELLOW}" fill-opacity=".9"/>'
                             f'<path d="M3 10h18" stroke="#C99A1A" stroke-width="1.2"/>',
    'mw-history': f'<path d="M4 12a8 8 0 1 0 2.4-5.7" {S}/><path d="M3.5 4.5v3.4h3.4" {S}/><path d="M12 7.5V12l3 2" {st(BLUE, 2)}/>',
    'mw-image-history': f'<rect x="6.5" y="3.5" width="14" height="11" rx="2" {st(L, 1.6)}/><rect x="3.5" y="8.5" width="14" height="11" rx="2" fill="{D}" {S}/>'
                        f'<path d="M3.5 16.5l3.5-3 2.5 2 3-2.5 5 4" {st(BLUE)}/><circle cx="13.5" cy="11.8" r="1.2" fill="{YELLOW}"/>',
    'mw-bug': f'<rect x="7" y="7.5" width="10" height="12.5" rx="5" fill="{ORANGE}" fill-opacity=".3" {st(ORANGE)}/><path d="M9 7.5a3 3 0 0 1 6 0M12 10v9.5M3.5 11h3.5M17 11h3.5M4 17l3-1.2M20 17l-3-1.2M5 5.5l2.4 2.2M19 5.5l-2.4 2.2" {S}/>',
    'mw-heart': f'<path d="M12 20s-7.5-4.6-8.7-9.4C2.4 6.9 5 4 8 4c1.8 0 3.2 1 4 2.4C12.8 5 14.2 4 16 4c3 0 5.6 2.9 4.7 6.6C19.5 15.4 12 20 12 20z" fill="{RED}"/>',
    'mw-follow': f'<path d="M4.5 4h4l11 16h-4z" {S}/><path d="M19.5 4L13.3 11M10.7 13L4.5 20" {S}/>',
    'mw-discord': f'<path d="M7.5 5.5c1.6-.6 3-.9 4.5-.9s2.9.3 4.5.9c2 2.7 3 5.8 3 9.6-1.5 1.5-3.2 2.4-5 2.9l-1-2c-1 .3-2.4.3-3 0l-1 2c-1.8-.5-3.5-1.4-5-2.9 0-3.8 1-6.9 3-9.6z" fill="#5865F2"/>'
                  f'<circle cx="9.3" cy="12.2" r="1.5" fill="#fff"/><circle cx="14.7" cy="12.2" r="1.5" fill="#fff"/>',
    'mw-info': f'<circle cx="12" cy="12" r="9.5" fill="{BLUE}"/><path d="M12 11v6" {st("#fff", 2.2)}/><circle cx="12" cy="7.6" r="1.3" fill="#fff"/>',
    'mw-region': None,           # = layer-shape-region
    'mw-fullscreen': None,       # = layer_fullscreen
    'mw-window': f'<rect x="3" y="4.5" width="18" height="15" rx="2.5" {S}/><path d="M3 9h18" {S}/>'
                 f'<circle cx="6" cy="6.8" r=".9" fill="{RED}"/><circle cx="8.8" cy="6.8" r=".9" fill="{YELLOW}"/><circle cx="11.6" cy="6.8" r=".9" fill="{GREEN}"/>',
    'mw-screen-recording': f'<rect x="2.5" y="4" width="19" height="13" rx="2" {S}/><path d="M9 21h6M12 17v4" {S}/><circle cx="12" cy="10.5" r="3.4" fill="{RED}"/>',
    'mw-film': f'<rect x="3" y="4" width="18" height="16" rx="2.5" {S}/><path d="M7 4v16M17 4v16" {st(L, 1.4)}/>'
               f'<text x="12" y="14.6" text-anchor="middle" font-family="Segoe UI" font-weight="800" font-size="6.6" fill="{ORANGE}">GIF</text>',
}
ALIAS = {'mw-camera': 'camera', 'mw-settings': 'wrench-screwdriver', 'mw-sliders': 'gear',
         'mw-region': 'layer-shape-region', 'mw-fullscreen': 'layer_fullscreen'}

CELL = 64


def main():
    names = list(ICONS)
    cols = 8
    rows = (len(names) + cols - 1) // cols
    cells = []
    for i, n in enumerate(names):
        body = ICONS[ALIAS.get(n, n)]
        x, y = (i % cols) * CELL, (i // cols) * CELL
        cells.append(f'<svg style="position:absolute;left:{x}px;top:{y}px" width="{CELL}" height="{CELL}" '
                     f'viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg">{body}</svg>')
    html = ('<!doctype html><html><head><style>html,body{margin:0;background:transparent}</style></head><body>'
            + ''.join(cells) + '</body></html>')
    tmp = tempfile.mkdtemp()
    page = os.path.join(tmp, 'icons.html')
    sheet = os.path.join(tmp, 'sheet.png')
    open(page, 'w', encoding='utf-8').write(html)
    subprocess.run([CHROME, '--headless=new', '--disable-gpu', '--hide-scrollbars', '--force-device-scale-factor=1',
                    '--default-background-color=00000000', f'--window-size={cols * CELL},{rows * CELL}',
                    f'--screenshot={sheet}', 'file:///' + page.replace('\\', '/')], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    im = Image.open(sheet).convert('RGBA')
    for i, n in enumerate(names):
        x, y = (i % cols) * CELL, (i // cols) * CELL
        im.crop((x, y, x + CELL, y + CELL)).save(os.path.join(RES, n + '.png'))
    # Preview on the toolbar colour at the sizes actually used (24px = 16 @150%, 32px = 16 @200%).
    pv = Image.new('RGBA', (cols * 90 + 20, rows * 90 + 20), (39, 39, 39, 255))
    for i, n in enumerate(names):
        ic = im.crop(((i % cols) * CELL, (i // cols) * CELL, (i % cols + 1) * CELL, (i // cols + 1) * CELL))
        x, y = 20 + (i % cols) * 90, 20 + (i // cols) * 90
        pv.alpha_composite(ic.resize((24, 24), Image.LANCZOS), (x, y))
        pv.alpha_composite(ic.resize((48, 48), Image.LANCZOS), (x + 30, y))
    pv.save(os.path.join(HERE, 'preview.png'))
    print(len(names), 'icons')


if __name__ == '__main__':
    sys.exit(main())

"""Redirect only the documentation helper's native drawing surface to WIC.

The production editor renderer, toolbar, layout, fonts and annotations remain
the exact production methods. WIC avoids unreliable PrintWindow GPU readback
on an offscreen window. No production source or installed binary is rewritten.
"""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = (root/'src/editor.cpp').read_text(encoding='utf-8')
patches = {
    'ComPtr<ID2D1HwndRenderTarget> rt;': 'ComPtr<ID2D1RenderTarget> rt;',
    'D2D()->CreateHwndRenderTarget(p, D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(cr.right, cr.bottom)), &rt);':
        'CreateReadmeRenderTarget(D2D(), hwnd, &rt);',
    'if (e->rt) e->rt->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l)));':
        'if (e->rt) { e->rt.Reset(); e->g = {}; }',
}
for old, new in patches.items():
    if source.count(old) != 1: raise RuntimeError('Production renderer changed: review the documentation adapter')
    source = source.replace(old, new)
out = root/'build/readme-native/editor_readme.inc'
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(source, encoding='utf-8')

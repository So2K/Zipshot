"""Generate a trace-only COPY of capture.cpp; never edit the production source.

The trace logs acquired-frame metadata and samples a single texture pixel before
the original zero-LastPresentTime retry branch. It is diagnostic instrumentation,
not a fair performance measurement. The separate plain binary uses untouched code.
"""
import hashlib
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
source=ROOT/'src/capture.cpp'
raw=source.read_bytes()
text=raw.decode('utf-8')
needle='        if (FAILED(hr) || fi.LastPresentTime.QuadPart != 0 || attempt == 2) break;'
if text.count(needle)!=1: raise RuntimeError('Capture retry branch changed; review before instrumentation.')
trace=r'''
        Log(L"DIAG acquire attempt=%d hr=%08X present=%lld mouse=%lld accumulated=%u resource=%d", attempt,(unsigned)hr,fi.LastPresentTime.QuadPart,fi.LastMouseUpdateTime.QuadPart,fi.AccumulatedFrames,res.Get()!=nullptr);
        if(SUCCEEDED(hr)&&res) {
            ComPtr<ID3D11Texture2D> diag_texture;
            if(SUCCEEDED(res.As(&diag_texture))) {
                D3D11_TEXTURE2D_DESC diag_desc{};diag_texture->GetDesc(&diag_desc);
                Log(L"DIAG texture width=%u height=%u format=%u",diag_desc.Width,diag_desc.Height,(unsigned)diag_desc.Format);
                D3D11_TEXTURE2D_DESC diag_read{};diag_read.Width=1;diag_read.Height=1;diag_read.MipLevels=1;diag_read.ArraySize=1;diag_read.Format=diag_desc.Format;diag_read.SampleDesc.Count=1;diag_read.Usage=D3D11_USAGE_STAGING;diag_read.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                ComPtr<ID3D11Texture2D> diag_pixel;
                if(SUCCEEDED(da.dev->CreateTexture2D(&diag_read,nullptr,&diag_pixel))) {
                    UINT dx=diag_desc.Width/2,dy=diag_desc.Height/2;D3D11_BOX box{dx,dy,0,dx+1,dy+1,1};
                    da.ctx->CopySubresourceRegion(diag_pixel.Get(),0,0,0,0,diag_texture.Get(),0,&box);
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    if(SUCCEEDED(da.ctx->Map(diag_pixel.Get(),0,D3D11_MAP_READ,0,&mapped))) {
                        Log(L"DIAG pre_retry_centre_pixel=%08X expected_fixture_rgb=00183B63",*(const uint32_t*)mapped.pData);
                        da.ctx->Unmap(diag_pixel.Get(),0);
                    }
                }
            }
        }
'''
output=ROOT/'build/runtime-compare'
output.mkdir(parents=True,exist_ok=True)
derived=text.replace(needle,trace+'\n'+needle)
(output/'capture_diagnostic_impl.cpp').write_text(derived,encoding='utf-8',newline='\n')
(output/'capture_api_manifest.json').write_text(json.dumps({'source':'src/capture.cpp','production_source_sha256':hashlib.sha256(raw).hexdigest(),'trace_copy_sha256':hashlib.sha256(derived.encode()).hexdigest(),'production_source_modified':False,'trace_changes':'Logging acquired-frame metadata and one texture-pixel read before the existing retry branch.'},indent=2)+'\n',encoding='utf-8')
print('Generated diagnostic copy; production capture.cpp untouched.')

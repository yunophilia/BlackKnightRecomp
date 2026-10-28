#include "ui_shot.h"

#include <cstdio>
#include <cstring>
#include <vector>

bool UI_SaveBoundTarget(ID3D11Device* dev, ID3D11DeviceContext* ctx, const char* path)
{
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return false;
    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    rtv->Release();
    ID3D11Texture2D* tex = nullptr;
    res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
    res->Release();
    if (!tex) return false;
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    bool ok = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &st));
    if (ok) ctx->CopyResource(st, tex);
    tex->Release();
    D3D11_MAPPED_SUBRESOURCE m;
    if (!ok || FAILED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m)))
    {
        if (st) st->Release();
        return false;
    }
    FILE* f = fopen(path, "wb");
    if (f)
    {
        uint32_t w = d.Width, h = d.Height, rowBytes = w * 3, pad = (4 - rowBytes % 4) % 4, img = (rowBytes + pad) * h;
        uint8_t hdr[54] = { 'B', 'M' };
        auto put32 = [&](int o, uint32_t v) { memcpy(hdr + o, &v, 4); };
        put32(2, 54 + img); put32(10, 54); put32(14, 40); put32(18, w); put32(22, h);
        hdr[26] = 1; hdr[28] = 24; put32(34, img);
        fwrite(hdr, 1, 54, f);
        std::vector<uint8_t> row(rowBytes + pad);
        for (int y = (int)h - 1; y >= 0; y--)
        {
            const uint8_t* src = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
            for (uint32_t x = 0; x < w; x++)
            {
                row[x * 3 + 0] = src[x * 4 + 2];
                row[x * 3 + 1] = src[x * 4 + 1];
                row[x * 3 + 2] = src[x * 4 + 0];
            }
            fwrite(row.data(), 1, row.size(), f);
        }
        fclose(f);
    }
    ctx->Unmap(st, 0);
    st->Release();
    return f != nullptr;
}

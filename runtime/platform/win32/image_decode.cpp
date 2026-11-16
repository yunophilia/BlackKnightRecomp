// The mod browser's web images through the Windows Imaging Component.
#include "../image.h"

#include <windows.h>
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")

bool Image_Decode(const void* data, size_t size, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba)
{
    static thread_local IWICImagingFactory* wic = nullptr;
    if (!wic)
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    }
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    bool ok = SUCCEEDED(wic->CreateStream(&stream)) &&
              SUCCEEDED(stream->InitializeFromMemory((BYTE*)data, (DWORD)size)) &&
              SUCCEEDED(wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom));
    UINT uw = 0, uh = 0;
    if (ok) ok = SUCCEEDED(conv->GetSize(&uw, &uh)) && uw && uh && uw <= 4096 && uh <= 4096;
    if (ok)
    {
        rgba.resize((size_t)uw * uh * 4);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, uw * 4, (UINT)rgba.size(), rgba.data()));
        width = uw;
        height = uh;
    }
    for (IUnknown* u : { (IUnknown*)conv, (IUnknown*)frame, (IUnknown*)dec, (IUnknown*)stream })
        if (u) u->Release();
    return ok;
}

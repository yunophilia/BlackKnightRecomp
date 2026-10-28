// PNG through the Windows Imaging Component.
#include "../image.h"

#include <windows.h>
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")

static IWICImagingFactory* s_wic = nullptr;

void Image_Init()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (!s_wic) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&s_wic));
}

bool Image_LoadPNG(const std::filesystem::path& path, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba)
{
    if (!s_wic) return false;
    IWICBitmapDecoder* dec = nullptr;
    if (FAILED(s_wic->CreateDecoderFromFilename(path.wstring().c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)))
        return false;
    bool ok = false;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    UINT w = 0, h = 0;
    if (SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(s_wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(conv->GetSize(&w, &h)) && w && h)
    {
        rgba.assign((size_t)w * h * 4, 0);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)rgba.size(), rgba.data()));
        width = w;
        height = h;
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    dec->Release();
    return ok;
}

bool Image_SavePNG(const std::filesystem::path& path, uint32_t w, uint32_t h, const uint32_t* rgba)
{
    if (!s_wic) return false;
    bool ok = false;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    if (SUCCEEDED(s_wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.wstring().c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(s_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(w, h)))
    {
        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppRGBA;
        frame->SetPixelFormat(&pf);
        frame->WritePixels(h, w * 4, w * h * 4, (BYTE*)rgba);
        frame->Commit();
        enc->Commit();
        ok = true;
    }
    if (frame) frame->Release();
    if (enc) enc->Release();
    if (stream) stream->Release();
    return ok;
}

#include "ui_image.h"
#include "../log.h"
#include "../net/http.h"

#include <d3d11.h>
#include <wincodec.h>

#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
struct Image
{
    enum State { Queued, Loading, Decoded, Ready, Failed } state = Queued;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    ID3D11ShaderResourceView* srv = nullptr;
};

std::mutex s_mutex;
std::condition_variable s_cv;
std::map<std::string, std::shared_ptr<Image>> s_images;
std::deque<std::string> s_queue;
bool s_worker = false;
ID3D11Device* s_device = nullptr;   // UI thread only

bool Decode(const std::string& data, std::vector<uint8_t>& rgba, int& w, int& h)
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
              SUCCEEDED(stream->InitializeFromMemory((BYTE*)data.data(), (DWORD)data.size())) &&
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
        w = (int)uw;
        h = (int)uh;
    }
    for (IUnknown* u : { (IUnknown*)conv, (IUnknown*)frame, (IUnknown*)dec, (IUnknown*)stream })
        if (u) u->Release();
    return ok;
}

void Worker()
{
    for (;;)
    {
        std::string url;
        std::shared_ptr<Image> img;
        {
            std::unique_lock lk(s_mutex);
            s_cv.wait(lk, [] { return !s_queue.empty(); });
            url = s_queue.back();   // the newest request first: what is on screen now
            s_queue.pop_back();
            auto it = s_images.find(url);
            if (it == s_images.end()) continue;   // dropped with its device
            img = it->second;
            img->state = Image::Loading;
        }
        std::string body, error;
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        bool ok = Http_Get(url, body, error) && Decode(body, rgba, w, h);
        if (!ok) LOG_DEBUG("ui image {}: {}", url, error.empty() ? "cannot decode" : error);
        std::lock_guard lk(s_mutex);
        img->rgba = std::move(rgba);
        img->w = w;
        img->h = h;
        img->state = ok ? Image::Decoded : Image::Failed;
    }
}
}   // namespace

void UiImage_SetDevice(ID3D11Device* device)
{
    std::lock_guard lk(s_mutex);
    if (device == s_device) return;
    for (auto& [url, img] : s_images)
        if (img->srv)
        {
            img->srv->Release();
            img->srv = nullptr;
            img->state = Image::Failed;
        }
    // dropped textures load again for the next device
    for (auto it = s_images.begin(); it != s_images.end();)
        it = it->second->state == Image::Failed ? s_images.erase(it) : std::next(it);
    s_device = device;
}

ImTextureID UiImage_Get(const std::string& url, ImVec2* size)
{
    if (url.empty() || !s_device) return ImTextureID{};
    std::lock_guard lk(s_mutex);
    auto& img = s_images[url];
    if (!img)
    {
        img = std::make_shared<Image>();
        s_queue.push_back(url);
        if (!s_worker)
        {
            s_worker = true;
            std::thread(Worker).detach();
        }
        s_cv.notify_one();
        return ImTextureID{};
    }
    if (img->state == Image::Decoded)
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = img->w;
        td.Height = img->h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{ img->rgba.data(), (UINT)img->w * 4, 0 };
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(s_device->CreateTexture2D(&td, &sd, &tex)))
        {
            s_device->CreateShaderResourceView(tex, nullptr, &img->srv);
            tex->Release();
        }
        img->rgba.clear();
        img->rgba.shrink_to_fit();
        img->state = img->srv ? Image::Ready : Image::Failed;
    }
    if (img->state != Image::Ready) return ImTextureID{};
    if (size) *size = ImVec2((float)img->w, (float)img->h);
    return (ImTextureID)(intptr_t)img->srv;
}

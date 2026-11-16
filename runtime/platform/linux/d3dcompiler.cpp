// D3DCompile and D3DCreateBlob for dxvk-native, on vkd3d-shader (HLSL -> DXBC, the
// compiler Wine and Proton use). The renderer's generated shaders, its utility shaders and
// ImGui's compile through this unless the shader cache has them.
#include <d3dcompiler.h>
#include <vkd3d_shader.h>

#include <atomic>
#include <cstring>
#include <new>
#include <vector>

namespace
{
class Blob final : public ID3D10Blob
{
public:
    explicit Blob(size_t n) : m_data(n) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D10Blob))
        {
            AddRef();
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG n = --m_refs;
        if (!n) delete this;
        return n;
    }
    LPVOID STDMETHODCALLTYPE GetBufferPointer() override { return m_data.data(); }
    SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return m_data.size(); }

private:
    std::atomic<ULONG> m_refs{ 1 };
    std::vector<uint8_t> m_data;
};

ID3DBlob* MakeBlob(const void* data, size_t n)
{
    Blob* b = new (std::nothrow) Blob(n);
    if (b && data && n) memcpy(b->GetBufferPointer(), data, n);
    return b;
}
}   // namespace

extern "C" HRESULT WINAPI D3DCreateBlob(SIZE_T size, ID3DBlob** blob)
{
    if (!blob) return E_INVALIDARG;
    *blob = MakeBlob(nullptr, size);
    return *blob ? S_OK : E_OUTOFMEMORY;
}

extern "C" HRESULT WINAPI D3DCompile(const void* data, SIZE_T size, const char* filename, const D3D_SHADER_MACRO* defines,
                                     ID3DInclude* include, const char* entry, const char* target, UINT flags1, UINT flags2,
                                     ID3DBlob** shader, ID3DBlob** errors)
{
    (void)flags1;
    (void)flags2;
    if (shader) *shader = nullptr;
    if (errors) *errors = nullptr;
    if (include) return E_NOTIMPL;   // not used by the runtime

    std::vector<vkd3d_shader_macro> macros;
    for (const D3D_SHADER_MACRO* d = defines; d && d->Name; d++) macros.push_back({ d->Name, d->Definition ? d->Definition : "" });

    vkd3d_shader_preprocess_info pp{};
    pp.type = VKD3D_SHADER_STRUCTURE_TYPE_PREPROCESS_INFO;
    pp.macros = macros.data();
    pp.macro_count = (unsigned)macros.size();

    vkd3d_shader_hlsl_source_info hlsl{};
    hlsl.type = VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO;
    hlsl.next = &pp;
    hlsl.entry_point = entry;
    hlsl.profile = target;

    vkd3d_shader_compile_option options[] = {
        { VKD3D_SHADER_COMPILE_OPTION_API_VERSION, VKD3D_SHADER_API_VERSION_1_19 },
    };
    vkd3d_shader_compile_info info{};
    info.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
    info.next = &hlsl;
    info.source.code = data;
    info.source.size = size;
    info.source_type = VKD3D_SHADER_SOURCE_HLSL;
    info.target_type = VKD3D_SHADER_TARGET_DXBC_TPF;
    info.options = options;
    info.option_count = sizeof(options) / sizeof(options[0]);
    info.log_level = VKD3D_SHADER_LOG_WARNING;
    info.source_name = filename;

    vkd3d_shader_code out{};
    char* messages = nullptr;
    int ret = vkd3d_shader_compile(&info, &out, &messages);
    if (messages && *messages && errors) *errors = MakeBlob(messages, strlen(messages) + 1);
    vkd3d_shader_free_messages(messages);
    if (ret < 0) return E_FAIL;
    if (shader) *shader = MakeBlob(out.code, out.size);
    vkd3d_shader_free_shader_code(&out);
    return S_OK;
}

// Exercises the production frame path through stage 2, including its raw-copy
// guard. No NGX, D3D12 session, ReShade or game is required. Debug validation
// catches an invalid raw CopyResource after MV format conversion.
#include "../src/dlss5-bridge.cpp"
#include <d3d11sdklayers.h>
#include <vector>

static void Check(HRESULT hr)
{
    if (FAILED(hr)) { printf("FAIL: HRESULT %08lX\n", hr); exit(1); }
}

static ID3D11Texture2D *Texture(ID3D11Device *dev, UINT w, UINT h,
                              DXGI_FORMAT format, UINT bindings, bool input)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = td.ArraySize = 1;
    td.Format = format; td.SampleDesc.Count = 1; td.BindFlags = bindings;
    const UINT channels = format == DXGI_FORMAT_R32G32_FLOAT ? 2u : 1u;
    std::vector<float> pixels(static_cast<size_t>(w) * h * channels, .25f);
    if (channels == 2)
        for (size_t i = 1; i < pixels.size(); i += 2) pixels[i] = -.5f;
    D3D11_SUBRESOURCE_DATA data = {};
    data.pSysMem = pixels.data(); data.SysMemPitch = w * channels * sizeof(float);
    ID3D11Texture2D *texture = nullptr;
    Check(dev->CreateTexture2D(&td, input ? &data : nullptr, &texture));
    return texture;
}

static void ReadCheck(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                      ID3D11Texture2D *texture, bool motion, bool converted)
{
    D3D11_TEXTURE2D_DESC td = {}; texture->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = td.MiscFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *read = nullptr; Check(dev->CreateTexture2D(&td, nullptr, &read));
    ctx->CopyResource(read, texture);
    D3D11_MAPPED_SUBRESOURCE mapped = {}; Check(ctx->Map(read, 0, D3D11_MAP_READ, 0, &mapped));
    for (UINT y = 0; y < td.Height; ++y) for (UINT x = 0; x < td.Width; ++x)
    {
        const BYTE *row = static_cast<const BYTE *>(mapped.pData) + y * mapped.RowPitch;
        bool correct = false;
        if (motion && converted)
        {
            const auto p = reinterpret_cast<const unsigned short *>(row) + x * 2;
            correct = p[0] == 0x3400 && p[1] == 0xb800; // .25 and -.5, binary16.
        }
        else
        {
            const auto p = reinterpret_cast<const float *>(row) + x * (motion ? 2 : 1);
            correct = p[0] == .25f && (!motion || p[1] == -.5f);
        }
        if (!correct)
        { printf("FAIL: %s pixel %u,%u of %ux%u\n", motion ? "MV" : "depth", x, y, td.Width, td.Height); exit(1); }
    }
    ctx->Unmap(read, 0); read->Release();
}

static void FrameCase(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                      UINT colour_w, UINT colour_h, UINT input_w, UINT input_h, bool converted)
{
    g_bridge = {};
    g_bridge.session_ready = g_bridge.frame_ready = true;
    g_bridge.width = g_bridge.out_width = g_bridge.render_w = g_bridge.ngx_out_w = colour_w;
    g_bridge.height = g_bridge.out_height = g_bridge.render_h = g_bridge.ngx_out_h = colour_h;
    ID3D11Texture2D *source[SLOT_COUNT] = {};
    SynthParams params;
    params.Set("Width", colour_w); params.Set("Height", colour_h);
    params.Set("OutWidth", colour_w); params.Set("OutHeight", colour_h);
    for (int i = 0; i < SLOT_COUNT; ++i)
    {
        const bool auxiliary = i == SLOT_DEPTH || i == SLOT_MV;
        const UINT w = auxiliary ? input_w : colour_w, h = auxiliary ? input_h : colour_h;
        const DXGI_FORMAT format = i == SLOT_MV ? DXGI_FORMAT_R32G32_FLOAT : DXGI_FORMAT_R32_FLOAT;
        source[i] = Texture(dev, w, h, format, D3D11_BIND_SHADER_RESOURCE, true);
        params.Set(kSlotKey[i], static_cast<ID3D11Resource *>(source[i]));
        g_bridge.slot_w[i] = w; g_bridge.slot_h[i] = h; g_bridge.fmt[i] = format;
        const DXGI_FORMAT target = i == SLOT_MV && converted ? DXGI_FORMAT_R16G16_FLOAT : format;
        g_bridge.tex11[i] = Texture(dev, w, h, target, auxiliary ? D3D11_BIND_UNORDERED_ACCESS : 0u, false);
    }
    Check(dev->CreateUnorderedAccessView(g_bridge.tex11[SLOT_DEPTH], nullptr, &g_bridge.depth_uav));
    if (!BridgeMakeDepthShader(dev)) { puts("FAIL: depth shader"); exit(1); }
    g_bridge.depth_converted = true;
    g_bridge.mv_converted = converted;
    if (converted)
    {
        Check(dev->CreateUnorderedAccessView(g_bridge.tex11[SLOT_MV], nullptr, &g_bridge.mv_uav));
        if (!BridgeMakeMVShader(dev)) { puts("FAIL: MV shader"); exit(1); }
    }
    const float sentinel[4] = {-4.f, -4.f, -4.f, -4.f};
    ctx->ClearUnorderedAccessViewFloat(g_bridge.depth_uav, sentinel);
    if (converted) ctx->ClearUnorderedAccessViewFloat(g_bridge.mv_uav, sentinel);
    g_cfg.stage = 2; g_cfg_last_read = static_cast<LONGLONG>(GetTickCount64());
    BridgeFrameInner(ctx, &params);
    if (g_bridge.disabled || g_bridge.frames_done != 1)
    { puts("FAIL: production frame did not reach stage 2"); exit(1); }
    ReadCheck(dev, ctx, g_bridge.tex11[SLOT_DEPTH], false, false);
    ReadCheck(dev, ctx, g_bridge.tex11[SLOT_MV], true, converted);
    ctx->ClearState();
    BridgeReleaseTextures();
    if (g_bridge.depth_cs) g_bridge.depth_cs->Release();
    if (g_bridge.mv_cs) g_bridge.mv_cs->Release();
    for (auto texture : source) texture->Release();
    printf("PASS: Color %ux%u, Depth/MV %ux%u, MV %s\n",
           colour_w, colour_h, input_w, input_h, converted ? "converted" : "raw copy");
}

int main(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--warp") != 0))
    { puts("Usage: d3d11-conversion.exe [--warp]"); return 2; }
    const D3D_DRIVER_TYPE driver = argc == 2 ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
    printf("D3D11 device: %s\n", driver == D3D_DRIVER_TYPE_WARP ? "WARP (software)" : "hardware");
    InitializeCriticalSection(&g_log_cs); strcpy_s(g_log_path, "d3d11-conversion.log");
    ID3D11Device *dev = nullptr; ID3D11DeviceContext *ctx = nullptr;
    // Only a debug device can catch the otherwise ignored invalid CopyResource
    // between different formats. Pixel coverage remains useful without it.
    HRESULT hr = D3D11CreateDevice(nullptr, driver, nullptr, D3D11_CREATE_DEVICE_DEBUG,
                                  nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING)
    {
        puts("SKIPPED: D3D11 debug validation unavailable; invalid raw-copy guard is not validated");
        hr = D3D11CreateDevice(nullptr, driver, nullptr, 0,
                              nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    }
    Check(hr);
    ID3D11InfoQueue *info = nullptr;
    dev->QueryInterface(IID_PPV_ARGS(&info));
    FrameCase(dev, ctx, 17, 9, 17, 9, true);
    FrameCase(dev, ctx, 17, 9, 65, 33, true);
    FrameCase(dev, ctx, 65, 33, 17, 9, true);
    FrameCase(dev, ctx, 17, 9, 17, 9, false);
    for (UINT64 i = 0; info != nullptr && i < info->GetNumStoredMessages(); ++i)
    {
        SIZE_T size = 0; Check(info->GetMessage(i, nullptr, &size));
        std::vector<BYTE> buffer(size); auto msg = reinterpret_cast<D3D11_MESSAGE *>(buffer.data());
        Check(info->GetMessage(i, msg, &size));
        if (msg->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
        { printf("FAIL: D3D11 validation: %s\n", msg->pDescription); return 1; }
    }
    printf("PASS: all pixels correct through the production frame path; D3D11 validation %s\n",
           info != nullptr ? "passed" : "SKIPPED");
    if (info != nullptr) info->Release();
    ctx->Release(); dev->Release(); DeleteCriticalSection(&g_log_cs);
    return 0;
}

// Implementation of the D3D11 -> D3D12 NGX bridge described in bridge.h.

static NVSDK_NGX_Result SafeEvaluateGuarded(PFN_D3D12EvaluateFeature fn,
                                            ID3D12GraphicsCommandList *list,
                                            const NVSDK_NGX_Handle *feature,
                                            const NVSDK_NGX_Parameter *p, DWORD *code)
{
    *code = 0;
    __try { return fn(list, feature, p, nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); return NGX_EXCEPTION_MARKER; }
}

// Everything worth trying here is a value, not code, and the add-on binary is
// locked while the game runs. So the tunables live in ngxprobe.cfg, re-read
// while playing: edit the file, and within a second the bridge picks it up and
// rebuilds whatever the change invalidates. One session, many experiments.
//
//   mode=2         0 never writes to the game, 1 transport only, 2 full path
//   flags=107      DLSS.Feature.Create.Flags. Bit 0 is IsHDR, which is what
//                  makes RenoDX apply its HDR codec; 106 clears it
//   subrects=0     value for DLSS.Enable.Output.Subrects
//   reset_every=0  1 forces Reset on every frame, killing temporal history
//   pixels=0       1 enables the centre-pixel readback (stalls the GPU hard)
//   stage=3        how much of the bridge runs, to find which part disturbs the
//                  game's own rendering:
//                    0 fully inert -- the game must look perfect
//                    1 the Color and MV copies only
//                    2 also the depth compute blit
//                    3 also the fence sync and the D3D12 evaluate
//   skip_game=1    do not forward the game's own DLSS evaluate. Its result is
//                  overwritten by this bridge anyway, so running it is wasted
//                  work -- this is where most of the frame cost comes back.
//                  Only takes effect while the bridge is actually delivering.
struct BridgeCfg
{
    int mode;
    int flags;
    int subrects;
    int reset_every;
    int pixels;
    int stage;
    int skip_game;
};

static BridgeCfg g_cfg = { 2, 107, 1, 0, 0, 3, 1 };

// Accumulates the two figures and reports them together, because either one
// alone is misleading.
static void TimingTick(LONGLONG entry, LONGLONG exit)
{
    if (g_bridge.qpf == 0)
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_bridge.qpf = f.QuadPart;
        g_bridge.span_start = entry;
    }

    g_bridge.cpu_ticks += (exit - entry);
    g_bridge.last_entry = entry;
    if (++g_bridge.timed_frames < 600) return;

    const double span_ms = 1000.0 * double(exit - g_bridge.span_start) / double(g_bridge.qpf);
    const double cpu_ms  = 1000.0 * double(g_bridge.cpu_ticks) / double(g_bridge.qpf);
    const double n       = double(g_bridge.timed_frames);

    Log("[bridge] %.0f frames: bridge CPU %.2f ms/frame | frame interval %.2f ms (%.1f fps) "
        "| bridge is %.0f%% of the frame",
        n, cpu_ms / n, span_ms / n, 1000.0 / (span_ms / n), 100.0 * cpu_ms / span_ms);

    g_bridge.cpu_ticks    = 0;
    g_bridge.timed_frames = 0;
    g_bridge.span_start   = exit;
}

// True only when skipping the game's own DLSS cannot leave a stale frame on
// screen: the bridge must be healthy and already have produced a valid output.
static bool BridgeWillDeliver()
{
    return g_cfg.skip_game != 0 && g_cfg.mode == 2 && g_cfg.stage >= 3 &&
           !g_bridge.disabled && g_bridge.session_ready && g_bridge.frame_ready &&
           g_bridge.frames_done > 0;
}

static void CfgPath(char *out)
{
    GetModuleFileNameA(g_self, out, MAX_PATH);
    if (char *s = strrchr(out, '\\'))
        strcpy_s(s + 1, MAX_PATH - (s + 1 - out), "dlss5-dx11-bridge.cfg");
}

static void CfgReload()
{
    char path[MAX_PATH];
    CfgPath(path);

    FILE *f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || f == nullptr) return;

    BridgeCfg next = g_cfg;
    char line[128];
    while (fgets(line, sizeof(line), f) != nullptr)
    {
        char key[64];
        int  val = 0;
        if (sscanf_s(line, "%63[^=]=%d", key, static_cast<unsigned>(sizeof(key)), &val) != 2) continue;
        if      (_stricmp(key, "mode")        == 0) next.mode        = val;
        else if (_stricmp(key, "flags")       == 0) next.flags       = val;
        else if (_stricmp(key, "subrects")    == 0) next.subrects    = val;
        else if (_stricmp(key, "reset_every") == 0) next.reset_every = val;
        else if (_stricmp(key, "pixels")      == 0) next.pixels      = val;
        else if (_stricmp(key, "stage")       == 0) next.stage       = val;
        else if (_stricmp(key, "skip_game")   == 0) next.skip_game   = val;
    }
    fclose(f);
    if (next.stage < 0 || next.stage > 3) next.stage = g_cfg.stage;

    if (next.mode < 0 || next.mode > 2) next.mode = g_cfg.mode;

    const bool rebuild = (next.flags != g_cfg.flags) || (next.subrects != g_cfg.subrects);
    const bool changed = rebuild || (next.mode != g_cfg.mode) ||
                         (next.reset_every != g_cfg.reset_every) || (next.pixels != g_cfg.pixels) ||
                         (next.stage != g_cfg.stage) || (next.skip_game != g_cfg.skip_game);
    if (!changed) return;

    g_cfg = next;
    Log("[bridge] config: stage=%d mode=%d flags=%d subrects=%d reset_every=%d pixels=%d skip_game=%d",
        g_cfg.stage, g_cfg.mode, g_cfg.flags, g_cfg.subrects, g_cfg.reset_every, g_cfg.pixels,
        g_cfg.skip_game);

    // Creation-time values only take effect on a new feature, so invalidate the
    // current one and let the next frame build it again.
    if (rebuild) { Log("[bridge] rebuilding feature for new creation flags"); g_bridge.frame_ready = false; }
}

static void CfgWriteDefault()
{
    char path[MAX_PATH];
    CfgPath(path);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return;

    FILE *f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || f == nullptr) return;
    fprintf(f, "stage=%d\nmode=%d\nskip_game=%d\nflags=%d\nsubrects=%d\nreset_every=%d\npixels=%d\n",
            g_cfg.stage, g_cfg.mode, g_cfg.skip_game, g_cfg.flags, g_cfg.subrects,
            g_cfg.reset_every, g_cfg.pixels);
    fclose(f);
    Log("[bridge] wrote default config to %s", path);
}

// Reads one pixel back to the CPU. Blocking and slow, so only used on the first
// few frames, but it settles what the buffers actually contain instead of
// guessing from what the screen looks like.
static void LogCenterPixel(ID3D11DeviceContext *ctx, ID3D11Device *dev11,
                           const char *label, ID3D11Texture2D *tex, DXGI_FORMAT fmt)
{
    if (tex == nullptr) return;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = 1;
    td.Height           = 1;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = fmt;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_STAGING;
    td.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D *staging = nullptr;
    if (FAILED(dev11->CreateTexture2D(&td, nullptr, &staging)) || staging == nullptr) return;

    const UINT cx = g_bridge.width / 2, cy = g_bridge.height / 2;
    D3D11_BOX box = { cx, cy, 0, cx + 1, cy + 1, 1 };
    ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, &box);

    D3D11_MAPPED_SUBRESOURCE ms = {};
    if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &ms)) && ms.pData != nullptr)
    {
        Log("[bridge]   %-8s centre pixel raw = 0x%08X", label,
            *static_cast<const UINT *>(ms.pData));
        ctx->Unmap(staging, 0);
    }
    staging->Release();
}

// Every path that gives up goes through here, so the user is told once, in the
// overlay, that the bridge stopped -- otherwise there is simply no effect on
// screen and nothing to explain it without opening a file.
static void BridgeDisable(const char *why)
{
    if (g_bridge.disabled) return;
    g_bridge.disabled = true;
    Warn("stopped: %s. The game renders normally. See dlss5-dx11-bridge.log "
         "in the game folder for the detail.", why);
}

static void BridgeFail(const char *what)
{
    if (++g_bridge.consecutive_fails >= 3)
    {
        BridgeDisable("the NGX session could not be opened");
        Log("[bridge] disabled after repeated failures (%s). Game rendering is untouched.", what);
    }
}

static bool   BeginCommands();
static UINT64 EndCommands();

typedef HRESULT (WINAPI *PFN_D3DCompile_)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
                                          ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT,
                                          ID3DBlob **, ID3DBlob **);

static bool BridgeMakeDepthShader(ID3D11Device *dev11)
{
    if (g_bridge.depth_cs != nullptr) return true;

    static const char kSrc[] =
        "Texture2D<float>   src : register(t0);\n"
        "RWTexture2D<float> dst : register(u0);\n"
        "[numthreads(8,8,1)]\n"
        "void main(uint3 id : SV_DispatchThreadID)\n"
        "{\n"
        "    uint w, h; dst.GetDimensions(w, h);\n"
        "    if (id.x < w && id.y < h) dst[id.xy] = src[id.xy];\n"
        "}\n";

    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = m != nullptr
        ? reinterpret_cast<PFN_D3DCompile_>(GetProcAddress(m, "D3DCompile")) : nullptr;
    if (compile == nullptr) { Log("[bridge] d3dcompiler_47.dll unavailable"); return false; }

    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = compile(kSrc, sizeof(kSrc) - 1, "depthblit", nullptr, nullptr,
                         "main", "cs_5_0", 0, 0, &code, &err);
    if (FAILED(hr) || code == nullptr)
    {
        Log("[bridge] depth shader compile failed 0x%08X: %s", hr,
            err != nullptr ? static_cast<const char *>(err->GetBufferPointer()) : "");
        if (err != nullptr) err->Release();
        return false;
    }
    if (err != nullptr) err->Release();

    hr = dev11->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                    &g_bridge.depth_cs);
    code->Release();
    if (FAILED(hr)) { Log("[bridge] CreateComputeShader failed 0x%08X", hr); return false; }

    Log("[bridge] depth conversion shader ready (cs_5_0)");
    return true;
}

// Converts the game's R24G8_TYPELESS depth into the shared R32_FLOAT texture.
// This runs on the immediate context in the middle of the game's frame, so the
// compute stage bindings it touches are saved and put back afterwards.
static bool BridgeBlitDepth(ID3D11DeviceContext *ctx, ID3D11Device *dev11, ID3D11Texture2D *src)
{
    if (g_bridge.depth_cs == nullptr || g_bridge.depth_uav == nullptr) return false;

    if (g_bridge.depth_src != src)
    {
        if (g_bridge.depth_srv != nullptr) { g_bridge.depth_srv->Release(); g_bridge.depth_srv = nullptr; }
        if (g_bridge.depth_src != nullptr) { g_bridge.depth_src->Release(); g_bridge.depth_src = nullptr; }

        // A depth buffer is typeless, and which view format is legal depends on
        // which typeless family it belongs to. Hardcoding one only ever works
        // for the game it was written against.
        D3D11_TEXTURE2D_DESC sdesc = {};
        src->GetDesc(&sdesc);

        DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
        switch (sdesc.Format)
        {
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:      view = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:              view = DXGI_FORMAT_R32_FLOAT; break;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:   view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:              view = DXGI_FORMAT_R16_UNORM; break;
        default:                                 view = sdesc.Format; break;  // already typed
        }

        // The numbers go alongside the names: a format this build has no name
        // for is exactly the one a report needs to identify.
        Log("[bridge] game depth is %s (%u), reading it as %s (%u)",
            FormatName(sdesc.Format), sdesc.Format, FormatName(view), view);

        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format              = view;
        sd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        HRESULT hr = dev11->CreateShaderResourceView(src, &sd, &g_bridge.depth_srv);
        if (FAILED(hr))
        {
            // Repeating this every frame produces hundreds of megabytes of log
            // and hides everything else, so say it once and stop trying.
            Log("[bridge] cannot read this game's depth buffer (format %u); depth will not "
                "reach DLSS. Please report this format.", sdesc.Format);
            Log("  Depth will not reach DLSS. Image quality will suffer, especially");
            Log("  around moving objects. Please report this format.");
            g_bridge.depth_converted = false;
            return false;
        }
        Log("[bridge] depth view: resource format %u read as %u", sdesc.Format, view);

        g_bridge.depth_src = src;
        src->AddRef();  // keep the pointer from being recycled under the cache
    }

    ID3D11ComputeShader       *old_cs  = nullptr;
    ID3D11ShaderResourceView  *old_srv = nullptr;
    ID3D11UnorderedAccessView *old_uav = nullptr;
    ctx->CSGetShader(&old_cs, nullptr, nullptr);
    ctx->CSGetShaderResources(0, 1, &old_srv);
    ctx->CSGetUnorderedAccessViews(0, 1, &old_uav);

    UINT keep = static_cast<UINT>(-1);
    ctx->CSSetShader(g_bridge.depth_cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &g_bridge.depth_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &g_bridge.depth_uav, &keep);
    ctx->Dispatch((g_bridge.width + 7) / 8, (g_bridge.height + 7) / 8, 1);

    // Unbind before restoring, so the shared texture is not left bound as a UAV
    // while the game's own views go back on the same slots.
    ID3D11ShaderResourceView  *no_srv = nullptr;
    ID3D11UnorderedAccessView *no_uav = nullptr;
    ctx->CSSetShaderResources(0, 1, &no_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &no_uav, &keep);

    ctx->CSSetShader(old_cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &old_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &old_uav, &keep);
    if (old_cs  != nullptr) old_cs->Release();
    if (old_srv != nullptr) old_srv->Release();
    if (old_uav != nullptr) old_uav->Release();
    return true;
}

// Converts the game's motion vectors into the shared R16G16_FLOAT texture, used
// when the driver refuses to share the game's own MV format. Same shape as the
// depth conversion: save the compute bindings, dispatch, put everything back.
static bool BridgeMakeMVShader(ID3D11Device *dev11)
{
    if (g_bridge.mv_cs != nullptr) return true;

    static const char kSrc[] =
        "Texture2D<float2>   src : register(t0);\n"
        "RWTexture2D<float2> dst : register(u0);\n"
        "[numthreads(8,8,1)]\n"
        "void main(uint3 id : SV_DispatchThreadID)\n"
        "{\n"
        "    uint w, h; dst.GetDimensions(w, h);\n"
        "    if (id.x < w && id.y < h) dst[id.xy] = src[id.xy];\n"
        "}\n";

    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = m != nullptr
        ? reinterpret_cast<PFN_D3DCompile_>(GetProcAddress(m, "D3DCompile")) : nullptr;
    if (compile == nullptr) { Log("[bridge] d3dcompiler_47.dll unavailable"); return false; }

    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = compile(kSrc, sizeof(kSrc) - 1, "mvblit", nullptr, nullptr,
                         "main", "cs_5_0", 0, 0, &code, &err);
    if (FAILED(hr) || code == nullptr)
    {
        Log("[bridge] MV shader compile failed 0x%08X: %s", hr,
            err != nullptr ? static_cast<const char *>(err->GetBufferPointer()) : "");
        if (err != nullptr) err->Release();
        return false;
    }
    if (err != nullptr) err->Release();

    hr = dev11->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                    &g_bridge.mv_cs);
    code->Release();
    if (FAILED(hr)) { Log("[bridge] MV CreateComputeShader failed 0x%08X", hr); return false; }

    Log("[bridge] MV conversion shader ready (cs_5_0)");
    return true;
}

static bool BridgeBlitMV(ID3D11DeviceContext *ctx, ID3D11Device *dev11, ID3D11Texture2D *src)
{
    if (g_bridge.mv_cs == nullptr || g_bridge.mv_uav == nullptr) return false;

    if (g_bridge.mv_src != src)
    {
        if (g_bridge.mv_srv != nullptr) { g_bridge.mv_srv->Release(); g_bridge.mv_srv = nullptr; }
        if (g_bridge.mv_src != nullptr) { g_bridge.mv_src->Release(); g_bridge.mv_src = nullptr; }

        // A null descriptor takes the resource's own format, which is invalid
        // for a typeless resource, and this branch is reachable with one: a game
        // supplying R32G32_TYPELESS motion vectors resolves through TypedFormat
        // to R32G32_FLOAT, fails to share, and enters the fallback. Name the
        // view format the same way BridgeBlitDepth names its depth view.
        D3D11_TEXTURE2D_DESC sdesc = {};
        src->GetDesc(&sdesc);

        DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
        switch (sdesc.Format)
        {
        case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R32G32_FLOAT:           view = DXGI_FORMAT_R32G32_FLOAT; break;
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R16G16_FLOAT:           view = DXGI_FORMAT_R16G16_FLOAT; break;
        case DXGI_FORMAT_R16G16_UNORM:           view = DXGI_FORMAT_R16G16_UNORM; break;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:      view = DXGI_FORMAT_R10G10B10A2_UNORM; break;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:     view = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
        default:                                 view = sdesc.Format; break;  // already typed
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format              = view;
        sd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        HRESULT hr = dev11->CreateShaderResourceView(src, &sd, &g_bridge.mv_srv);
        if (FAILED(hr))
        {
            // Like the depth case, say it once. Continuing would mean DLSS reads
            // a motion-vector texture nobody ever writes, and the image smears.
            //
            // Unlike depth, this stops the bridge rather than degrading to a
            // placeholder: depth's fallback quietly drops one input, but an MV
            // texture nobody writes smears every frame, and zero is not a
            // neutral motion value. Stopping returns rendering to the game's
            // own working DLSS instead. Deliberate divergence from BridgeBlitDepth.
            Log("[bridge] cannot read this game's motion vectors (format %u); the "
                "bridge is stopping rather than feed it stale ones. Please report "
                "this format.", sdesc.Format);
            BridgeDisable("the game's MV format cannot be read");
            return false;
        }
        Log("[bridge] MV view: resource format %u read as %u", sdesc.Format, view);

        g_bridge.mv_src = src;
        src->AddRef();
    }

    ID3D11ComputeShader       *old_cs  = nullptr;
    ID3D11ShaderResourceView  *old_srv = nullptr;
    ID3D11UnorderedAccessView *old_uav = nullptr;
    ctx->CSGetShader(&old_cs, nullptr, nullptr);
    ctx->CSGetShaderResources(0, 1, &old_srv);
    ctx->CSGetUnorderedAccessViews(0, 1, &old_uav);

    UINT keep = static_cast<UINT>(-1);
    ctx->CSSetShader(g_bridge.mv_cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &g_bridge.mv_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &g_bridge.mv_uav, &keep);
    ctx->Dispatch((g_bridge.width + 7) / 8, (g_bridge.height + 7) / 8, 1);

    ID3D11ShaderResourceView  *no_srv = nullptr;
    ID3D11UnorderedAccessView *no_uav = nullptr;
    ctx->CSSetShaderResources(0, 1, &no_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &no_uav, &keep);

    ctx->CSSetShader(old_cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &old_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &old_uav, &keep);
    if (old_cs  != nullptr) old_cs->Release();
    if (old_srv != nullptr) old_srv->Release();
    if (old_uav != nullptr) old_uav->Release();
    return true;
}

// D3D12 leaves resource lifetime to the caller: releasing a texture or an NGX
// feature while a submitted command list still references it corrupts driver
// state and surfaces later as an access violation inside NGX. Every teardown
// therefore drains the queue first. Rebuilds are rare, so the stall is free.
static void BridgeDrainGpu()
{
    if (g_bridge.queue == nullptr || g_bridge.fence12 == nullptr) return;

    const UINT64 v = ++g_bridge.fence_value;
    g_bridge.queue->Signal(g_bridge.fence12, v);
    if (g_bridge.fence12->GetCompletedValue() < v && g_bridge.fence_event != nullptr)
    {
        g_bridge.fence12->SetEventOnCompletion(v, g_bridge.fence_event);
        if (WaitForSingleObject(g_bridge.fence_event, 5000) != WAIT_OBJECT_0)
            Log("[bridge] timed out draining the queue before teardown");
    }
    // Every allocator is now retired, so none of them needs waiting on again.
    for (int i = 0; i < Bridge::kFrames; ++i) g_bridge.alloc_fence[i] = 0;
}

static void BridgeReleaseTextures()
{
    BridgeDrainGpu();

    if (g_bridge.depth_uav != nullptr) { g_bridge.depth_uav->Release(); g_bridge.depth_uav = nullptr; }
    if (g_bridge.depth_srv != nullptr) { g_bridge.depth_srv->Release(); g_bridge.depth_srv = nullptr; }
    if (g_bridge.depth_src != nullptr) { g_bridge.depth_src->Release(); g_bridge.depth_src = nullptr; }
    if (g_bridge.mv_uav    != nullptr) { g_bridge.mv_uav->Release();    g_bridge.mv_uav    = nullptr; }
    if (g_bridge.mv_srv    != nullptr) { g_bridge.mv_srv->Release();    g_bridge.mv_srv    = nullptr; }
    if (g_bridge.mv_src    != nullptr) { g_bridge.mv_src->Release();    g_bridge.mv_src    = nullptr; }
    for (int i = 0; i < SLOT_COUNT; ++i)
    {
        if (g_bridge.tex11[i]  != nullptr) { g_bridge.tex11[i]->Release();  g_bridge.tex11[i]  = nullptr; }
        if (g_bridge.tex12[i]  != nullptr) { g_bridge.tex12[i]->Release();  g_bridge.tex12[i]  = nullptr; }
        if (g_bridge.shared[i] != nullptr) { CloseHandle(g_bridge.shared[i]); g_bridge.shared[i] = nullptr; }
    }
    if (g_bridge.feature != nullptr && g_bridge.release_feature != nullptr)
    {
        g_bridge.release_feature(g_bridge.feature);
        g_bridge.feature = nullptr;
    }
    g_bridge.frame_ready = false;
}

// Reads one texture slot out of the game's parameter block and returns its
// description, so the bridge can tell when BG3 has recreated its render targets.
static ID3D11Texture2D *GameTexture(const NVSDK_NGX_Parameter *p, const char *key,
                                    D3D11_TEXTURE2D_DESC *desc)
{
    ID3D11Resource *res = nullptr;
    if (p->Get(key, &res) != NGX_SUCCESS || res == nullptr) return nullptr;
    ID3D11Texture2D *tex = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex))) ||
        tex == nullptr)
        return nullptr;
    if (desc != nullptr) tex->GetDesc(desc);
    return tex;  // caller releases
}

static bool BridgeInitSession(ID3D11Device *dev11, ID3D11DeviceContext *ctx)
{
    Log("");
    Breadcrumb("opening the D3D12 session");
    Log("################ bridge: opening D3D12 session ################");
    CfgReload();
    Log("[bridge] config: mode=%d flags=%d subrects=%d reset_every=%d pixels=%d",
        g_cfg.mode, g_cfg.flags, g_cfg.subrects, g_cfg.reset_every, g_cfg.pixels);

    IDXGIDevice  *dxgi_dev = nullptr;
    IDXGIAdapter *adapter  = nullptr;
    if (SUCCEEDED(dev11->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi_dev))) &&
        dxgi_dev != nullptr)
    {
        dxgi_dev->GetAdapter(&adapter);
        dxgi_dev->Release();
    }

    // Which GPU and which driver: NGX feature availability depends on both, and
    // a report without them cannot be told apart from a bug in here.
    if (adapter != nullptr)
    {
        DXGI_ADAPTER_DESC ad = {};
        adapter->GetDesc(&ad);
        Log("[bridge] adapter: %ls  vram=%llu MB", ad.Description,
            static_cast<unsigned long long>(ad.DedicatedVideoMemory / (1024 * 1024)));

        LARGE_INTEGER umd = {};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
            Log("[bridge] driver: %u.%u.%u.%u",
                static_cast<unsigned>((umd.HighPart >> 16) & 0xFFFF),
                static_cast<unsigned>(umd.HighPart & 0xFFFF),
                static_cast<unsigned>((umd.LowPart >> 16) & 0xFFFF),
                static_cast<unsigned>(umd.LowPart & 0xFFFF));
    }

    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    auto create_device = d3d12 != nullptr
        ? reinterpret_cast<PFN_D3D12CreateDevice_>(GetProcAddress(d3d12, "D3D12CreateDevice"))
        : nullptr;
    if (create_device == nullptr) { Log("[bridge] no D3D12CreateDevice"); goto fail; }

    {
        HRESULT hr = create_device(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                   reinterpret_cast<void **>(&g_bridge.dev12));
        if (FAILED(hr) || g_bridge.dev12 == nullptr)
        { Log("[bridge] D3D12CreateDevice failed 0x%08X", hr); goto fail; }

        HMODULE ngx = FindNgxLoader();
        if (ngx == nullptr) { Log("[bridge] NGX D3D12 exports not present"); goto fail; }

        auto init_ext = reinterpret_cast<PFN_Init_Ext>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init_Ext"));
        g_bridge.alloc_params = reinterpret_cast<PFN_AllocateParameters>(
            GetProcAddress(ngx, "NVSDK_NGX_D3D12_AllocateParameters"));
        g_bridge.create_feature = reinterpret_cast<PFN_D3D12CreateFeature>(
            GetProcAddress(ngx, "NVSDK_NGX_D3D12_CreateFeature"));
        g_bridge.eval_feature = reinterpret_cast<PFN_D3D12EvaluateFeature>(
            GetProcAddress(ngx, "NVSDK_NGX_D3D12_EvaluateFeature"));
        g_bridge.release_feature = reinterpret_cast<PFN_D3D12ReleaseFeature>(
            GetProcAddress(ngx, "NVSDK_NGX_D3D12_ReleaseFeature"));

        LogEntryBytes("NVSDK_NGX_D3D12_CreateFeature",
                      reinterpret_cast<void *>(g_bridge.create_feature));
        LogEntryBytes("NVSDK_NGX_D3D12_EvaluateFeature",
                      reinterpret_cast<void *>(g_bridge.eval_feature));

        if (init_ext == nullptr || g_bridge.alloc_params == nullptr ||
            g_bridge.create_feature == nullptr || g_bridge.eval_feature == nullptr)
        { Log("[bridge] required NGX exports missing"); goto fail; }

        wchar_t data_path[MAX_PATH] = {};
        GetModuleFileNameW(g_self, data_path, MAX_PATH);
        if (wchar_t *s = wcsrchr(data_path, L'\\')) *(s + 1) = L'\0';

        // The SDK version constant is not documented and has moved between NGX
        // releases, so it is negotiated rather than assumed. Hardcoding the one
        // value that worked on the machine this was written on made the whole
        // session fail on a different driver. Init_ProjectID is tried as well:
        // some drivers accept one entry point and fault on the other.
        auto init_project = reinterpret_cast<PFN_Init_ProjectID>(
            GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init_ProjectID"));

        Breadcrumb("initialising NGX on D3D12");
        DWORD code    = 0;
        bool  inited  = false;
        bool  faulted = false;

        // "Refused" and "faulted" are not the same answer and must not be
        // treated the same. A refusal is NGX declining a version constant, and
        // trying the next one is the whole point of this loop. A fault means NGX
        // died inside itself, and everything it owns is now in an unknown state
        // -- calling back in is what turned a disabled add-on into a killed
        // process on Final Fantasy VII Remake. Stop at the first one.
        for (int ver = 0x13; ver <= 0x16 && !inited && !faulted; ++ver)
        {
            if (init_ext != nullptr)
            {
                NVSDK_NGX_Result r = SafeInitExt(init_ext, 0x1000000ULL, data_path,
                                                 g_bridge.dev12, ver, &code);
                if (code != 0)
                {
                    Log("[bridge] Init_Ext(version=0x%02X) raised 0x%08X", ver, code);
                    faulted = true;
                }
                else
                {
                    Log("[bridge] Init_Ext(version=0x%02X) -> %s", ver,
                        r == NGX_SUCCESS ? "ok" : "refused");
                    if (r == NGX_SUCCESS) inited = true;
                }
            }
            if (!inited && !faulted && init_project != nullptr)
            {
                NVSDK_NGX_Result r = SafeInitProjectID(init_project,
                                                       "a0f57b54-1daf-4934-90ae-c4035c19df04",
                                                       data_path, g_bridge.dev12, ver, &code);
                if (code != 0)
                {
                    Log("[bridge] Init_ProjectID(version=0x%02X) raised 0x%08X", ver, code);
                    faulted = true;
                }
                else
                {
                    Log("[bridge] Init_ProjectID(version=0x%02X) -> %s", ver,
                        r == NGX_SUCCESS ? "ok" : "refused");
                    if (r == NGX_SUCCESS) inited = true;
                }
            }
        }
        if (faulted)
        {
            Log("[bridge] NGX raised an exception opening its D3D12 session, so the");
            Log("  bridge stops here rather than calling into it again. The game keeps");
            Log("  rendering on its own; nothing else in this add-on runs this session.");
            goto fail;
        }
        if (!inited)
        { Log("[bridge] no NGX D3D12 init entry point was accepted by this driver"); goto fail; }
        Log("[bridge] NGX D3D12 session initialised");

        // What this GPU and driver will actually agree to run. Without it a
        // report of "nothing happens" cannot be told apart from the feature
        // simply not being available on the hardware.
        if (auto get_caps = reinterpret_cast<PFN_GetCapabilityParameters>(
                GetProcAddress(ngx, "NVSDK_NGX_D3D12_GetCapabilityParameters")))
        {
            NVSDK_NGX_Parameter *caps = nullptr;
            NVSDK_NGX_Result rc = SafeGetCaps(get_caps, &caps, &code);
            if (code != 0)              Log("[bridge] capability query raised 0x%08X", code);
            else if (rc != NGX_SUCCESS) Log("[bridge] capability query failed 0x%08X", rc);
            else if (caps != nullptr)   { Log("[bridge] NGX capabilities:"); DumpCapability(caps); }
        }

        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        g_bridge.dev12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue),
                                           reinterpret_cast<void **>(&g_bridge.queue));
        for (int i = 0; i < Bridge::kFrames; ++i)
            g_bridge.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   __uuidof(ID3D12CommandAllocator),
                                                   reinterpret_cast<void **>(&g_bridge.alloc[i]));
        if (g_bridge.alloc[0] != nullptr)
            g_bridge.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_bridge.alloc[0],
                                              nullptr, __uuidof(ID3D12GraphicsCommandList),
                                              reinterpret_cast<void **>(&g_bridge.list));
        if (g_bridge.list != nullptr) g_bridge.list->Close();
        g_bridge.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        // One fence, visible to both APIs, is what orders the two queues.
        HANDLE fh = nullptr;
        hr = g_bridge.dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence),
                                         reinterpret_cast<void **>(&g_bridge.fence12));
        if (SUCCEEDED(hr))
            hr = g_bridge.dev12->CreateSharedHandle(g_bridge.fence12, nullptr, GENERIC_ALL,
                                                    nullptr, &fh);
        ID3D11Device5 *dev5 = nullptr;
        if (SUCCEEDED(hr) &&
            SUCCEEDED(dev11->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void **>(&dev5))) &&
            dev5 != nullptr)
        {
            hr = dev5->OpenSharedFence(fh, __uuidof(ID3D11Fence),
                                       reinterpret_cast<void **>(&g_bridge.fence11));
            dev5->Release();
        }
        if (fh != nullptr) CloseHandle(fh);
        if (FAILED(hr) || g_bridge.fence11 == nullptr)
        { Log("[bridge] shared fence setup failed 0x%08X", hr); goto fail; }

        if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext4),
                                       reinterpret_cast<void **>(&g_bridge.ctx4))) ||
            g_bridge.ctx4 == nullptr)
        { Log("[bridge] ID3D11DeviceContext4 unavailable"); goto fail; }

        Log("[bridge] context: %s",
            ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE ? "immediate" : "DEFERRED");

        if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11Multithread),
                                          reinterpret_cast<void **>(&g_bridge.mt))) &&
            g_bridge.mt != nullptr)
            Log("[bridge] multithread protection: %s",
                g_bridge.mt->GetMultithreadProtected() ? "on (locking per frame)" : "off");

        NVSDK_NGX_Result rp = SafeAllocParams(g_bridge.alloc_params, &g_bridge.params, &code);
        if (code != 0 || rp != NGX_SUCCESS || g_bridge.params == nullptr)
        { Log("[bridge] AllocateParameters failed 0x%08X", rp); goto fail; }

        Log("[bridge] session ready: queue=%p list=%p fence12=%p fence11=%p (%d frames in flight)",
            static_cast<void *>(g_bridge.queue), static_cast<void *>(g_bridge.list),
            static_cast<void *>(g_bridge.fence12), static_cast<void *>(g_bridge.fence11),
            Bridge::kFrames);
        Log("############# bridge: session open #############");
        Log("");
        if (adapter != nullptr) adapter->Release();
        g_bridge.session_ready = true;
        return true;
    }

fail:
    if (adapter != nullptr) adapter->Release();
    BridgeDisable("the D3D12 session failed to start");
    Log("[bridge] disabled. Game rendering is untouched.");
    Log("############# bridge: session failed #############");
    return false;
}

// Creates the four shared textures on the D3D12 side and opens D3D11 aliases for
// them. The game's own textures have MiscFlags = 0 and cannot be shared, so the
// bridge owns its copies and moves data with CopyResource.
// What the D3D12 side actually received. A texture created on D3D11 and opened
// on D3D12 keeps its own idea of format and flags, and DLSS writes the output
// through a UAV: if ALLOW_UNORDERED_ACCESS did not survive the crossing, that
// write is undefined and the result accumulates as garbage rather than image.
static void LogD3D12Desc(int i)
{
    if (g_bridge.tex12[i] == nullptr) return;
    D3D12_RESOURCE_DESC rd = g_bridge.tex12[i]->GetDesc();
    Log("[bridge]   d3d12 desc: %llux%u fmt=%u flags=0x%X%s%s",
        rd.Width, rd.Height, rd.Format, rd.Flags,
        (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ? " UAV" : "",
        (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) ? " SIMULTANEOUS" : "");
    if (i == SLOT_OUTPUT && !(rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
        Log("[bridge]   *** Output has no UAV flag on the D3D12 side: DLSS cannot write it ***");
}

// One texture visible to both APIs. Two ways exist to get one and neither is
// guaranteed on a given driver, so both are attempted and the log records which
// one this machine accepts.
//
//   A. create on D3D12 (HEAP_FLAG_SHARED + ALLOW_SIMULTANEOUS_ACCESS, which D3D11
//      needs because it cannot perform D3D12 state transitions), open on D3D11.
//   B. create on D3D11 (SHARED_NTHANDLE | SHARED), open on D3D12.
static bool MakeSharedPair(ID3D11Device *dev11, ID3D11Device1 *dev1, int i,
                           UINT w, UINT h, DXGI_FORMAT fmt, bool uav)
{
    // --- attempt A -------------------------------------------------------
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = w;
    rd.Height           = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
                          (uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                               : D3D12_RESOURCE_FLAG_NONE);

    HRESULT hr = g_bridge.dev12->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void **>(&g_bridge.tex12[i]));
    if (SUCCEEDED(hr))
        hr = g_bridge.dev12->CreateSharedHandle(g_bridge.tex12[i], nullptr, GENERIC_ALL,
                                                nullptr, &g_bridge.shared[i]);
    if (SUCCEEDED(hr))
        hr = dev1->OpenSharedResource1(g_bridge.shared[i], __uuidof(ID3D11Texture2D),
                                       reinterpret_cast<void **>(&g_bridge.tex11[i]));
    if (SUCCEEDED(hr))
    {
        Log("[bridge] %-6s %ux%u fmt=%u  via D3D12->D3D11  d3d12=%p d3d11=%p",
            kSlotName[i], w, h, fmt, static_cast<void *>(g_bridge.tex12[i]),
            static_cast<void *>(g_bridge.tex11[i]));
        return true;
    }
    Log("[bridge] %s: D3D12->D3D11 path failed 0x%08X, trying the other direction",
        kSlotName[i], hr);

    if (g_bridge.tex11[i]  != nullptr) { g_bridge.tex11[i]->Release();  g_bridge.tex11[i]  = nullptr; }
    if (g_bridge.tex12[i]  != nullptr) { g_bridge.tex12[i]->Release();  g_bridge.tex12[i]  = nullptr; }
    if (g_bridge.shared[i] != nullptr) { CloseHandle(g_bridge.shared[i]); g_bridge.shared[i] = nullptr; }

    // --- attempt B -------------------------------------------------------
    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = w;
    td.Height           = h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = fmt;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE |
                          (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
    td.MiscFlags        = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

    hr = dev11->CreateTexture2D(&td, nullptr, &g_bridge.tex11[i]);
    if (FAILED(hr))
    { Log("[bridge] %s: CreateTexture2D failed 0x%08X", kSlotName[i], hr); return false; }

    IDXGIResource1 *dxgi_res = nullptr;
    hr = g_bridge.tex11[i]->QueryInterface(__uuidof(IDXGIResource1),
                                           reinterpret_cast<void **>(&dxgi_res));
    if (SUCCEEDED(hr))
    {
        hr = dxgi_res->CreateSharedHandle(nullptr,
                                          DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                          nullptr, &g_bridge.shared[i]);
        dxgi_res->Release();
    }
    if (SUCCEEDED(hr))
        hr = g_bridge.dev12->OpenSharedHandle(g_bridge.shared[i], __uuidof(ID3D12Resource),
                                              reinterpret_cast<void **>(&g_bridge.tex12[i]));
    if (FAILED(hr))
    { Log("[bridge] %s: D3D11->D3D12 path failed 0x%08X", kSlotName[i], hr); return false; }

    Log("[bridge] %-6s %ux%u fmt=%u  via D3D11->D3D12  d3d12=%p d3d11=%p",
        kSlotName[i], w, h, fmt, static_cast<void *>(g_bridge.tex12[i]),
        static_cast<void *>(g_bridge.tex11[i]));
    LogD3D12Desc(i);
    return true;
}

// Sizes and the NGX contract are taken from g_bridge, which BridgeFrame fills in
// from the game's own parameter block. Nothing here is derived or assumed: in an
// upscaling mode the Output texture is larger than the inputs, and the rendered
// area is smaller than the texture that holds it.
static void CopyUIntParam(const NVSDK_NGX_Parameter *src, const char *key, unsigned int fallback);

static bool BridgeBuildResources(ID3D11Device *dev11, const DXGI_FORMAT *fmt,
                                 const NVSDK_NGX_Parameter *gp)
{
    Breadcrumb("building shared textures");
    const UINT w = g_bridge.width, h = g_bridge.height;
    BridgeReleaseTextures();

    ID3D11Device1 *dev1 = nullptr;
    if (FAILED(dev11->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void **>(&dev1))) ||
        dev1 == nullptr)
    { Log("[bridge] ID3D11Device1 unavailable"); return false; }

    bool ok = true;
    g_bridge.depth_placeholder = false;
    g_bridge.depth_converted   = false;
    g_bridge.mv_converted      = false;
    for (int i = 0; i < SLOT_COUNT && ok; ++i)
    {
        // Depth crosses as R32_FLOAT, written by a compute pass, because its
        // native R24G8_TYPELESS cannot be shared. It therefore needs a UAV too.
        const bool        is_depth   = (i == SLOT_DEPTH);
        DXGI_FORMAT       share_fmt  = is_depth ? DXGI_FORMAT_R32_FLOAT
                                                : TypedFormat(fmt[i]);
        if (!is_depth && share_fmt != fmt[i])
            Log("[bridge] %s is %s; the shared copy uses %s, because nothing can view "
                "a typeless texture", kSlotKey[i], FormatName(fmt[i]), FormatName(share_fmt));
        // MV gets a UAV purely so it can be cleared for the isolation test.
        const bool        needs_uav  = is_depth || (i == SLOT_OUTPUT) || (i == SLOT_MV);
        const UINT        tw = (i == SLOT_OUTPUT) ? g_bridge.out_width  : w;
        const UINT        th = (i == SLOT_OUTPUT) ? g_bridge.out_height : h;

        bool made = MakeSharedPair(dev11, dev1, i, tw, th, share_fmt, needs_uav);

        // The game's own MV format can be one the driver will not share --
        // R32G32_FLOAT is absent from the kernel's shared-format list, and
        // CreateTexture2D then rejects it in every flag combination. R16G16_FLOAT,
        // the DLSS-recommended motion-vector format, is in every list, so retry
        // with that and convert per frame, the way depth is converted. TypedFormat
        // has already had its pass by here, so a typeless game MV that shares fine
        // never reaches this; a retried mirror is the only one that needs converting.
        bool mv_fallback = false;
        if (!made && i == SLOT_MV)
        {
            share_fmt = DXGI_FORMAT_R16G16_FLOAT;
            Log("[bridge] MV: format %u cannot be shared on this driver; retrying as R16G16_FLOAT",
                fmt[i]);
            made = MakeSharedPair(dev11, dev1, i, tw, th, share_fmt, needs_uav);
            mv_fallback = true;
        }

        if (made)
        {
            if (i == SLOT_MV)
            {
                // A retried mirror is R16G16_FLOAT while the game's own texture is
                // not, so CopyResource cannot fill it: the conversion pass does.
                if (mv_fallback)
                {
                    // The conversion pass writes the shared MV through a UAV.
                    D3D11_UNORDERED_ACCESS_VIEW_DESC mu = {};
                    mu.Format        = share_fmt;
                    mu.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
                    if (SUCCEEDED(dev11->CreateUnorderedAccessView(g_bridge.tex11[i], &mu, &g_bridge.mv_uav)) &&
                        g_bridge.mv_uav != nullptr &&
                        BridgeMakeMVShader(dev11))
                    {
                        g_bridge.mv_converted = true;
                        Log("[bridge] MV will be converted fmt=%u -> R16G16_FLOAT by a compute pass",
                            fmt[i]);
                        continue;
                    }
                    // Without the conversion pass there is no way to fill the
                    // shared MV, and DLSS reading a texture nobody writes smears
                    // the image. Stop cleanly rather than deliver that.
                    Breadcrumb("setting up the MV conversion pass");
                    Log("[bridge] MV conversion setup failed; this game's MV format %u cannot be bridged",
                        fmt[i]);
                    if (g_bridge.mv_uav    != nullptr) { g_bridge.mv_uav->Release();    g_bridge.mv_uav    = nullptr; }
                    if (g_bridge.tex11[i]  != nullptr) { g_bridge.tex11[i]->Release();  g_bridge.tex11[i]  = nullptr; }
                    if (g_bridge.tex12[i]  != nullptr) { g_bridge.tex12[i]->Release();  g_bridge.tex12[i]  = nullptr; }
                    if (g_bridge.shared[i] != nullptr) { CloseHandle(g_bridge.shared[i]); g_bridge.shared[i] = nullptr; }
                    ok = false;
                }
                else
                    continue;
            }
            if (!is_depth) continue;

            D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
            ud.Format        = DXGI_FORMAT_R32_FLOAT;
            ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            HRESULT hr = dev11->CreateUnorderedAccessView(g_bridge.tex11[i], &ud,
                                                          &g_bridge.depth_uav);
            if (SUCCEEDED(hr) && BridgeMakeDepthShader(dev11))
            {
                g_bridge.depth_converted = true;
                // The source format is not known here -- it is read from the
                // game's texture at blit time, and that is where it is logged.
                // This line used to name R24_UNORM_X8_TYPELESS unconditionally,
                // which read as a wrong conversion on every game whose depth is
                // not BG3's.
                Log("[bridge] depth will be converted to R32_FLOAT for DLSS");
                continue;
            }
            Log("[bridge] depth UAV/shader setup failed 0x%08X, falling back", hr);
            if (g_bridge.depth_uav != nullptr) { g_bridge.depth_uav->Release(); g_bridge.depth_uav = nullptr; }
            if (g_bridge.tex11[i]  != nullptr) { g_bridge.tex11[i]->Release();  g_bridge.tex11[i]  = nullptr; }
            if (g_bridge.tex12[i]  != nullptr) { g_bridge.tex12[i]->Release();  g_bridge.tex12[i]  = nullptr; }
            if (g_bridge.shared[i] != nullptr) { CloseHandle(g_bridge.shared[i]); g_bridge.shared[i] = nullptr; }
        }

        // Depth is allowed to degrade rather than take the whole bridge down:
        // a D3D12-only texture still satisfies the NGX contract. Committed
        // resources on a DEFAULT heap are zero-filled, and with DepthInverted
        // set in the create flags that reads as the far plane rather than noise.
        if (i == SLOT_DEPTH)
        {
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width            = w;
            rd.Height           = h;
            rd.DepthOrArraySize = 1;
            rd.MipLevels        = 1;
            rd.Format           = DXGI_FORMAT_R32_FLOAT;
            rd.SampleDesc.Count = 1;
            rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            HRESULT hr = g_bridge.dev12->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                __uuidof(ID3D12Resource), reinterpret_cast<void **>(&g_bridge.tex12[i]));
            if (SUCCEEDED(hr))
            {
                g_bridge.depth_placeholder = true;
                Log("[bridge] Depth  %ux%u  PLACEHOLDER (R32_FLOAT, zero-filled, not shared)", w, h);
                Log("[bridge] the game's real depth is not crossing; image quality will suffer");
                continue;
            }
            Log("[bridge] Depth placeholder failed 0x%08X", hr);
        }
        ok = false;
    }
    dev1->Release();
    if (!ok) { BridgeReleaseTextures(); return false; }

    // The feature is created with the game's own render and output sizes, not
    // with the texture sizes: in an upscaling mode those are not the same thing.
    g_bridge.params->Set("Width", g_bridge.render_w);
    g_bridge.params->Set("Height", g_bridge.render_h);
    g_bridge.params->Set("OutWidth", g_bridge.ngx_out_w);
    g_bridge.params->Set("OutHeight", g_bridge.ngx_out_h);
    g_bridge.params->Set("PerfQualityValue", 2);
    g_bridge.params->Set("DLSS.Feature.Create.Flags", g_cfg.flags);
    // Mirror the game's own answer where it gave one. Deus Ex: Mankind Divided
    // asks for 0 here while BG3 asks for 1, and forcing either on the other is
    // inventing part of a contract that is meant to be copied.
    if (gp != nullptr)
        CopyUIntParam(gp, "DLSS.Enable.Output.Subrects",
                      static_cast<unsigned int>(g_cfg.subrects));
    else
        g_bridge.params->Set("DLSS.Enable.Output.Subrects", g_cfg.subrects);
    g_bridge.params->Set("CreationNodeMask", 1u);
    g_bridge.params->Set("VisibilityNodeMask", 1u);
    g_bridge.params->Set("RTXValue", 0);

    if (!BeginCommands()) { Log("[bridge] could not start a command list"); return false; }

    DWORD code = 0;
    Breadcrumb("creating the D3D12 DLSS feature");
    NVSDK_NGX_Result rf = SafeCreateFeature(g_bridge.create_feature, g_bridge.list,
                                            g_bridge.params, &g_bridge.feature, &code);
    const UINT64 v = EndCommands();

    // Feature creation is a one-off, so block until it has actually run -- but
    // still on the game's render thread, so not for long enough to be mistaken
    // for a hang.
    if (g_bridge.fence12->GetCompletedValue() < v)
    {
        g_bridge.fence12->SetEventOnCompletion(v, g_bridge.fence_event);
        if (WaitForSingleObject(g_bridge.fence_event, 4000) != WAIT_OBJECT_0)
        {
            Log("[bridge] feature creation did not complete within 4 s; giving up.");
            BridgeDisable("creating the DLSS feature crashed");
            return false;
        }
    }

    if (code != 0)
    {
        // NGX loads its feature implementations from the game folder before the
        // driver's copy. A stale or mismatched nvngx_*.dll there is the usual
        // reason creating a feature faults rather than returning an error.
        Warn("creating the DLSS feature crashed. If you copied any nvngx_*.dll into "
             "the game folder by hand, a version mismatch there is the likeliest "
             "cause -- try removing them.");
        Log("  NGX prefers nvngx_*.dll files sitting next to the game over the");
        Log("  driver's own. If any were dropped in by hand, a version mismatch");
        Log("  there is the likeliest cause -- try removing them.");
        BridgeDisable("the DLSS feature could not be created");
        return false;
    }
    if (rf != NGX_SUCCESS || g_bridge.feature == nullptr)
    { Log("[bridge] CreateFeature failed with result 0x%08X", rf); return false; }

    Log("[bridge] feature ready: render %ux%u -> output %ux%u  (textures in %ux%u out %ux%u) handle=%p",
        g_bridge.render_w, g_bridge.render_h, g_bridge.ngx_out_w, g_bridge.ngx_out_h,
        w, h, g_bridge.out_width, g_bridge.out_height, static_cast<void *>(g_bridge.feature));

    // What a DLSS 5 add-on will ask of the textures the bridge hands it, asked
    // here first. Escape from Tarkov failed on exactly this and said so only
    // from the other add-on's side, as "output format 1 is not a supported typed
    // codec format" -- by which point the bridge looked healthy and the fault
    // looked like someone else's. These textures are the bridge's own, so a
    // refusal here is a bug here.
    for (int i = 0; i < SLOT_COUNT; ++i)
    {
        if (i != SLOT_OUTPUT && i != SLOT_COLOR) continue;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {};
        fs.Format = (i == SLOT_DEPTH) ? DXGI_FORMAT_R32_FLOAT : TypedFormat(fmt[i]);
        if (FAILED(g_bridge.dev12->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                                                       &fs, sizeof(fs))))
            continue;
        const bool can_sample = (fs.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) != 0;
        const bool can_uav    = (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0 &&
                                (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
        const bool needs_uav  = (i == SLOT_OUTPUT);
        if (can_sample && (can_uav || !needs_uav)) continue;
        Warn("the bridge's %s texture is %s, which this GPU cannot %s. A DLSS 5 "
             "add-on needs that and will refuse the frame.",
             kSlotKey[i], FormatName(fs.Format),
             !can_sample ? "sample in a shader" : "write through a typed UAV");
    }
    for (int i = 0; i < SLOT_COUNT; ++i) g_bridge.fmt[i] = fmt[i];
    g_bridge.need_reset  = true;
    g_bridge.frame_ready = true;
    return true;
}

// Takes the next allocator in the ring, waiting first if the GPU has not yet
// finished with the frame that last used it.
static bool BeginCommands()
{
    const int slot = g_bridge.frame_slot;
    const UINT64 retire = g_bridge.alloc_fence[slot];
    if (retire != 0 && g_bridge.fence12->GetCompletedValue() < retire)
    {
        g_bridge.fence12->SetEventOnCompletion(retire, g_bridge.fence_event);
        // This blocks the thread the game renders on, so it has to give up long
        // before a render-thread watchdog would fire. A missed frame is
        // recoverable; a thirty-second stall is the game being killed.
        if (WaitForSingleObject(g_bridge.fence_event, 2000) != WAIT_OBJECT_0)
        {
            Log("[bridge] the GPU did not retire allocator slot %d within 2 s.", slot);
            Log("  Something downstream is not completing. Stopping rather than");
            Log("  stalling the render thread any further.");
            BridgeDisable("the GPU stopped completing work");
            return false;
        }
    }
    if (g_bridge.alloc[slot] == nullptr) return false;
    if (FAILED(g_bridge.alloc[slot]->Reset())) return false;
    return SUCCEEDED(g_bridge.list->Reset(g_bridge.alloc[slot], nullptr));
}

// Submits, then records which fence value retires the allocator just used.
static UINT64 EndCommands()
{
    g_bridge.list->Close();
    ID3D12CommandList *lists[] = { g_bridge.list };
    g_bridge.queue->ExecuteCommandLists(1, lists);

    const UINT64 v = ++g_bridge.fence_value;
    g_bridge.queue->Signal(g_bridge.fence12, v);
    g_bridge.alloc_fence[g_bridge.frame_slot] = v;
    g_bridge.frame_slot = (g_bridge.frame_slot + 1) % Bridge::kFrames;
    return v;
}

static void Barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *res,
                    D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = res;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter  = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &b);
}

// Copies one float or int parameter straight across from the game's block, so the
// D3D12 evaluate sees the same jitter and exposure the game computed this frame.
// A failed Get is not required to leave the destination alone, so the result is
// checked rather than assumed. Silently forwarding a zeroed value is how a
// parameter the game never set becomes a wrong one the bridge invented.
static void CopyFloatParam(const NVSDK_NGX_Parameter *src, const char *key, float fallback)
{
    float v = 0.0f;
    if (src->Get(key, &v) != NGX_SUCCESS) v = fallback;
    g_bridge.params->Set(key, v);
}

static void CopyUIntParam(const NVSDK_NGX_Parameter *src, const char *key, unsigned int fallback)
{
    unsigned int v = 0;
    if (src->Get(key, &v) != NGX_SUCCESS) v = fallback;
    g_bridge.params->Set(key, v);
}

static void BridgeFrameInner(ID3D11DeviceContext *ctx, const NVSDK_NGX_Parameter *gp)
{
    if (g_bridge.disabled || !g_bridge.session_ready || gp == nullptr) return;

    // Signal and Wait are immediate-context operations. On a deferred context
    // they are illegal, the fence is never signalled, and the D3D11 queue then
    // waits forever on it -- which a game's render thread watchdog reports as a
    // hang rather than as anything traceable to this add-on.
    if (ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
    {
        Log("[bridge] DLSS runs on a deferred context here; the bridge cannot work "
            "and has stopped. The game is unaffected.");
        Log("  This bridge synchronises two APIs with a shared fence, and that only");
        Log("  works on the immediate context. Continuing would deadlock the render");
        Log("  thread, so the bridge is stopping. The game is unaffected.");
        BridgeDisable("DLSS runs on a deferred context here");
        return;
    }

    if ((g_bridge.frames_done % 60) == 0) CfgReload();

    // Stage 0 touches nothing at all, not even a read of the game's parameters.
    if (g_cfg.stage <= 0) { ++g_bridge.frames_done; return; }

    D3D11_TEXTURE2D_DESC d[SLOT_COUNT] = {};
    ID3D11Texture2D     *g[SLOT_COUNT] = {};
    for (int i = 0; i < SLOT_COUNT; ++i)
    {
        g[i] = GameTexture(gp, kSlotKey[i], &d[i]);
        if (g[i] == nullptr)
        {
            for (int k = 0; k < i; ++k) g[k]->Release();
            return;  // nothing to do this frame
        }
    }

    // A multisampled texture cannot be copied into a single-sample one, and a
    // TEXTURE2D view over it is illegal. Watch Dogs 2 at MSAA 2x hits both: the
    // depth view fails on a format the table maps correctly, and the colour copy
    // quietly does nothing, so DLSS would be handed whatever the textures held
    // last. The game's own DLSS fails there too, before the bridge touches it.
    //
    // This waits rather than disabling for the session: turning MSAA off is the
    // usual fix, and clearing frame_ready means the rebuild happens by itself
    // when the sample count drops. A change of sample count alone does not move
    // any size or format, so nothing else would have noticed it.
    UINT samples = 1;
    for (int i = 0; i < SLOT_COUNT; ++i)
        if (d[i].SampleDesc.Count > samples) samples = d[i].SampleDesc.Count;
    if (samples > 1)
    {
        if (!g_bridge.msaa_reported)
        {
            g_bridge.msaa_reported = true;
            Warn("this game is rendering with %ux MSAA. The bridge cannot read "
                 "multisampled buffers; turn MSAA off in the game's settings to "
                 "use it. The game is unaffected.", samples);
        }
        g_bridge.frame_ready = false;
        for (int i = 0; i < SLOT_COUNT; ++i) g[i]->Release();
        return;
    }
    if (g_bridge.msaa_reported)
    {
        g_bridge.msaa_reported = false;
        Log("[bridge] MSAA is off now; rebuilding.");
    }

    ID3D11Device *dev11 = nullptr;
    ctx->GetDevice(&dev11);

    // The whole contract comes from the game, never from assumptions about it.
    // Switching between DLAA and an upscaling preset changes the render size
    // while the textures stay put, so both have to be watched.
    unsigned int gw = 0, gh = 0, gow = 0, goh = 0;
    gp->Get("Width", &gw);
    gp->Get("Height", &gh);
    gp->Get("OutWidth", &gow);
    gp->Get("OutHeight", &goh);
    if (gw == 0 || gh == 0) { gw = d[SLOT_COLOR].Width;  gh = d[SLOT_COLOR].Height; }
    if (gow == 0 || goh == 0) { gow = d[SLOT_OUTPUT].Width; goh = d[SLOT_OUTPUT].Height; }

    bool shape_changed = (d[SLOT_COLOR].Width  != g_bridge.width)     ||
                         (d[SLOT_COLOR].Height != g_bridge.height)    ||
                         (d[SLOT_OUTPUT].Width != g_bridge.out_width) ||
                         (d[SLOT_OUTPUT].Height!= g_bridge.out_height)||
                         (gw != g_bridge.render_w)  || (gh != g_bridge.render_h) ||
                         (gow != g_bridge.ngx_out_w) || (goh != g_bridge.ngx_out_h);
    for (int i = 0; i < SLOT_COUNT && !shape_changed; ++i)
        shape_changed = (d[i].Format != g_bridge.fmt[i]);

    if (!g_bridge.frame_ready || shape_changed)
    {
        DXGI_FORMAT fmt[SLOT_COUNT];
        for (int i = 0; i < SLOT_COUNT; ++i) fmt[i] = d[i].Format;
        g_bridge.width      = d[SLOT_COLOR].Width;
        g_bridge.height     = d[SLOT_COLOR].Height;
        g_bridge.out_width  = d[SLOT_OUTPUT].Width;
        g_bridge.out_height = d[SLOT_OUTPUT].Height;
        g_bridge.render_w   = gw;
        g_bridge.render_h   = gh;
        g_bridge.ngx_out_w  = gow;
        g_bridge.ngx_out_h  = goh;
        Log("[bridge] rebuilding: render %ux%u -> %ux%u", gw, gh, gow, goh);
        if (!BridgeBuildResources(dev11, fmt, gp))
        {
            BridgeFail("resource build");
            for (int i = 0; i < SLOT_COUNT; ++i) g[i]->Release();
            dev11->Release();
            return;
        }
        g_bridge.consecutive_fails = 0;
    }

    // If the game alternates its Color and Output textures, writing the result
    // into Output could feed back into the next frame's input. Record the
    // identities for the first stretch of frames so that can be ruled in or out.
    {
        static void *seen[SLOT_COUNT] = {};
        static int   swaps = 0;
        bool moved = false;
        for (int i = 0; i < SLOT_COUNT; ++i)
            if (seen[i] != static_cast<void *>(g[i])) moved = true;
        if (moved && swaps < 24)
        {
            ++swaps;
            int reset_now = 0;
            gp->Get("Reset", &reset_now);
            Log("[bridge] frame %llu textures changed: C=%p O=%p D=%p MV=%p reset=%d",
                g_bridge.frames_done + 1, static_cast<void *>(g[SLOT_COLOR]),
                static_cast<void *>(g[SLOT_OUTPUT]), static_cast<void *>(g[SLOT_DEPTH]),
                static_cast<void *>(g[SLOT_MV]), reset_now);
            for (int i = 0; i < SLOT_COUNT; ++i) seen[i] = g[i];
        }
    }

    // 1. game -> shared, on the D3D11 immediate context. MV goes through a
    //    conversion pass when the driver would not share the game's format.
    ctx->CopyResource(g_bridge.tex11[SLOT_COLOR], g[SLOT_COLOR]);
    if (g_bridge.mv_converted) BridgeBlitMV(ctx, dev11, g[SLOT_MV]);
    else                       ctx->CopyResource(g_bridge.tex11[SLOT_MV], g[SLOT_MV]);
    if (g_cfg.stage >= 2 && g_bridge.depth_converted)
        BridgeBlitDepth(ctx, dev11, g[SLOT_DEPTH]);

    if (g_cfg.stage < 3)
    {
        ++g_bridge.frames_done;
        for (int i = 0; i < SLOT_COUNT; ++i) g[i]->Release();
        dev11->Release();
        return;
    }

    // 2. publish those copies to the other queue. The flush matters: without it
    //    the signal may sit unsubmitted and the D3D12 wait never completes.
    const UINT64 v_in = ++g_bridge.fence_value;
    g_bridge.ctx4->Signal(g_bridge.fence11, v_in);
    ctx->Flush();

    // 3. D3D12 side: wait, evaluate, signal back
    g_bridge.queue->Wait(g_bridge.fence12, v_in);
    if (!BeginCommands())
    {
        BridgeFail("command list");
        for (int i = 0; i < SLOT_COUNT; ++i) g[i]->Release();
        dev11->Release();
        return;
    }

    Barrier(g_bridge.list, g_bridge.tex12[SLOT_COLOR], D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(g_bridge.list, g_bridge.tex12[SLOT_DEPTH], D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(g_bridge.list, g_bridge.tex12[SLOT_MV], D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(g_bridge.list, g_bridge.tex12[SLOT_OUTPUT], D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    g_bridge.params->Set("Color", g_bridge.tex12[SLOT_COLOR]);
    g_bridge.params->Set("Output", g_bridge.tex12[SLOT_OUTPUT]);
    g_bridge.params->Set("Depth", g_bridge.tex12[SLOT_DEPTH]);
    g_bridge.params->Set("MotionVectors", g_bridge.tex12[SLOT_MV]);
    // Mirror the game's own subrect description rather than inventing one. In an
    // upscaling preset the rendered area is a corner of a larger texture, and
    // guessing it is what made DLSS Quality produce a soft, effect-free image.
    CopyUIntParam(gp, "DLSS.Render.Subrect.Dimensions.Width", g_bridge.render_w);
    CopyUIntParam(gp, "DLSS.Render.Subrect.Dimensions.Height", g_bridge.render_h);
    CopyUIntParam(gp, "DLSS.Input.Color.Subrect.Base.X", 0u);
    CopyUIntParam(gp, "DLSS.Input.Color.Subrect.Base.Y", 0u);
    CopyUIntParam(gp, "DLSS.Input.Depth.Subrect.Base.X", 0u);
    CopyUIntParam(gp, "DLSS.Input.Depth.Subrect.Base.Y", 0u);
    CopyUIntParam(gp, "DLSS.Input.MV.Subrect.Base.X", 0u);
    CopyUIntParam(gp, "DLSS.Input.MV.Subrect.Base.Y", 0u);
    CopyUIntParam(gp, "DLSS.Output.Subrect.Base.X", 0u);
    CopyUIntParam(gp, "DLSS.Output.Subrect.Base.Y", 0u);
    CopyFloatParam(gp, "MV.Scale.X", -1.0f);
    CopyFloatParam(gp, "MV.Scale.Y", -1.0f);
    CopyFloatParam(gp, "Jitter.Offset.X", 0.0f);
    CopyFloatParam(gp, "Jitter.Offset.Y", 0.0f);
    CopyFloatParam(gp, "Sharpness", 0.0f);
    CopyFloatParam(gp, "DLSS.Pre.Exposure", 1.0f);
    CopyFloatParam(gp, "DLSS.Exposure.Scale", 1.0f);

    int reset = 0;
    gp->Get("Reset", &reset);
    g_bridge.params->Set("Reset", (g_bridge.need_reset || reset || g_cfg.reset_every) ? 1 : 0);
    g_bridge.need_reset = false;

    const int mode = g_cfg.mode;

    DWORD code = 0;
    Breadcrumb("running the D3D12 evaluate");
    NVSDK_NGX_Result re = NGX_SUCCESS;
    if (mode != 1)
        re = SafeEvaluateGuarded(g_bridge.eval_feature, g_bridge.list,
                                 g_bridge.feature, g_bridge.params, &code);

    Barrier(g_bridge.list, g_bridge.tex12[SLOT_COLOR],
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(g_bridge.list, g_bridge.tex12[SLOT_DEPTH],
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(g_bridge.list, g_bridge.tex12[SLOT_MV],
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(g_bridge.list, g_bridge.tex12[SLOT_OUTPUT],
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

    const UINT64 v_out = EndCommands();

    if (code != 0)
    {
        // An access violation inside NGX means its internal state is no longer
        // trustworthy. Retrying would likely take the game down, so stop here
        // and let it render on its own.
        Log("[bridge] evaluate raised exception 0x%08X -- disabling to protect the game", code);
        BridgeDisable("repeated failures");
    }
    else if (re != NGX_SUCCESS)
    {
        Log("[bridge] evaluate failed with result 0x%08X", re);
        BridgeFail("evaluate");
    }
    else
    {
        // 4. take the result back into the game's own output texture
        g_bridge.ctx4->Wait(g_bridge.fence11, v_out);

        const UINT64 n = ++g_bridge.frames_done;

        if (mode == 2)      ctx->CopyResource(g[SLOT_OUTPUT], g_bridge.tex11[SLOT_OUTPUT]);
        else if (mode == 1) ctx->CopyResource(g[SLOT_OUTPUT], g_bridge.tex11[SLOT_COLOR]);
        // mode 0 writes nothing back

        g_bridge.consecutive_fails = 0;

        if (g_cfg.pixels && n >= 2 && n <= 4)
        {
            Log("[bridge] frame %llu buffer contents:", n);
            LogCenterPixel(ctx, dev11, "game in",  g[SLOT_COLOR],               d[SLOT_COLOR].Format);
            LogCenterPixel(ctx, dev11, "shared in", g_bridge.tex11[SLOT_COLOR], d[SLOT_COLOR].Format);
            LogCenterPixel(ctx, dev11, "depth",     g_bridge.tex11[SLOT_DEPTH], DXGI_FORMAT_R32_FLOAT);
            LogCenterPixel(ctx, dev11, "dlss out",  g_bridge.tex11[SLOT_OUTPUT], d[SLOT_OUTPUT].Format);
        }
        else if (n <= 1 || (n % 1800) == 0)
        {
            Log("[bridge] frame %llu delivered (%ux%u)", n, g_bridge.width, g_bridge.height);
        }
    }

    for (int i = 0; i < SLOT_COUNT; ++i) g[i]->Release();
    dev11->Release();
}

// Wrapper so every exit path from the frame work is timed, including the early
// ones taken when a stage is disabled or a rebuild fails.
static void BridgeFrame(ID3D11DeviceContext *ctx, const NVSDK_NGX_Parameter *gp)
{
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    // One lock around the whole frame's worth of context work, so no other
    // thread can land between the copies, the fence signal and the copy back.
    const bool lock = g_bridge.mt != nullptr && g_bridge.mt->GetMultithreadProtected();
    if (lock) g_bridge.mt->Enter();
    BridgeFrameInner(ctx, gp);
    if (lock) g_bridge.mt->Leave();

    QueryPerformanceCounter(&t1);
    TimingTick(t0.QuadPart, t1.QuadPart);
}

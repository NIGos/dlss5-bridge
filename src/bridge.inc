// Implementation of the D3D11 -> D3D12 NGX bridge described in bridge.h.

static NVSDK_NGX_Result SafeEvaluateGuarded(PFN_D3D12EvaluateFeature fn,
                                            ID3D12GraphicsCommandList *list,
                                            const NVSDK_NGX_Handle *feature,
                                            const NVSDK_NGX_Parameter *p, DWORD *code)
{
    *code = 0;
    __try { return fn(list, feature, p, nullptr); }
    __except (CaptureFault(GetExceptionInformation()))
    { *code = GetExceptionCode(); return NGX_EXCEPTION_MARKER; }
}

// Everything worth trying here is a value, not code, and the add-on binary is
// locked while the game runs. So the tunables live in dlss5-bridge.cfg,
// re-read while playing: edit it, and within a second the bridge picks the
// change up and rebuilds whatever it invalidates. One session, many
// experiments.
//
//   mode=2         0 never writes to the game, 1 transport only, 2 full path
//   skip_exe=1     hold off on the host executable's own NGX entry points, and
//                  hook them only if nothing has called DLSS a minute later. A
//                  game that links the NGX client statically -- The Elder
//                  Scrolls Online does -- would otherwise have its code section
//                  patched at startup, which is what an integrity check
//                  terminates a process for. 0 hooks them at once, 2 never
//   dred=1         ask D3D12 to record what the GPU was executing, so a reset
//                  can be explained. Read once, when the session opens, so a
//                  change takes effect on the next launch
//   flags=-1       DLSS.Feature.Create.Flags. -1 copies whatever the game
//                  asked for, which is the only value guaranteed to describe
//                  the game's own buffers. Any other value forces that value:
//                  bit 0 is IsHDR, which is what makes RenoDX apply its HDR
//                  codec, and 107 is what Baldur's Gate 3 asks for
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
//   source=auto    which contract the D3D12 side runs on: auto, mirror, synth
//                  or off. A word rather than a number, because auto is not a
//                  quantity. Only mirror exists to be selected today; auto is
//                  it, and the other two are read and reported and do nothing
//   synth_after=0  seconds to wait for the game's own DLSS before a synthetic
//                  contract may start. 0 is off. There is no such contract yet,
//                  so this is read and reported and nothing waits on it
// The create flags are the only place the game states what its own buffers
// mean: whether the colour carries values above 1.0, whether the depth is
// reversed, whether the motion vectors are at render resolution. Spelling them
// out costs one line and removes the arithmetic from every report.
// The spelling on its own, because the status panel has to print the same names
// into an ImGui line rather than into the log. Two copies of this list is two
// lists to forget to extend the day NGX defines another bit.
static void CreateFlagNames(char *b, size_t n, unsigned int f)
{
    b[0] = 0;
    if (f & 0x01) strcat_s(b, n, " IsHDR");
    if (f & 0x02) strcat_s(b, n, " MVLowRes");
    if (f & 0x04) strcat_s(b, n, " MVJittered");
    if (f & 0x08) strcat_s(b, n, " DepthInverted");
    if (f & 0x20) strcat_s(b, n, " DoSharpening");
    if (f & 0x40) strcat_s(b, n, " AutoExposure");
    if (b[0] == 0) strcat_s(b, n, " none");
}

static void LogCreateFlags(const char *what, unsigned int f)
{
    char b[192];
    CreateFlagNames(b, sizeof(b), f);
    Log("[bridge] %s flags %u:%s", what, f, b);
}

// An 8-bit normalised format cannot hold a value above 1.0, so a buffer in one
// is not HDR whatever the flags say. The DLSS 5 add-on picks its codec from
// that flag, and picking the HDR codec for an SDR buffer is a silent wrong
// answer rather than an error, so it is worth stating outright.
static bool FormatIsLowPrecisionUnorm(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return true;
    default:
        return false;
    }
}

struct BridgeCfg
{
    int mode;
    int flags;
    int subrects;
    int reset_every;
    int pixels;
    int stage;
    int skip_game;
    int dred;
    int skip_exe;
    int unwrap;
    int source;        // CfgSource
    int synth_after;   // seconds, 0 off
    // Which way a third-party motion-vector shader points its Y, and its X.
    // Neither ReshadeMotionEstimation nor qUINT_of.fx states the direction in
    // its declaration, and there is no parameter, semantic or annotation that
    // does -- so this cannot be derived, only tried. MV.Scale carries the sign,
    // and a sign that is wrong gathers the temporal history from the wrong side
    // of the movement: exactly the smear Trails beyond the Horizon showed when
    // -1.0 was invented for it. Guessing once and shipping it is how that
    // happened; a knob is how a user fixes it in the version they already have.
    int mv_sign_x;   // -1 / +1 force; 0 uses the provider's own convention
    int mv_sign_y;
    // NVOFA output grid size: 0 off, else 1, 2 or 4. Both the on/off switch and
    // the calibration knob, so there is no second key. Default 2, measured on an
    // RTX 5090 at 3840x1600: 1.6 ms/frame and 109 MB driver-side, and a median
    // error of 0.09-0.11 px against grid 4's 0.12-0.19 px on rotation and zoom.
    // Grid 4 is 0.6 ms and not broken; grid 1 buys nothing measurable over 2 and
    // costs 8.8 ms.
    int ofa_grid;
    // 5 SLOW, 10 MEDIUM, 20 FAST -- NVOFA's own NV_OF_PERF_LEVEL values, so a
    // report quoting this number can be read against NVIDIA's table directly.
    int ofa_perf;
};

// auto is not a fourth point on a scale with the other three, so this is stored
// as a word in the file: "source=1" would have to be looked up to be read, and
// the value it names is the one thing in this file a user is most likely to be
// told to change by someone else.
enum CfgSource { CFG_SRC_AUTO, CFG_SRC_MIRROR, CFG_SRC_SYNTH, CFG_SRC_OFF };

static const char *kCfgSourceName[] = { "auto", "mirror", "synth", "off" };

static int ParseSourceWord(const char *w, int fallback)
{
    for (int i = 0; i < static_cast<int>(_countof(kCfgSourceName)); ++i)
        if (_stricmp(w, kCfgSourceName[i]) == 0) return i;
    return fallback;   // an unreadable value is not a request to change anything
}

static BridgeCfg g_cfg = { 2, -1, 1, 0, 0, 3, 1, 1, 1, 1, CFG_SRC_AUTO, 0, 0, 0, 2, 20 };

// CfgReload's one-second gate, promoted out of that function so the panel can
// reach it. Two uses, both of them reads of a clock rather than of a setting:
// zeroing it after a panel write makes the next delivered frame re-read the file
// instead of waiting the second out, and a value still zero seconds after a
// write is how the panel tells "the value was rejected" from "nothing has called
// CfgReload at all". It is a gate, not a setting: g_cfg keeps exactly one writer.
static volatile LONGLONG g_cfg_last_read;

// Accumulates the two figures and reports them together, because either one
// alone is misleading.
// Whether this process owns the foreground window, asked once per report rather
// than per frame. An out-of-focus window has now been read as a performance
// finding three separate times in one day of log analysis: the game throttles,
// the bridge's share of a frame moves, and the number looks like a regression
// that is not there. A figure that cannot be read in isolation should not be
// printed as though it can.
static bool ForegroundIsThisProcess()
{
    HWND fg = GetForegroundWindow();
    if (fg == nullptr) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

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

    // The gap between one frame's arrival and the next. Averaged over 600
    // frames this says nothing about smoothness; the widest and narrowest do.
    if (g_bridge.last_entry != 0)
    {
        const LONGLONG iv = entry - g_bridge.last_entry;
        if (g_bridge.iv_min == 0 || iv < g_bridge.iv_min) g_bridge.iv_min = iv;
        if (iv > g_bridge.iv_max) g_bridge.iv_max = iv;
    }
    g_bridge.last_entry = entry;

    if (++g_bridge.timed_frames < 600) return;

    const double span_ms = 1000.0 * double(exit - g_bridge.span_start) / double(g_bridge.qpf);
    const double cpu_ms  = 1000.0 * double(g_bridge.cpu_ticks) / double(g_bridge.qpf);
    const double n       = double(g_bridge.timed_frames);

    // How far behind the D3D12 side is running. The game's queue is made to wait
    // for these values, so a gap that grows instead of staying at a frame or two
    // is the transport stalling -- and the last line before a log stops dead is
    // then evidence rather than an inference.
    const UINT64 done = g_bridge.fence12 != nullptr
                      ? g_bridge.fence12->GetCompletedValue() : 0;
    if (done == UINT64_MAX)
    {
        Log("[bridge] %.0f frames: bridge CPU %.2f ms/frame | frame interval %.2f ms "
            "(%.1f fps) | spread %.2f-%.2f ms | d3d12 DEVICE REMOVED",
            n, cpu_ms / n, span_ms / n, 1000.0 / (span_ms / n),
            1000.0 * double(g_bridge.iv_min) / double(g_bridge.qpf),
            1000.0 * double(g_bridge.iv_max) / double(g_bridge.qpf));
        g_bridge.iv_min = 0; g_bridge.iv_max = 0;
        g_bridge.cpu_ticks = 0; g_bridge.timed_frames = 0; g_bridge.span_start = exit;
        g_bridge.ofa_ticks = 0; g_bridge.ofa_frames = 0;
        return;
    }

    const double lo = 1000.0 * double(g_bridge.iv_min) / double(g_bridge.qpf);
    const double hi = 1000.0 * double(g_bridge.iv_max) / double(g_bridge.qpf);

    // The synthetic path's optical flow, as a clause that is empty whenever no
    // flow ran. In the nine shipped titles ofa_frames is always 0, so the line
    // below is byte-identical to every one already in a mirror log. It carries
    // its own denominator on purpose: if that count disagrees with the leading
    // frame count, flow ran on frames that were not delivered, and the line says
    // so rather than quietly averaging over the wrong number.
    char ofa[80] = "";
    if (g_bridge.ofa_frames != 0)
        _snprintf_s(ofa, sizeof(ofa), _TRUNCATE,
                    " | optical flow %.2f ms over %llu frames",
                    1000.0 * double(g_bridge.ofa_ticks) / double(g_bridge.qpf)
                           / double(g_bridge.ofa_frames),
                    g_bridge.ofa_frames);

    Log("[bridge] %.0f frames: bridge CPU %.2f ms/frame | frame interval %.2f ms (%.1f fps) "
        "| spread %.2f-%.2f ms | bridge is %.0f%% of the frame | d3d12 %llu/%llu (%lld behind)%s%s",
        n, cpu_ms / n, span_ms / n, 1000.0 / (span_ms / n), lo, hi,
        100.0 * cpu_ms / span_ms, done, g_bridge.fence_value,
        static_cast<long long>(g_bridge.fence_value) - static_cast<long long>(done),
        ofa,
        ForegroundIsThisProcess() ? "" : "  <== WINDOW NOT IN FOCUS, do not read as steady state");

    // This measures the gap between the game's own DLSS calls, so a loading
    // screen, an alt-tab or a paused game all land here as one enormous frame --
    // Baldur's Gate 3 produced 13370 ms and carried on for thousands of frames
    // afterwards. It is reported because Windows resets a GPU that stops
    // answering for about two seconds, and Escape from Tarkov recorded 2534 ms
    // eight seconds before losing its device. The figure alone does not say
    // which of the two happened.
    if (hi >= 1500.0)
        Log("[bridge]   one frame took %.0f ms. Usually that is a loading screen, "
            "an alt-tab or a pause, since this measures the gap between the game's "
            "own DLSS calls. It is noted because Windows resets a GPU that stops "
            "answering for about two seconds, so the same figure appears before a "
            "device removal.", hi);

    g_bridge.iv_min       = 0;
    g_bridge.iv_max       = 0;
    g_bridge.cpu_ticks    = 0;
    g_bridge.timed_frames = 0;
    g_bridge.ofa_ticks    = 0;
    g_bridge.ofa_frames   = 0;
    g_bridge.span_start   = exit;
}

// True only when skipping the game's own DLSS cannot leave a stale frame on
// screen: the bridge must be healthy and already have produced a valid output.
// This is only ever asked in order to suppress the game's own DLSS evaluate, so
// the frames it counts have to be frames that replace it. During a handover the
// flags below still read healthy while the frames behind them were produced by
// somebody else, and skipping the game on that evidence leaves the last mirrored
// image on screen under live audio and live input.
static bool BridgeWillDeliver()
{
    return g_source == SRC_MIRROR &&
           g_cfg.skip_game != 0 && g_cfg.mode == 2 && g_cfg.stage >= 3 &&
           !g_bridge.disabled && g_bridge.session_ready && g_bridge.frame_ready &&
           !g_bridge.partial_output && g_bridge.frames_done > 0;
}

// The add-on was called dlss5-dx11-bridge until 1.1.0, so a user upgrading has a
// cfg under the old name with their settings in it. Take the new name when it
// exists, the old one when only that does, and the new name when neither does,
// so a fresh install writes the current one. Silently reverting somebody to
// defaults because a file was renamed is not an upgrade anyone forgives.
static void CfgPath(char *out)
{
    GetModuleFileNameA(g_self, out, MAX_PATH);
    char *s = strrchr(out, '\\');
    if (s == nullptr) return;
    const size_t room = MAX_PATH - (s + 1 - out);

    strcpy_s(s + 1, room, "dlss5-bridge.cfg");
    if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return;

    strcpy_s(s + 1, room, "dlss5-dx11-bridge.cfg");
    if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return;

    strcpy_s(s + 1, room, "dlss5-bridge.cfg");
}

static void CfgReload()
{
    // Called from the frame path, which used to throttle on a frame counter that
    // stops moving the moment the bridge stops delivering. A game that never
    // delivers -- Watch Dogs 2 at MSAA -- then opened and parsed this file on the
    // render thread every single frame, inside the lock the game's own threads
    // contend for. The clock does not stop when the counter does.
    // ponytail: one second, fixed; a per-caller rate only if one ever earns it.
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG last_read = static_cast<ULONGLONG>(g_cfg_last_read);
    if (last_read != 0 && now - last_read < 1000) return;
    g_cfg_last_read = static_cast<LONGLONG>(now);

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
        if (sscanf_s(line, "%63[^=]=%d", key, static_cast<unsigned>(sizeof(key)), &val) != 2)
        {
            // Only source= takes a word, so the numeric scan stays exactly as it
            // was and a line it rejects gets one more chance rather than a
            // rewritten parser -- every other key in this file is a number, and
            // the ones the nine shipped titles depend on are all in that set.
            char word[16];
            if (sscanf_s(line, "%63[^=]=%15s", key, static_cast<unsigned>(sizeof(key)),
                         word, static_cast<unsigned>(sizeof(word))) == 2 &&
                _stricmp(key, "source") == 0)
                next.source = ParseSourceWord(word, next.source);
            continue;
        }
        if      (_stricmp(key, "mode")        == 0) next.mode        = val;
        else if (_stricmp(key, "flags")       == 0) next.flags       = val;
        else if (_stricmp(key, "dred")        == 0) next.dred        = val;
        else if (_stricmp(key, "skip_exe")    == 0) next.skip_exe    = val;
        else if (_stricmp(key, "unwrap")      == 0) next.unwrap      = val;
        else if (_stricmp(key, "subrects")    == 0) next.subrects    = val;
        else if (_stricmp(key, "reset_every") == 0) next.reset_every = val;
        else if (_stricmp(key, "pixels")      == 0) next.pixels      = val;
        else if (_stricmp(key, "stage")       == 0) next.stage       = val;
        else if (_stricmp(key, "skip_game")   == 0) next.skip_game   = val;
        else if (_stricmp(key, "synth_after") == 0) next.synth_after = val;
        else if (_stricmp(key, "ofa_grid")    == 0) next.ofa_grid    = val;
        else if (_stricmp(key, "ofa_perf")    == 0) next.ofa_perf    = val;
        // Only the sign is taken, because only the sign is unknown: the
        // magnitude is the back buffer size and this add-on measures that. A
        // number here that is not a sign would be a second way to say something
        // already derived, and the two would disagree.
        // Three-valued, not two. 0 means "use whatever the motion-vector provider
        // documents", which for vort_Shaders is a fact read out of MIT source rather
        // than a guess; -1 and +1 force, and forcing still wins, because a real
        // installation is where a documented convention gets contradicted.
        else if (_stricmp(key, "mv_sign_x")   == 0) next.mv_sign_x   = (val < 0) ? -1 : (val > 0 ? 1 : 0);
        else if (_stricmp(key, "mv_sign_y")   == 0) next.mv_sign_y   = (val < 0) ? -1 : (val > 0 ? 1 : 0);
    }
    fclose(f);
    if (next.stage < 0 || next.stage > 3) next.stage = g_cfg.stage;

    if (next.mode < 0 || next.mode > 2) next.mode = g_cfg.mode;

    // Same rule stage and mode already follow: a value that is not one of the
    // four the driver was measured with is not a request to change anything.
    if (next.ofa_grid != 0 && next.ofa_grid != 1 && next.ofa_grid != 2 && next.ofa_grid != 4)
        next.ofa_grid = g_cfg.ofa_grid;
    // Anything else is a typo rather than a request; keep what was working.
    if (next.ofa_perf != 5 && next.ofa_perf != 10 && next.ofa_perf != 20)
        next.ofa_perf = g_cfg.ofa_perf;

    // Every field, not a chosen few. dred, skip_exe and unwrap were absent here,
    // so editing one of them mid-session was applied and never reported -- the
    // config line then described a file it no longer matched, which is worse
    // than not printing one. A key worth reading is worth comparing.
    const bool rebuild = (next.flags != g_cfg.flags) || (next.subrects != g_cfg.subrects);
    const bool changed = rebuild || (next.mode != g_cfg.mode) ||
                         (next.reset_every != g_cfg.reset_every) || (next.pixels != g_cfg.pixels) ||
                         (next.stage != g_cfg.stage) || (next.skip_game != g_cfg.skip_game) ||
                         (next.dred != g_cfg.dred) || (next.skip_exe != g_cfg.skip_exe) ||
                         (next.unwrap != g_cfg.unwrap) || (next.source != g_cfg.source) ||
                         (next.synth_after != g_cfg.synth_after) ||
                         (next.mv_sign_x != g_cfg.mv_sign_x) ||
                         (next.mv_sign_y != g_cfg.mv_sign_y) ||
                         (next.ofa_grid != g_cfg.ofa_grid) ||
                         (next.ofa_perf != g_cfg.ofa_perf);
    if (!changed) return;

    g_cfg = next;
    Log("[bridge] config: stage=%d mode=%d flags=%d subrects=%d reset_every=%d pixels=%d "
        "skip_game=%d dred=%d skip_exe=%d unwrap=%d source=%s synth_after=%d "
        "mv_sign_x=%d mv_sign_y=%d ofa_grid=%d ofa_perf=%d",
        g_cfg.stage, g_cfg.mode, g_cfg.flags, g_cfg.subrects, g_cfg.reset_every, g_cfg.pixels,
        g_cfg.skip_game, g_cfg.dred, g_cfg.skip_exe, g_cfg.unwrap,
        kCfgSourceName[g_cfg.source], g_cfg.synth_after, g_cfg.mv_sign_x, g_cfg.mv_sign_y,
        g_cfg.ofa_grid, g_cfg.ofa_perf);

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
    fprintf(f, "stage=%d\nmode=%d\nskip_game=%d\nflags=%d\nsubrects=%d\nreset_every=%d\npixels=%d\ndred=%d\nskip_exe=%d\nunwrap=%d\nsource=%s\nsynth_after=%d\nmv_sign_x=%d\nmv_sign_y=%d\nofa_grid=%d\nofa_perf=%d\n",
            g_cfg.stage, g_cfg.mode, g_cfg.skip_game, g_cfg.flags, g_cfg.subrects,
            g_cfg.reset_every, g_cfg.pixels, g_cfg.dred, g_cfg.skip_exe, g_cfg.unwrap,
            kCfgSourceName[g_cfg.source], g_cfg.synth_after, g_cfg.mv_sign_x, g_cfg.mv_sign_y,
            g_cfg.ofa_grid, g_cfg.ofa_perf);
    fclose(f);
    Log("[bridge] wrote default config to %s", path);
}

// The status panel's only writer, and the only place in this add-on that writes
// a file a user owns. It rewrites ONE key and leaves every other byte --
// comments, keys this build has never heard of, ordering, CRLF -- exactly as it
// found them: the file is the single source of truth and this add-on is not its
// author. That is also why CfgWriteDefault is not reused as the writer; its one
// fprintf prints the struct and would delete everything else in the file.
//
// The read happens one instruction before the write, so an edit a user saved
// from a text editor two frames ago is in the buffer being rewritten rather than
// being overwritten by a snapshot taken at start-up. Nothing here touches g_cfg:
// CfgReload picks this up from the file on the next delivered frame, through the
// same parser, the same validators and the same change detector a hand edit goes
// through, which is what keeps one source of truth and one writer of g_cfg.
// ponytail: last writer wins inside this one call; no lock file, no watcher.
// The one fiddly part of the writer, kept pure so it can be run on its own: no
// file, no globals, in and out are buffers. Copies every line of in verbatim,
// replaces the value on the first line whose key matches, appends the key if no
// line matched, and returns the length written -- or 0 if the result would not
// fit, which the caller reports rather than truncating a user's file.
//
// The key comparison is the text before the first '=', trimmed of spaces, tabs
// and a CR, so a CRLF file and an indented line both match. Line endings are
// carried through byte for byte because a line is copied with its own newline;
// only the line this writes uses \n.
static size_t CfgRewriteLine(const char *in, size_t n, const char *key, const char *value,
                             char *out, size_t out_n)
{
    size_t o = 0;
    bool   done = false;
    const size_t klen = strlen(key);
    for (size_t i = 0; i < n; )
    {
        size_t e = i;
        while (e < n && in[e] != '\n') ++e;
        const size_t len = (e < n) ? (e - i + 1) : (e - i);   // the newline stays with the line

        size_t k0 = i;
        while (k0 < e && (in[k0] == ' ' || in[k0] == '\t')) ++k0;
        size_t k1 = k0;
        while (k1 < e && in[k1] != '=') ++k1;
        size_t k2 = k1;
        while (k2 > k0 && (in[k2 - 1] == ' ' || in[k2 - 1] == '\t' || in[k2 - 1] == '\r')) --k2;

        if (!done && k1 < e && (k2 - k0) == klen && _strnicmp(in + k0, key, klen) == 0)
        {
            const int w = _snprintf_s(out + o, out_n - o, _TRUNCATE, "%s=%s\n", key, value);
            if (w < 0) return 0;
            o += static_cast<size_t>(w);
            done = true;
        }
        else
        {
            if (o + len >= out_n) return 0;
            memcpy(out + o, in + i, len);
            o += len;
        }
        i = (e < n) ? e + 1 : e;
    }
    if (!done)
    {
        // A file whose last line has no newline of its own -- which a text
        // editor is perfectly happy to save -- would otherwise have the appended
        // key run onto the end of it, producing one line that parses as neither.
        if (o > 0 && out[o - 1] != '\n')
        {
            if (o + 1 >= out_n) return 0;
            out[o++] = '\n';
        }
        const int w = _snprintf_s(out + o, out_n - o, _TRUNCATE, "%s=%s\n", key, value);
        if (w < 0) return 0;
        o += static_cast<size_t>(w);
    }
    return o;
}

static bool CfgSetKey(const char *key, const char *value, char *why, size_t why_n)
{
    char path[MAX_PATH];
    CfgPath(path);

    // Normally a no-op, because it returns early when the file exists. If the
    // file was deleted mid-session it comes back with ALL sixteen keys at this
    // session's current values -- it prints g_cfg, not build defaults -- and
    // writing only our one line into a vanished file would instead leave fifteen
    // keys absent, which is invisible now and reverts a user's whole tuning on
    // the next launch.
    CfgWriteDefault();

    char   buf[4096];
    size_t n = 0;
    FILE  *f = nullptr;
    if (fopen_s(&f, path, "rb") == 0 && f != nullptr)
    {
        n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
    }
    // Most editors save by truncate-then-write, so a zero-byte read of a file
    // that exists means mid-save rather than empty. Refusing costs one click.
    if (n == 0)
    {
        strcpy_s(why, why_n, "the config file could not be read just now; try again");
        return false;
    }
    // A writer that silently truncates at its buffer would destroy an annotated
    // config. Refuse and say so.
    if (n == sizeof(buf))
    {
        strcpy_s(why, why_n,
                 "this file is larger than the panel will rewrite; change it in a text editor");
        return false;
    }
    buf[n] = 0;

    char   out[4352];
    const size_t o = CfgRewriteLine(buf, n, key, value, out, sizeof(out));
    if (o == 0)
    {
        strcpy_s(why, why_n,
                 "this file is larger than the panel will rewrite; change it in a text editor");
        return false;
    }

    // Temp plus rename, so a failure leaves the original byte-for-byte intact
    // and CfgReload on the other thread never sees a half-written file.
    char tmp[MAX_PATH + 8];
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s.tmp", path);

    FILE *w = nullptr;
    if (fopen_s(&w, tmp, "wb") != 0 || w == nullptr)
    {
        _snprintf_s(why, why_n, _TRUNCATE,
                    "dlss5-bridge.cfg could not be written (error %lu)", GetLastError());
        return false;
    }
    const bool wrote = fwrite(out, 1, o, w) == o;
    fflush(w);
    fclose(w);
    if (!wrote)
    {
        DeleteFileA(tmp);
        strcpy_s(why, why_n, "dlss5-bridge.cfg could not be written in full");
        return false;
    }

    // CfgReload's own fopen can hold the target at this instant -- MSVC's fopen
    // does not ask for FILE_SHARE_DELETE -- so a sharing violation here is a
    // race with a reader rather than a broken folder. Three tries, no sleep.
    DWORD err = 0;
    for (int t = 0; t < 3; ++t)
    {
        if (MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
        {
            // Not a setting: the reload gate, so the next delivered frame reads
            // the file instead of waiting the one-second rate limit out.
            InterlockedExchange64(reinterpret_cast<volatile LONG64 *>(&g_cfg_last_read), 0);
            // Once per click, never per frame: Log takes g_log_cs and the render
            // thread takes it too.
            Log("[panel] wrote %s=%s to %s", key, value, path);
            return true;
        }
        err = GetLastError();
    }
    DeleteFileA(tmp);
    _snprintf_s(why, why_n, _TRUNCATE, "the config file was busy; try again (error %lu)", err);
    return false;
}

// Reads one pixel back to the CPU. Blocking and slow, so only used on the first
// few frames, but it settles what the buffers actually contain instead of
// guessing from what the screen looks like.
//
// The coordinate is a parameter and the value is returned rather than logged,
// because synth.inc asks the same question of a texture that is not the mirror's
// and at points that are not its centre. Two readbacks would be two sets of
// staging-texture and Map mistakes to make; this stays the only one.
static bool ReadPixelRaw(ID3D11DeviceContext *ctx, ID3D11Device *dev11,
                         ID3D11Texture2D *tex, DXGI_FORMAT fmt, UINT x, UINT y, UINT *out)
{
    if (tex == nullptr || ctx == nullptr || dev11 == nullptr || out == nullptr) return false;

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
    if (FAILED(dev11->CreateTexture2D(&td, nullptr, &staging)) || staging == nullptr) return false;

    D3D11_BOX box = { x, y, 0, x + 1, y + 1, 1 };
    ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, &box);

    bool got = false;
    D3D11_MAPPED_SUBRESOURCE ms = {};
    if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &ms)) && ms.pData != nullptr)
    {
        *out = *static_cast<const UINT *>(ms.pData);
        got  = true;
        ctx->Unmap(staging, 0);
    }
    staging->Release();
    return got;
}

static void LogCenterPixel(ID3D11DeviceContext *ctx, ID3D11Device *dev11,
                           const char *label, ID3D11Texture2D *tex, DXGI_FORMAT fmt)
{
    UINT raw = 0;
    if (ReadPixelRaw(ctx, dev11, tex, fmt, g_bridge.width / 2, g_bridge.height / 2, &raw))
        Log("[bridge]   %-8s centre pixel raw = 0x%08X", label, raw);
}

// Every path that gives up goes through here, so the user is told once, in the
// overlay, that the bridge stopped -- otherwise there is simply no effect on
// screen and nothing to explain it without opening a file.
static void BridgeReleaseExposureFwd();

static void BridgeReleaseTextures();

// It is also the only shutdown this add-on has. It used to set a flag and
// nothing else, which left the DLSS feature, four shared textures and their NT
// handles resident for the rest of the session -- while telling the user the
// game was rendering normally.
// How much of the card is spoken for. Escape from Tarkov runs 128-bit-per-pixel
// buffers, which makes every mirror here four times the size of Baldur's Gate
// 3's, and running past the budget is one of the few things that produces a
// reset without a page fault.
static void LogVideoMemory(const char *when)
{
    if (g_bridge.adapter3 == nullptr) return;
    DXGI_QUERY_VIDEO_MEMORY_INFO vm = {};
    if (FAILED(g_bridge.adapter3->QueryVideoMemoryInfo(
            0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm)))
        return;
    Log("[bridge] video memory %s: %llu MB used of %llu MB budget%s", when,
        vm.CurrentUsage / (1024 * 1024), vm.Budget / (1024 * 1024),
        vm.CurrentUsage > vm.Budget ? "  -- over budget" : "");
}

// Device Removed Extended Data. When Windows resets a hung GPU, the only
// account of what it was doing lives in the driver, and DRED is how it is asked
// for: a per-command-list record of how many operations completed, and the
// allocation behind a bad address. Escape from Tarkov reports
// DXGI_ERROR_DEVICE_HUNG about twenty seconds in, and nothing else about it is
// known. This must be switched on before the device exists, so it is done here
// rather than where the removal is noticed.
// ReShade wraps every D3D12 device created through the export, this add-on's
// own included, and then broadcasts init_pipeline, init_sampler and the rest to
// every loaded add-on for everything built from that device. There is no filter
// saying which add-on asked for it.
//
// Luma is a DX11-only framework. Its OnInitDevice is the one handler that does
// return early for a non-D3D11 device, so ReShade never gives it per-device
// data -- and every other handler then dereferences that null. Prey 2017 does
// not opt into the compile-time API guard that would have stopped the rest, so
// NVSDK_NGX_D3D12_Init_Ext creating pipeline state on this device faulted at an
// address inside Luma-Prey.addon.
//
// IID_UnwrappedObject is ReShade's own way out of its proxy, from its
// com_utils.hpp; the D3D12 proxy answers it by AddRef-ing and returning the
// real device. RenoDX asks the same question of every command list before
// forwarding it to NGX, so a native pointer is the case NGX callers prefer.
//
// The proxy is deliberately not released. Nothing releases this device for the
// life of the process, and releasing it would announce destroy_device to those
// same add-ons for a device still in use.
static void UnwrapReShadeProxy(ID3D12Device **dev)
{
    static const GUID kUnwrapped =
        { 0x7f2c9a11, 0x3b4e, 0x4d6a, { 0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42 } };

    ID3D12Device *raw = nullptr;
    if (FAILED((*dev)->QueryInterface(kUnwrapped, reinterpret_cast<void **>(&raw))) ||
        raw == nullptr)
    {
        Log("[bridge] the D3D12 device is not wrapped; nothing to unwrap. On ReShade "
            "older than 6.6 this is expected and the session runs as before.");
        return;
    }
    Log("[bridge] the D3D12 device came back wrapped by ReShade: %p over %p. NGX is "
        "given the unwrapped one, so no other add-on is handed D3D12 objects it "
        "never asked for.", static_cast<void *>(*dev), static_cast<void *>(raw));
    *dev = raw;
}

// The fault here was reported inside D3D12Core.dll on an older release; device
// creation now succeeds and what follows is where Prey stops.
static HRESULT SafeCreateDevice(PFN_D3D12CreateDevice_ fn, IDXGIAdapter *ad,
                                ID3D12Device **out, DWORD *code)
{
    *code = 0;
    HRESULT hr = E_FAIL;
    __try { hr = fn(ad, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                    reinterpret_cast<void **>(out)); }
    __except (CaptureFault(GetExceptionInformation()))
    { *code = GetExceptionCode(); return E_FAIL; }

    if (SUCCEEDED(hr) && *out != nullptr && g_cfg.unwrap != 0) UnwrapReShadeProxy(out);
    return hr;
}

static bool EnableDredGuarded(HMODULE d3d12, DWORD *code)
{
    *code = 0;
    __try
    {
        auto get_debug = reinterpret_cast<HRESULT (WINAPI *)(REFIID, void **)>(
            GetProcAddress(d3d12, "D3D12GetDebugInterface"));
        if (get_debug == nullptr) return false;

        ID3D12DeviceRemovedExtendedDataSettings *set = nullptr;
        if (FAILED(get_debug(__uuidof(ID3D12DeviceRemovedExtendedDataSettings),
                             reinterpret_cast<void **>(&set))) || set == nullptr)
            return false;

        set->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        set->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        set->Release();
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *code = GetExceptionCode();
        return false;
    }
}

static void EnableDred(HMODULE d3d12)
{
    DWORD code = 0;
    if (EnableDredGuarded(d3d12, &code))
    {
        Log("[bridge] DRED is on: if the GPU is reset, what it was executing will be "
            "recorded here.");
        return;
    }
    if (code != 0)
        Log("[bridge] enabling DRED raised 0x%08X. It is only a diagnostic, so it is "
            "left off and the session continues without it.", code);
    else
        Log("[bridge] DRED is unavailable on this Windows and driver combination.");
}

// The breadcrumb history is a list of these, and a bare number in a report
// costs whoever reads it a trip to d3d12.h. Names taken from the enum itself.
static const char *BreadcrumbOpName(unsigned op)
{
    static const char *k[] = {
        "SetMarker", "BeginEvent", "EndEvent", "DrawInstanced",
        "DrawIndexedInstanced", "ExecuteIndirect", "Dispatch", "CopyBufferRegion",
        "CopyTextureRegion", "CopyResource", "CopyTiles", "ResolveSubresource",
        "ClearRenderTargetView", "ClearUnorderedAccessView", "ClearDepthStencilView",
        "ResourceBarrier", "ExecuteBundle", "Present", "ResolveQueryData",
        "BeginSubmission", "EndSubmission", "DecodeFrame", "ProcessFrames",
        "AtomicCopyBufferUint", "AtomicCopyBufferUint64", "ResolveSubresourceRegion",
        "WriteBufferImmediate", "DecodeFrame1", "SetProtectedResourceSession",
        "DecodeFrame2", "ProcessFrames1", "BuildRaytracingAccelerationStructure",
        "EmitRaytracingAccelerationStructurePostbuildInfo",
        "CopyRaytracingAccelerationStructure", "DispatchRays",
        "InitializeMetaCommand", "ExecuteMetaCommand", "EstimateMotion",
        "ResolveMotionVectorHeap", "SetPipelineState1",
    };
    return op < _countof(k) ? k[op] : "unknown";
}

// Reads back what DRED collected. Only meaningful after the device is gone.
static void BridgeDisable(const char *why)
{
    if (g_bridge.disabled) return;
    g_bridge.disabled = true;
    Warn("stopped: %s. The game renders normally. See dlss5-bridge.log "
         "in the game folder for the detail.", why);

    // Before anything else. The game's queue may be waiting on a fence value
    // this side will never reach, and a fence can be signalled from the CPU.
    // Releasing it first also lets the drain below complete, because the D3D12
    // queue is waiting on a signal that the blocked queue owes it.
    if (g_bridge.pending_out != 0 && g_bridge.fence12 != nullptr)
    {
        Log("[bridge] releasing the game's queue from fence %llu on the way out.",
            g_bridge.pending_out);
        g_bridge.fence12->Signal(g_bridge.pending_out);
        g_bridge.pending_out = 0;
    }
    BridgeReleaseTextures();
    if (g_bridge.adapter3 != nullptr)
    { g_bridge.adapter3->Release(); g_bridge.adapter3 = nullptr; }
}

// The reason reaches the user unchanged. It used to be replaced with "the NGX
// session could not be opened", which is false at all three call sites: every
// one is downstream of the session_ready check, so the session had already
// opened. The message pointed every bug report at the init code.
// NGX answers in hex and nothing in this log said what the hex meant, so every
// report cost a trip to nvsdk_ngx_defs.h -- the same tax the DRED breadcrumbs
// charged until they were given names. Values are NVSDK_NGX_Result_Fail | n,
// taken from that header.
static const char *NgxResultName(NVSDK_NGX_Result r)
{
    if (r == NGX_SUCCESS) return "Success";
    if ((static_cast<unsigned>(r) & 0xFFF00000u) != 0xBAD00000u) return "not an NGX result";
    static const char *k[] = {
        "Fail", "FeatureNotSupported", "PlatformError", "FeatureAlreadyExists",
        "FeatureNotFound", "InvalidParameter", "ScratchBufferTooSmall",
        "NotInitialized", "UnsupportedInputFormat", "RWFlagMissing", "MissingInput",
        "UnableToInitializeFeature", "OutOfDate", "OutOfGPUMemory",
        "UnsupportedFormat", "UnableToWriteToAppDataPath", "UnsupportedParameter",
        "Denied", "NotImplemented",
    };
    const unsigned n = static_cast<unsigned>(r) & 0xFFFFFu;
    return n < _countof(k) ? k[n] : "unknown";
}

static void BridgeFail(const char *what)
{
    // Clearing this on the first failure, not the third, is what keeps the
    // promise BridgeWillDeliver makes. That decision is taken at the top of the
    // evaluate, before this frame runs, so a frame_ready left standing through
    // two more failures suppresses the game's own DLSS on frames into which
    // nothing is then written. The counter still governs when to stop; it no
    // longer governs whether the game may draw.
    g_bridge.frame_ready = false;

    if (++g_bridge.consecutive_fails >= 3)
    {
        Log("[bridge] disabled after repeated failures (%s). Game rendering is untouched.", what);
        BridgeDisable(what);
    }
}

static bool   BeginCommands();
static UINT64 EndCommands();

typedef HRESULT (WINAPI *PFN_D3DCompile_)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
                                          ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT,
                                          ID3DBlob **, ID3DBlob **);

// LoadLibraryW searches the executable's directory first, so a game shipping
// its own d3dcompiler_47.dll hands us that one -- whatever it was built from,
// years ago. The Elder Scrolls Online ships one, and renaming it away is what
// made ESO work with this add-on loaded. Microsoft's copy in System32 is the
// only one we have any reason to want: these two shaders are ours, compiled
// once per session, and nothing about them needs a game's compiler.
static PFN_D3DCompile_ LoadSystemD3DCompiler()
{
    static PFN_D3DCompile_ cached;
    static bool            tried;
    if (tried) return cached;
    tried = true;

    HMODULE m = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr,
                               LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (m == nullptr)
    {
        // Older Windows without LOAD_LIBRARY_SEARCH_SYSTEM32 support falls back
        // to the plain search, which is what this add-on did everywhere until
        // now. Say which one was taken either way.
        m = LoadLibraryW(L"d3dcompiler_47.dll");
        if (m != nullptr)
            Log("[bridge] d3dcompiler_47.dll loaded by the default search order; "
                "a copy in the game folder wins over the one in System32.");
    }
    if (m == nullptr) return nullptr;

    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(m, path, MAX_PATH);
    Log("[bridge] shader compiler: %ls", path);

    cached = reinterpret_cast<PFN_D3DCompile_>(GetProcAddress(m, "D3DCompile"));
    return cached;
}


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

    PFN_D3DCompile_ compile = LoadSystemD3DCompiler();
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

    PFN_D3DCompile_ compile = LoadSystemD3DCompiler();
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
        // The wake is not the answer; the fence is. SetEventOnCompletion cannot
        // be cancelled, so a wait that timed out earlier leaves a live
        // registration, and one auto-reset event serves all three waits here --
        // a stale signal would otherwise satisfy this one for the wrong value
        // and let the release below run while command lists still reference the
        // resources, which is exactly what this drain exists to prevent.
        if (WaitForSingleObject(g_bridge.fence_event, 5000) != WAIT_OBJECT_0 ||
            g_bridge.fence12->GetCompletedValue() < v)
            Log("[bridge] timed out draining the queue before teardown");
    }
    // Every allocator is now retired, so none of them needs waiting on again.
    for (int i = 0; i < Bridge::kFrames; ++i) g_bridge.alloc_fence[i] = 0;
}

static void BridgeReleaseTextures()
{
    BridgeDrainGpu();

    // The exposure mirror goes with the rest. Leaving it out kept a shared NT
    // handle and a D3D12 resource alive for the life of the process on a
    // session that had already told the user it was disabled.
    BridgeReleaseExposureFwd();

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

// The one place a D3D11 description is reduced to the four facts the frame path
// reads. A second backend fills the same TexFacts from its own desc type here,
// rather than teaching the dimension repairs a second struct to read.
static void TexFactsFromD3D11(const D3D11_TEXTURE2D_DESC &d, TexFacts *out)
{
    out->Width   = d.Width;
    out->Height  = d.Height;
    out->Format  = d.Format;
    out->Samples = d.SampleDesc.Count;
}

// Reads one texture slot out of the game's parameter block and returns its
// description, so the bridge can tell when BG3 has recreated its render targets.
static ID3D11Texture2D *GameTexture(const NVSDK_NGX_Parameter *p, const char *key,
                                    TexFacts *facts)
{
    ID3D11Resource *res = nullptr;
    if (p->Get(key, &res) != NGX_SUCCESS || res == nullptr) return nullptr;
    ID3D11Texture2D *tex = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex))) ||
        tex == nullptr)
        return nullptr;
    if (facts != nullptr)
    {
        D3D11_TEXTURE2D_DESC desc = {};
        tex->GetDesc(&desc);
        TexFactsFromD3D11(desc, facts);
    }
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
        // The adapter, its ids and the driver are recorded at the first DLSS
        // call, which every game reaches before this point. Repeating them here
        // is four lines of nothing.

        // Windows resets a GPU that has not answered within TdrDelay seconds.
        // The default is 2, but it is a registry value and people change it, so
        // a stall figure in this log cannot be read against a guess.
        {
            DWORD delay = 0, ddi = 0, cb = sizeof(DWORD);
            const LSTATUS a1 = RegGetValueW(HKEY_LOCAL_MACHINE,
                L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
                L"TdrDelay", RRF_RT_REG_DWORD, nullptr, &delay, &cb);
            cb = sizeof(DWORD);
            const LSTATUS a2 = RegGetValueW(HKEY_LOCAL_MACHINE,
                L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
                L"TdrDdiDelay", RRF_RT_REG_DWORD, nullptr, &ddi, &cb);
            Log("[bridge] GPU reset timeout: TdrDelay %s, TdrDdiDelay %s",
                a1 == ERROR_SUCCESS ? "set" : "default 2 s",
                a2 == ERROR_SUCCESS ? "set" : "default 5 s");
            if (a1 == ERROR_SUCCESS) Log("[bridge]   TdrDelay = %lu s", delay);
            if (a2 == ERROR_SUCCESS) Log("[bridge]   TdrDdiDelay = %lu s", ddi);
        }

        adapter->QueryInterface(__uuidof(IDXGIAdapter3),
                                reinterpret_cast<void **>(&g_bridge.adapter3));
        LogVideoMemory("at session open");

    }

    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    auto create_device = d3d12 != nullptr
        ? reinterpret_cast<PFN_D3D12CreateDevice_>(GetProcAddress(d3d12, "D3D12CreateDevice"))
        : nullptr;
    if (create_device == nullptr) { Log("[bridge] no D3D12CreateDevice"); goto fail; }

    // Prey 2017 faults inside D3D12Core.dll during this call, on the same driver
    // that four other titles open a session on without trouble. It is also the
    // only reported process carrying two third-party NGX exporters besides the
    // driver and the snippets -- a winmm proxy injector and the Luma add-on --
    // and an injector that has hooked device creation would fault here exactly
    // like this. Whether the function has been patched before us is one line to
    // establish and was never printed.
    LogPrologue("D3D12CreateDevice", reinterpret_cast<const BYTE *>(create_device));
    Log("[bridge]   ReShade hooks this function by design, so a jump here is normal "
        "and Baldur's Gate 3 shows one while working. What is worth comparing "
        "between reports is the bytes, not the fact of a jump: a second hook on "
        "top of ReShade's is what a game-side injector would leave.");
    if (g_cfg.dred != 0) EnableDred(d3d12);
    else Log("[bridge] DRED is off by config; a GPU reset will not be explained.");

    {
        DWORD dcode = 0;
        HRESULT hr = SafeCreateDevice(create_device, adapter, &g_bridge.dev12, &dcode);
        // A retry lived here that announced "turning DRED off and trying once
        // more" and then called the identical function under the identical
        // process-global setting: D3D12_DRED_ENABLEMENT_DISABLED is written
        // nowhere in this add-on. Had the second attempt happened to succeed it
        // would have reported DRED as the cause of a configuration that never
        // existed. dred=0 is the only thing that produces a DRED-off attempt.
        if (dcode != 0)
        {
            {
                wchar_t owner[MAX_PATH] = {};
                if (NameFaultOwner(owner, MAX_PATH))
                    Log("[bridge]   the fault was at %p, inside %ls",
                        g_last_fault_at, owner);
            }
            Log("[bridge] D3D12CreateDevice raised 0x%08X inside the D3D12 runtime. "
                "Nothing this add-on can do about that, so it stops here and the "
                "game keeps its own rendering.%s", dcode,
                g_cfg.dred != 0
                    ? " DRED was requested for this session; set dred=0 in "
                      "dlss5-bridge.cfg to make an attempt without it."
                    : " DRED was off for this attempt.");
            goto fail;
        }
        if (FAILED(hr) || g_bridge.dev12 == nullptr)
        { Log("[bridge] D3D12CreateDevice failed 0x%08X", hr); goto fail; }

        // The D3D11 synthetic transport comes through here rather than through
        // Synth12Open, so the bootstrap has to be at both call sites. A game with
        // no DLSS of its own has never loaded NGX -- nothing had a reason to --
        // and this is where Skyrim Special Edition stopped on 2026-08-30 with the
        // contract already built. Guarded on the source: on the mirror path,
        // nothing exporting NGX means the game never called DLSS, and that is a
        // fact to report rather than one to paper over by loading NGX for it.
        HMODULE ngx = FindNgxLoader();
        if (ngx == nullptr && g_source == SRC_SYNTH && EnsureNgxLoaderLoaded())
            ngx = FindNgxLoader();
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

        // NGX is told where the application data lives. This was the add-on's own
        // folder unconditionally, which is the game's folder in every title reported
        // so far -- but ReShade can load add-ons from elsewhere, and Phantasy Star
        // Online 2 is the first report where it does. There the NGX snippets sit
        // beside the executable, and that session came up with every capability
        // reading zero on an RTX 5080, which that card does not otherwise do.
        //
        // Whether the data path causes that is not established. What is established
        // is that pointing NGX at a folder holding none of its files was never
        // intended, so the executable's folder wins when the two differ, and both
        // are logged so a report can settle it.
        wchar_t data_path[MAX_PATH] = {};
        wchar_t self_dir[MAX_PATH] = {};
        GetModuleFileNameW(g_self, self_dir, MAX_PATH);
        if (wchar_t *sd = wcsrchr(self_dir, (wchar_t)92)) *(sd + 1) = 0;

        wchar_t exe_dir[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe_dir, MAX_PATH);
        if (wchar_t *ed = wcsrchr(exe_dir, (wchar_t)92)) *(ed + 1) = 0;

        wcscpy_s(data_path, _wcsicmp(self_dir, exe_dir) == 0 ? self_dir : exe_dir);
        Log("[bridge] NGX data path: %ls", data_path);
        if (_wcsicmp(self_dir, exe_dir) != 0)
            Log("[bridge]   the add-on itself is in %ls. The game's folder is used "
                "because that is where the NGX snippets are.", self_dir);

        // The SDK version constant is not documented and has moved between NGX
        // releases, so it is negotiated rather than assumed. Hardcoding the one
        // value that worked on the machine this was written on made the whole
        // session fail on a different driver. Init_ProjectID is tried as well:
        // some drivers accept one entry point and fault on the other.
        auto init_project = reinterpret_cast<PFN_Init_ProjectID>(
            GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init_ProjectID"));

        // Nothing may patch a module between here and the end of the init: NGX
        // loads its own snippets inside these calls, and the loader notification
        // that fires for them arrives on this very thread.
        g_ngx_init_in_flight = true;

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
                    {
                        wchar_t owner[MAX_PATH] = {};
                        if (NameFaultOwner(owner, MAX_PATH))
                            Log("[bridge] Init_Ext(version=0x%02X) raised 0x%08X at %p, inside %ls",
                                ver, code, g_last_fault_at, owner);
                        else
                            Log("[bridge] Init_Ext(version=0x%02X) raised 0x%08X", ver, code);
                    }
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
                    {
                        wchar_t owner[MAX_PATH] = {};
                        if (NameFaultOwner(owner, MAX_PATH))
                            Log("[bridge] Init_ProjectID(version=0x%02X) raised 0x%08X at %p, inside %ls",
                                ver, code, g_last_fault_at, owner);
                        else
                            Log("[bridge] Init_ProjectID(version=0x%02X) raised 0x%08X", ver, code);
                    }
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
        g_ngx_init_in_flight = false;

        if (faulted && g_ngx_proxy[0] != 0)
        {
            Log("[bridge] %ls exports the NGX entry points as well and is not the "
                "driver's loader, so an NGX proxy is running in this process. Such a "
                "proxy patches code inside the driver's NGX module and opens its own "
                "session on it before this one. Prey 2017 with OptiScaler installed as "
                "WINMM.dll is the reported case, and it is the difference from titles "
                "where this same driver and module initialise. Removing the proxy is "
                "the test that would settle it.", g_ngx_proxy);
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
            else if (rc != NGX_SUCCESS)
                Log("[bridge] capability query failed 0x%08X, %s", rc, NgxResultName(rc));
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
        { Log("[bridge] AllocateParameters failed 0x%08X, %s", rp, NgxResultName(rp)); goto fail; }

        // DRED reports a hung GPU by naming the command list and queue that
        // stopped. Unnamed objects come back as blanks, which is useless in a
        // process holding several of each.
        if (g_bridge.queue != nullptr) g_bridge.queue->SetName(L"dlss5-bridge queue");
        if (g_bridge.list  != nullptr) g_bridge.list->SetName(L"dlss5-bridge list");

        Log("[bridge] session ready: queue=%p list=%p fence12=%p fence11=%p (%d frames in flight)",
            static_cast<void *>(g_bridge.queue), static_cast<void *>(g_bridge.list),
            static_cast<void *>(g_bridge.fence12), static_cast<void *>(g_bridge.fence11),
            Bridge::kFrames);
        Log("############# bridge: session open #############");
        Log("");
        if (adapter != nullptr) adapter->Release();
        // The queue, the list and the event were created without checking any
        // of them, and every one is dereferenced on the acting path: the queue
        // in ExecuteCommandLists on the very first frame, the list in Reset, and
        // a null event is worse than a crash -- SetEventOnCompletion on a null
        // handle blocks the render thread with no timeout. The teardown path
        // already null-checks exactly these three; the guard existed where
        // nothing acts and was missing where everything does.
        if (g_bridge.queue == nullptr || g_bridge.list == nullptr ||
            g_bridge.fence_event == nullptr)
        {
            Log("[bridge] the command queue, list or fence event could not be "
                "created, so the session is not opened.");
            goto fail;
        }

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

    if (g_bridge.dev12 != nullptr)
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {};
        fs.Format = rd.Format;
        if (SUCCEEDED(g_bridge.dev12->CheckFeatureSupport(
                D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))))
        {
            const bool sample = (fs.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) != 0;
            const bool uav    = (fs.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0;
            const bool load   = (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0;
            const bool store  = (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
            Log("[bridge]   fmt %u support: sample %s, typed UAV %s, UAV load %s, UAV store %s",
                rd.Format, sample ? "yes" : "NO", uav ? "yes" : "NO",
                load ? "yes" : "NO", store ? "yes" : "NO");
            if (i == SLOT_OUTPUT && !store)
                Log("[bridge]   *** the driver does not support typed UAV stores in this "
                    "format, and DLSS writes its result through one ***");
        }
    }
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
    {
        wchar_t n[64];
        _snwprintf_s(n, _TRUNCATE, L"dlss5-bridge %hs", kSlotName[i]);
        g_bridge.tex12[i]->SetName(n);
    }
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
    {
        Log("[bridge] %s: D3D11->D3D12 path failed 0x%08X", kSlotName[i], hr);
        // The caller may retry this slot with another format, and the retry
        // overwrites both pointers. Let go of them here or they are lost.
        if (g_bridge.tex11[i]  != nullptr) { g_bridge.tex11[i]->Release();  g_bridge.tex11[i]  = nullptr; }
        if (g_bridge.shared[i] != nullptr) { CloseHandle(g_bridge.shared[i]); g_bridge.shared[i] = nullptr; }
        return false;
    }

    {
        wchar_t n[64];
        _snwprintf_s(n, _TRUNCATE, L"dlss5-bridge %hs", kSlotName[i]);
        g_bridge.tex12[i]->SetName(n);
    }
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

// One field, three meanings: g_cfg.flags carries the user's forced value, -1 for
// "copy whoever is driving", and 107 for the default this add-on shipped before
// 1.0.16 and now treats as unset. A second contract source would be a fourth
// meaning in the same int, and the log line that reports the value would have no
// way left to say where it came from -- which is the failure the note above
// NoteAbsentParam names. So the origin is returned alongside the value and the
// label is read off it, rather than reconstructed from the config at the point
// of printing.
enum FlagsFrom { FLAGS_NONE, FLAGS_GAME, FLAGS_SYNTH, FLAGS_CONFIG };

static const char *kFlagsFromLabel[] = {
    // FLAGS_NONE is 107 with nobody behind it -- no source and no config -- and
    // says "forced by config" because that is what it said before this function
    // existed, and a log line that changes meaning breaks every comparison
    // against an older session.
    "sent to D3D12, forced by config",
    "sent to D3D12, copied from the game",
    "sent to D3D12, built by the synthetic contract",
    "sent to D3D12, forced by config",
};

// What actually went into the parameter block NGX was handed, and which of the
// four origins above put it there. Both are locals at the two places that Set
// the key -- BridgeBuildResources here and Synth12Build in synth.inc -- and the
// status panel has to name them from ReShade's UI thread, where dereferencing
// either parameter block to ask again is not allowed. So they are published
// here instead of read back there. -1 means no create has happened yet, which
// is a different answer from flags 0 and the panel says so.
//
// Written by whichever thread built the feature and read by the UI thread with
// no lock: a LONG and a pointer to a string literal, both aligned, so a reader
// sees one value or the other and never half of one. The literal is static
// storage that outlives the process, so the pointer cannot go stale.
static volatile LONG  g_panel_flags = -1;
static const char    *g_panel_flags_from;

// Precedence: an explicit config value, else the active source's own, else 107.
// FLAGS_SYNTH has no producer in this build; it is declared so the value and its
// label arrive together the first time one exists, instead of being retrofitted
// into a field that already carries too much.
static unsigned int ResolveCreateFlags(const NVSDK_NGX_Parameter *gp, FlagsFrom *from)
{
    if (g_cfg.flags >= 0 && g_cfg.flags != 107)
    {
        *from = FLAGS_CONFIG;
        return static_cast<unsigned int>(g_cfg.flags);
    }

    // A source that is present is the source, whether or not it declared this
    // particular key: 107 stands in for the key, not for the source. Whether the
    // game declared it is a separate question, and the line above this one --
    // "the game's create flags" -- is what answers it.
    if (gp != nullptr)
    {
        unsigned int v = 0;
        // Which source, asked of g_source rather than reconstructed from the
        // value. A synthetic block answers this Get and would otherwise be
        // labelled "copied from the game" in a session where there is no game
        // to copy from -- a log line stating something it did not derive, and
        // the one thing the flags storage was split per source to prevent.
        // Absence still means 107 here, so the synthetic block being complete
        // is the self-check's business, not this line's.
        *from = (g_source == SRC_SYNTH) ? FLAGS_SYNTH : FLAGS_GAME;
        return gp->Get("DLSS.Feature.Create.Flags", &v) == NGX_SUCCESS ? v : 107u;
    }

    *from = FLAGS_NONE;
    return 107u;
}

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
        // MV needs a UAV only on the fallback path, where a compute pass
        // writes the shared copy instead of CopyResource.
        const bool        needs_uav  = is_depth || (i == SLOT_OUTPUT) || (i == SLOT_MV);
        // Every mirror is sized from the texture it mirrors. Depth and motion
        // vectors used to be sized from Color, which is only correct while all
        // three agree: a game supplying display-resolution motion vectors -- the
        // configuration the MVLowRes create flag exists to distinguish -- got a
        // Color-sized mirror, CopyResource silently refused the mismatch, and
        // DLSS then read a motion-vector texture nobody had ever written.
        const UINT        tw = g_bridge.slot_w[i];
        const UINT        th = g_bridge.slot_h[i];

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
    // The quality preset is the game's statement of which DLSS model it asked
    // for, and it was hardcoded to 2. That is Baldur's Gate 3's own answer
    // again -- the same mistake as the create flags, in the line above them.
    // Assetto Corsa and Gallipoli both ask for 1 and were given 2.
    if (gp != nullptr) CopyUIntParam(gp, "PerfQualityValue", 2u);
    else               g_bridge.params->Set("PerfQualityValue", 2);
    {
        unsigned int q = 0, asked_q = 0;
        g_bridge.params->Get("PerfQualityValue", &q);
        static const char *kQ[] = { "MaxPerf", "Balanced", "MaxQuality",
                                    "UltraPerformance", "UltraQuality", "DLAA" };
        const bool have_q =
            gp != nullptr && gp->Get("PerfQualityValue", &asked_q) == NGX_SUCCESS;
        Log("[bridge] quality preset %u (%s), %s", q,
            q < _countof(kQ) ? kQ[q] : "unknown",
            have_q ? "copied from the game" : "the game set none, so 2 is used");
    }

    // The create flags describe the game's own buffers: whether the colour is
    // HDR, whether the depth is reversed, whether the motion vectors are at
    // render resolution. Forcing a value here invents that description. The
    // default 107 was Baldur's Gate 3's own answer, so it was right there and
    // nowhere else: Assetto Corsa asks for 2, and was being told its SDR colour
    // is HDR and its normal depth is inverted. Copy it, and let the config
    // force a value only when it is set to something other than -1.
    // 107 was this add-on's own default until 1.0.15, so a configuration file
    // carrying it is repeating that default rather than stating a choice --
    // and it keeps every existing installation on the old behaviour, where the
    // correction reaches only whoever reads the log. Trails beyond the Horizon
    // showed the cost: the game asks for 75, 107 adds DoSharpening, and the
    // game had set Sharpness to 1.0 without ever enabling it. The old default
    // is treated as unset, and the substitution is announced.
    const bool old_default = (g_cfg.flags == 107);

    FlagsFrom          flags_from = FLAGS_NONE;
    const unsigned int resolved   = ResolveCreateFlags(gp, &flags_from);
    const bool         copying    = (flags_from == FLAGS_GAME);

    g_bridge.params->Set("DLSS.Feature.Create.Flags", resolved);
    {
        unsigned int used = 0;
        g_bridge.params->Get("DLSS.Feature.Create.Flags", &used);
        unsigned int asked = 0;
        const bool have_asked =
            gp != nullptr && gp->Get("DLSS.Feature.Create.Flags", &asked) == NGX_SUCCESS;

        if (have_asked) LogCreateFlags("the game's create", asked);
        LogCreateFlags(kFlagsFromLabel[flags_from], used);
        // Published for the panel from the same value the line above prints, so
        // the two can never disagree about what was sent.
        // Origin first, then the value: the interlocked write is the barrier, so
        // a reader that sees a value has already been able to see the label that
        // goes with it.
        g_panel_flags_from = kFlagsFromLabel[flags_from];
        InterlockedExchange(&g_panel_flags, static_cast<LONG>(used));

        if (old_default && copying)
            Log("[bridge]   dlss5-bridge.cfg says flags=107, which was this "
                "add-on's default before 1.0.16 rather than a description of this "
                "game. It is being ignored in favour of the game's own value. To "
                "force it, use flags=108 -- any value that is not 107 or -1 is "
                "taken as deliberate.");

        if (!copying && have_asked && asked != used)
            Log("[bridge]   these differ. Set flags=-1 in dlss5-bridge.cfg to "
                "copy the game's own value; the file is re-read while playing.");

        // DLSS applies its sharpening pass only when DoSharpening is set, and
        // reads its strength from Sharpness whether or not the game meant it to
        // be used. Adding the flag over a game that set one and not the other
        // turns on a pass it deliberately left off.
        if ((used & 0x20) != 0 && have_asked && (asked & 0x20) == 0)
        {
            float sharp = 0.0f;
            gp->Get("Sharpness", &sharp);
            Log("[bridge]   DoSharpening is set here but not by the game, and "
                "Sharpness is %.2f. DLSS will sharpen at that strength; the game's "
                "own DLSS would not have.", sharp);
        }

        // IsHDR is the flag the DLSS 5 add-on reads to choose between its HDR and
        // its SDR codec, so asserting it over an 8-bit buffer sends it down the
        // wrong path without any call failing.
        if ((used & 0x01) != 0 && FormatIsLowPrecisionUnorm(g_bridge.fmt[SLOT_COLOR]))
            Log("[bridge]   IsHDR is set, but the game's colour buffer is 8-bit "
                "normalised (fmt=%u) and cannot hold a value above 1.0. The DLSS 5 "
                "add-on selects its codec from this flag.",
                static_cast<unsigned>(g_bridge.fmt[SLOT_COLOR]));
    }
    // Mirror the game's own answer where it gave one. Deus Ex: Mankind Divided
    // asks for 0 here while BG3 asks for 1, and forcing either on the other is
    // inventing part of a contract that is meant to be copied.
    if (gp != nullptr)
        CopyUIntParam(gp, "DLSS.Enable.Output.Subrects",
                      static_cast<unsigned int>(g_cfg.subrects));
    else
        g_bridge.params->Set("DLSS.Enable.Output.Subrects", g_cfg.subrects);
    // The render preset picks which DLSS model weights run, and the D3D12 feature
    // was getting NGX's default while the game's own D3D11 feature ran whatever
    // it asked for -- so the two sides upscaled with different models and the
    // neural-rendering add-on worked from ours. Prey 2017 reaches DLSS through
    // Luma, which sets these; so do DLSSTweaks and OptiScaler, and so do plenty
    // of games directly.
    //
    // nvsdk_ngx_helpers.h does not write these keys at all, so unlike MV.Scale
    // there is no neutral to fall back on: absence means NVSDK_NGX_DLSS_Hint_
    // Render_Preset_Default, which is what an unset key already gives. Mirror
    // only what the game set. Key strings are from nvsdk_ngx_defs.h.
    if (gp != nullptr)
    {
        static const char *preset_keys[] = {
            "DLSS.Hint.Render.Preset.DLAA",
            "DLSS.Hint.Render.Preset.Quality",
            "DLSS.Hint.Render.Preset.Balanced",
            "DLSS.Hint.Render.Preset.Performance",
            "DLSS.Hint.Render.Preset.UltraPerformance",
            "DLSS.Hint.Render.Preset.UltraQuality",
        };
        for (const char *k : preset_keys)
        {
            unsigned int v = 0;
            if (gp->Get(k, &v) != NGX_SUCCESS) continue;
            g_bridge.params->Set(k, v);
            if (!g_bridge.presets_reported)
                Log("[bridge] %s = %u, copied from the game", k, v);
        }
        g_bridge.presets_reported = true;
    }

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
        if (WaitForSingleObject(g_bridge.fence_event, 4000) != WAIT_OBJECT_0 ||
            g_bridge.fence12->GetCompletedValue() < v)
        {
            Log("[bridge] feature creation did not complete within 4 s; giving up.");
            BridgeDisable("creating the DLSS feature crashed");
            return false;
        }
    }

    if (code != 0)
    {
        // What was observed, first, and on its own line: SafeCreateFeature runs
        // under CaptureFault, so the faulting address is known and the module
        // that owns it can be named. Which module it is decides the diagnosis --
        // the driver's own loader, a snippet sitting in the game folder, or
        // another add-on's detour in front of the call are three different
        // problems -- and none of them can be told apart from a sentence that
        // names a cause instead of an address.
        wchar_t who[MAX_PATH + 64] = {};
        Warn("creating the DLSS feature raised exception 0x%08X.", code);
        if (NameFaultOwner(who, _countof(who)))
            Log("[bridge]   it faulted in %ls", who);
        else
            Log("[bridge]   no faulting address was recorded, so the module cannot "
                "be named.");

        // Offered as something to try, not as the cause. A reporter on 2026-08-31
        // made every nvngx_*.dll on the machine the same 310.8.0.0 and the create
        // still faulted, so a version mismatch is one explanation among several
        // rather than the answer.
        Log("[bridge]   NGX loads its feature snippets from the game folder before the");
        Log("[bridge]   driver's own copy, so an nvngx_*.dll dropped in there is worth");
        Log("[bridge]   removing as a test. It is not established as the cause: the same");
        Log("[bridge]   fault has been reported with every copy on the machine matching.");
        BridgeDisable("the DLSS feature could not be created");
        return false;
    }
    if (rf != NGX_SUCCESS || g_bridge.feature == nullptr)
    {
        Log("[bridge] CreateFeature failed with result 0x%08X, %s", rf, NgxResultName(rf));
        if (rf == static_cast<NVSDK_NGX_Result>(0xBAD0000B))
            Log("[bridge]   NGX could not build the feature from the create "
                "parameters above. Those are the width, height, output size, "
                "creation flags and quality preset logged just before this line.");
        return false;
    }

    Log("[bridge] feature ready: render %ux%u -> output %ux%u  (textures in %ux%u out %ux%u) handle=%p",
        g_bridge.render_w, g_bridge.render_h, g_bridge.ngx_out_w, g_bridge.ngx_out_h,
        w, h, g_bridge.out_width, g_bridge.out_height, static_cast<void *>(g_bridge.feature));

    // A DLSS 5 add-on captures its contract by detouring
    // NVSDK_NGX_D3D12_CreateFeature and reading the create it sees. It arms when
    // NGX appears -- and on the synthetic path NGX appears because THIS add-on
    // loaded it, so the two race and the create can win. Skyrim Special Edition
    // on 2026-08-30: create at 22:37:44.856, the other add-on's hook at .883.
    // Twenty-seven milliseconds, and the neural pass never saw a contract.
    //
    // The prologue is taken here so a later change can be seen. A detour is
    // written over the first bytes of the export, so those bytes changing is
    // that add-on arriving -- which is a fact about memory rather than a guess
    // about how long it takes.
    if (!g_bridge.create_prologue_taken && g_bridge.create_feature != nullptr)
    {
        memcpy(g_bridge.create_prologue,
               reinterpret_cast<const void *>(g_bridge.create_feature),
               sizeof(g_bridge.create_prologue));
        g_bridge.create_prologue_taken = true;
    }

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
        if (WaitForSingleObject(g_bridge.fence_event, 2000) != WAIT_OBJECT_0 ||
            g_bridge.fence12->GetCompletedValue() < retire)
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

// Frame N makes the game's queue wait for the D3D12 side to reach v_out. That
// wait cannot be cancelled and has no timeout, so if the work behind it never
// completes the game presents nothing ever again -- while its own loop, audio
// and input carry on, which is what a frozen image with a live character is.
// The 2 s check in BeginCommands cannot catch it: that code runs on the render
// thread, and the render thread is what the block puts to sleep inside Present.
//
// So the value is remembered and checked on the frames that follow, while the
// game is still submitting. Being behind by a frame or two is ordinary
// pipelining and is what keeps the CPU free; being behind by half a second is
// not. An ID3D12Fence can be signalled from the CPU, and that releases the
// game's queue whether or not the work ever arrives.
// ID3D12Fence::GetCompletedValue returns UINT64_MAX for exactly one reason: the
// device has been removed. Escape from Tarkov produced it mid-session, and
// nothing noticed -- the bridge went on copying its last good Output over the
// game's every frame, so the world froze while the interface, drawn after that
// copy, kept updating. Opening the inventory appeared to fix it.
static void LogDred()
{
    if (g_bridge.dev12 == nullptr) return;

    ID3D12DeviceRemovedExtendedData *dred = nullptr;
    if (FAILED(g_bridge.dev12->QueryInterface(__uuidof(ID3D12DeviceRemovedExtendedData),
                                              reinterpret_cast<void **>(&dred))) ||
        dred == nullptr)
    {
        Log("  DRED did not attach, so what the GPU was executing cannot be recovered.");
        return;
    }

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc = {};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc)))
    {
        int shown = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE *node = bc.pHeadAutoBreadcrumbNode;
             node != nullptr && shown < 8; node = node->pNext, ++shown)
        {
            const UINT done = node->pLastBreadcrumbValue != nullptr
                            ? *node->pLastBreadcrumbValue : 0;
            Log("  breadcrumb: list %ls on queue %ls finished %u of %u operations",
                node->pCommandListDebugNameW != nullptr
                    ? node->pCommandListDebugNameW : L"(unnamed)",
                node->pCommandQueueDebugNameW != nullptr
                    ? node->pCommandQueueDebugNameW : L"(unnamed)",
                done, node->BreadcrumbCount);

            // The operation at the completed count is the one still running when
            // the GPU stopped, which is the whole point of asking.
            if (node->pCommandHistory != nullptr && done < node->BreadcrumbCount)
            {
                Log("    stopped inside operation %u, a %s.", done,
                    BreadcrumbOpName(static_cast<unsigned>(node->pCommandHistory[done])));

                const UINT from = done > 12 ? done - 12 : 0;
                const UINT to   = done + 4 < node->BreadcrumbCount
                                ? done + 4 : node->BreadcrumbCount;
                Log("    operations %u to %u:", from, to - 1);
                for (UINT k = from; k < to; ++k)
                    Log("      %3u %s%s", k,
                        BreadcrumbOpName(static_cast<unsigned>(node->pCommandHistory[k])),
                        k == done ? "   <== stopped here" : "");
            }
        }
        if (bc.pHeadAutoBreadcrumbNode != nullptr)
            Log("  This add-on records four barriers, hands the list to NGX between "
                "a BeginEvent and an EndEvent, then records four more. An operation "
                "that is neither among the first five nor the last five belongs to "
                "NGX rather than to this add-on.");

        if (bc.pHeadAutoBreadcrumbNode == nullptr)
            Log("  no breadcrumbs were recorded, so the reset did not happen inside "
                "a command list this add-on submitted.");
    }

    D3D12_DRED_PAGE_FAULT_OUTPUT pf = {};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf)) && pf.PageFaultVA != 0)
    {
        Log("  a page fault at GPU virtual address 0x%llX caused the reset.",
            static_cast<unsigned long long>(pf.PageFaultVA));
        int shown = 0;
        for (const D3D12_DRED_ALLOCATION_NODE *a = pf.pHeadExistingAllocationNode;
             a != nullptr && shown < 6; a = a->pNext, ++shown)
            Log("    inside live allocation %ls",
                a->ObjectNameW != nullptr ? a->ObjectNameW : L"(unnamed)");
        shown = 0;
        for (const D3D12_DRED_ALLOCATION_NODE *a = pf.pHeadRecentFreedAllocationNode;
             a != nullptr && shown < 6; a = a->pNext, ++shown)
            Log("    inside recently freed allocation %ls -- freed while the GPU "
                "was still reading it", a->ObjectNameW != nullptr ? a->ObjectNameW : L"(unnamed)");
    }
    dred->Release();
}

static bool BridgeDeviceLost()
{
    if (g_bridge.fence12 == nullptr) return false;
    if (g_bridge.fence12->GetCompletedValue() != UINT64_MAX) return false;

    LogVideoMemory("at the moment the device was lost");

    const HRESULT why = g_bridge.dev12 != nullptr
                      ? g_bridge.dev12->GetDeviceRemovedReason() : 0;
    Log("[bridge] the D3D12 device has been removed (reason 0x%08X).", why);
    switch (why)
    {
    case DXGI_ERROR_DEVICE_HUNG:
        Log("  The GPU stopped responding to work submitted on it -- a TDR."); break;
    case DXGI_ERROR_DEVICE_REMOVED:
        Log("  The adapter was removed or reset underneath the process."); break;
    case DXGI_ERROR_DEVICE_RESET:
        Log("  The device was reset after an invalid command."); break;
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
        Log("  The driver reported an internal error."); break;
    default: break;
    }
    LogDred();
    Log("  Nothing can be recovered from here, and continuing would copy a frozen");
    Log("  image over the game's own every frame. Stopping.");
    BridgeDisable("the D3D12 device was removed");
    return true;
}

static void BridgeRescueStalledQueue()
{
    // Once the bridge has stopped there is nothing left to rescue, and without
    // this the device-removed diagnosis below repeats on every later frame.
    if (g_bridge.disabled) return;
    if (g_bridge.fence12 == nullptr) return;
    if (BridgeDeviceLost()) return;

    const UINT64 done = g_bridge.fence12->GetCompletedValue();
    const ULONGLONG now = GetTickCount64();

    // The only honest measure of "the GPU stopped completing" is that the
    // completed value has stopped moving. Being a frame or two behind is
    // pipelining and is the state this bridge is built to keep; two earlier
    // attempts here timed the wrong quantity and were wrong in both directions
    // -- one that could never fire while frames arrived, then one that fired
    // while Baldur's Gate 3 was zero frames behind at 120 fps.
    if (done != g_bridge.last_completed)
    {
        g_bridge.last_completed = done;
        g_bridge.pending_since  = now;
        if (g_bridge.pending_out != 0 && done >= g_bridge.pending_out)
            g_bridge.pending_out = 0;
        return;
    }

    // Nothing outstanding means nothing to rescue, however long it has been.
    if (g_bridge.pending_out == 0 || done >= g_bridge.pending_out) return;
    if (now - g_bridge.pending_since < 500) return;

    Log("[bridge] the D3D12 side has completed nothing for 500 ms while the game's");
    Log("  queue waits on fence %llu (completed %llu). Releasing that wait from the",
        g_bridge.pending_out, done);
    Log("  CPU so the game can present again. The neural pass runs inside that work,");
    Log("  so whatever stopped is there rather than in the transport.");
    BridgeDisable("the D3D12 side stopped completing");
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
static void CopyUIntParam(const NVSDK_NGX_Parameter *src, const char *key, unsigned int fallback)
{
    unsigned int v = 0;
    if (src->Get(key, &v) != NGX_SUCCESS) v = fallback;
    g_bridge.params->Set(key, v);
}


// A note for whoever adds a second source of contract to this file.
//
// Everything from here down to MirrorFloatParam exists to tell "the game
// declared X" apart from "the game declared nothing". That distinction is a
// signal only while absence is abnormal. A synthetic contract -- ReShade depth
// and estimated motion vectors, for a game that ships no DLSS of its own -- is a
// legitimate second source, but it has to arrive already complete: every key set
// explicitly by whatever builds it, and no call into the helpers below. Route one
// through them and absence becomes a normal operating mode, NoteAbsentParam turns
// into noise on half the frames, and the report that found the MV.Scale defect
// stops being able to find the next one.
//
// The same holds for the size fallback in BridgeFrame, where absent scalars are
// derived from the Color and Output descriptors. That is a last-resort repair for
// a game that under-declares, not a supply route.
//
// DLSS.Feature.Create.Flags needs storage per source rather than one field. It
// already carries two meanings -- the game's declared value, and 107 standing for
// unset -- and adding an authoritative third from a synthetic source or a user
// override to the same field is how this acquires a defect it cannot diagnose.

// A parameter the game did not set is not a parameter whose default this bridge
// gets to choose. Trails beyond the Horizon sets no MV.Scale, and the -1.0
// invented for it told DLSS to reverse every motion vector: the temporal
// history is then gathered from the wrong side of the movement, which is the
// smeared ground and foliage reported there. Whatever NGX does with an unset
// parameter is exactly what the game's own DLSS call gets, so leaving it unset
// is the only way to match. These mirror; they do not fill in.
//
// For the mirror an absent key is news about the game. For a synthetic contract
// it is news about this add-on: the note above requires such a contract to
// arrive already complete, so arriving here at all means the synthesiser left a
// key unset and the value that follows was chosen by nobody. Same call site,
// opposite meaning, so it cannot be the same sentence.
static void NoteAbsentParam(const char *key)
{
    static const char *seen[32];
    static int         count;
    static LONG        seen_source = SRC_NONE;

    // Thirty-two slots between every source there will ever be. One source that
    // runs first fills them and the next one's report comes out empty, which
    // reads exactly like a complete contract. Start the table again instead:
    // both accounts are wanted in full, and a repeat after a handover is the
    // point rather than noise.
    const LONG src = g_source;
    if (src != seen_source) { count = 0; seen_source = src; }

    for (int i = 0; i < count; ++i)
        if (strcmp(seen[i], key) == 0) return;
    if (count < static_cast<int>(_countof(seen))) seen[count++] = key;

    if (src == SRC_SYNTH)
    {
        Log("[synth] DEFECT: the synthetic contract sets no %s, and this path exists "
            "to describe a game that under-declares. A contract built here is built "
            "complete or not at all, so the neutral now being sent is standing in for "
            "a decision nothing made.", key);
        return;
    }
    Log("[bridge] the game sets no %s; the neutral from nvsdk_ngx_helpers.h is sent "
        "instead, which is what a game using the official helper would send.", key);
}

// Sends the game's value where it has one, and the neutral from
// nvsdk_ngx_helpers.h where it does not -- which is what every game using the
// official helper sends. Absence is still reported once per key, because that
// report is what found the MV.Scale defect in the first place.
static void NeutralFloatParam(const NVSDK_NGX_Parameter *src, const char *key,
                              float neutral)
{
    float v = 0.0f;
    if (src->Get(key, &v) != NGX_SUCCESS || v == 0.0f)
    {
        NoteAbsentParam(key);
        v = neutral;
    }
    g_bridge.params->Set(key, v);
}

static void NeutralUIntParam(const NVSDK_NGX_Parameter *src, const char *key,
                             unsigned int neutral)
{
    unsigned int v = 0;
    if (src->Get(key, &v) != NGX_SUCCESS) { NoteAbsentParam(key); v = neutral; }
    g_bridge.params->Set(key, v);
}

// Jitter and sharpness are the two the helper passes straight through with no
// substitution, so there is no neutral to fall back on and a wrong value is
// worse than none. Every game that runs DLSS sets jitter; sharpness matters
// only with DoSharpening, which such a game also sets.
static void MirrorFloatParam(const NVSDK_NGX_Parameter *src, const char *key)
{
    float v = 0.0f;
    if (src->Get(key, &v) != NGX_SUCCESS) { NoteAbsentParam(key); return; }
    g_bridge.params->Set(key, v);
}

// Mount & Blade II: Bannerlord creates its feature with flags 59 -- IsHDR,
// MVLowRes, DepthInverted, DoSharpening -- and no AutoExposure, then supplies a
// 1x1 ExposureTexture on every evaluate. The bridge copied the flags faithfully
// and mirrored four textures, so its D3D12 feature ran with auto exposure off
// and no exposure input at all: DLSS accumulated without knowing the exposure,
// which is ghosting. Before 1.0.16 the forced flags=107 carried AutoExposure and
// hid this; copying the game's flags removed that cover.
//
// Kept out of the slot array on purpose. Games that supply no exposure texture
// are the common case and every SLOT_COUNT loop would have to learn about a
// null.
static void BridgeReleaseExposure()
{
    // NGX does not AddRef the resources in a parameter block -- that is why
    // games must keep them alive across the call -- and this block is allocated
    // once per session and never Reset. Freeing the texture without clearing
    // the key leaves the driver a freed pointer on the very next evaluate.
    // The drain is this file's own rule: a submitted command list may still
    // reference the surface, and up to three are in flight.
    if (g_bridge.exp12 != nullptr)
    {
        BridgeDrainGpu();
        if (g_bridge.params != nullptr)
            g_bridge.params->Set("ExposureTexture", static_cast<ID3D12Resource *>(nullptr));
    }
    if (g_bridge.exp12   != nullptr) { g_bridge.exp12->Release();   g_bridge.exp12   = nullptr; }
    if (g_bridge.exp11   != nullptr) { g_bridge.exp11->Release();   g_bridge.exp11   = nullptr; }
    if (g_bridge.exp_shared != nullptr) { CloseHandle(g_bridge.exp_shared); g_bridge.exp_shared = nullptr; }
    g_bridge.exp_w = g_bridge.exp_h = 0;
}
static void BridgeReleaseExposureFwd() { BridgeReleaseExposure(); }


// Builds the mirror on first use and whenever the game's exposure texture
// changes shape. Returns false when it cannot be built, which is not fatal:
// the frame then runs exactly as it did before, without exposure.
static bool BridgeEnsureExposure(ID3D11Device *dev11, ID3D11Texture2D *src)
{
    D3D11_TEXTURE2D_DESC sd = {};
    src->GetDesc(&sd);

    if (g_bridge.exp12 != nullptr && g_bridge.exp_w == sd.Width &&
        g_bridge.exp_h == sd.Height && g_bridge.exp_fmt == sd.Format)
        return true;

    BridgeReleaseExposure();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = sd.Width;
    td.Height           = sd.Height;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    // Every other mirror runs the game's format through TypedFormat, and for a
    // recorded reason: a typeless mirror reaches the D3D12 add-on and it refuses
    // the surface while this bridge still looks healthy. PSO2 supplies a 1x1
    // R32_TYPELESS exposure texture, so skipping it here would have left PSO2
    // with the exact defect this mirror was written to fix.
    td.Format           = TypedFormat(sd.Format);
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags        = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

    HRESULT hr = dev11->CreateTexture2D(&td, nullptr, &g_bridge.exp11);
    if (SUCCEEDED(hr))
    {
        IDXGIResource1 *r = nullptr;
        hr = g_bridge.exp11->QueryInterface(__uuidof(IDXGIResource1),
                                            reinterpret_cast<void **>(&r));
        if (SUCCEEDED(hr))
        {
            hr = r->CreateSharedHandle(nullptr,
                                       DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                       nullptr, &g_bridge.exp_shared);
            r->Release();
        }
    }
    if (SUCCEEDED(hr))
        hr = g_bridge.dev12->OpenSharedHandle(g_bridge.exp_shared, __uuidof(ID3D12Resource),
                                              reinterpret_cast<void **>(&g_bridge.exp12));
    if (FAILED(hr))
    {
        Log("[bridge] the exposure texture could not be mirrored 0x%08X. The session "
            "continues without it, which is what every release before this one did.", hr);
        BridgeReleaseExposure();
        return false;
    }

    g_bridge.exp_w   = sd.Width;
    g_bridge.exp_h   = sd.Height;
    g_bridge.exp_fmt = sd.Format;
    g_bridge.exp12->SetName(L"dlss5-bridge Exposure");
    if (!g_bridge.exp_reported)
    {
        g_bridge.exp_reported = true;
        Log("[bridge] the game supplies an exposure texture (%ux%u fmt=%u); it is "
            "mirrored to D3D12 so both sides run the same contract.",
            sd.Width, sd.Height, static_cast<unsigned>(sd.Format));
    }
    return true;
}

static void BridgeFrameInner(ID3D11DeviceContext *ctx, const NVSDK_NGX_Parameter *gp)
{
    BridgeRescueStalledQueue();
    if (g_bridge.disabled || !g_bridge.session_ready || gp == nullptr) return;

    if (g_bridge.paused_until != 0)
    {
        if (GetTickCount64() < g_bridge.paused_until) return;
        g_bridge.paused_until  = 0;
        g_bridge.rebuilds      = 0;
        g_bridge.rebuild_since = GetTickCount64();
        if (!g_bridge.resume_reported)
        {
            g_bridge.resume_reported = true;
            Log("[bridge] resuming: trying the render shape again. Further pauses and "
                "resumes are not reported; the frame counter shows them.");
        }
    }

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

    CfgReload();   // rate limited inside, on the clock rather than on frames

    // Stage 0 touches nothing at all, not even a read of the game's parameters.
    if (g_cfg.stage <= 0) { ++g_bridge.frames_done; return; }

    TexFacts             d[SLOT_COUNT] = {};
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
        if (d[i].Samples > samples) samples = d[i].Samples;
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
    // This used to clear msaa_reported, so a game alternating sample counts
    // re-armed the warning every other frame and the two messages traded places
    // for as long as it lasted. Said once each, per session: the frame counter
    // shows the rest.
    if (g_bridge.msaa_reported && !g_bridge.msaa_cleared)
    {
        g_bridge.msaa_cleared = true;
        Log("[bridge] MSAA is off now; rebuilding. Further changes of sample count "
            "are not reported.");
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

    // DLSS never produces an image smaller than the one it is given, so an
    // output size below the render size is not a contract: the two pairs have
    // been written the wrong way round. Four engines do it -- Gallipoli, The
    // Elder Scrolls Online, Phantasy Star Online 2 and House Party's Unity
    // plugin -- and the right repair is to swap them back, not to patch one side
    // from a texture.
    //
    // Patching from the texture was what this code did, and it works only while
    // the allocation happens to equal the display size. Under dynamic resolution
    // it never does: House Party pads every texture to 5120x2048 while the true
    // output is 5120x1440 and the true render is a 2970x836 subrect, so the
    // texture fallback built a feature no evaluate could satisfy and NGX
    // answered InvalidParameter on every frame of gameplay.
    if (gow < gw || goh < gh)
    {
        static bool said_swap = false;
        if (!said_swap)
        {
            said_swap = true;
            Log("[bridge] the game reports an input of %ux%u and an output of %ux%u, "
                "which is the wrong way round: DLSS never returns an image smaller "
                "than the one it is given.", gw, gh, gow, goh);
            Log("  Reading them as output and render respectively, the contract is "
                "%ux%u -> %ux%u.", gow, goh, gw, gh);
        }
        const unsigned int tw = gw, th = gh;
        gw = gow; gh = goh;
        gow = tw; goh = th;
    }

    // What follows is the older repair, kept for a block that is inconsistent in
    // some way the swap above does not resolve. Gallipoli reports 2560x1440 in and 1485x835 out, which is the
    // Balanced render size written into the output fields; NGX answers
    // 0xBAD00005 and the bridge used to rebuild every texture, fail, and repeat
    // until three failures switched it off.
    //
    // The output texture is an allocation rather than a claim, so when the two
    // disagree this way the texture wins.
    if (gow < gw || goh < gh)
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            Log("[bridge] the game asks for an output of %ux%u from an input of %ux%u, "
                "which is smaller rather than larger.", gow, goh, gw, gh);
            Log("  Its output texture is %ux%u, and a texture is an allocation rather "
                "than a claim, so that is used instead.",
                d[SLOT_OUTPUT].Width, d[SLOT_OUTPUT].Height);
        }
        gow = d[SLOT_OUTPUT].Width;
        goh = d[SLOT_OUTPUT].Height;
    }

    // The same repair the other way round, and it has to come second: fixing the
    // input first would make the test above compare two already-corrected values
    // and never fire.
    //
    // A render size cannot exceed the texture holding it -- a subrect has to fit
    // inside its own allocation -- so when it does, the fields have been filled
    // in the wrong order. The Elder Scrolls Online reports Width=2560x1440 with
    // a 1485x835 Color texture and a 2560x1440 Output texture, which is
    // 1485x835 -> 2560x1440 written backwards. Reading it as given built a 1:1
    // feature at output size and did the upscaling work for nothing.
    //
    // Where a game genuinely renders into part of a larger texture the render
    // size is smaller than the texture, not larger, so that case is untouched:
    // Final Fantasy XIV renders 2227x1253 inside a 3840x2160 Color texture and
    // does not come through here.
    if (gw > d[SLOT_COLOR].Width || gh > d[SLOT_COLOR].Height)
    {
        static bool said_in = false;
        if (!said_in)
        {
            said_in = true;
            Log("[bridge] the game asks for a render of %ux%u from a Color texture of "
                "%ux%u, which is larger than the texture holding it.", gw, gh,
                d[SLOT_COLOR].Width, d[SLOT_COLOR].Height);
            Log("  A subrect has to fit inside its own allocation, so the texture is "
                "used instead and the contract reads %ux%u -> %ux%u.",
                d[SLOT_COLOR].Width, d[SLOT_COLOR].Height, gow, goh);
        }
        gw = d[SLOT_COLOR].Width;
        gh = d[SLOT_COLOR].Height;
    }

    // If even the texture cannot satisfy it, nothing here can: say so once and
    // stop, rather than hand NGX parameters it will refuse on every frame.
    if (gow < gw || goh < gh)
    {
        Warn("this game's DLSS output (%ux%u) is smaller than its input (%ux%u). "
             "The bridge cannot work from that and has stopped. The game is unaffected.",
             gow, goh, gw, gh);
        BridgeDisable("the game's output is smaller than its input");
        for (int i = 0; i < SLOT_COUNT; ++i) g[i]->Release();
        dev11->Release();
        return;
    }

    // Gallipoli's map screen creates a 1440x1440 feature while its textures stay
    // 2560x1440: the game is drawing into a square sub-region of them. Handing
    // DLSS the whole texture for a feature that size is the contradiction NGX
    // answers with 0xBAD00005, and where it does not, DLSS writes 1440 columns
    // of a 2560-wide texture and the rest stays whatever it was -- the two
    // halves people see. The subrect origins this bridge copies all read zero,
    // so there is nothing to place the region by. Stand aside and let the game
    // render that screen itself.
    const bool partial = (gow != d[SLOT_OUTPUT].Width) || (goh != d[SLOT_OUTPUT].Height);
    g_bridge.partial_output = partial;
    if (partial)
    {
        if (!g_bridge.partial_reported)
        {
            g_bridge.partial_reported = true;
            Log("[bridge] the game asks for a %ux%u result inside a %ux%u texture, so "
                "it is drawing into part of it. Filling only that part would leave "
                "the rest of the frame stale, and where the region sits is not in "
                "the parameters. The game renders these frames itself.",
                gow, goh, d[SLOT_OUTPUT].Width, d[SLOT_OUTPUT].Height);
        }
        g_bridge.frame_ready = false;
        for (int i = 0; i < SLOT_COUNT; ++i) g[i]->Release();
        dev11->Release();
        return;
    }

    bool shape_changed = (gw != g_bridge.render_w)  || (gh != g_bridge.render_h) ||
                         (gow != g_bridge.ngx_out_w) || (goh != g_bridge.ngx_out_h);
    // Every slot's own size, not only Color's and Output's. Depth or motion
    // vectors changing size on their own used to move nothing here.
    for (int i = 0; i < SLOT_COUNT && !shape_changed; ++i)
        shape_changed = (d[i].Format != g_bridge.fmt[i]) ||
                        (d[i].Width  != g_bridge.slot_w[i]) ||
                        (d[i].Height != g_bridge.slot_h[i]);

    if (!g_bridge.frame_ready || shape_changed)
    {
        // A game changing resolution rebuilds once. A game alternating between
        // two buffers rebuilds forever, and each rebuild costs four shared
        // texture pairs and an NGX feature. Past a rebuild every few frames the
        // honest reading is that there is no single shape to serve, so the
        // bridge stands aside and lets the game render itself until one settles.
        const ULONGLONG now = GetTickCount64();
        if (now - g_bridge.rebuild_since > 2000)
        { g_bridge.rebuild_since = now; g_bridge.rebuilds = 0; }

        if (++g_bridge.rebuilds > 8)
        {
            // rebuilds is zeroed on every resume, so it can never reach a
            // threshold that spans pauses. This one is not, and a shape that
            // never settles stops costing eight rebuilds every three seconds
            // for the rest of the session.
            if (++g_bridge.pause_cycles >= 5)
            {
                Log("[bridge] the render shape has not settled across %d pauses. "
                    "Continuing would rebuild eight times every three seconds for "
                    "the rest of the session, so the bridge is stopping. The game "
                    "is unaffected.", g_bridge.pause_cycles);
                BridgeDisable("the render shape never settles");
                for (int i = 0; i < SLOT_COUNT; ++i) g[i]->Release();
                dev11->Release();
                return;
            }

            if (!g_bridge.pause_reported)
            {
                Log("[bridge] the render shape has changed %d times in two seconds, so "
                    "there is no one shape to mirror. Standing aside until it settles; "
                    "the game renders on its own meanwhile.", g_bridge.rebuilds);
                for (int i = 0; i < SLOT_COUNT; ++i)
                    Log("[bridge]   slot %d: %ux%u fmt=%u", i, d[i].Width, d[i].Height,
                        static_cast<unsigned>(d[i].Format));
                g_bridge.pause_reported = true;
            }
            g_bridge.paused_until = now + 3000;
            g_bridge.frame_ready  = false;
            for (int i = 0; i < SLOT_COUNT; ++i) g[i]->Release();
            dev11->Release();
            return;
        }

        DXGI_FORMAT fmt[SLOT_COUNT];
        for (int i = 0; i < SLOT_COUNT; ++i) fmt[i] = d[i].Format;
        g_bridge.width      = d[SLOT_COLOR].Width;
        g_bridge.height     = d[SLOT_COLOR].Height;
        g_bridge.out_width  = d[SLOT_OUTPUT].Width;
        g_bridge.out_height = d[SLOT_OUTPUT].Height;
        for (int i = 0; i < SLOT_COUNT; ++i)
        { g_bridge.slot_w[i] = d[i].Width; g_bridge.slot_h[i] = d[i].Height; }
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

    // Mirrored per frame like the rest: it is 1x1 in every report so far, and a
    // stale exposure is as wrong as none.
    {
        ID3D11Resource *ge = nullptr;
        if (gp->Get("ExposureTexture", &ge) == NGX_SUCCESS && ge != nullptr)
        {
            ID3D11Texture2D *gt = nullptr;
            if (SUCCEEDED(ge->QueryInterface(__uuidof(ID3D11Texture2D),
                                             reinterpret_cast<void **>(&gt))) && gt != nullptr)
            {
                if (BridgeEnsureExposure(dev11, gt))
                    ctx->CopyResource(g_bridge.exp11, gt);
                gt->Release();
            }
        }
        else if (g_bridge.exp12 != nullptr)
        {
            // The game stopped supplying one. Sending the last frame's exposure
            // for the rest of the session would be worse than sending none.
            Log("[bridge] the game no longer supplies an exposure texture; the mirror "
                "is dropped rather than left holding a stale one.");
            BridgeReleaseExposure();
        }
    }

    // BridgeDisable is the one shutdown routine and releases every mirror, so
    // anything that can reach it from inside this frame leaves the rest of the
    // frame with null textures to barrier and copy. Checking the flag once at
    // the top is no longer enough now that it can be set from here.
    if (g_bridge.disabled)
    {
        for (int i = 0; i < SLOT_COUNT; ++i) g[i]->Release();
        dev11->Release();
        return;
    }
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
    if (g_bridge.exp12 != nullptr)
        g_bridge.params->Set("ExposureTexture", g_bridge.exp12);
    // Mirror the game's own subrect description rather than inventing one. In an
    // upscaling preset the rendered area is a corner of a larger texture, and
    // guessing it is what made DLSS Quality produce a soft, effect-free image.
    // These two are not the game's to declare on our behalf: they state the size
    // of the region THIS feature is being asked to process, and this bridge knows
    // that exactly because it created the feature. Mirroring is right for MV.Scale
    // and exposure, which describe the game's own data and are corrupted by any
    // invention. It is wrong here, and 1.0.18 stopped sending them to a game that
    // sets none -- taking information away from whatever reads the block after us.
    CopyUIntParam(gp, "DLSS.Render.Subrect.Dimensions.Width", g_bridge.render_w);
    CopyUIntParam(gp, "DLSS.Render.Subrect.Dimensions.Height", g_bridge.render_h);
    NeutralUIntParam(gp, "DLSS.Input.Color.Subrect.Base.X", 0u);
    NeutralUIntParam(gp, "DLSS.Input.Color.Subrect.Base.Y", 0u);
    NeutralUIntParam(gp, "DLSS.Input.Depth.Subrect.Base.X", 0u);
    NeutralUIntParam(gp, "DLSS.Input.Depth.Subrect.Base.Y", 0u);
    NeutralUIntParam(gp, "DLSS.Input.MV.Subrect.Base.X", 0u);
    NeutralUIntParam(gp, "DLSS.Input.MV.Subrect.Base.Y", 0u);
    NeutralUIntParam(gp, "DLSS.Output.Subrect.Base.X", 0u);
    NeutralUIntParam(gp, "DLSS.Output.Subrect.Base.Y", 0u);
    NeutralFloatParam(gp, "MV.Scale.X", 1.0f);
    NeutralFloatParam(gp, "MV.Scale.Y", 1.0f);
    MirrorFloatParam(gp, "Jitter.Offset.X");
    MirrorFloatParam(gp, "Jitter.Offset.Y");
    MirrorFloatParam(gp, "Sharpness");
    NeutralFloatParam(gp, "DLSS.Pre.Exposure", 1.0f);
    NeutralFloatParam(gp, "DLSS.Exposure.Scale", 1.0f);

    int reset = 0;
    gp->Get("Reset", &reset);
    g_bridge.params->Set("Reset", (g_bridge.need_reset || reset || g_cfg.reset_every) ? 1 : 0);
    g_bridge.need_reset = false;

    const int mode = g_cfg.mode;

    DWORD code = 0;
    Breadcrumb("running the D3D12 evaluate");
    NVSDK_NGX_Result re = NGX_SUCCESS;
    static const char kNgxMark[] = "ngx evaluate";
    g_bridge.list->BeginEvent(1, kNgxMark, sizeof(kNgxMark));
    if (mode != 1)
        re = SafeEvaluateGuarded(g_bridge.eval_feature, g_bridge.list,
                                 g_bridge.feature, g_bridge.params, &code);
    g_bridge.list->EndEvent();

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
        BridgeDisable("NGX raised an exception inside the D3D12 evaluate");
    }
    else if (re != NGX_SUCCESS)
    {
        Log("[bridge] evaluate failed with result 0x%08X, %s", re, NgxResultName(re));
        BridgeFail("evaluate");
    }
    else
    {
        // Only a completed evaluate clears the failure count.
        g_bridge.consecutive_fails = 0;

        // 4. take the result back into the game's own output texture.
        //
        // The wait stays on the GPU: moving it to the CPU costs about nine
        // milliseconds a frame, because the CPU then stops instead of running
        // ahead to prepare the next one. What it cannot stay is unattended --
        // a GPU-side wait has no timeout and cannot be cancelled, so if the
        // D3D12 side never reaches v_out the game's queue stops for good.
        // BridgeRescueStalledQueue watches this value on later frames.
        g_bridge.ctx4->Wait(g_bridge.fence11, v_out);
        // Always the newest value: fences are monotonic, so signalling the
        // latest satisfies every wait queued behind it. The deadline is not
        // armed here -- BridgeRescueStalledQueue keeps it, against the last time
        // the GPU completed anything at all.
        g_bridge.pending_out = v_out;

        const UINT64 n = ++g_bridge.frames_done;
        // The one place a completed evaluate exists to be copied back, so the
        // only honest place to count a delivered frame. The stage<3 returns
        // above move frames_done as well, and they deliver nothing.
        InterlockedIncrement(&g_frames_delivered);

        // Did a DLSS 5 add-on detour CreateFeature after this add-on had already
        // called it? Only asked on the synthetic path, and only for the first
        // few frames: in a mirror session the game creates features of its own
        // and that add-on catches one of those, but here nothing else will ever
        // call CreateFeature again, so the one create it missed was the only one
        // there was going to be. Comparing the bytes is the whole test -- a
        // detour is written over them -- so this states what it observed rather
        // than waiting a length of time somebody guessed.
        if (g_source == SRC_SYNTH && g_bridge.create_prologue_taken &&
            !g_bridge.recreated_for_addon && n >= 2 && n <= 240 &&
            g_bridge.create_feature != nullptr &&
            memcmp(g_bridge.create_prologue,
                   reinterpret_cast<const void *>(g_bridge.create_feature),
                   sizeof(g_bridge.create_prologue)) != 0)
        {
            g_bridge.recreated_for_addon = true;
            Log("[bridge] NVSDK_NGX_D3D12_CreateFeature has been detoured since this add-on "
                "created its feature, which is a DLSS 5 add-on arming after the fact. It "
                "captures its contract from a create it sees, and it did not see this one. "
                "Rebuilding once so that it does.");
            g_bridge.frame_ready = false;
        }

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

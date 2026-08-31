// ---------------------------------------------------------------------------
// vkmirror.inc -- the Vulkan mirror
//
// A Vulkan game that has DLSS of its own. Its NVSDK_NGX_VULKAN CreateFeature,
// CreateFeature1, EvaluateFeature and EvaluateFeature_C are hooked with the same
// fourteen-byte jump the D3D11 mirror uses, forwarded untouched, and the same
// contract is then mirrored onto the private D3D12 session synth.inc already
// builds -- which is where a D3D12-only DLSS 5 add-on inserts itself.
//
// Everything the game declares is COPIED and nothing is invented: the four
// resources come out of the game's own NGX parameter block, so their size,
// format and usage are right by construction. That is the whole reason both
// paths exist. The synthetic path has to build a contract out of ReShade's
// buffers and meets three obstacles on depth; the mirror meets none of them.
//
// THE ONE FACT THAT DECIDES THE ARCHITECTURE. At the moment the evaluate is
// detoured, the game's Color/Depth/MotionVectors DO NOT YET CONTAIN THIS FRAME:
// the rendering that fills them is recorded into the same open VkCommandBuffer
// and has not executed. So the copies have to be recorded into that command
// buffer -- argument 1 of the detour -- and not issued from anywhere else. But a
// VkCommandBuffer carries no device and no queue and cannot be submitted, and
// there is no Vulkan equivalent of ID3D11DeviceContext::Flush, so the D3D12
// evaluate cannot complete inside the detour the way the D3D11 mirror's does.
//
// What closes that gap is a park. The copies in are recorded, then a
// vkCmdSetEvent announces them, then a vkCmdWaitEvents stalls the game's own
// command buffer inside its own submission until a worker thread host-signals
// the second event -- and the copy back over the game's Output is recorded after
// the wait, so it executes once the D3D12 evaluate has finished. The worker sees
// the first event, runs the D3D12 evaluate, sets the second. No queue is ever
// touched, no vkQueueSubmit is intercepted, and therefore no Vulkan layer and no
// manifest are needed. Seven Vulkan 1.0 core entry points and two Win32 events.
//
// The park is what makes the time budget load-bearing rather than tidy: the
// game's GPU is stopped for its duration, and Windows resets a device that stops
// for two seconds. 300 ms to observe the copies plus 900 ms for the evaluate is
// 1.2 s worst case, and every failure path sets the release event before it
// returns. A parked vkCmdWaitEvents whose event is never set is a device loss
// for the whole game, so that is written as an unconditional statement at the
// end of the worker's body and not as a line inside an if.
//
// Off unless vk_mirror=1 is in dlss5-bridge.cfg. Nothing here is reached,
// and none of the four Vulkan entry points is even hooked, without it.
//
// The transport shape -- create the shared textures on D3D12 and import THOSE
// into the game's VkDevice, rather than importing the game's images -- and the
// event park are after AlanBacker/dlss5-vk-bridge (MIT), which is itself a
// Vulkan port of this project. Nothing is copied from it verbatim; see README.
// ---------------------------------------------------------------------------

// The parameter block's Vulkan resource, hand-declared. NVIDIA's
// nvsdk_ngx_defs_vk.h is the authority for the layout; the asserts below are the
// check on this transcription of it, and they are the point of writing it out
// rather than including <vulkan/vulkan.h> -- which this add-on cannot do,
// because an import of vulkan-1.dll would stop it at the loader in every D3D11
// game on a machine with no Vulkan runtime.
//
// Format at 36 is NOT eight-byte aligned. Padding it to 40 -- which is the
// obvious instinct next to two 64-bit handles -- puts Width where Height
// belongs, and the result is a plausible frame at the wrong size rather than an
// error. Image at +8 is what the mirror wants; ImageView at +0 is what a
// careless cast to VkImage* would return.
struct MVkImageViewInfo
{
    uint64_t ImageView;        //  0
    uint64_t Image;            //  8
    uint32_t aspectMask;       // 16   VkImageSubresourceRange, five uint32
    uint32_t baseMipLevel;     // 20
    uint32_t levelCount;       // 24
    uint32_t baseArrayLayer;   // 28
    uint32_t layerCount;       // 32
    uint32_t Format;           // 36
    uint32_t Width;            // 40
    uint32_t Height;           // 44
};

struct MVkResource
{
    MVkImageViewInfo iv;       //  0
    uint32_t         Type;     // 48   0 = VK_IMAGEVIEW, 1 = VK_BUFFER
    unsigned char    ReadWrite;// 52   bool, one byte, three bytes of tail padding
};

static_assert(offsetof(MVkImageViewInfo, ImageView)      ==  0, "MVk ImageView");
static_assert(offsetof(MVkImageViewInfo, Image)          ==  8, "MVk Image");
static_assert(offsetof(MVkImageViewInfo, aspectMask)     == 16, "MVk SubresourceRange");
static_assert(offsetof(MVkImageViewInfo, Format)         == 36, "MVk Format");
static_assert(offsetof(MVkImageViewInfo, Width)          == 40, "MVk Width");
static_assert(offsetof(MVkImageViewInfo, Height)         == 44, "MVk Height");
static_assert(offsetof(MVkResource, Type)                == 48, "MVk Type");
static_assert(offsetof(MVkResource, ReadWrite)           == 52, "MVk ReadWrite");
static_assert(sizeof(MVkResource)                        == 56, "MVk sizeof");

// VkFormat numbers. Written out rather than included, same argument as the
// SVk block in synth.inc: a published ABI that cannot change.
static const uint32_t kVkFmtR8G8B8A8Unorm   = 37;
static const uint32_t kVkFmtR8G8B8A8Srgb    = 43;
static const uint32_t kVkFmtB8G8R8A8Unorm   = 44;
static const uint32_t kVkFmtB8G8R8A8Srgb    = 50;
static const uint32_t kVkFmtA2B10G10R10     = 64;
static const uint32_t kVkFmtR16G16B16A16Sf  = 97;
static const uint32_t kVkFmtB10G11R11Uf     = 122;
static const uint32_t kVkFmtD16UnormS8Uint  = 128;
static const uint32_t kVkFmtD24UnormS8Uint  = 129;
static const uint32_t kVkFmtD32Sfloat       = 126;
static const uint32_t kVkFmtD32SfloatS8Uint = 130;

// The inverse of SynthVkFormat, and it has to exist separately: that one maps
// the formats this add-on chose for its own textures, and the mirror has to
// accept whatever the game presents. Seven entries, covering what DLSS titles
// actually hand NGX as Color. Anything else refuses by number, so the table is
// extended from a report rather than guessed at -- there is deliberately no blit
// fallback, because a blit converts, and a conversion nobody measured is exactly
// the invention this project keeps out.
static DXGI_FORMAT MapVkColor(uint32_t vkfmt)
{
    switch (vkfmt)
    {
    case kVkFmtR16G16B16A16Sf:  return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case kVkFmtA2B10G10R10:     return DXGI_FORMAT_R10G10B10A2_UNORM;
    case kVkFmtB8G8R8A8Unorm:   return DXGI_FORMAT_B8G8R8A8_UNORM;
    case kVkFmtB8G8R8A8Srgb:    return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case kVkFmtR8G8B8A8Unorm:   return DXGI_FORMAT_R8G8B8A8_UNORM;
    case kVkFmtR8G8B8A8Srgb:    return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case kVkFmtB10G11R11Uf:     return DXGI_FORMAT_R11G11B10_FLOAT;
    default:                    return DXGI_FORMAT_UNKNOWN;
    }
}

// Bytes per texel for the same seven formats and nothing else, because the only
// question it is asked is whether two images may legally be copied into one
// another: vkCmdCopyImage between colour images requires equal texel block size.
// Zero means "not a format this add-on knows", which is a refusal and not a
// guess.
static uint32_t VkColorTexelBytes(uint32_t vkfmt)
{
    switch (vkfmt)
    {
    case kVkFmtR16G16B16A16Sf:  return 8;
    case kVkFmtA2B10G10R10:
    case kVkFmtB8G8R8A8Unorm:
    case kVkFmtB8G8R8A8Srgb:
    case kVkFmtR8G8B8A8Unorm:
    case kVkFmtR8G8B8A8Srgb:
    case kVkFmtB10G11R11Uf:     return 4;
    default:                    return 0;
    }
}

// VK_IMAGE_ASPECT_STENCIL_BIT. The barrier aspect for the depth slot is derived
// from the FORMAT and not from the view's own aspectMask, and the difference is
// not pedantry: a barrier on a depth-stencil image must name both aspects, and a
// barrier naming STENCIL on an image that has none is equally invalid. The game's
// view legitimately carries either -- Baldur's Gate 3 hands NGX a DEPTH|STENCIL
// view -- so the view is the wrong thing to copy here.
static const uint32_t kVkmAspectStencil = 4;

// EVERY combined format, not just the one that happened to get here first.
// resources.adoc: the depth and stencil aspects of an image subresource may only
// be in different layouts when separateDepthStencilLayouts is enabled, and the
// add-on cannot see whether the game enabled it. Naming DEPTH alone on a D24S8
// image is therefore VUID-VkImageMemoryBarrier-image-03319 on any device that
// did not -- it never fired only because the format refusal below returned
// before any barrier was recorded, and unblocking VkFormat 129 is exactly what
// starts recording it.
static uint32_t VkmDepthAspect(uint32_t vkfmt)
{
    return (vkfmt == kVkFmtD16UnormS8Uint || vkfmt == kVkFmtD24UnormS8Uint ||
            vkfmt == kVkFmtD32SfloatS8Uint)
               ? (kSVkAspectDepth | kVkmAspectStencil)
               : kSVkAspectDepth;
}

// What one resource key yielded. Borrowed, never owned: Get(key, void**) hands
// back a pointer NGX still owns, valid for the duration of this evaluate only.
// It must not be Released -- unlike Get(key, ID3D11Resource**) on the D3D11
// path, which returns an AddRef'd pointer that bridge.inc does Release -- and it
// must not be cached across frames.
struct VkmRes
{
    uint64_t image;
    uint64_t view;
    uint32_t fmt;
    uint32_t aspect;
    uint32_t mip;
    uint32_t layer;
    uint32_t w, h;
};

// Budgets, in milliseconds, and each one is a measurement about the game rather
// than a preference. 300 + 900 is 1.2 s of parked GPU worst case against a 2 s
// Windows TDR default; the peer's 1000 + 1000 exceeds it, which is a device
// removal for the whole game rather than a dropped frame.
static const DWORD kVkmSeeCopiesMs = 300;

// Stalls before the mirror gives up, counted over the SESSION rather than
// consecutively -- and the difference is not cosmetic. The drain gate in
// VkMirrorFrame returns before the worker is woken, so a queue that stops
// answering altogether freezes this counter instead of advancing it: reaching 300
// means the queue came back 299 times. Each stall also costs the worker its full
// 300 ms poll plus the drain frames after it, so 300 of them is a minute and a
// half at best, not the few seconds this comment used to claim.
//
// A queue that never comes back is therefore not covered by this at all. The
// mirror contributes nothing and says nothing further, and the game keeps
// rendering with its own DLSS. That is the accepted cost of not retiring on a
// hiccup, in code that measured zero stalls across roughly 18000 frames: the
// threshold sits far above anything observed because retiring early is silent --
// the picture stays right, so a mirror that gave up looks like one that works.
static const LONG  kVkmStallGiveUp = 300;
static const DWORD kVkmEvaluateMs  = 900;

// Derived, not chosen, and it was chosen once: 1000 against a worker whose own
// documented worst case is 300 + 900 = 1200, so the guard was 200 ms shorter
// than the thing it guards. The render thread waits this long for the previous
// frame's worker to finish and refuses the mirror permanently if it does not --
// so a hand-picked number below the budget means one slow-but-successful
// evaluate retires the mirror for the whole session, silently, because the game
// keeps rendering with its own DLSS and the picture stays right. That is the
// same silent death the stall counter exists to prevent, arriving through the
// other timer. The margin is for the wake-up and the CPU frame that separates
// the worker's clock from this one; past this, a worker really is stuck, which
// is a fault and not a slow frame.
static const DWORD kVkmIdleMs      = kVkmSeeCopiesMs + kVkmEvaluateMs + 200;

struct VkMirror
{
    bool    refused;     // and refused, or something later stood the mirror down

    // TWO pairs, alternating, and the second pair is not pipelining -- there is
    // still exactly one frame in flight. It exists because a VkEvent must not be
    // host-reset while a submitted vkCmdWaitEvents may still be about to observe
    // it: the worker sets ev_out and releases the render thread, but the game's
    // GPU has not necessarily executed that wait yet, and resetting the event out
    // from under it parks the game for ever.
    //
    // Alternating makes it provable rather than likely. At the top of frame N+2
    // the render thread has waited on idle, which the worker sets only after it
    // has seen frame N+1's ev_in signalled by the GPU -- so everything frame N
    // recorded, its vkCmdWaitEvents included, is earlier in queue order and has
    // completed. Frame N's pair is therefore free to reset.
    SVkHandle ev_in[2];  // device -> host: the copies in have executed
    SVkHandle ev_out[2]; // host -> device: the D3D12 evaluate is done
    int       next;      // the pair the next frame takes; render thread only
    volatile LONG cur;   // the pair the worker is servicing
    HANDLE    idle;      // manual reset, set while no frame is parked
    HANDLE    wake;      // auto reset, kicks the worker
    HANDLE    worker;
    volatile LONG stop;
    // Consecutive frames whose D3D12 evaluate did not succeed. A clean refusal
    // leaves the session usable and is retried, but retrying for ever means
    // parking the game's GPU for up to 900 ms on every frame it will never use --
    // a game at one frame a second, with a log line and no end to it.
    volatile LONG fails;
    // Consecutive frames the game's queue did not reach. Whatever reaches this
    // counter cannot be told apart from a shutdown, so it counts rather than
    // retires -- see the worker. Measured 2026-08-31 in Baldur's Gate 3 on
    // Vulkan: this stayed at zero across three sessions, ~18000 mirrored frames,
    // a save load and a minimise/restore. The paths that were expected to land
    // here did not.
    volatile LONG stalls;
    // Non-zero while the mirror is standing down waiting for the game's queue to
    // catch up after a stall, and it holds the stalled pair's slot plus one. See
    // the drain gate in VkMirrorFrame: making a stall survivable is what created
    // the need for this, because the alternation proof above assumes the worker
    // saw the GPU reach a frame's ev_in before the render thread may reuse a
    // pair, and the stall path releases the render thread having seen nothing.
    volatile LONG drain;
    // Set when the game recreated its own feature and cleared when the rebuild
    // that follows has named itself. Only the log reads it.
    bool    rebuild_on_game_create;

    // The contract the worker evaluates. Single slot: the render thread waits on
    // idle before touching this, so there is no second writer.
    SynthParams sp;

    UINT64 frames;
    LONG   n;
    bool   said_no_runtime;
};

// One NGX call at a time in this process, across both backends.
//
// The mirror's worker runs a D3D12 NGX evaluate while the game's render thread
// runs its own Vulkan NGX evaluate, and until now nothing kept the two apart:
// the worker takes no lock, and the render thread's g_hook_cs is released before
// VkMirrorFrame wakes it. Two threads inside one NGX, on Red Dead Redemption 2,
// fault -- and the evidence that it is a race rather than a bad contract is that
// it is not reproducible in shape. Measured 2026-08-31, same build, same game,
// same values: once after 89 delivered frames inside nvapi64_impl.dll, once
// after 145 inside nvoglv64.dll, with every per-frame parameter dumped at the
// fault and every one of them in range, and the private device reporting S_OK.
//
// Deliberately NOT g_hook_cs. That one is taken from the loader notification
// while the loader lock is held, and adding a second waiter on it is the
// deadlock the note in TryInstallHooks was written about.
static CRITICAL_SECTION g_ngx_cs;
static bool             g_ngx_cs_ready;

static VkMirror g_vkm;

// Latched from ReShade's own init_effect_runtime, because inside an NGX detour
// there is no runtime argument to read one from. The D3D12 session opener and
// the Vulkan opener both want one, and they want the runtime that presents
// frames -- which is exactly what the two-ReShades check below tests.
static reshade::api::effect_runtime *g_vkm_rt;

static void OnVkmInitRuntime(reshade::api::effect_runtime *rt)    { g_vkm_rt = rt; }
static void OnVkmDestroyRuntime(reshade::api::effect_runtime *rt) { if (g_vkm_rt == rt) g_vkm_rt = nullptr; }

// g_vk_mirror is declared beside the layer table, because the module scan reads
// it and that runs long before this file is included.
// The one runnable check this file leaves behind, and it checks the one thing
// two tables have to agree about. MapVkColor turns the game's VkFormat into the
// DXGI format the D3D12 texture is created as; SynthVkImport then turns that
// DXGI format back into a VkFormat to import it with. If the round trip does not
// land back where it started, the frame is copied through a texture the two APIs
// disagree about -- which is wrong pixels and no error anywhere. A compile-time
// assert cannot express it because both are switch statements, so it is one loop
// at attach over the seven entries, and it says the number rather than nothing.
static void VkmCheckFormatTables()
{
    static const uint32_t kProbe[] = {
        kVkFmtR8G8B8A8Unorm, kVkFmtR8G8B8A8Srgb, kVkFmtB8G8R8A8Unorm, kVkFmtB8G8R8A8Srgb,
        kVkFmtA2B10G10R10,   kVkFmtR16G16B16A16Sf, kVkFmtB10G11R11Uf,
    };
    int ok = 0;
    for (size_t i = 0; i < _countof(kProbe); ++i)
    {
        const DXGI_FORMAT d = MapVkColor(kProbe[i]);
        const uint32_t back = d != DXGI_FORMAT_UNKNOWN ? SynthVkFormat(d) : 0;
        if (back == kProbe[i]) { ++ok; continue; }
        Log("[vkmirror] DEFECT: VkFormat %u maps to %s, which maps back to VkFormat %u. "
            "MapVkColor and SynthVkFormat disagree, so a frame in that format would be "
            "copied through a texture the two APIs read differently.",
            kProbe[i], FormatName(d), back);
    }
    Log("[vkmirror] colour format tables agree on %d of %d entries.",
        ok, static_cast<int>(_countof(kProbe)));
}

static void VkmRegisterEvents()
{
    if (g_vk_mirror == 0 || g_reshade_dll == nullptr) return;
    VkmCheckFormatTables();
    auto reg = reinterpret_cast<PFN_ReShadeRegisterEvent>(
        GetProcAddress(g_reshade_dll, "ReShadeRegisterEvent"));
    if (reg == nullptr)
    {
        Log("[vkmirror] this ReShade exports no ReShadeRegisterEvent, so the mirror cannot "
            "learn which runtime presents frames and will not run. The D3D11 mirror is "
            "unaffected.");
        return;
    }
    reg(reshade::addon_event::init_effect_runtime,    reinterpret_cast<void *>(&OnVkmInitRuntime));
    reg(reshade::addon_event::destroy_effect_runtime, reinterpret_cast<void *>(&OnVkmDestroyRuntime));
    Log("[vkmirror] vk_mirror=1: the Vulkan NGX entry points will be hooked and a Vulkan "
        "game's own DLSS contract mirrored onto the private D3D12 session.");
}

// Said once, and then the mirror never tries again this session. Everything it
// refuses on is a property of the game or the machine, not of the frame.
static void VkmRefuse(const char *why)
{
    if (g_vkm.refused) return;
    g_vkm.refused = true;
    Log("[vkmirror] %s. The game's own DLSS was forwarded and is what is on screen, so "
        "the picture is correct; the mirror does nothing for the rest of this session.",
        why);
}

// ---------------------------------------------------------------------------
// The worker
// ---------------------------------------------------------------------------

static DWORD WINAPI VkmWorkerProc(LPVOID)
{
    for (;;)
    {
        WaitForSingleObject(g_vkm.wake, INFINITE);
        if (g_vkm.stop != 0)
        {
            // The one break in this loop, and it must release the game on its
            // way out. VkmShutdown sets both of these before it sets stop, but
            // it holds no lock against the render thread, which resets ev_out
            // for the pair it is arming -- so a frame armed inside that window
            // would undo the release and then park on an event no code left in
            // the process would ever set. Both, not the current slot: which
            // pair the render thread armed is exactly what is unknowable here.
            if (g_svk.EvSet != nullptr && g_svk.dev != nullptr)
                for (int i = 0; i < 2; ++i)
                    if (g_vkm.ev_out[i] != 0) g_svk.EvSet(g_svk.dev, g_vkm.ev_out[i]);
            SetEvent(g_vkm.idle);
            break;
        }

        // Poll rather than wait: a VkEvent signalled by the GPU has no waitable
        // Win32 handle behind it. SwitchToThread first so a fast frame costs no
        // sleep quantum, Sleep(1) after the first millisecond so a slow one does
        // not burn a core.
        const int slot = static_cast<int>(g_vkm.cur);
        const ULONGLONG t0 = GetTickCount64();
        bool seen = false;
        for (;;)
        {
            if (g_svk.EvStatus(g_svk.dev, g_vkm.ev_in[slot]) == kSVkEventSet) { seen = true; break; }
            const ULONGLONG waited = GetTickCount64() - t0;
            if (waited > kVkmSeeCopiesMs) break;
            if (waited == 0) SwitchToThread(); else Sleep(1);
        }

        if (seen)
        {
            // Counted HERE and not where the frame was recorded. g_frames_delivered
            // is what the panel and ReportIdle read to say the bridge produced a
            // picture, and at record time nothing has been produced: the D3D12
            // evaluate has not run and may still refuse. This is still one claim
            // ahead of the truth -- the copy back executes on the game's GPU after
            // the detour returns -- but it is the last point this thread can
            // observe, and stage 2 records no copy back at all.
            // Serialised against the game's own NGX evaluate -- see g_ngx_cs.
            if (g_ngx_cs_ready) EnterCriticalSection(&g_ngx_cs);
            const bool ev_ok = Synth12EvaluateShared(&g_vkm.sp);
            if (g_ngx_cs_ready) LeaveCriticalSection(&g_ngx_cs);
            if (ev_ok)
            {
                // Both counters clear here: a frame that arrived and evaluated is
                // the evidence that neither the stall nor the refusals were the
                // permanent thing they would become if they kept coming.
                if (g_vkm.stalls != 0)
                {
                    Log("[vkmirror] the game's queue is answering again after %ld stalled "
                        "frame%s, plus the drain frames each one costs, which are not "
                        "counted here; the mirror was not retired.", g_vkm.stalls,
                        g_vkm.stalls == 1 ? "" : "s");
                    g_vkm.stalls = 0;
                }
                g_vkm.fails = 0;
                if (g_cfg.stage >= 3) InterlockedIncrement(&g_frames_delivered);
            }
            else if (InterlockedIncrement(&g_vkm.fails) >= 5)
                VkmRefuse("five consecutive D3D12 evaluates were refused -- the results are "
                          "named above -- and each one parks the game's GPU while it fails");
        }
        else
        {
            // A stall is not a failure, so this skips and counts rather than
            // retiring -- the same shape the D3D11 bridge already uses. Anything
            // that clears resets the count, so a session that stalls once a
            // minute forever still mirrors every other frame.
            //
            // What actually arrives here is NOT known. The guesses this comment
            // used to state as fact were tested on 2026-08-31 in Baldur's Gate 3
            // on Vulkan and did not hold: a save load never stalls, because the
            // game keeps presenting the loading screen and the queue keeps
            // answering; a minimise/restore takes the lost-effect-runtime path
            // above instead and never reaches this branch. Across three sessions
            // and roughly 18000 mirrored frames the counter stayed at zero.
            //
            // Treat this as an untested safety net. It is written to be harmless
            // when it never runs, which so far is always.
            const LONG n = InterlockedIncrement(&g_vkm.stalls);
            if (n == 1 || (n % 60) == 0)
                Log("[vkmirror] the game's queue did not reach the bridge's copies within "
                    "%d ms, so this frame is skipped (%ld stalls so far). The count clears "
                    "on the first frame that arrives and evaluates; if it keeps climbing, "
                    "this log is the first record of what reaches this path.",
                    kVkmSeeCopiesMs, n);
            // Hand the render thread a stand-down before releasing it. This
            // frame's commands are still queued and unexecuted, so its pair must
            // not be reset by the next frame that comes round to it -- set this
            // BEFORE SetEvent(idle) below, which is what the render thread is
            // waiting on, or it can be read after the frame that needed it.
            InterlockedExchange(&g_vkm.drain, static_cast<LONG>(slot) + 1);

            // Falls through to the unconditional event pair below rather than
            // continuing the loop: the game's command buffer is stopped at a
            // vkCmdWaitEvents on ev_out, and a path that skips setting it hangs
            // the game instead of dropping a frame.
            if (n >= kVkmStallGiveUp)
            {
                char msg[224];
                _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                            "the game's queue has failed to reach the bridge's copies on "
                            "%ld frames this session, which is no longer a pause",
                            kVkmStallGiveUp);
                VkmRefuse(msg);
            }
        }

        // UNCONDITIONAL, and it is written here rather than inside either branch
        // for exactly that reason. The game's own command buffer is stopped at a
        // vkCmdWaitEvents on this event; an exit path that does not set it is a
        // device loss for the whole game rather than a dropped frame.
        g_svk.EvSet(g_svk.dev, g_vkm.ev_out[slot]);
        SetEvent(g_vkm.idle);
    }
    return 0;
}

// Detach, and the one case that has to work: the add-on unloaded while a frame
// is parked. Release the game first, then stop the thread.
static void VkmShutdown()
{
    if (g_vkm.worker == nullptr) return;

    // FIRST, before anything is released. VkMirrorFrame's entry guard reads this,
    // so a render thread that has not started a frame yet cannot arm one behind
    // this function -- which would reset ev_out after the release below and then
    // park the game's GPU on an event no code left in the process would ever set.
    g_vkm.refused = true;

    if (g_svk.EvSet != nullptr && g_svk.dev != nullptr)
        for (int i = 0; i < 2; ++i)
            if (g_vkm.ev_out[i] != 0) g_svk.EvSet(g_svk.dev, g_vkm.ev_out[i]);
    g_vkm.stop = 1;
    SetEvent(g_vkm.wake);
    WaitForSingleObject(g_vkm.worker, 2000);

    // Again, and this pass is the one that counts. The set above races a render
    // thread that was already inside VkMirrorFrame when refused went true and has
    // not yet reached the re-check before the arm -- the reset is that thread's,
    // not the worker's. This pass runs after both windows have closed. It also
    // covers the 2000 ms wait expiring on a worker still inside its evaluate,
    // which leaves the loop's own release unreached.
    if (g_svk.EvSet != nullptr && g_svk.dev != nullptr)
        for (int i = 0; i < 2; ++i)
            if (g_vkm.ev_out[i] != 0) g_svk.EvSet(g_svk.dev, g_vkm.ev_out[i]);

    CloseHandle(g_vkm.worker);
    g_vkm.worker = nullptr;
}

// ---------------------------------------------------------------------------
// Reading the game's contract
// ---------------------------------------------------------------------------

// The Vulkan spelling of DenoiserKeyPresent. It is a separate function and not a
// parameter on that one because Get(const char*, void**) and
// Get(const char*, ID3D11Resource**) are different vtable slots -- one function
// cannot serve both. The key list itself is shared verbatim.
static const char *DenoiserKeyPresentVk(const NVSDK_NGX_Parameter *p)
{
    if (p == nullptr) return nullptr;
    for (size_t i = 0; i < _countof(kDenoiserKeys); ++i)
    {
        void *v = nullptr;
        if (p->Get(kDenoiserKeys[i], &v) == NGX_SUCCESS && v != nullptr)
            return kDenoiserKeys[i];
    }
    return nullptr;
}

static bool VkmGetRes(const NVSDK_NGX_Parameter *p, const char *key, VkmRes *out,
                      const char **why)
{
    void *v = nullptr;
    if (p->Get(key, &v) != NGX_SUCCESS || v == nullptr)
    { *why = "the game's parameter block does not carry it"; return false; }

    // Borrowed. No Release, ever, and nothing kept past this evaluate.
    const MVkResource *r = static_cast<const MVkResource *>(v);
    if (r->Type != 0)
    { *why = "it is a VkBuffer and not an image view"; return false; }
    if (r->iv.Image == 0)
    { *why = "its image view carries no VkImage"; return false; }

    out->image  = r->iv.Image;
    out->view   = r->iv.ImageView;
    out->fmt    = r->iv.Format;
    // A zero aspect mask is not a depth image declaring nothing; it is a block
    // that left the field alone. Colour is what the other three are.
    out->aspect = r->iv.aspectMask != 0 ? r->iv.aspectMask : kSVkAspectColor;
    out->mip    = r->iv.baseMipLevel;
    out->layer  = r->iv.baseArrayLayer;
    out->w      = r->iv.Width;
    out->h      = r->iv.Height;
    return true;
}

static void VkmMirrorU(const NVSDK_NGX_Parameter *g, SynthParams *sp, const char *key,
                       unsigned int fallback)
{
    unsigned int v = 0;
    sp->Set(key, g->Get(key, &v) == NGX_SUCCESS ? v : fallback);
}

static void VkmMirrorF(const NVSDK_NGX_Parameter *g, SynthParams *sp, const char *key,
                       float fallback)
{
    float v = 0.0f;
    sp->Set(key, g->Get(key, &v) == NGX_SUCCESS ? v : fallback);
}

// The whole contract, mirrored. Every scalar comes off the game's own block and
// the fallbacks are the ones bridge.inc already uses for a key a game leaves
// out; nothing here is derived from a measurement this add-on took, because the
// point of the mirror is that the game already stated all of it.
//
// This is also why none of bridge.inc's six copy helpers is reused or
// duplicated. They exist to preserve a distinction -- the game set this, versus
// the game did not -- while writing into a live NGX block, where absence is
// itself information. A SynthParams is complete by construction and
// SynthSelfCheck proves it, so the distinction is made once, here, at the moment
// of reading, and downstream there is nothing left for those helpers to do.
static bool VkmReadContract(const NVSDK_NGX_Parameter *gp, SynthParams *sp, VkmRes res[SLOT_COUNT])
{
    sp->Clear();

    for (int i = 0; i < SLOT_COUNT; ++i)
    {
        const char *why = nullptr;
        if (!VkmGetRes(gp, kSlotKey[i], &res[i], &why))
        {
            static bool said[SLOT_COUNT];
            if (!said[i])
            {
                said[i] = true;
                Log("[vkmirror] the evaluate's \"%s\" could not be read: %s. This frame is "
                    "skipped and the next one is tried again.", kSlotKey[i], why);
            }
            return false;
        }
        // The VkImage handles ride the SK_RES slots exactly as the synthetic
        // path's ReShade handles do -- cast opaquely, never dereferenced -- so
        // SynthSelfCheck still proves the block complete on this path too.
        sp->Set(kSlotKey[i], reinterpret_cast<ID3D11Resource *>(
                                 static_cast<uintptr_t>(res[i].image)));
    }

    // Sizes: mirrored, with the resource's own declared size as the fallback,
    // because that is the one other place the same fact is stated.
    VkmMirrorU(gp, sp, "Width",     res[SLOT_COLOR].w);
    VkmMirrorU(gp, sp, "Height",    res[SLOT_COLOR].h);
    VkmMirrorU(gp, sp, "OutWidth",  res[SLOT_OUTPUT].w);
    VkmMirrorU(gp, sp, "OutHeight", res[SLOT_OUTPUT].h);

    unsigned int rw = 0, rh = 0;
    sp->Get("Width", &rw);
    sp->Get("Height", &rh);

    VkmMirrorU(gp, sp, "PerfQualityValue", 1u);
    // Zero is a fallback and NOT a mirrored value, and that distinction matters
    // more here than anywhere else in this function: zero says not HDR, not
    // reversed-Z, motion vectors at output resolution and no auto exposure --
    // four statements about the game that nobody measured. The evaluate block is
    // where bridge.inc reads it too, and nine D3D11 titles carry it there, so
    // absence is worth reporting rather than refusing on. If a Vulkan log ever
    // shows this line, the flags belong captured at CreateFeature instead.
    {
        unsigned int fl = 0;
        static bool said_absent = false;
        if (gp->Get("DLSS.Feature.Create.Flags", &fl) != NGX_SUCCESS && !said_absent)
        {
            said_absent = true;
            Log("[vkmirror] this evaluate's parameter block carries no "
                "\"DLSS.Feature.Create.Flags\", so the mirrored feature is created with zero "
                "-- not HDR, not reversed-Z, motion vectors at output resolution, no auto "
                "exposure. None of that was read off the game; if the picture is wrong this "
                "line is the first thing to blame.");
        }
    }
    VkmMirrorU(gp, sp, "DLSS.Feature.Create.Flags", 0u);
    // And the config override on top, so the flags the feature is CREATED with are
    // the ones the exposure gate tested. Without this the gate opens on a value
    // the feature does not carry, and NGX is told to read an ExposureTexture this
    // transport never provides.
    //
    // The one place the mirror does not copy, and still not an invention: the
    // value comes from a user who asked for it by name.
    {
        FlagsFrom ff = FLAGS_NONE;
        const unsigned int forced = ResolveCreateFlags(gp, &ff);
        if (ff == FLAGS_CONFIG) sp->Set("DLSS.Feature.Create.Flags", forced);
    }
    VkmMirrorU(gp, sp, "DLSS.Enable.Output.Subrects", 0u);

    // The render subrect falls back to the mirrored render size, which is what
    // NGX itself reads when the key is absent.
    VkmMirrorU(gp, sp, "DLSS.Render.Subrect.Dimensions.Width",  rw);
    VkmMirrorU(gp, sp, "DLSS.Render.Subrect.Dimensions.Height", rh);
    VkmMirrorU(gp, sp, "DLSS.Input.Color.Subrect.Base.X", 0u);
    VkmMirrorU(gp, sp, "DLSS.Input.Color.Subrect.Base.Y", 0u);
    VkmMirrorU(gp, sp, "DLSS.Input.Depth.Subrect.Base.X", 0u);
    VkmMirrorU(gp, sp, "DLSS.Input.Depth.Subrect.Base.Y", 0u);
    VkmMirrorU(gp, sp, "DLSS.Input.MV.Subrect.Base.X", 0u);
    VkmMirrorU(gp, sp, "DLSS.Input.MV.Subrect.Base.Y", 0u);
    VkmMirrorU(gp, sp, "DLSS.Output.Subrect.Base.X", 0u);
    VkmMirrorU(gp, sp, "DLSS.Output.Subrect.Base.Y", 0u);

    VkmMirrorF(gp, sp, "MV.Scale.X", 1.0f);
    VkmMirrorF(gp, sp, "MV.Scale.Y", 1.0f);
    VkmMirrorF(gp, sp, "Jitter.Offset.X", 0.0f);
    VkmMirrorF(gp, sp, "Jitter.Offset.Y", 0.0f);
    VkmMirrorF(gp, sp, "Sharpness", 0.0f);
    // "DLSS.Pre.Exposure" and "DLSS.Exposure.Scale" are the defined key names.
    // The peer writes "Pre.Exposure" and "Exposure.Scale", which are not NGX
    // keys at all and are therefore dead writes.
    VkmMirrorF(gp, sp, "DLSS.Pre.Exposure",   1.0f);
    VkmMirrorF(gp, sp, "DLSS.Exposure.Scale", 1.0f);

    int reset = 0;
    gp->Get("Reset", &reset);
    sp->Set("Reset", reset);

    SynthSelfCheck(sp);
    return true;
}

// What the first few evaluates report, and the whole reason to run this stage
// with the transport switched off first: nothing in this project has ever seen
// what a Vulkan title actually declares in an NGX block.
static void VkmLogContract(const SynthParams *sp, const VkmRes res[SLOT_COUNT], LONG n)
{
    unsigned int w = 0, h = 0, ow = 0, oh = 0, q = 0, fl = 0;
    sp->Get("Width", &w); sp->Get("Height", &h);
    sp->Get("OutWidth", &ow); sp->Get("OutHeight", &oh);
    sp->Get("PerfQualityValue", &q);
    sp->Get("DLSS.Feature.Create.Flags", &fl);
    float jx = 0.0f, jy = 0.0f, mx = 0.0f, my = 0.0f;
    sp->Get("Jitter.Offset.X", &jx); sp->Get("Jitter.Offset.Y", &jy);
    sp->Get("MV.Scale.X", &mx); sp->Get("MV.Scale.Y", &my);

    Log("=== Vulkan evaluate #%ld ===", n);
    Log("  contract: %ux%u -> %ux%u  quality=%u  jitter %.4f,%.4f  MV.Scale %.1f,%.1f",
        w, h, ow, oh, q, jx, jy, mx, my);
    LogCreateFlags("the game's own create", fl);
    for (int i = 0; i < SLOT_COUNT; ++i)
        Log("  %-13s VkImage=%016llX view=%016llX VkFormat=%u %ux%u aspect=0x%X mip=%u layer=%u",
            kSlotName[i], static_cast<unsigned long long>(res[i].image),
            static_cast<unsigned long long>(res[i].view), res[i].fmt,
            res[i].w, res[i].h, res[i].aspect, res[i].mip, res[i].layer);
}

// ---------------------------------------------------------------------------
// The frame
// ---------------------------------------------------------------------------

static bool VkmEnsureEvents()
{
    if (g_vkm.ev_in[0] != 0) return true;

    SVkEventCreateInfo eci = {};
    eci.sType = kSVkStEventCreateInfo;
    for (int i = 0; i < 2; ++i)
        if (g_svk.EvCreate(g_svk.dev, &eci, nullptr, &g_vkm.ev_in[i]) != kSVkSuccess ||
            g_svk.EvCreate(g_svk.dev, &eci, nullptr, &g_vkm.ev_out[i]) != kSVkSuccess)
        { VkmRefuse("vkCreateEvent refused, so the frame cannot be parked"); return false; }

    g_vkm.idle = CreateEventW(nullptr, TRUE, TRUE, nullptr);    // manual reset, starts set
    g_vkm.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto reset
    if (g_vkm.idle == nullptr || g_vkm.wake == nullptr)
    { VkmRefuse("the park's own Win32 events could not be created"); return false; }

    g_vkm.worker = CreateThread(nullptr, 0, &VkmWorkerProc, nullptr, 0, nullptr);
    if (g_vkm.worker == nullptr)
    { VkmRefuse("the mirror's worker thread would not start"); return false; }

    // ponytail: the four VkEvents and the worker thread live for the life of the
    // process. Two events per pair is 64 bytes of driver object; destroying them
    // would need a point at which no submitted command buffer can still name
    // them, and the only such point is the one VkmShutdown already covers, where
    // the device may already be gone. Bounded and reported; revisit if a title
    // ever creates and destroys its Vulkan device more than once.
    return true;
}

// One frame, recorded into the game's own command buffer after its own evaluate
// has been forwarded -- so the game's Color holds this frame and its Output may
// be replaced.
static void VkMirrorFrame(void *cmd_raw, const NVSDK_NGX_Parameter *gp)
{
    if (g_vkm.refused || g_cfg.stage < 2) return;

    if (g_vkm_rt == nullptr)
    {
        if (!g_vkm.said_no_runtime)
        {
            g_vkm.said_no_runtime = true;
            Log("[vkmirror] no ReShade effect runtime exists yet, so there is no VkDevice to "
                "import into. Retried on the next evaluate.");
        }
        return;
    }

    // The two-ReShades condition, and it is a real one: with dxgi.dll in the game
    // folder AND the Vulkan layer registered, a Vulkan game can create a
    // secondary D3D12 device that ReShade latches onto first, and the add-on then
    // attaches to a runtime that is not where frames are presented. Baldur's Gate
    // 3 did exactly this and reported a 3840x2160 back buffer instead of the real
    // 3840x1600. Refusing beats mirroring onto the wrong device.
    reshade::api::device *rdev = g_vkm_rt->get_device();
    if (rdev == nullptr || rdev->get_api() != reshade::api::device_api::vulkan)
    {
        VkmRefuse("a Vulkan NGX evaluate arrived but ReShade is attached to a runtime that is "
                  "not Vulkan -- with dxgi.dll in the game folder a Vulkan game can create a "
                  "secondary D3D12 device that ReShade latches onto first. Rename dxgi.dll out "
                  "of the game folder so ReShade loads from C:\\ProgramData\\ReShade\\");
        return;
    }

    if (!Synth12Open(g_vkm_rt))
    { VkmRefuse("the private D3D12 session could not be opened; the lines above say why"); return; }
    if (!SynthVkOpen(g_vkm_rt))
    { VkmRefuse("the Vulkan side of the transport could not be opened; the lines above say why"); return; }
    // The game's GPU is parked behind this wait, so it is not the synthetic
    // path's four seconds.
    g_s12.wait_ms = kVkmEvaluateMs;

    // BEFORE the contract is read, not after it, and that ordering is the whole
    // point: g_vkm.sp is the single slot the worker is still reading out of while
    // the previous frame's Synth12EvaluateShared runs. Writing it while that call
    // is in flight is a data race on the block NGX is being handed. idle is
    // manual-reset and is only cleared further down, at the point the park is
    // actually armed, so every refusal between here and there returns with it
    // still set and costs the next frame nothing.
    // ponytail: a per-frame pipeline bubble, the same one the synthetic Vulkan
    // path already pays at its wait_idle. Three slots with per-slot events and a
    // retire fence if a title ever measures badly enough to be worth it.
    if (g_vkm.idle != nullptr && WaitForSingleObject(g_vkm.idle, kVkmIdleMs) != WAIT_OBJECT_0)
    {
        // The number is the whole diagnostic here: this refusal retires the mirror
        // for the session, so the one thing a report needs is how long the worker
        // was actually given. Printed from the constant rather than spelled out in
        // prose, which is what drifted when kVkmIdleMs stopped being 1000.
        char msg[192];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "the previous frame's park had not cleared %lu ms later, so nothing was "
                    "recorded for this one", kVkmIdleMs);
        VkmRefuse(msg);
        return;
    }

    VkmRes res[SLOT_COUNT];
    if (!VkmReadContract(gp, &g_vkm.sp, res)) return;

    const LONG n = ++g_vkm.n;
    if (n <= 3 || (n % 1800) == 0) VkmLogContract(&g_vkm.sp, res, n);

    unsigned int rw = 0, rh = 0, ow = 0, oh = 0, flags = 0;
    g_vkm.sp.Get("Width", &rw);
    g_vkm.sp.Get("Height", &rh);
    g_vkm.sp.Get("OutWidth", &ow);
    g_vkm.sp.Get("OutHeight", &oh);
    g_vkm.sp.Get("DLSS.Feature.Create.Flags", &flags);

    if (rw == 0 || rh == 0 || ow == 0 || oh == 0)
    { VkmRefuse("the contract declares a zero dimension, so there is nothing to build"); return; }

    // The four D3D11 engines that forced bridge.inc's size repairs are not
    // carried here: no Vulkan title has been measured swapping the pairs, and a
    // repair applied to a case nobody has seen is a guess. Refuse and name it;
    // the repair goes in the day a Vulkan log shows it.
    if (ow < rw || oh < rh)
    {
        char msg[192];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "the contract declares an output of %ux%u smaller than its render size of "
                    "%ux%u, which no Vulkan title has been measured doing", ow, oh, rw, rh);
        VkmRefuse(msg);
        return;
    }

    // An exposure texture the mirror does not carry across is wrong brightness
    // rather than a broken frame, which is the worse failure: it looks like a
    // decision somebody made. AutoExposure set means DLSS computes its own and
    // the texture is not read.
    {
        // The contract's own flags, and there is no way round this one.
        //
        // It was tried, on 2026-08-31, against Red Dead Redemption 2: the gate was
        // made to read the config-resolved flags so a user could force
        // AutoExposure and run anyway, and the mirrored feature was created with
        // those same forced flags so the two agreed. The evaluate still raised
        // 0xC0000005 and the game stopped presenting. So the exposure texture is
        // not the whole of what that title's contract needs from a transport this
        // build does not have -- it is the first thing that is detectably absent,
        // not the last thing that is missing.
        //
        // A refusal that stops a session is worth more than an override that hangs
        // one. This stays shut, and no config key opens it.
        // The RESOLVED flags, so a user can force AutoExposure and run anyway.
        //
        // That escape hatch was added, removed and added again in one afternoon,
        // and the middle step is the one worth recording: forcing it appeared to
        // fault the evaluate, so the gate was shut again and the refusal told
        // users there was no way past. Both conclusions were wrong. The fault was
        // the NGX race g_ngx_cs now closes, which fires on this title with or
        // without the override; once the two backends were serialised, Red Dead
        // Redemption 2 ran 1800 mirrored frames with AutoExposure forced and the
        // brightness was reported as good.
        //
        // So the gate is real, the override is real, and the thing that made them
        // look connected was underneath both.
        FlagsFrom exp_from = FLAGS_NONE;
        const unsigned int eff_flags = ResolveCreateFlags(gp, &exp_from);

        void *exp = nullptr;
        if (gp->Get("ExposureTexture", &exp) == NGX_SUCCESS && exp != nullptr &&
            (eff_flags & 0x40) == 0)
        {
            VkmRefuse("the game drives DLSS from an exposure texture and has not set "
                      "AutoExposure, and this build does not mirror that texture -- "
                      "delivering without it would be wrong brightness. To run anyway "
                      "and let DLSS compute its own exposure, set flags in "
                      "dlss5-bridge.cfg to the game's own value with bit 0x40 added -- "
                      "but not to 107, which is treated as unset. Red Dead Redemption 2 "
                      "declares 43, so 75");
            return;
        }
    }

    const DXGI_FORMAT color_fmt = MapVkColor(res[SLOT_COLOR].fmt);
    if (color_fmt == DXGI_FORMAT_UNKNOWN)
    {
        char msg[192];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "the game's Color is VkFormat %u and this add-on has no DXGI format for "
                    "it, so it will not guess one -- add the entry and it works",
                    res[SLOT_COLOR].fmt);
        VkmRefuse(msg);
        return;
    }

    // Output. Never looked at before this: the shared Output texture is created
    // from the COLOUR format, and the last thing the frame does is
    // vkCmdCopyImage it over the game's own Output image. A copy between two
    // images whose texel block sizes differ is invalid, and the driver is not
    // required to say so -- so the game's Output format has to be one this add-on
    // knows and has to be the same width in bytes as its Colour. Refused by
    // number rather than converted: a conversion nobody measured is exactly the
    // invention this project keeps out.
    if (VkColorTexelBytes(res[SLOT_OUTPUT].fmt) == 0 ||
        VkColorTexelBytes(res[SLOT_OUTPUT].fmt) != VkColorTexelBytes(res[SLOT_COLOR].fmt))
    {
        char msg[224];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "the game's Output is VkFormat %u against a Colour of VkFormat %u, and "
                    "this add-on will not copy between two formats it cannot show are the "
                    "same size -- add the entry to MapVkColor and it works",
                    res[SLOT_OUTPUT].fmt, res[SLOT_COLOR].fmt);
        VkmRefuse(msg);
        return;
    }

    // Depth. The transport carries the depth aspect out through a buffer and back
    // into the shared texture, and what those bytes mean is fixed by the format:
    //
    //   VkFormat 126 and 130 -- the aspect is D32_SFLOAT, so the shared texture
    //   is R32_FLOAT and the bytes are already what NGX wants. Untouched.
    //
    //   VkFormat 129 -- the aspect is addressed as X8_D24_UNORM_PACK32, still one
    //   32-bit word per texel, so the buffer and both copies are the same size
    //   and the same commands; the shared texture is declared R32_UINT and a
    //   D3D12 compute unpacks it. See Synth12MakeDepthUnpack for the three spec
    //   clauses that leave the driver no freedom in what it writes there.
    //
    // Anything else is refused by number rather than converted. 124 and 128 copy
    // their depth aspect two bytes per texel, which the staging buffer's size and
    // the 32-bit staging texture are both wrong for; that is an R16_UNORM slot
    // and a different change, and no measured title needs it.
    const bool depth_packed = res[SLOT_DEPTH].fmt == kVkFmtD24UnormS8Uint;
    if (res[SLOT_DEPTH].fmt != kVkFmtD32Sfloat && res[SLOT_DEPTH].fmt != kVkFmtD32SfloatS8Uint &&
        !depth_packed)
    {
        char msg[224];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "the game's depth is VkFormat %u, and this build carries the 32-bit float "
                    "aspect (VkFormat %u or %u) and the packed 24-bit one (VkFormat %u) and no "
                    "other: a 16-bit depth aspect copies two bytes per texel, which the staging "
                    "buffer and its texture are both sized wrong for",
                    res[SLOT_DEPTH].fmt, kVkFmtD32Sfloat, kVkFmtD32SfloatS8Uint,
                    kVkFmtD24UnormS8Uint);
        VkmRefuse(msg);
        return;
    }

    if (!VkmEnsureEvents()) return;

    const bool rebuild = g_s12.feature == nullptr || g_s12.w != rw || g_s12.h != rh ||
                         g_s12.out_w != ow || g_s12.out_h != oh || g_s12.fmt != color_fmt ||
                         g_s12.depth_packed != depth_packed;
    if (rebuild)
    {
        // Which of the seven conditions fired, because "building" twice at an
        // unchanged shape is a question this line could answer and did not --
        // and it was asked, on 2026-08-31, about two builds 245 ms apart.
        Log("[vkmirror] building the D3D12 feature and its Vulkan import for %ux%u -> %ux%u %s "
            "(%s)", rw, rh, ow, oh, FormatName(color_fmt),
            g_s12.feature == nullptr ? "no feature yet"
            // Before the size test, because a zero width cannot equal any real
            // render width: a handover zeroes it deliberately, and letting the
            // size test answer for it printed "render size changed" over a
            // session where nothing about the size had changed.
            : g_s12.w == 0 ? (g_vkm.rebuild_on_game_create
                                  ? "the game replaced its own feature"
                                  : "the source changed hands")
            : g_s12.w != rw || g_s12.h != rh ? "render size changed"
            : g_s12.out_w != ow || g_s12.out_h != oh ? "output size changed"
            : g_s12.fmt != color_fmt ? "colour format changed"
            : "depth packing changed");
        // Every submitted command buffer that names the old images has to be
        // finished with before they are destroyed, and inside a detour there is
        // no queue to idle -- so the device is. A resolution change costs one
        // visible hitch, which is what it costs everywhere else too.
        g_vkm.rebuild_on_game_create = false;
        g_svk.DeviceWaitIdle(g_svk.dev);
        SynthVkReleaseImages();
        // The same two teardowns the synthetic path already does before its own
        // rebuild, and for a reason that only shows up in the superseded case:
        // Synth12Build's first act is Synth12ReleaseTextures, which Releases the
        // Colour and motion-vector textures and closes their shared NT handles.
        // If synth ran first this session, the optical-flow engine holds D3D11
        // aliases opened over exactly those handles -- so without this they are
        // left pointing at freed textures and pin the allocations, the private
        // D3D11 device and the NVOF session for the rest of the process. In a
        // mirror-only session both of these no-op.
        SynthOfaVkRebuild();
        SynthDepthCsRelease();
        if (!Synth12Build(&g_vkm.sp, rw, rh, ow, oh, color_fmt, depth_packed))
        { g_s12.ready = false; SetEvent(g_vkm.idle); VkmRefuse("the D3D12 feature could not be built"); return; }
        // AFTER the build, not before it. Everything this says about the packed
        // path is only true if Synth12Build made the texture, the shader, the
        // root signature, the pipeline and the heap -- and saying it first is
        // how a log ends up asserting a capability the build does not have,
        // which is the defect that started this work.
        if (depth_packed)
            Log("[vkmirror] the game's depth is VkFormat %u; the Vulkan spec copies that depth "
                "aspect as X8_D24_UNORM_PACK32 -- one 32-bit word per texel, depth in the low "
                "24 bits -- so the staging texture is R32_UINT and the D3D12 compute above "
                "unpacks it into the R32_FLOAT the feature reads.", res[SLOT_DEPTH].fmt);
        if (!SynthVkImport())
        { SynthVkReleaseImages(); g_svk.ready = false; SetEvent(g_vkm.idle);
          VkmRefuse("the D3D12 textures could not be imported into the game's VkDevice"); return; }
    }

    SVkCommandBuffer cb = reinterpret_cast<SVkCommandBuffer>(cmd_raw);

    // The drain gate. A stall released the render thread without the worker
    // having seen the GPU reach anything, so the alternation proof at the top of
    // this file does not cover the next frames: the stalled frame's command
    // buffer is still queued, its vkCmdWaitEvents unexecuted, and the resets
    // just below would pull ev_out out from under it -- which parks the game
    // until some later worker happens to set it again, on every following frame,
    // with the 1.2 s worst case no longer bounding anything against the 2 s TDR
    // window.
    //
    // So record nothing at all until the GPU is seen to reach the stalled
    // frame's own copies-done event. The game renders these frames with its own
    // DLSS and the picture stays right; the mirror simply contributes nothing
    // while the queue catches up. Doing less is always available here, and it is
    // the only response to "the GPU is behind" that cannot make things worse.
    const LONG drain = InterlockedCompareExchange(&g_vkm.drain, 0, 0);
    if (drain != 0)
    {
        if (g_svk.EvStatus(g_svk.dev, g_vkm.ev_in[drain - 1]) != kSVkEventSet)
            return;
        // The GPU has executed that frame's vkCmdSetEvent. The vkCmdWaitEvents
        // immediately after it waits on an ev_out this worker already host-set,
        // so it cannot block -- but "cannot block" is not "has retired", and
        // skipping this frame too buys the game's own present interval as
        // margin. An argument, not a proof: there is no queue handle in a detour
        // to fence on, which is the same reason the two-pair scheme exists.
        InterlockedExchange(&g_vkm.drain, 0);
        Log("[vkmirror] the game's queue reached the stalled frame's copies; the mirror "
            "resumes on the next frame after one skipped for margin.");
        return;
    }

    // Re-read after the idle wait above, which blocks for kVkmIdleMs: a detach
    // that ran VkmShutdown inside that window has already released the game, and
    // arming a pair now would reset ev_out and park on a worker that has exited.
    if (g_vkm.refused) return;

    const int pair = g_vkm.next;
    g_vkm.next ^= 1;
    g_svk.EvReset(g_svk.dev, g_vkm.ev_in[pair]);
    g_svk.EvReset(g_svk.dev, g_vkm.ev_out[pair]);
    ResetEvent(g_vkm.idle);

    // ASSUMED, NOT MEASURED: that the game's four images are in
    // VK_IMAGE_LAYOUT_GENERAL at the evaluate. NVSDK_NGX_Resource_VK carries no
    // layout field, so there is nothing to read, and every implementation of this
    // transport assumes the same thing. A game handing DLSS an image in
    // DEPTH_STENCIL_READ_ONLY_OPTIMAL is fine for DLSS and undefined for us.
    // Derived from the slot's own format, not from the view's aspectMask -- see
    // VkmDepthAspect. Colour, motion vectors and output are colour by
    // construction: MapVkColor already refused anything that is not.
    const uint32_t barrier_aspect[SLOT_COUNT] = {
        kSVkAspectColor, kSVkAspectColor,
        VkmDepthAspect(res[SLOT_DEPTH].fmt), kSVkAspectColor,
    };

    for (int i = 0; i < SLOT_COUNT; ++i)
    {
        if (i == SLOT_OUTPUT) continue;
        SynthVkBarrier(cb, res[i].image, kSVkLayoutGeneral, kSVkLayoutTransferSrc,
                       barrier_aspect[i]);
        SynthVkBarrier(cb, g_svk.img[i], g_svk.laid_out ? kSVkLayoutGeneral : kSVkLayoutUndefined,
                       kSVkLayoutGeneral);
    }
    SynthVkBarrier(cb, g_svk.img[SLOT_OUTPUT],
                   g_svk.laid_out ? kSVkLayoutGeneral : kSVkLayoutUndefined, kSVkLayoutGeneral);
    g_svk.laid_out = true;

    SynthVkCopyImage(cb, res[SLOT_COLOR].image, kSVkLayoutTransferSrc,
                     g_svk.img[SLOT_COLOR], kSVkLayoutGeneral,
                     res[SLOT_COLOR].w, res[SLOT_COLOR].h, rw, rh);
    SynthVkCopyImage(cb, res[SLOT_MV].image, kSVkLayoutTransferSrc,
                     g_svk.img[SLOT_MV], kSVkLayoutGeneral,
                     res[SLOT_MV].w, res[SLOT_MV].h, rw, rh);
    {
        // The depth aspect out to the buffer and back in as colour, tightly
        // packed -- exactly what the synthetic Vulkan transport does, for the
        // same reason: vkCmdCopyImage requires the two formats to match exactly
        // when either side is a depth format, and the shared D3D12 texture is a
        // colour format because a depth-stencil resource is not shareable. Which
        // colour format is the only thing the packed case changes -- R32_UINT
        // rather than R32_FLOAT -- and these two commands do not know or care.
        const UINT dw = res[SLOT_DEPTH].w < rw ? res[SLOT_DEPTH].w : rw;
        const UINT dh = res[SLOT_DEPTH].h < rh ? res[SLOT_DEPTH].h : rh;

        SVkBufferImageCopy c = {};
        c.imageSubresource.aspectMask = kSVkAspectDepth;
        c.imageSubresource.layerCount = 1;
        c.imageExtent.width  = dw;
        c.imageExtent.height = dh;
        c.imageExtent.depth  = 1;
        g_svk.CmdCopyImageToBuffer(cb, res[SLOT_DEPTH].image, kSVkLayoutTransferSrc,
                                   g_svk.buf, 1, &c);

        SVkMemoryBarrier mb = {};
        mb.sType         = kSVkStMemoryBarrier;
        mb.srcAccessMask = kSVkAccessMemoryWrite;
        mb.dstAccessMask = kSVkAccessMemoryRead | kSVkAccessMemoryWrite;
        g_svk.CmdPipelineBarrier(cb, kSVkStageAllCommands, kSVkStageAllCommands, 0,
                                 1, &mb, 0, nullptr, 0, nullptr);

        c.imageSubresource.aspectMask = kSVkAspectColor;
        g_svk.CmdCopyBufferToImage(cb, g_svk.buf, g_svk.img[SLOT_DEPTH],
                                   kSVkLayoutGeneral, 1, &c);
    }

    for (int i = 0; i < SLOT_COUNT; ++i)
        if (i != SLOT_OUTPUT)
            SynthVkBarrier(cb, res[i].image, kSVkLayoutTransferSrc, kSVkLayoutGeneral,
                           barrier_aspect[i]);

    // The park. Everything above has to have executed before the worker submits
    // the D3D12 evaluate that reads it, and the copy back below has to run after
    // that evaluate has finished -- and the game's own post-processing is
    // recorded after this function returns, which is why the copy back cannot
    // wait for a later frame.
    g_svk.CmdSetEvent(cb, g_vkm.ev_in[pair], kSVkStageAllCommands);
    // Written before the worker is woken and not touched again until the worker
    // sets idle, so there is one writer and one reader and never both at once.
    g_vkm.cur = pair;
    SetEvent(g_vkm.wake);

    {
        SVkMemoryBarrier mb = {};
        mb.sType         = kSVkStMemoryBarrier;
        mb.srcAccessMask = kSVkAccessMemoryRead | kSVkAccessMemoryWrite;
        mb.dstAccessMask = kSVkAccessMemoryRead | kSVkAccessMemoryWrite;
        const SVkHandle ev = g_vkm.ev_out[pair];
        g_svk.CmdWaitEvents(cb, 1, &ev, kSVkStageHost, kSVkStageAllCommands,
                            1, &mb, 0, nullptr, 0, nullptr);
    }

    // stage 2 parks and evaluates and writes nothing, which proves the park is
    // safe and inside its budget without ever putting a wrong pixel on screen.
    // stage 3 is the copy back. mode is tested with it because mode=1 skips the
    // NGX evaluate entirely -- Synth12EvaluateShared returns success without
    // running it -- so this copy would put a never-written texture over the
    // game's own image, and mode=0 is documented everywhere else as never
    // writing to the game. Both sibling paths already gate on mode; only this
    // one did not.
    if (g_cfg.stage >= 3 && g_cfg.mode == 2)
    {
        SynthVkBarrier(cb, g_svk.img[SLOT_OUTPUT], kSVkLayoutGeneral, kSVkLayoutTransferSrc);
        SynthVkBarrier(cb, res[SLOT_OUTPUT].image, kSVkLayoutGeneral, kSVkLayoutTransferDst,
                       kSVkAspectColor);
        SynthVkCopyImage(cb, g_svk.img[SLOT_OUTPUT], kSVkLayoutTransferSrc,
                         res[SLOT_OUTPUT].image, kSVkLayoutTransferDst,
                         ow, oh, res[SLOT_OUTPUT].w, res[SLOT_OUTPUT].h);
        SynthVkBarrier(cb, res[SLOT_OUTPUT].image, kSVkLayoutTransferDst, kSVkLayoutGeneral,
                       kSVkAspectColor);
        SynthVkBarrier(cb, g_svk.img[SLOT_OUTPUT], kSVkLayoutTransferSrc, kSVkLayoutGeneral);
    }

    // Counted where it is recorded rather than where it completes: the worker
    // finishes this frame after the detour has returned, and by then the game has
    // already submitted the command buffer that carries it.
    const UINT64 f = ++g_vkm.frames;
    if (f <= 1 || (f % 1800) == 0)
        Log("[vkmirror] frame %llu recorded (%ux%u -> %ux%u), stage=%d", f, rw, rh, ow, oh,
            g_cfg.stage);
}

// ---------------------------------------------------------------------------
// The detours
//
// Structurally the D3D11 mirror's, and copied rather than shared for one
// reason: ForwardEvaluate takes an ID3D11DeviceContext * and ends in BridgeFrame,
// and threading a variant type through it to serve two APIs is more code than the
// forty lines it duplicates -- in the file nine shipped titles depend on. What IS
// shared is everything the copy calls: g_nest, HookRemove/HookRestore, g_hook_cs,
// the bogus-pointer guard, IsOtherFeature and its reuse invalidation, g_eval_count,
// RetractIdleNote, TryInstallHooks, and the one-way g_source latch.
//
// Lift policy is the same too: one hook lifted per forwarded call, g_nest for the
// rest. The peer lifts every evaluate patch and justifies it with an X4 stack
// overflow that has since been traced to its own DllMain missing a
// DLL_PROCESS_DETACH branch -- so there is no measured reason to change a policy
// nine shipping titles run on.
// ---------------------------------------------------------------------------

typedef NVSDK_NGX_Result (*PFN_VkEvaluate)(void *, const NVSDK_NGX_Handle *,
                                           const NVSDK_NGX_Parameter *, void *);
typedef NVSDK_NGX_Result (*PFN_VkCreate)(void *, int, NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
typedef NVSDK_NGX_Result (*PFN_VkCreate1)(void *, void *, int, NVSDK_NGX_Parameter *,
                                          NVSDK_NGX_Handle **);

// g_ngx_cs OUTSIDE g_hook_cs, always in that order, so the pair is never taken
// the other way round anywhere. The worker takes g_ngx_cs alone.
#define VKM_FORWARD(call)                        \
    if (g_ngx_cs_ready) EnterCriticalSection(&g_ngx_cs); \
    EnterCriticalSection(&g_hook_cs);            \
    HookRemove(h);                               \
    NVSDK_NGX_Result r_ = (call);                \
    HookRestore(h);                              \
    LeaveCriticalSection(&g_hook_cs);            \
    if (g_ngx_cs_ready) LeaveCriticalSection(&g_ngx_cs); \
    return r_;

static NVSDK_NGX_Result ForwardVkEvaluate(Hook &h, const char *tag, void *cmd,
                                          const NVSDK_NGX_Handle *handle,
                                          const NVSDK_NGX_Parameter *p, void *cb)
{
    auto fwd = reinterpret_cast<PFN_VkEvaluate>(h.target);

    // Nested: the layer above already handled this frame and is calling down
    // through NGX's own plumbing.
    if (g_nest > 0) { VKM_FORWARD(fwd(cmd, handle, p, cb)) }
    NestGuard nest;

    // The bottom 64 KB of the address space can never be mapped, so an argument
    // below it is whatever was in the register when a confused detour was
    // entered, not a pointer.
    if (reinterpret_cast<uintptr_t>(p) < 0x10000 ||
        reinterpret_cast<uintptr_t>(handle) < 0x10000 ||
        reinterpret_cast<uintptr_t>(cmd) < 0x10000)
    {
        static LONG said = 0;
        if (InterlockedCompareExchange(&said, 1, 0) == 0)
            Log("[vkmirror] an evaluate arrived with handle=%p params=%p cmd=%p -- none of "
                "those can be real. Forwarding it untouched and ignoring it.",
                static_cast<const void *>(handle), static_cast<const void *>(p), cmd);
        VKM_FORWARD(fwd(cmd, handle, p, cb))
    }

    if (IsOtherFeature(handle)) { VKM_FORWARD(fwd(cmd, handle, p, cb)) }

    const LONG n = InterlockedIncrement(&g_eval_count);
    if (n == 1) RetractIdleNote();

    // One-way and unconditional, exactly as on D3D11: the game's own DLSS wins
    // from any state, and a synthetic contract that was holding the field loses
    // it whatever it was in the middle of.
    const LONG prev_source = InterlockedExchange(&g_source, SRC_MIRROR);

    if (g_scan_pending && !g_ngx_init_in_flight)
    { g_scan_pending = false; TryInstallHooks(); }
    if ((n % 600) == 0) TryInstallHooks();
    if (n <= 5 || (n % 1800) == 0) Log("  (entry point: %s)", tag);

    // Ray Reconstruction hands over the identical Color, Depth, MotionVectors and
    // Output, so the four resources cannot tell it apart from super resolution.
    // Mirroring one upscales still-noisy colour and copies it over an output the
    // game had already denoised: on screen that is denoising switched off.
    if (const char *dkey = DenoiserKeyPresentVk(p))
    {
        static LONG said_rr = 0;
        if (InterlockedCompareExchange(&said_rr, 1, 0) == 0)
            Log("[vkmirror] this evaluate sets \"%s\", which only the denoiser sets, so it is "
                "Ray Reconstruction and not super resolution. Forwarded untouched.", dkey);
        VKM_FORWARD(fwd(cmd, handle, p, cb))
    }

    // ALWAYS forwarded. skip_game does not apply here: if the mirror's copy back
    // never lands -- a refusal, a park timeout, a stand-down, an exception -- the
    // frame on screen is the game's own DLSS output and is correct. That property
    // is what lets nearly every failure row end in a working frame.
    Breadcrumb("forwarding the game's Vulkan DLSS evaluate");
    EnterCriticalSection(&g_hook_cs);
    HookRemove(h);
    NVSDK_NGX_Result r = fwd(cmd, handle, p, cb);
    HookRestore(h);
    LeaveCriticalSection(&g_hook_cs);

    if (n <= 5) Log("--- Vulkan EvaluateFeature #%ld returned %d ---", n, r);

    EnterCriticalSection(&g_bridge_cs);
    if (prev_source == SRC_SYNTH)
    {
        // A substitute that was mid-frame when this arrived has left a feature
        // standing that was built for a DLAA contract over ReShade's depth and
        // somebody else's motion vectors.
        g_s12.w = 0;
        g_s12.need_reset = true;
    }
    VkMirrorFrame(cmd, p);
    LeaveCriticalSection(&g_bridge_cs);

    return r;
}

// Both create entry points record every feature_id other than 1 into the same
// g_other_feature table the D3D11 path fills, with the same reuse invalidation:
// NGX recycles freed handle addresses, and a super-resolution feature created at
// an address the table still holds would be ignored for the rest of the session.
static void VkmNoteCreated(int feature_id, NVSDK_NGX_Result r, NVSDK_NGX_Handle **out)
{
    if (r != NGX_SUCCESS || out == nullptr || *out == nullptr) return;

    if (feature_id == 1)
    {
        for (LONG i = 0; i < g_other_feature_count; ++i)
            if (g_other_feature[i] == *out)
            {
                g_other_feature[i] = g_other_feature[--g_other_feature_count];
                Log("  handle %p was recorded as another feature and has been reused for "
                    "super resolution; the old entry is dropped.", static_cast<void *>(*out));
                break;
            }
        return;
    }
    if (g_other_feature_count < static_cast<LONG>(_countof(g_other_feature)))
    {
        g_other_feature[g_other_feature_count++] = *out;
        Log("  feature %d is not super resolution; evaluates on this handle will be "
            "forwarded and otherwise ignored.", feature_id);
    }
}

static NVSDK_NGX_Result ForwardVkCreate(Hook &h, void *cmd, int feature_id,
                                        NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **out)
{
    auto fwd = reinterpret_cast<PFN_VkCreate>(h.target);
    if (g_nest > 0) { VKM_FORWARD(fwd(cmd, feature_id, p, out)) }
    NestGuard nest;

    if (reinterpret_cast<uintptr_t>(p) < 0x10000 || reinterpret_cast<uintptr_t>(cmd) < 0x10000)
    {
        static LONG said = 0;
        if (InterlockedCompareExchange(&said, 1, 0) == 0)
            Log("[vkmirror] a create arrived with params=%p cmd=%p -- neither can be real. "
                "Forwarding it untouched and ignoring it.", static_cast<void *>(p), cmd);
        VKM_FORWARD(fwd(cmd, feature_id, p, out))
    }

    Breadcrumb("forwarding the game's Vulkan DLSS create");
    const LONG n = InterlockedIncrement(&g_create_count);
    if (n == 1) RetractIdleNote();
    Log("=== Vulkan CreateFeature #%ld  feature_id=%d  thread %lu ===", n, feature_id,
        GetCurrentThreadId());

    EnterCriticalSection(&g_hook_cs);
    HookRemove(h);
    NVSDK_NGX_Result r = fwd(cmd, feature_id, p, out);
    HookRestore(h);
    LeaveCriticalSection(&g_hook_cs);

    Log("=== Vulkan CreateFeature #%ld returned %d, handle=%p ===", n, r,
        (out != nullptr && *out != nullptr) ? static_cast<void *>(*out) : nullptr);
    VkmNoteCreated(feature_id, r, out);

    // The game replacing its own super-resolution feature invalidates ours, and
    // this did nothing about it until now: the shape test is the only thing that
    // triggers a rebuild, and a game can recreate at an unchanged render and
    // output size. Red Dead Redemption 2 does exactly that when HDR is switched
    // on or the window goes fullscreen -- three creates in a minute at
    // 2560x1067 -> 3840x1600 throughout -- and the mirror carried its old feature
    // across all three.
    //
    // Two things are wrong with carrying it. The temporal history it holds was
    // accumulated under the configuration the game has just discarded, which is
    // the same objection this file already makes to swapping motion-vector
    // estimators mid-session. And the neural add-on downstream matches features
    // by create: ours never created again, so it had nothing to match and said
    // so -- "no NR feature matched" -- while the mirror went on delivering.
    //
    // A zero width is the rebuild request the handover path already uses, so this
    // is the same mechanism rather than a new one. feature_id 1 only: a denoiser
    // or another feature being created says nothing about the one we mirror.
    if (r == NGX_SUCCESS && feature_id == 1)
    {
        EnterCriticalSection(&g_bridge_cs);
        g_s12.w = 0;
        g_s12.need_reset = true;
        // Which of the two zeroes this is. Both ask for the same rebuild, and the
        // log line has to tell them apart or it reports a handover that did not
        // happen -- the source did not change, the game replaced its own feature.
        g_vkm.rebuild_on_game_create = true;
        LeaveCriticalSection(&g_bridge_cs);
    }
    return r;
}

// CreateFeature1 has no D3D11 or D3D12 analogue: it prepends a VkDevice. It is a
// distinct export at a distinct address, so a game reaching DLSS through it while
// only CreateFeature is patched would be unobserved.
static NVSDK_NGX_Result ForwardVkCreate1(Hook &h, void *dev, void *cmd, int feature_id,
                                         NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **out)
{
    auto fwd = reinterpret_cast<PFN_VkCreate1>(h.target);
    if (g_nest > 0) { VKM_FORWARD(fwd(dev, cmd, feature_id, p, out)) }
    NestGuard nest;

    if (reinterpret_cast<uintptr_t>(p) < 0x10000 || reinterpret_cast<uintptr_t>(cmd) < 0x10000 ||
        reinterpret_cast<uintptr_t>(dev) < 0x10000)
    {
        static LONG said = 0;
        if (InterlockedCompareExchange(&said, 1, 0) == 0)
            Log("[vkmirror] a create1 arrived with dev=%p params=%p cmd=%p -- none of those "
                "can be real. Forwarding it untouched and ignoring it.", dev,
                static_cast<void *>(p), cmd);
        VKM_FORWARD(fwd(dev, cmd, feature_id, p, out))
    }

    Breadcrumb("forwarding the game's Vulkan DLSS create1");
    const LONG n = InterlockedIncrement(&g_create_count);
    if (n == 1) RetractIdleNote();
    Log("=== Vulkan CreateFeature1 #%ld  feature_id=%d  VkDevice=%p  thread %lu ===", n,
        feature_id, dev, GetCurrentThreadId());

    EnterCriticalSection(&g_hook_cs);
    HookRemove(h);
    NVSDK_NGX_Result r = fwd(dev, cmd, feature_id, p, out);
    HookRestore(h);
    LeaveCriticalSection(&g_hook_cs);

    Log("=== Vulkan CreateFeature1 #%ld returned %d, handle=%p ===", n, r,
        (out != nullptr && *out != nullptr) ? static_cast<void *>(*out) : nullptr);
    VkmNoteCreated(feature_id, r, out);
    return r;
}

#undef VKM_FORWARD

// ---------------------------------------------------------------------------
// synth.inc -- a DLSS contract built from ReShade's own buffers
//
// Two halves, and the first still runs on every session. The measuring half
// answers one question in the log: if this add-on had to build a DLSS contract
// out of ReShade's depth buffer and a motion-vector shader, would the pieces be
// there. It reads, it measures, and it prints, and it touches no NGX parameter.
//
// The building half, below the report, does the thing the report was measuring
// for -- but only when the config asks for it, the configured delay has passed
// and the game has still never called DLSS itself. It ships off: synth_after
// defaults to 0, and a user opts in with one line.
//
// Kept out of bridge.inc on purpose. bridge.inc is the file that runs in nine
// shipped titles and nobody should have to edit it for a reason that belongs to
// synthesis; the note above NoteAbsentParam is the whole argument.
//
// The ReShade headers under reshade/ are vendored verbatim rather than
// hand-declared. Reaching find_texture_variable means indexing virtual slot 37
// of effect_runtime, and the only two ways to get 37 are to count 37 pure
// virtuals by hand or to let the compiler count them. A hand-written table that
// drifts produces a jump into whatever ReShade put at that slot -- no compile
// error, no crash near the mistake. This project has already lost releases to
// code landing in the wrong function; it does not need a table of forty-five
// dummy declarations that nothing verifies. Their licence is BSD-3-Clause OR
// MIT, so the MIT arm applies here; reshade/LICENSE.md carries the notice.

// ---------------------------------------------------------------------------

#include "reshade/reshade_events.hpp"

// Set by RegisterWithReShade the moment it succeeds. The module handle is needed
// because ReShadeRegisterEvent is resolved from the same DLL the registration
// came from, and the version is needed because it decides whether any of this is
// allowed to touch an effect_runtime at all.
static HMODULE  g_reshade_dll;
static uint32_t g_reshade_api;

// find_texture_variable is virtual slot 37 of effect_runtime, and
// get_texture_binding slot 45, in every ReShade from 6.0.0 (API 10) through
// 6.8.0 (API 20) -- which is the layout the vendored headers describe. ReShade
// 5.9.0 (API 8) puts them at 33 and 41: effect_runtime derived from swapchain
// rather than device_object then, so the whole table sits four slots earlier.
//
// RegisterWithReShade counts its requested version down to 5 so the D3D11 mirror
// keeps working on those older builds, which deliberately defeats the ABI guard
// ReShade would otherwise apply. That is correct for the mirror, which never
// touches an effect_runtime, and fatal here. So the floor is checked separately
// and stated with both numbers, rather than assumed from the fact that we
// registered at all.
static const uint32_t kSynthApiFloor = 10;

// Any texture annotated ": DEPTH" binds to the same entry in ReShade's semantic
// map, so the NAME is arbitrary and a list of names can never be complete. There
// is no find-by-semantic in the add-on API, so a list is what there is. Two so
// far, both observed: ReShade.fxh declares "texture DepthBufferTex : DEPTH;" in
// namespace ReShade; vort_Shaders declares "texture2D DepthTex : DEPTH;" in
// namespace Depth (vort_Depth.fxh:5), and with only vort loaded in Baldur's
// Gate 3 this add-on reported "NOT PRESENT" for a depth buffer that was bound.
// find_texture_variable matches the plain declared name as well as the mangled
// one, so the unqualified name is what to ask for.
static const char *kSynthDepthNames[] = { "DepthBufferTex", "DepthTex" };

// The providers do not agree on a name, and they do not agree on a direction
// either. Order is preference: the first one that is bound wins.
//
// sign_x/sign_y are the provider's own convention where it can be read out of
// source we are allowed to read, and 0 where it cannot. vort_Shaders is MIT, so
// its convention is a fact rather than a guess: CalcMV samples the previous
// frame at "i.uv + total_motion" (vort_MotionVectors.fxh:232), which makes the
// stored vector uv_prev - uv_cur in ReShade's +y-down UV -- the direction DLSS
// wants, unmodified, hence +1,+1. GetCameraVelocity agrees from the other side,
// computing curr_screen - prev_screen then scaling by (-0.5, +0.5)
// (vort_MotionUtils.fxh:81-92). The other two are all-rights-reserved or
// non-commercial, so their direction is not readable here and stays 0, which
// means "fall back to the config knob".
//
// vort is first because it is the only one of the three that is MIT, compiles on
// current ReShade (verified: 0.59 s in Baldur's Gate 3, 2026-08-30) and states
// its direction in source. Note it writes MotVectTexVort only at V_MV_MODE 1,
// its default and the only mode where it COMPUTES vectors; at modes 2 and 3 it
// consumes someone else's, which is why the other two names still matter.
struct SynthMvProvider
{
    const char *name;
    int         sign_x;
    int         sign_y;
    const char *label;
};

// Row 0 has no ReShade texture. NVOFA is the driver's own optical flow engine
// and this add-on creates the texture it writes into, so there is no name to
// hand find_texture_variable and the two loops that walk these names start at 1.
// Everything else about a provider -- the label the report prints, the direction
// MV.Scale takes its sign from, the index the panel reads -- is the same
// question for this row as for the other three, so it is the same table.
//
// Its direction is not documented, it is measured: 1,166,400 vectors on this
// machine, 100% within 0.5 px of ground truth, with inputFrame = the current
// frame and referenceFrame = the previous one. That is the DLSS convention on
// both axes with no Y flip.
static const SynthMvProvider kSynthMv[] = {
    { "NVIDIA optical flow", 1, 1, "the driver's own optical flow engine (measured)" },
    { "MotVectTexVort",      1, 1, "vort_Shaders vort_Motion.fx (MIT)" },
    { "texMotionVectors",    0, 0, "a texMotionVectors provider (DRME, qUINT_of)" },
    { "MotionVectorsTex",    0, 0, "iMMERSE Launchpad" },
};

// Where the ReShade texture names start. kSynthMv[0] is not one.
static const int kSynthMvReShadeFirst = 1;

// The four ReShade.fxh definitions that say how to read the depth values. They
// are #ifndef defaults in the shader header, not engine state, so a user who has
// not overridden one leaves it absent -- and absent means the default, which for
// RESHADE_DEPTH_INPUT_IS_REVERSED is 1 rather than 0. Reading a miss as "off"
// would invert the one that matters.
struct SynthDepthDef
{
    const char *name;
    int         fxh_default;
};

static const SynthDepthDef kSynthDepthDefs[] = {
    { "RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN",        0 },
    { "RESHADE_DEPTH_INPUT_IS_REVERSED",           1 },
    { "RESHADE_DEPTH_INPUT_IS_LOGARITHMIC",        0 },
    { "RESHADE_DEPTH_LINEARIZATION_FAR_PLANE", 1000 },
};

enum { SYNTH_DEF_UPSIDE_DOWN, SYNTH_DEF_REVERSED, SYNTH_DEF_LOGARITHMIC, SYNTH_DEF_FAR_PLANE };

// One frame's worth of answers. Everything the report prints is either in here
// or derived from it, so the report cannot state something that was not measured.
struct SynthFacts
{
    uint32_t api;
    // reshade::api::device_api, exactly as the runtime's own device answered it.
    // Stored rather than reduced to a yes/no, because there are two transports
    // now and the report has to name the one this session actually got.
    uint32_t device_api;
    uint32_t back_w, back_h;

    int      depth_found;            // a texture variable of that name exists
    int      depth_bound;            // and the DEPTH semantic has a live binding
    UINT     depth_w, depth_h, depth_samples;
    int      depth_is_dsv;           // the binding is the game's own depth-stencil texture
    DXGI_FORMAT depth_fmt;
    int      depth_read;             // the readback actually returned pixels
    int      depth_varies;
    UINT     depth_lo, depth_hi;

    // Only read on Vulkan, and measured everywhere so the report and the refusal
    // both come off the same struct rather than off a second lookup.
    uint32_t depth_usage, mv_usage;

    int      mv_which;               // index into kSynthMv, -1 for none
    int      depth_which;            // index into kSynthDepthNames, -1 for none
    int      mv_bound;
    UINT     mv_w, mv_h, mv_samples;
    DXGI_FORMAT mv_fmt;

    int      def_present[_countof(kSynthDepthDefs)];
    int      def_value[_countof(kSynthDepthDefs)];
};

// The report is printed on change, not on a timer, so it has to be possible to
// say two frames gave the same answer. Hashing the measured struct means a new
// fact added above is compared without anybody remembering to extend a list.
static uint64_t SynthSignature(const SynthFacts *f)
{
    uint64_t h = 1469598103934665603ull;
    const unsigned char *p = reinterpret_cast<const unsigned char *>(f);
    for (size_t i = 0; i < sizeof(*f); ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// D3D24-with-stencil depth keeps the stencil in the top byte, and stencil that
// varies over a flat depth buffer would read as depth that varies. The point of
// the check is to catch the buffer that is present and says nothing, so the byte
// that is not depth is dropped before anything is compared.
static UINT SynthDepthBits(DXGI_FORMAT f, UINT raw)
{
    switch (f)
    {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
        return raw & 0x00FFFFFFu;
    default:
        return raw;
    }
}

// A ReShade resource_view handle on a D3D11 device is the ID3D11View itself --
// ReShade's own to_handle takes ID3D11View * and reinterpret_casts it, nothing
// else -- so once the binding is in hand no further ReShade type is involved.
//
// ID3D11View rather than ID3D11ShaderResourceView because stage 3 asks this the
// same question about the render target view reshade_begin_effects hands over.
// GetResource is ID3D11View's, so both answer it; naming the base is what makes
// that true by declaration rather than by the two vtables happening to agree.
static ID3D11Texture2D *SynthTextureFromView(reshade::api::resource_view v)
{
    if (v.handle == 0) return nullptr;
    auto *view = reinterpret_cast<ID3D11View *>(static_cast<uintptr_t>(v.handle));

    ID3D11Resource *res = nullptr;
    view->GetResource(&res);
    if (res == nullptr) return nullptr;

    ID3D11Texture2D *tex = nullptr;
    res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex));
    res->Release();
    return tex;   // caller releases
}

// One bound texture, in whichever API the effect runtime is on.
//
// res is what both transports and NGX take: the device's own native resource
// pointer, which is exactly what a reshade::api::resource handle is documented
// to be -- an ID3D11Resource on a D3D11 device, an ID3D12Resource on a D3D12
// one. tex11 is the same object seen as D3D11 and is null on any other API; it
// exists so the depth readback and the D3D11 measurement keep the exact calls
// they were verified with.
struct SynthBound
{
    reshade::api::resource res;
    ID3D11Texture2D       *tex11;   // caller releases when not null
    UINT        w, h, samples;
    DXGI_FORMAT fmt;
    int         is_dsv;
    // reshade::api::resource_usage, straight off the description ReShade keeps
    // for the resource. It exists for one question, asked only on Vulkan: a
    // transfer command may not read an image that was not created as a transfer
    // source, and neither of the two textures this add-on reads is created by
    // this add-on. Left zero on D3D11, where nothing asks.
    uint32_t    usage;
};

// The D3D11 half is the code that was already here, moved rather than rewritten:
// it runs in every shipped session and its facts have to keep coming from the
// same GetDesc. The D3D12 half exists because a D3D12 resource view handle is a
// descriptor handle and says nothing about the texture behind it, so the
// resource has to be asked for by name.
static void SynthResolveView(reshade::api::device *dev, uint32_t api,
                             reshade::api::resource_view v, SynthBound *out)
{
    SynthBound blank = {};
    *out = blank;
    if (v.handle == 0) return;

    if (api == static_cast<uint32_t>(reshade::api::device_api::d3d11))
    {
        out->tex11 = SynthTextureFromView(v);
        if (out->tex11 == nullptr) return;
        D3D11_TEXTURE2D_DESC td = {};
        out->tex11->GetDesc(&td);
        out->res     = reshade::api::resource { reinterpret_cast<uintptr_t>(out->tex11) };
        out->w       = td.Width;
        out->h       = td.Height;
        out->fmt     = td.Format;
        out->samples = td.SampleDesc.Count;
        out->is_dsv  = (td.BindFlags & D3D11_BIND_DEPTH_STENCIL) ? 1 : 0;
        return;
    }

    if (dev == nullptr) return;
    out->res = dev->get_resource_from_view(v);
    if (out->res.handle == 0) return;   // D3D12 allows a view over no resource

    // reshade::api::format carries DXGI's own numbering for every DXGI format,
    // so this cast is the identity it looks like rather than a table lookup
    // somebody has to keep in step.
    const reshade::api::resource_desc rd = dev->get_resource_desc(out->res);
    out->w       = rd.texture.width;
    out->h       = rd.texture.height;
    out->fmt     = static_cast<DXGI_FORMAT>(rd.texture.format);
    out->samples = rd.texture.samples;
    out->is_dsv  = ((rd.usage & reshade::api::resource_usage::depth_stencil) !=
                    reshade::api::resource_usage::undefined) ? 1 : 0;
    out->usage   = static_cast<uint32_t>(rd.usage);
}

// Returns whether the name exists at all, and separately what it is bound to.
// Those are different answers with different fixes -- "no loaded effect declares
// it" versus "declared but the semantic has no live binding" -- and
// get_texture_binding on its own cannot tell them apart, because it answers zero
// for a name that does not exist too.
//
// get_texture_binding leaves both outputs completely untouched when the handle
// is valid but its semantic has no entry in ReShade's binding map, which is what
// generic depth leaves behind every time no depth buffer is selected: it erases
// the entry rather than storing a zero. An uninitialised local would then be
// read back as a live shader resource view. Zeroing first is the caller's job
// and nobody else's, so it is done here where it cannot be forgotten.
static bool SynthLookup(reshade::api::effect_runtime *rt, const char *name,
                        reshade::api::resource_view *out_srv)
{
    *out_srv = reshade::api::resource_view { 0 };

    // A null handle is also what comes back while effects are reloading, so this
    // is safe to call on any frame.
    reshade::api::effect_texture_variable var = rt->find_texture_variable(nullptr, name);
    if (var.handle == 0) return false;

    reshade::api::resource_view srv_srgb = { 0 };
    rt->get_texture_binding(var, out_srv, &srv_srgb);
    return true;
}

static void SynthReadDefs(reshade::api::effect_runtime *rt, SynthFacts *f)
{
    for (int i = 0; i < static_cast<int>(_countof(kSynthDepthDefs)); ++i)
    {
        char   text[64] = {};
        size_t n        = sizeof(text);

        // A miss returns false and writes nothing, so the default from
        // ReShade.fxh is what the shader actually compiled with. A hit with an
        // empty value is a bare #define, which the FX preprocessor cannot
        // evaluate as a number either, so that also falls back.
        f->def_present[i] = rt->get_preprocessor_definition(kSynthDepthDefs[i].name, text, &n) ? 1 : 0;
        f->def_value[i]   = (f->def_present[i] && text[0] != '\0')
                                ? atoi(text)
                                : kSynthDepthDefs[i].fxh_default;
    }
}

// Five points rather than one. A single sample cannot distinguish a depth buffer
// from a cleared one, and "present but flat" is the total-failure case: DLSS
// given a constant depth has no geometry to reproject against and produces a
// smear, with nothing in any log to say why. The neighbouring project misses
// this by testing a handle to a render target it created itself, which is always
// present and never flat.
static void SynthProbeDepth(reshade::api::command_list *cmd, ID3D11Texture2D *tex, SynthFacts *f)
{
    auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(
        static_cast<uintptr_t>(cmd->get_native()));
    if (ctx == nullptr) return;

    // Multisampled depth cannot be copied into a single-sampled staging texture,
    // so there is nothing to read and saying so beats reporting a failed read as
    // a flat buffer.
    if (f->depth_samples != 1) return;

    // Same reasoning, and the case that actually bites: D3D11 will only copy a
    // depth-stencil resource whole -- CopySubresourceRegion demands a null box
    // when either side carries BIND_DEPTH_STENCIL -- and ReShade binds the
    // game's own depth texture directly whenever generic depth has no backup
    // copy to hand out. The boxed copy below is then rejected, Map still
    // succeeds on a staging texture nothing wrote, and all five points come back
    // equal: a buffer full of geometry reported as FLAT, which is the loudest
    // wrong answer this file could give.
    if (f->depth_is_dsv) return;

    ID3D11Device *dev11 = nullptr;
    ctx->GetDevice(&dev11);
    if (dev11 == nullptr) return;

    const UINT xs[5] = { f->depth_w / 4, f->depth_w * 3 / 4, f->depth_w / 2,
                         f->depth_w / 4, f->depth_w * 3 / 4 };
    const UINT ys[5] = { f->depth_h / 4, f->depth_h / 4, f->depth_h / 2,
                         f->depth_h * 3 / 4, f->depth_h * 3 / 4 };

    int got = 0;
    for (int i = 0; i < 5; ++i)
    {
        UINT raw = 0;
        if (!ReadPixelRaw(ctx, dev11, tex, f->depth_fmt, xs[i], ys[i], &raw)) continue;
        const UINT d = SynthDepthBits(f->depth_fmt, raw);
        if (got == 0) { f->depth_lo = f->depth_hi = d; }
        else if (d < f->depth_lo) f->depth_lo = d;
        else if (d > f->depth_hi) f->depth_hi = d;
        ++got;
    }

    dev11->Release();
    f->depth_read   = (got > 0) ? 1 : 0;
    f->depth_varies = (f->depth_hi != f->depth_lo) ? 1 : 0;
}

static const char *SynthFormatName(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16_FLOAT:          return "R16G16_FLOAT";
    case DXGI_FORMAT_R32G32_FLOAT:          return "R32G32_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_FLOAT:    return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R32_FLOAT:             return "R32_FLOAT";
    case DXGI_FORMAT_R32_TYPELESS:          return "R32_TYPELESS";
    case DXGI_FORMAT_R24G8_TYPELESS:        return "R24G8_TYPELESS";
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return "R24_UNORM_X8_TYPELESS";
    case DXGI_FORMAT_D24_UNORM_S8_UINT:     return "D24_UNORM_S8_UINT";
    case DXGI_FORMAT_D32_FLOAT:             return "D32_FLOAT";
    // The two a Vulkan game routinely arrives with. Naming them is what lets the
    // refusal for a depth format the Vulkan transport cannot carry point at the
    // measurement instead of at a number.
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:  return "D32_FLOAT_S8X24_UINT";
    case DXGI_FORMAT_D16_UNORM:             return "D16_UNORM";
    default:                                return "other";
    }
}

static const char *SynthApiName(uint32_t api)
{
    switch (static_cast<reshade::api::device_api>(api))
    {
    case reshade::api::device_api::d3d9:   return "D3D9";
    case reshade::api::device_api::d3d10:  return "D3D10";
    case reshade::api::device_api::d3d11:  return "D3D11";
    case reshade::api::device_api::d3d12:  return "D3D12";
    case reshade::api::device_api::opengl: return "OpenGL";
    case reshade::api::device_api::vulkan: return "Vulkan";
    default:                               return "an API this add-on has no name for";
    }
}

// Three transports ship in one binary, and which one a session gets is decided
// by the device ReShade is on -- never by what was compiled in. Everything that
// prints a backend reads it through here, off SynthFacts::device_api, which was
// filled from the runtime's own get_api.
static bool SynthApiHasBackend(uint32_t api)
{
    return api == static_cast<uint32_t>(reshade::api::device_api::d3d11) ||
           api == static_cast<uint32_t>(reshade::api::device_api::d3d12) ||
           api == static_cast<uint32_t>(reshade::api::device_api::vulkan);
}

// ---------------------------------------------------------------------------
// The verdict, computed once
//
// Extracted from SynthReport so the report and the report's own rate limit read
// the SAME computation. The alternative -- a second copy of these conditions
// kept in step by hand -- has already failed twice in this file: SynthArmRefusal
// and this verdict disagreed about the depth size and about the sample count,
// and each time the report said "every precondition holds" over a session the
// gate refused. A comparison that is meant to detect a changed verdict has to be
// derived from the verdict itself, or it drifts the same way.
// ---------------------------------------------------------------------------

struct SynthVerdictFacts
{
    bool depth_ok;
    bool mv_ok;
    bool aside;
    bool depth_size_ok;
    bool vk_depth_ok;
    bool vk_mv_ok;
};

static SynthVerdictFacts SynthVerdictOf(const SynthFacts *f)
{
    SynthVerdictFacts v = {};
    // The two extra conditions the Vulkan transport puts on the same two
    // textures, folded in here so the verdict is the verdict for the transport
    // this session would actually use rather than for a different one.
    const bool vk = f->device_api == static_cast<uint32_t>(reshade::api::device_api::vulkan);
    const uint32_t copy_src = static_cast<uint32_t>(reshade::api::resource_usage::copy_source);
    const uint32_t shader_r = static_cast<uint32_t>(reshade::api::resource_usage::shader_resource);
    // Rewritten with the refusal it mirrors: the depth is sampled now, so what
    // it needs is shader_resource, and its format is no longer a question at all.
    v.vk_depth_ok = !vk || (f->depth_usage & shader_r) != 0;
    // Same clause as the gate's, for the same reason: row 0 has no ReShade
    // texture to ask about.
    v.vk_mv_ok    = !vk || f->mv_which == 0 || (f->mv_usage & copy_src) != 0;

    // The size test was in SynthArmRefusal and missing here, so this verdict could
    // read "every precondition holds" over a session the gate would refuse -- and
    // it did, in Baldur's Gate 3 on 2026-08-30: depth 2560x1067 against a
    // 3840x1600 back buffer, because a game that runs DLSS renders its depth at
    // render resolution while ReShade presents at output resolution. A game with
    // no DLSS of its own -- the only kind synth is for -- has the two equal, so
    // this would have stayed dormant until a field report nobody could explain.
    // Every condition the gate refuses on has to be a condition this verdict
    // fails on, or the report is worse than no report.
    // Checked against SynthArmRefusal condition by condition on 2026-08-30. Two
    // were missing: the size, and this one -- the motion vector arm tested
    // mv_samples == 1 while the depth arm tested nothing, so a multisampled
    // depth buffer passed the verdict and was refused by the gate.
    v.depth_size_ok = f->depth_w == f->back_w && f->depth_h == f->back_h;
    // The probe clause reads the way the GATE reads it and not the other way
    // round, and the difference matters on Vulkan: the five-point probe is
    // written against D3D11 staging textures and never runs there, so
    // depth_read is 0 and this verdict used to print "not viable -- depth
    // unverified" over a session the gate would have armed. Nothing was
    // disproved by not sampling, which is exactly what the gate says by refusing
    // only on read-AND-flat.
    v.depth_ok = f->depth_bound && v.depth_size_ok && f->depth_samples == 1 &&
                          (!f->depth_read || f->depth_varies) && v.vk_depth_ok;
    v.mv_ok    = f->mv_bound && f->mv_fmt == DXGI_FORMAT_R16G16_FLOAT &&
                          f->mv_w == f->back_w && f->mv_h == f->back_h &&
                          f->mv_samples == 1 && v.vk_mv_ok;
    v.aside    = f->def_value[SYNTH_DEF_UPSIDE_DOWN] != 0 ||
                          f->def_value[SYNTH_DEF_LOGARITHMIC] != 0;

    return v;
}

// The depth half of the verdict, as one index into the table below. Most
// specific true reason first: size and sample count are read off a descriptor,
// and an unbound depth has no descriptor -- its width is zero, so an earlier
// size test fires and reports "wrong size" for a buffer whose real problem is
// that generic depth never picked one. Baldur's Gate 3 on 2026-08-31 printed
// exactly that contradiction: "present but NOT BOUND" one line above "depth
// wrong size".
static uint32_t SynthDepthReason(const SynthFacts *f, const SynthVerdictFacts &v)
{
    return v.depth_ok             ? 0u :
           !f->depth_found        ? 1u :
           !f->depth_bound        ? 2u :
           !v.depth_size_ok       ? 3u :
           f->depth_samples != 1  ? 4u :
           // Only reachable on Vulkan, and only for a binding with no
           // shader_resource usage -- which generic_depth's backup always has,
           // so this is the surprise case and is named as one rather than
           // folded into "no".
           !v.vk_depth_ok         ? 5u : 6u;
}

// Indexed by SynthDepthReason, and that is the point: the verdict line and the
// verdict key read the same ladder from the same function, so a reason added to
// one cannot go missing from the other.
static const char *const kSynthDepthReason[] = {
    "ok", "no", "not bound", "wrong size", "multisampled", "not sampleable", "flat"
};

// The ladder's largest index is 6, so the table needs seven entries and the key
// needs three bits -- which is why the mv and aside flags start at 0x10.
static_assert(_countof(kSynthDepthReason) == 7,
              "kSynthDepthReason must have one entry per SynthDepthReason result");

// A small integer standing for exactly what the verdict line prints -- the depth
// reason, whether the motion vectors are ok, and whether a depth encoding stands
// the session aside. Nothing else: the point is that two probes with the same
// story compare equal even when unrelated bytes of SynthFacts moved.
//
// depth_read and depth_varies reach this through depth_ok, so it is only
// meaningful AFTER SynthProbeDepth has run. Computing it before the readback
// makes the flat-depth verdict structurally invisible to the comparison.
static uint32_t SynthVerdictKey(const SynthFacts *f)
{
    const SynthVerdictFacts v = SynthVerdictOf(f);
    return SynthDepthReason(f, v) | (v.mv_ok ? 0x10u : 0u) | (v.aside ? 0x20u : 0u);
}

// Prints every precondition with the measurement behind it. A verdict on its own
// sends people to a forum; the number that produced it is what lets them fix it.
static void SynthReport(const SynthFacts *f)
{
    Log("[synth] ----- could a synthetic DLSS contract be built here? -----");

    Log("[synth]   ReShade add-on API: registered at %u, synth needs %u or newer -- %s",
        f->api, kSynthApiFloor,
        f->api >= kSynthApiFloor ? "ok"
                                 : "REFUSED; effect_runtime has a different vtable below that");
    if (f->api < kSynthApiFloor) return;

    // Derived from the device, not from the build. The two transports differ in
    // where the evaluate happens and how many devices the frame touches, and a
    // support report that does not say which one ran cannot be read at all.
    Log("[synth]   backend: the effect runtime is on %s, so %s",
        SynthApiName(f->device_api),
        f->device_api == static_cast<uint32_t>(reshade::api::device_api::d3d11)
            ? "the frame is copied onto a private D3D12 device and the evaluate runs there"
            : f->device_api == static_cast<uint32_t>(reshade::api::device_api::d3d12)
                ? "the evaluate runs on the game's own D3D12 device and no frame data "
                  "crosses between devices"
            : f->device_api == static_cast<uint32_t>(reshade::api::device_api::vulkan)
                ? "the frame crosses into a private D3D12 device through shared textures "
                  "imported back into the game's Vulkan device, and the evaluate runs there"
                : "there is no transport for it and nothing below can be measured");
    if (!SynthApiHasBackend(f->device_api)) return;

    Log("[synth]   back buffer: %ux%u", f->back_w, f->back_h);

    // Not present and not bound are different failures with different fixes:
    // one needs an effect that samples depth to be enabled, the other needs
    // generic depth to have picked a buffer.
    if (!f->depth_found)
    {
        char tried[128] = {};
        for (int i = 0; i < static_cast<int>(_countof(kSynthDepthNames)); ++i)
        {
            if (i != 0) strcat_s(tried, " or ");
            strcat_s(tried, kSynthDepthNames[i]);
        }
        // "None of the names this add-on knows" and "no depth buffer" are not
        // the same statement, and only the first one is derived here: any
        // texture annotated ": DEPTH" binds to the same semantic, so a provider
        // that spells it differently is invisible to this walk.
        Log("[synth]   depth: NOT PRESENT under %s -- no loaded effect declares a depth "
            "texture under a name this add-on knows. Any texture annotated \": DEPTH\" "
            "would bind to the same semantic, so a shader that spells it otherwise is "
            "invisible here. Generic depth may well have a depth buffer.", tried);
    }
    else if (!f->depth_bound)
        Log("[synth]   depth (%s): present but NOT BOUND -- the DEPTH semantic has no live "
            "binding, which is what generic depth leaves behind when it has not selected a "
            "depth buffer.", kSynthDepthNames[f->depth_which]);
    else
    {
        Log("[synth]   depth (%s): bound, %ux%u %s, %u sample%s", kSynthDepthNames[f->depth_which],
            f->depth_w, f->depth_h, SynthFormatName(f->depth_fmt),
            f->depth_samples, f->depth_samples == 1 ? "" : "s");

        // Said here rather than left to be inferred from two numbers on one
        // line. NGX takes depth at the render resolution the contract declares,
        // and this contract declares the back buffer; a depth surface of another
        // size is a broken contract only NGX can refuse, one evaluate at a time.
        if (f->depth_w != f->back_w || f->depth_h != f->back_h)
            Log("[synth]     size: MISMATCH -- %ux%u against a %ux%u back buffer. A game "
                "running its own upscaler renders depth at render resolution while ReShade "
                "presents at output resolution, which is this case and is not something a "
                "setting fixes. A game with no upscaler of its own has the two equal.",
                f->depth_w, f->depth_h, f->back_w, f->back_h);

        if (!f->depth_read)
            // Each reason is read off a fact that was measured. "not 1 sample"
            // used to stand for all three, so a view this add-on could not
            // resolve to a Texture2D -- which leaves the count at zero, never at
            // two -- was reported as multisampled.
            Log("[synth]   depth values: could not be read back%s",
                f->device_api != static_cast<uint32_t>(reshade::api::device_api::d3d11)
                    ? " -- the five-point probe is written against D3D11 staging textures "
                      "and this runtime is not on D3D11. Nothing was disproved; the buffer "
                      "was simply not sampled"
                : f->depth_is_dsv
                    ? " -- the binding is the game's own depth-stencil texture, and D3D11 "
                      "copies such a resource only whole, never the points this samples"
                    : f->depth_samples > 1
                          ? " -- a multisampled depth buffer cannot be copied to a staging texture"
                          : "");
        else if (!f->depth_varies)
            Log("[synth]   depth values: FLAT -- five points across the buffer all read "
                "0x%08X. A constant depth gives DLSS no geometry to reproject against, and "
                "the result is a smear rather than an error.", f->depth_lo);
        else
            Log("[synth]   depth values: vary across the buffer, 0x%08X to 0x%08X over five "
                "points", f->depth_lo, f->depth_hi);
    }

    if (f->mv_which < 0)
    {
        char tried[128] = {};
        // From kSynthMvReShadeFirst: row 0 is the driver's own optical flow
        // engine and no effect can declare it, so naming it here would send a
        // reader looking for a shader that does not exist. See the table's note.
        for (int i = kSynthMvReShadeFirst; i < static_cast<int>(_countof(kSynthMv)); ++i)
        {
            if (i != kSynthMvReShadeFirst) strcat_s(tried, " or ");
            strcat_s(tried, kSynthMv[i].name);
        }
        Log("[synth]   motion vectors: NOT PRESENT -- no loaded effect declares %s. A motion "
            "estimation shader has to be installed and enabled before there is anything to "
            "read.", tried);
    }
    else if (!f->mv_bound)
    {
        Log("[synth]   motion vectors (%s): present but NOT BOUND", kSynthMv[f->mv_which].name);
    }
    else
    {
        Log("[synth]   motion vectors (%s): bound, %ux%u %s, %u sample%s -- %s",
            kSynthMv[f->mv_which].name, f->mv_w, f->mv_h, SynthFormatName(f->mv_fmt),
            f->mv_samples, f->mv_samples == 1 ? "" : "s", kSynthMv[f->mv_which].label);

        // Which of the two answers produced MV.Scale's sign. Without this line a
        // report of smearing cannot be told apart from a report of a wrong knob.
        {
            const int px = g_cfg.mv_sign_x != 0 ? g_cfg.mv_sign_x : kSynthMv[f->mv_which].sign_x;
            const int py = g_cfg.mv_sign_y != 0 ? g_cfg.mv_sign_y : kSynthMv[f->mv_which].sign_y;
            if (g_cfg.mv_sign_x != 0 || g_cfg.mv_sign_y != 0)
                Log("[synth]     direction: %+d,%+d from mv_sign_x/mv_sign_y in the config", px, py);
            else if (f->mv_which == 0)
                // Above the provider branch below because this one is not a
                // convention read out of somebody's source. It was measured here.
                Log("[synth]     direction: %+d,%+d, measured -- 1,166,400 vectors within "
                    "0.5 px of ground truth on this machine, not a convention read out of "
                    "somebody's source", px, py);
            else if (kSynthMv[f->mv_which].sign_x != 0)
                Log("[synth]     direction: %+d,%+d, this provider's own documented convention",
                    px, py);
            else
                Log("[synth]     direction: this provider does not state one and its source is "
                    "not readable, so +1,+1 is assumed. If the image smears in the direction of "
                    "movement, set mv_sign_y=-1 in dlss5-bridge.cfg.");
        }

        // Both shipped providers declare RG16F at BUFFER_WIDTH x BUFFER_HEIGHT.
        // Anything else is not the texture this was written against, and saying
        // what it actually is turns a report into a fix.
        if (f->mv_fmt != DXGI_FORMAT_R16G16_FLOAT)
            Log("[synth]     shape: WRONG FORMAT -- %s, expected R16G16_FLOAT, which is what "
                "both shipped motion estimation shaders declare",
                SynthFormatName(f->mv_fmt));
        if (f->mv_w != f->back_w || f->mv_h != f->back_h)
            Log("[synth]     shape: WRONG SIZE -- %ux%u against a %ux%u back buffer",
                f->mv_w, f->mv_h, f->back_w, f->back_h);
        if (f->mv_samples != 1)
            Log("[synth]     shape: MULTISAMPLED -- %u samples, and NGX takes single-sampled "
                "motion vectors", f->mv_samples);
        if (f->mv_fmt == DXGI_FORMAT_R16G16_FLOAT && f->mv_w == f->back_w &&
            f->mv_h == f->back_h && f->mv_samples == 1)
            Log("[synth]     shape: matches the back buffer at R16G16_FLOAT");
    }

    // Only on Vulkan, because only there is it a question, and the two textures
    // are asked different questions because they are read in different ways: the
    // motion vectors are copied, so they need copy_source, while the depth is
    // SAMPLED by a compute pass, so it needs shader_resource and nothing else.
    // Neither of them is created by this add-on.
    if (f->device_api == static_cast<uint32_t>(reshade::api::device_api::vulkan))
    {
        const uint32_t copy_src = static_cast<uint32_t>(reshade::api::resource_usage::copy_source);
        const uint32_t shader_r = static_cast<uint32_t>(reshade::api::resource_usage::shader_resource);
        Log("[synth]   Vulkan usage: depth 0x%08X (%s be sampled), motion vectors 0x%08X "
            "(%s be copied from)",
            f->depth_usage, (f->depth_usage & shader_r) ? "can" : "CANNOT",
            f->mv_usage,    (f->mv_usage    & copy_src) ? "can" : "CANNOT");
        Log("[synth]     on Vulkan the DEPTH semantic is bound to ReShade's own depth "
            "backup texture rather than to the game's depth image -- generic_depth forces "
            "a backup on this API -- and that backup is created shader_resource and "
            "copy_dest, with NO copy_source. That is why the depth is read by a compute "
            "pass that samples it rather than by a transfer: a copy cannot be issued "
            "against it, a sample can, and a sampled depth-aspect texel is a normalised "
            "float whatever the storage format is. Neither the format nor the missing "
            "copy_source is a refusal any more.%s",
            (f->depth_usage & copy_src) ? " This binding does carry copy_source as well, "
                                          "which is unusual on this API." : "");
    }

    // Absent means the ReShade.fxh default compiled in, which is not the same as
    // zero -- IS_REVERSED defaults to 1 -- so both the state and the value are
    // printed rather than one standing for the other.
    for (int i = 0; i < static_cast<int>(_countof(kSynthDepthDefs)); ++i)
        Log("[synth]   %s = %d (%s)", kSynthDepthDefs[i].name, f->def_value[i],
            f->def_present[i] ? "set in the preset or the global config"
                              : "not present, so the ReShade.fxh default applies");

    // Standing aside is not the same as failing. These two describe depth this
    // add-on can read but cannot describe to NGX: the create flags say whether
    // depth is inverted and nothing else about its encoding, so a flipped or
    // logarithmic buffer would be handed over as if it were neither.
    if (f->def_value[SYNTH_DEF_UPSIDE_DOWN])
        Log("[synth]   STAND ASIDE: RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN is %d. NGX has no "
            "create flag or parameter for a vertically flipped depth buffer, so there is no "
            "way to state it and it would be read upside down in silence.",
            f->def_value[SYNTH_DEF_UPSIDE_DOWN]);
    if (f->def_value[SYNTH_DEF_LOGARITHMIC])
        Log("[synth]   STAND ASIDE: RESHADE_DEPTH_INPUT_IS_LOGARITHMIC is %d. NGX takes "
            "hardware depth and has no flag for a logarithmic encoding.",
            f->def_value[SYNTH_DEF_LOGARITHMIC]);
    Log("[synth]   RESHADE_DEPTH_INPUT_IS_REVERSED is expressible: it is the DepthInverted "
        "create flag, bit 0x08.");
    Log("[synth]   RESHADE_DEPTH_LINEARIZATION_FAR_PLANE has nothing to express: NGX is given "
        "raw depth, and the far plane is a shader-side convenience.");

    // Only the three the verdict line reads. depth_size_ok and vk_depth_ok were
    // named here too when this was extracted, and their sole remaining use is
    // inside SynthDepthReason, which takes v and asks for itself.
    const SynthVerdictFacts v = SynthVerdictOf(f);
    const bool depth_ok = v.depth_ok;
    const bool mv_ok    = v.mv_ok;
    const bool aside    = v.aside;

    if (depth_ok && mv_ok && !aside)
        Log("[synth]   verdict: every precondition holds. Whether the synthetic contract "
            "is also armed is a separate question -- synth_after, source and the game's "
            "own DLSS decide it, and the arming line says so when it happens.");
    else
        // A depth buffer that is bound but could not be sampled is not the same
        // answer as one that is missing or flat: nothing was disproved, only
        // left unmeasured, and the line above says which. Printing "no" for it
        // would send someone to change a depth setting that is already right.
        Log("[synth]   verdict: not viable as things stand -- depth %s, motion vectors %s%s.",
            kSynthDepthReason[SynthDepthReason(f, v)],
            mv_ok ? "ok" : "no",
            aside ? ", and a depth encoding that cannot be stated to NGX" : "");

    Log("[synth] ----- end of report -----");
}

// ---------------------------------------------------------------------------
// Stage 3: the synthetic contract
//
// The seam this uses was already here. NVSDK_NGX_Parameter is a pure abstract
// class and BridgeFrame takes a const NVSDK_NGX_Parameter *, so a second source
// of contract is a subclass and nothing in the mirror's frame path has to learn
// that it exists. Everything below builds one such object, answers the Gets
// bridge.inc performs, and hands it to the same BridgeFrame the game's evaluate
// detour calls.
//
// The note above NoteAbsentParam binds all of it: this contract arrives already
// complete. Every key in the table below is set explicitly by SynthBuild, and
// SynthSelfCheck reads every one of them back through the object to prove it,
// because a synthetic block that reaches NoteAbsentParam has turned absence into
// a normal operating mode -- and the absence report is what found the MV.Scale
// defect that cost two releases.
// ---------------------------------------------------------------------------

// What a parameter block returns for a key it does not carry. Not invented:
// 0xBAD00010 is UnsupportedParameter, which is what the game's own NGX block
// answers for an unknown key in every log this project has collected, and
// bridge.inc's NgxResultName already names it.
static const NVSDK_NGX_Result kSynthNoSuchKey = static_cast<NVSDK_NGX_Result>(0xBAD00010);

enum SynthKind { SK_UINT, SK_INT, SK_FLOAT, SK_RES };

// The complete inventory of what bridge.inc reads off a source's parameter
// block, derived by reading every Get in the mirror's frame path rather than
// taken from a list: GameTexture's four slots and ExposureTexture in
// BridgeFrameInner, the four size scalars, Reset, the ten subrect keys, the
// seven floats, and PerfQualityValue, DLSS.Feature.Create.Flags and
// DLSS.Enable.Output.Subrects in BridgeBuildResources.
//
// Color, Output, Depth and MotionVectors appear here as the source's own D3D11
// textures. What bridge.inc sets into its D3D12 parameter block are its own
// shared mirrors of them, which is a different set of keys on a different block
// and no business of this one.
//
// Deliberately absent, and absent is the answer rather than an omission:
//
//   ExposureTexture -- there is no exposure texture. BridgeFrameInner reads it
//   directly rather than through the neutral helpers, and treats a failed Get as
//   "the source supplies none", which is exactly true here. Answering with a
//   null would be supplying one.
//
//   the six DLSS.Hint.Render.Preset.* keys -- bridge.inc mirrors only what the
//   source set, because nvsdk_ngx_helpers.h never writes these and absence
//   already means NVSDK_NGX_DLSS_Hint_Render_Preset_Default. Setting one would
//   pick a model this add-on has no measurement to justify picking. Neither
//   reaches NoteAbsentParam, so constraint (a) is untouched either way.
static const struct { const char *key; SynthKind kind; } kSynthSlots[] = {
    { "Color",                                SK_RES   },
    { "Output",                               SK_RES   },
    { "Depth",                                SK_RES   },
    { "MotionVectors",                        SK_RES   },

    { "Width",                                SK_UINT  },
    { "Height",                               SK_UINT  },
    { "OutWidth",                             SK_UINT  },
    { "OutHeight",                            SK_UINT  },

    { "PerfQualityValue",                     SK_UINT  },
    { "DLSS.Feature.Create.Flags",            SK_UINT  },
    { "DLSS.Enable.Output.Subrects",          SK_UINT  },

    { "DLSS.Render.Subrect.Dimensions.Width",  SK_UINT },
    { "DLSS.Render.Subrect.Dimensions.Height", SK_UINT },
    { "DLSS.Input.Color.Subrect.Base.X",      SK_UINT  },
    { "DLSS.Input.Color.Subrect.Base.Y",      SK_UINT  },
    { "DLSS.Input.Depth.Subrect.Base.X",      SK_UINT  },
    { "DLSS.Input.Depth.Subrect.Base.Y",      SK_UINT  },
    { "DLSS.Input.MV.Subrect.Base.X",         SK_UINT  },
    { "DLSS.Input.MV.Subrect.Base.Y",         SK_UINT  },
    { "DLSS.Output.Subrect.Base.X",           SK_UINT  },
    { "DLSS.Output.Subrect.Base.Y",           SK_UINT  },

    { "MV.Scale.X",                           SK_FLOAT },
    { "MV.Scale.Y",                           SK_FLOAT },
    { "Jitter.Offset.X",                      SK_FLOAT },
    { "Jitter.Offset.Y",                      SK_FLOAT },
    { "Sharpness",                            SK_FLOAT },
    { "DLSS.Pre.Exposure",                    SK_FLOAT },
    { "DLSS.Exposure.Scale",                  SK_FLOAT },

    { "Reset",                                SK_INT   },
};

class SynthParams : public NVSDK_NGX_Parameter
{
public:
    SynthParams() { Clear(); }

    void Clear()
    {
        memset(m_v, 0, sizeof(m_v));
        memset(m_set, 0, sizeof(m_set));
    }

    // Set is how the contract is built, so it goes through the same table Get
    // answers from. A key the table does not carry is a mistake in the builder,
    // not a parameter to store: storing it would make SynthSelfCheck pass over a
    // contract bridge.inc cannot read.
    void Set(const char *n, unsigned long long v) override { (void)v; Unknown(n, "unsigned long long"); }
    void Set(const char *n, double v)             override { (void)v; Unknown(n, "double"); }
    void Set(const char *n, void *v)              override { (void)v; Unknown(n, "void *"); }
    void Set(const char *n, ID3D12Resource *v)    override { (void)v; Unknown(n, "ID3D12Resource *"); }

    void Set(const char *n, unsigned int v) override
    { const int i = Find(n, SK_UINT);  if (i < 0) { Unknown(n, "unsigned int"); return; } m_v[i].u = v; m_set[i] = true; }
    void Set(const char *n, int v) override
    { const int i = Find(n, SK_INT);   if (i < 0) { Unknown(n, "int"); return; }          m_v[i].i = v; m_set[i] = true; }
    void Set(const char *n, float v) override
    { const int i = Find(n, SK_FLOAT); if (i < 0) { Unknown(n, "float"); return; }        m_v[i].f = v; m_set[i] = true; }
    void Set(const char *n, ID3D11Resource *v) override
    { const int i = Find(n, SK_RES);   if (i < 0) { Unknown(n, "ID3D11Resource *"); return; } m_v[i].res = v; m_set[i] = true; }

    NVSDK_NGX_Result Get(const char *n, unsigned long long *) const override { (void)n; return kSynthNoSuchKey; }
    NVSDK_NGX_Result Get(const char *n, double *)             const override { (void)n; return kSynthNoSuchKey; }
    NVSDK_NGX_Result Get(const char *n, void **)              const override { (void)n; return kSynthNoSuchKey; }
    NVSDK_NGX_Result Get(const char *n, ID3D12Resource **)    const override { (void)n; return kSynthNoSuchKey; }

    NVSDK_NGX_Result Get(const char *n, unsigned int *out) const override
    { const int i = Find(n, SK_UINT);  if (i < 0 || !m_set[i]) return kSynthNoSuchKey; *out = m_v[i].u; return NGX_SUCCESS; }
    NVSDK_NGX_Result Get(const char *n, int *out) const override
    { const int i = Find(n, SK_INT);   if (i < 0 || !m_set[i]) return kSynthNoSuchKey; *out = m_v[i].i; return NGX_SUCCESS; }
    NVSDK_NGX_Result Get(const char *n, float *out) const override
    { const int i = Find(n, SK_FLOAT); if (i < 0 || !m_set[i]) return kSynthNoSuchKey; *out = m_v[i].f; return NGX_SUCCESS; }

    NVSDK_NGX_Result Get(const char *n, ID3D11Resource **out) const override
    {
        const int i = Find(n, SK_RES);
        if (i < 0 || !m_set[i] || m_v[i].res == nullptr) return kSynthNoSuchKey;

        // No AddRef here, and this is the one line in the file that is silent
        // for hours if it is wrong. NGX does not AddRef the resources in a
        // parameter block -- that is precisely why a game has to keep them alive
        // across the call, and why BridgeReleaseExposure has to clear the key
        // before freeing the texture. GameTexture in bridge.inc balances its own
        // QueryInterface against the caller's Release; the ID3D11Resource it
        // takes out of this Get is never released by anybody. An AddRef here is
        // therefore one reference leaked per texture per frame, with no failure,
        // no log line and no visible symptom until the process runs out of
        // memory an hour later.
        *out = m_v[i].res;
        return NGX_SUCCESS;
    }

    void Reset() override { Clear(); }

private:
    struct Val { unsigned int u; int i; float f; ID3D11Resource *res; };

    // Name and type together. A Set that supplies the right name at the wrong
    // width would otherwise store a value no Get in bridge.inc can retrieve, and
    // the self-check would report the key as unset without saying why.
    static int Find(const char *n, SynthKind k)
    {
        for (int i = 0; i < static_cast<int>(_countof(kSynthSlots)); ++i)
            if (kSynthSlots[i].kind == k && strcmp(kSynthSlots[i].key, n) == 0) return i;
        return -1;
    }

    static void Unknown(const char *n, const char *type)
    {
        // Once per key. This fires only when the builder and the table disagree,
        // which is a defect in this file rather than anything about the game, so
        // it is worth saying and not worth repeating sixty times a second.
        static const char *seen[16];
        static int         count;
        for (int i = 0; i < count; ++i) if (strcmp(seen[i], n) == 0) return;
        if (count < static_cast<int>(_countof(seen))) seen[count++] = n;
        Log("[synth] DEFECT: the synthetic contract set %s as %s, which is not a key "
            "the mirror's frame path reads at that type. The value is discarded; add "
            "it to kSynthSlots or stop setting it.", n, type);
    }

    Val  m_v[_countof(kSynthSlots)];
    bool m_set[_countof(kSynthSlots)];
};

// The one runnable check this stage leaves behind, and it ships enabled. It
// reads every key back through the object's own virtual Get -- not out of the
// table it was written into -- so it exercises the same path bridge.inc will,
// and fails on exactly the defect constraint (a) exists to prevent: a synthetic
// contract that arrives incomplete and lets NoteAbsentParam choose a value
// nothing measured.
static void SynthSelfCheck(const SynthParams *sp, bool mv_from_engine = false)
{
    // Per key rather than per session: a key set only inside a branch would
    // otherwise be reported on the first frame and never again, or not at all
    // depending on which frame ran the check.
    static bool said[_countof(kSynthSlots)];

    for (int i = 0; i < static_cast<int>(_countof(kSynthSlots)); ++i)
    {
        bool ok = false;
        switch (kSynthSlots[i].kind)
        {
        case SK_UINT:  { unsigned int    v = 0;       ok = sp->Get(kSynthSlots[i].key, &v) == NGX_SUCCESS; break; }
        case SK_INT:   { int             v = 0;       ok = sp->Get(kSynthSlots[i].key, &v) == NGX_SUCCESS; break; }
        case SK_FLOAT: { float           v = 0.0f;    ok = sp->Get(kSynthSlots[i].key, &v) == NGX_SUCCESS; break; }
        case SK_RES:   { ID3D11Resource *v = nullptr; ok = sp->Get(kSynthSlots[i].key, &v) == NGX_SUCCESS; break; }
        }
        // MotionVectors legitimately has no ReShade resource when the driver's own
        // optical flow engine is the source: the vectors are written into the
        // shared D3D12 texture from the other side, and the D3D12 evaluate sets
        // that key itself. Reporting a DEFECT for it would be this file accusing
        // itself of the arrangement it was built to have.
        if (mv_from_engine && strcmp(kSynthSlots[i].key, "MotionVectors") == 0)
            continue;
        if (ok || said[i]) continue;
        said[i] = true;
        Log("[synth] DEFECT: synthetic contract left %s unset", kSynthSlots[i].key);
    }
}

// IsHDR states that the colour carries values outside [0,1]. That is a property
// of the format's encoding and of nothing else: every UNORM format is clamped to
// [0,1] by definition, the ten-bit ones included, so only a float format can
// carry what the flag claims.
//
// FormatIsLowPrecisionUnorm next door answers a narrower question -- it names
// the seven eight-bit formats, for a diagnostic the mirror prints over flags the
// game itself declared -- and reading its "false" as "can hold more than 1.0"
// set IsHDR over an R10G10B10A2_UNORM back buffer, which is what ReShade sees on
// an HDR10 swapchain. The DLSS 5 add-on picks its codec from this flag, so that
// is a silent wrong answer rather than an error, and the mirror's own warning
// for it does not fire because that format is not eight-bit either.
static bool SynthFormatCanExceedOne(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return true;
    default:
        return false;
    }
}

// Each bit off something this add-on measured, and the reason is logged beside
// the bit rather than described in the abstract.
//
// MVLowRes (0x02) is deliberately not set. At 1:1 the motion vectors are at
// render resolution, so the flag would happen to be true -- but nothing here
// measured "the motion vectors are lower resolution than the colour", it
// measured that two textures are the same size, and a flag set because a value
// coincides with it is exactly the invention the create-flags comment in
// bridge.inc was written about. MVJittered, DoSharpening and AutoExposure are
// unset for the same reason: no measurement supports any of them.
static unsigned int SynthCreateFlags(DXGI_FORMAT back_fmt, int depth_reversed)
{
    unsigned int f = 0;

    // IsHDR selects the DLSS 5 add-on's codec. The measurement is the back
    // buffer's own format, and the question it has to answer is whether that
    // format can hold a value above 1.0 -- not whether it is wider than eight
    // bits, which is a different question with a different answer at ten.
    if (SynthFormatCanExceedOne(back_fmt)) f |= 0x01;

    // DepthInverted is the one thing about ReShade's depth encoding that NGX has
    // a way to state, and RESHADE_DEPTH_INPUT_IS_REVERSED is the definition that
    // states it. Absent means the ReShade.fxh default, which is 1 here -- so the
    // common case sets this bit, and reading a missing definition as 0 would
    // have inverted it.
    if (depth_reversed) f |= 0x08;

    return f;
}

// Everything the block carries, set explicitly, from a value that was measured
// on this frame. The four size scalars are all set here, which is what makes the
// size fallback in BridgeFrameInner -- absent scalars derived from the Color and
// Output descriptors -- dead code on this path: constraint (b) says that repair
// is for a game that under-declares and never a supply route.
// The three textures arrive as reshade::api::resource rather than as
// ID3D11Texture2D, and that is what makes one builder serve both backends. A
// resource handle is documented as the device's own native pointer -- an
// ID3D11Resource on a D3D11 device, an ID3D12Resource on a D3D12 one -- so the
// SK_RES slots below carry exactly what a slot in NVSDK_NGX_Parameter carries,
// and SynthParams is untouched. Nothing dereferences these here; the transport
// that consumes the contract knows which API it is on and the block does not
// have to.
static void SynthBuild(SynthParams *sp, unsigned int w, unsigned int h,
                       DXGI_FORMAT back_fmt, reshade::api::resource back,
                       reshade::api::resource depth, reshade::api::resource mv,
                       int depth_reversed, int which)
{
    sp->Clear();

    // Colour in and colour out are the same surface. There is no second buffer
    // to render into: ReShade hands over the back buffer it is about to apply
    // effects to, and the result has to land back in it to be seen.
    sp->Set("Color",         reinterpret_cast<ID3D11Resource *>(static_cast<uintptr_t>(back.handle)));
    sp->Set("Output",        reinterpret_cast<ID3D11Resource *>(static_cast<uintptr_t>(back.handle)));
    sp->Set("Depth",         reinterpret_cast<ID3D11Resource *>(static_cast<uintptr_t>(depth.handle)));
    sp->Set("MotionVectors", reinterpret_cast<ID3D11Resource *>(static_cast<uintptr_t>(mv.handle)));

    // Strictly 1:1. This add-on can measure the back buffer and nothing else:
    // there is no render target at a lower resolution to upscale from, because
    // the game already finished drawing at full size before ReShade was called.
    // Input equals output and the preset is DLAA, which is the only contract the
    // available inputs describe.
    sp->Set("Width",     w);
    sp->Set("Height",    h);
    sp->Set("OutWidth",  w);
    sp->Set("OutHeight", h);
    sp->Set("PerfQualityValue", 5u);   // NVSDK_NGX_PerfQuality_Value_DLAA

    sp->Set("DLSS.Feature.Create.Flags", SynthCreateFlags(back_fmt, depth_reversed));

    // No subrects: every input is a whole texture at the same size, so there is
    // no region to place and the bases are zero because they were measured to be
    // zero, not because zero is a safe default.
    sp->Set("DLSS.Enable.Output.Subrects", 0u);
    sp->Set("DLSS.Render.Subrect.Dimensions.Width",  w);
    sp->Set("DLSS.Render.Subrect.Dimensions.Height", h);
    sp->Set("DLSS.Input.Color.Subrect.Base.X", 0u);
    sp->Set("DLSS.Input.Color.Subrect.Base.Y", 0u);
    sp->Set("DLSS.Input.Depth.Subrect.Base.X", 0u);
    sp->Set("DLSS.Input.Depth.Subrect.Base.Y", 0u);
    sp->Set("DLSS.Input.MV.Subrect.Base.X", 0u);
    sp->Set("DLSS.Input.MV.Subrect.Base.Y", 0u);
    sp->Set("DLSS.Output.Subrect.Base.X", 0u);
    sp->Set("DLSS.Output.Subrect.Base.Y", 0u);

    // Every provider writes UV-space motion, so the magnitude that turns one into
    // pixels is the back buffer's own size -- measured, not assumed.
    //
    // The sign is a config value where the provider does not state one, and the
    // provider's own documented convention where it does. Only vort_Shaders does:
    // it is MIT, so its direction was read out of its source rather than guessed
    // (kSynthMv). An explicit mv_sign_x/mv_sign_y still wins, because a real
    // installation is where a documented convention gets contradicted.
    const int sign_x = g_cfg.mv_sign_x != 0 ? g_cfg.mv_sign_x
                     : (which >= 0 && kSynthMv[which].sign_x != 0 ? kSynthMv[which].sign_x : 1);
    const int sign_y = g_cfg.mv_sign_y != 0 ? g_cfg.mv_sign_y
                     : (which >= 0 && kSynthMv[which].sign_y != 0 ? kSynthMv[which].sign_y : 1);
    sp->Set("MV.Scale.X", static_cast<float>(sign_x) * static_cast<float>(w));
    sp->Set("MV.Scale.Y", static_cast<float>(sign_y) * static_cast<float>(h));

    // Zero, and this is a copied fact rather than a shortcut. ReShade's first
    // opportunity to see a frame is after the game has rasterised it, so this
    // add-on cannot move the projection matrix and the game is not moving it
    // either: every frame samples the same sub-pixel grid. A jitter this block
    // reported would be a jitter nothing applied, and DLSS would resolve its
    // history against a sub-pixel offset that never happened.
    sp->Set("Jitter.Offset.X", 0.0f);
    sp->Set("Jitter.Offset.Y", 0.0f);

    // DoSharpening is not set above, so DLSS runs no sharpening pass and this
    // value is not read. It is still stated, because the note requires every key
    // the frame path reads to be set by whatever built the block.
    sp->Set("Sharpness", 0.0f);

    // No exposure texture and no AutoExposure, so both of these are the identity
    // that nvsdk_ngx_helpers.h sends for a source with nothing to say about
    // exposure -- which is this source exactly.
    sp->Set("DLSS.Pre.Exposure",   1.0f);
    sp->Set("DLSS.Exposure.Scale", 1.0f);

    // The camera cut this source can detect is none: ReShade publishes no such
    // signal and a cut inferred from the depth buffer would be a guess. Zero is
    // the measured answer, and BridgeFrameInner adds its own need_reset on top
    // of it whenever the shape changed underneath.
    sp->Set("Reset", 0);
}

// ---------------------------------------------------------------------------
// The D3D12 transport
//
// Same contract, same self-check, same arming gate. The only new thing is how
// the game's frame reaches an NGX evaluate and how the result gets back.
//
// It runs on the GAME'S OWN D3D12 device, not on a private one. The private
// device in bridge.inc is not a preference: a D3D11 game has no D3D12 device at
// all, so one has to be conjured, and everything downstream of that -- four
// shared textures with NT handles, a D3D12_FENCE_FLAG_SHARED fence opened twice,
// the R24G8 depth conversion and the R32G32 motion-vector retry, both of which
// exist only because the kernel's shared-format list is narrower than DXGI --
// is machinery for crossing between two devices. A D3D12 game already has the
// device the evaluate needs, so none of that machinery has anything to do. What
// is left is two textures and two copies.
//
// UNVERIFIED, and assumed rather than measured: whether NGX will hold two
// D3D12 sessions on one ID3D12Device. Only the D3D11 NGX entry points are
// hooked, so g_create_count and g_eval_count cannot see a D3D12 game's own DLSS
// and the arming gate cannot refuse on it. Nothing here can establish it either
// -- so it is stated in the log at the moment the session opens, where a report
// carries it, rather than left as a silent premise. The intended target is a
// game that ships no DLSS of its own, which has no first session.
//
// UNVERIFIED, and resolved rather than assumed: whether ReShade's get_native
// hands back its own D3D12 proxy or the device underneath. UnwrapReShadeProxy
// answers that by asking, with the GUID ReShade answers, and says which branch
// it took -- so both cases are correct and the log records which one happened.
// ---------------------------------------------------------------------------

struct Synth12
{
    bool tried;      // the session open was attempted; a failure is never retried
    bool ready;      // device, NGX session, queue, allocator ring

    // The device is this add-on's own rather than the game's, so all four
    // textures have to be created shareable and the frame has to be carried
    // across. True on the Vulkan transport and nowhere else.
    bool shared;

    ID3D12Device              *dev;
    ID3D12CommandQueue        *queue;   // ReShade's own, so ordering is queue order
    ID3D12GraphicsCommandList *list;

    // Same reason as the mirror's ring: an allocator must not be reset while the
    // GPU is still executing what was recorded in it.
    static const int           kFrames = 3;
    ID3D12CommandAllocator    *alloc[kFrames];
    UINT64                     alloc_fence[kFrames];
    int                        slot;
    ID3D12Fence               *fence;
    UINT64                     fence_value;
    HANDLE                     event;

    PFN_D3D12CreateFeature   create_feature;
    PFN_D3D12EvaluateFeature eval_feature;
    PFN_D3D12ReleaseFeature  release_feature;
    PFN_AllocateParameters   alloc_params;

    NVSDK_NGX_Parameter *params;
    NVSDK_NGX_Handle    *feature;

    // Colour in and colour out cannot be the same texture: NGX reads one as a
    // shader resource and writes the other through a UAV in the same evaluate.
    // The back buffer is neither, so it is copied in and copied back out.
    //
    // Indexed by the mirror's own SLOT_ constants, because the shared case below
    // needs all four of them and a second set of names for the same four slots
    // is a second thing to keep in step. On the game's own D3D12 device only
    // SLOT_COLOR and SLOT_OUTPUT are filled: Depth and MotionVectors are the
    // game's resources and are handed to NGX untouched.
    ID3D12Resource *tex[SLOT_COUNT];
    // Only when shared: the NT handle the other API imports the texture through.
    HANDLE          share[SLOT_COUNT];

    // The Vulkan mirror's depth arrives packed rather than as floats when the
    // game's depth is D24_UNORM_S8_UINT -- see Synth12MakeDepthUnpack. Then
    // tex[SLOT_DEPTH] is R32_UINT, these four exist, and NGX is handed depth_cvt
    // instead. On every other path they are null and nothing below runs.
    bool                  depth_packed;
    ID3D12Resource       *depth_cvt;
    ID3D12RootSignature  *cvt_rs;
    ID3D12PipelineState  *cvt_pso;
    ID3D12DescriptorHeap *cvt_heap;

    UINT            w, h;
    // The Output slot's own size. On the synthetic path it is always w,h -- that
    // contract is DLAA by construction -- but the Vulkan mirror copies a real
    // game's contract, where an upscaling feature declares OutWidth/OutHeight
    // larger than Width/Height and Output has to be that size or NGX refuses.
    UINT            out_w, out_h;
    DXGI_FORMAT     fmt;
    // How long the evaluate's completion is waited for. 0 means the 4 s default.
    // The Vulkan mirror shortens it, because the game's own GPU is parked behind
    // that wait and Windows resets a device that stops for two seconds.
    DWORD           wait_ms;
    bool            need_reset;
    UINT64          frames;
};

static Synth12 g_s12;

// A UAV cannot be created over an sRGB format, and the DLSS output is written
// through one. The sRGB and UNORM spellings are the same typeless family, so
// CopyResource still moves the result back into the back buffer untouched --
// the encoding is reinterpreted, not converted, which is what the back buffer's
// own render target view does to it anyway.
static DXGI_FORMAT Synth12UavFormat(DXGI_FORMAT f)
{
    switch (TypedFormat(f))
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
    default:                              return TypedFormat(f);
    }
}

// The contract arrives complete -- constraint (a) -- so there is no fallback
// here and none is wanted: a Get that fails is a defect SynthSelfCheck has
// already reported by name, and inventing a value on top of it would hide it.
static void Synth12CopyU(const NVSDK_NGX_Parameter *src, NVSDK_NGX_Parameter *dst,
                         const char *key)
{
    unsigned int v = 0;
    if (src->Get(key, &v) == NGX_SUCCESS) dst->Set(key, v);
}

static void Synth12CopyF(const NVSDK_NGX_Parameter *src, NVSDK_NGX_Parameter *dst,
                         const char *key)
{
    float v = 0.0f;
    if (src->Get(key, &v) == NGX_SUCCESS) dst->Set(key, v);
}

// D3D12 keeps no reference to the resources a submitted command list names, so
// the last frame has to be finished with before any of them is freed. This runs
// on a resolution change, which is rare enough that a full wait costs nothing
// and a use-after-free costs a GPU reset.
static void Synth12ReleaseTextures()
{
    if (g_s12.fence != nullptr && g_s12.event != nullptr &&
        g_s12.fence->GetCompletedValue() < g_s12.fence_value)
    {
        g_s12.fence->SetEventOnCompletion(g_s12.fence_value, g_s12.event);
        WaitForSingleObject(g_s12.event, 4000);
    }
    if (g_s12.feature != nullptr && g_s12.release_feature != nullptr)
        g_s12.release_feature(g_s12.feature);
    g_s12.feature = nullptr;
    for (int i = 0; i < SLOT_COUNT; ++i)
    {
        if (g_s12.tex[i]   != nullptr) { g_s12.tex[i]->Release(); g_s12.tex[i] = nullptr; }
        // The handle is the importing API's only route to the texture, and the
        // driver duplicated it at import time rather than taking ownership, so
        // this side still owns it and still has to close it.
        if (g_s12.share[i] != nullptr) { CloseHandle(g_s12.share[i]); g_s12.share[i] = nullptr; }
    }

    // The depth unpack pass. The heap holds views of the two textures being freed
    // above and below, so it cannot outlive them; the root signature and the PSO
    // could, but rebuilding all four together is one lifetime to reason about
    // instead of two, and a resolution change is the only thing that gets here.
    if (g_s12.depth_cvt != nullptr) { g_s12.depth_cvt->Release(); g_s12.depth_cvt = nullptr; }
    if (g_s12.cvt_heap  != nullptr) { g_s12.cvt_heap->Release();  g_s12.cvt_heap  = nullptr; }
    if (g_s12.cvt_pso   != nullptr) { g_s12.cvt_pso->Release();   g_s12.cvt_pso   = nullptr; }
    if (g_s12.cvt_rs    != nullptr) { g_s12.cvt_rs->Release();    g_s12.cvt_rs    = nullptr; }
}

static bool Synth12Begin()
{
    const int slot = g_s12.slot;
    const UINT64 retire = g_s12.alloc_fence[slot];
    if (retire != 0 && g_s12.fence->GetCompletedValue() < retire)
    {
        if (FAILED(g_s12.fence->SetEventOnCompletion(retire, g_s12.event))) return false;
        if (WaitForSingleObject(g_s12.event, 4000) != WAIT_OBJECT_0) return false;
    }
    if (FAILED(g_s12.alloc[slot]->Reset())) return false;
    if (FAILED(g_s12.list->Reset(g_s12.alloc[slot], nullptr))) return false;
    return true;
}

// Submitted on ReShade's own queue, which is what removes the fence entirely.
// Two lists on one queue execute in submission order, so ReShade's recorded work
// runs before this and everything it records afterwards runs after it. The flush
// in Synth12Frame is what puts ReShade's half of that into the queue first;
// without it its commands are still sitting in an open list.
static UINT64 Synth12End()
{
    g_s12.list->Close();
    ID3D12CommandList *lists[] = { g_s12.list };
    g_s12.queue->ExecuteCommandLists(1, lists);

    const UINT64 v = ++g_s12.fence_value;
    g_s12.queue->Signal(g_s12.fence, v);
    g_s12.alloc_fence[g_s12.slot] = v;
    g_s12.slot = (g_s12.slot + 1) % Synth12::kFrames;
    return v;
}

static bool Synth12Open(reshade::api::effect_runtime *rt)
{
    if (g_s12.tried) return g_s12.ready;
    g_s12.tried = true;

    reshade::api::device *rdev = rt->get_device();
    // Whose device this session ends up on is decided here and read everywhere
    // below, so no line further down can claim the wrong one.
    g_s12.shared = rdev != nullptr &&
                   rdev->get_api() == reshade::api::device_api::vulkan;
    const char *whose = g_s12.shared ? "this add-on's own D3D12 device"
                                     : "the game's D3D12 device";

    Log("");
    Breadcrumb("opening the D3D12 synthetic session");
    Log("################ synth: opening on %s ################", whose);

    if (g_s12.shared)
    {
        // A Vulkan game has no D3D12 device, so one is conjured for exactly the
        // reason the D3D11 mirror conjures one: the evaluate a DLSS 5 add-on
        // detours is NVSDK_NGX_D3D12_EvaluateFeature, and there is no Vulkan
        // entry point to offer it instead.
        //
        // ponytail: the default adapter. ReShade exposes no VkPhysicalDevice --
        // it keeps _physical_device private and get_native answers for the
        // device, the queue and the swapchain only -- so there is no LUID to
        // match the game's GPU against. On a machine with one GPU that is the
        // right adapter; on a hybrid laptop the memory import below fails and
        // says so, which is the diagnosis. Matching by LUID needs a
        // VkPhysicalDevice, and getting one needs the vkCreateDevice hook this
        // transport was written to avoid.
        HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
        auto create_device = d3d12 != nullptr
            ? reinterpret_cast<PFN_D3D12CreateDevice_>(GetProcAddress(d3d12, "D3D12CreateDevice"))
            : nullptr;
        if (create_device == nullptr)
        {
            Log("[synth] d3d12.dll is not present or exports no D3D12CreateDevice, so there "
                "is no device to run an evaluate on. The game is unaffected.");
            return false;
        }

        DWORD dcode = 0;
        // SafeCreateDevice is the mirror's own, guard and unwrap included: a
        // fault inside the D3D12 runtime comes back as a code rather than as a
        // dead process, which is the whole reason it exists.
        HRESULT hr = SafeCreateDevice(create_device, nullptr, &g_s12.dev, &dcode);
        if (dcode != 0)
        {
            Log("[synth] D3D12CreateDevice raised 0x%08X inside the D3D12 runtime, so "
                "nothing further is attempted. The game keeps rendering with Vulkan.", dcode);
            return false;
        }
        if (FAILED(hr) || g_s12.dev == nullptr)
        { Log("[synth] D3D12CreateDevice failed 0x%08X", hr); return false; }

        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        g_s12.dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue),
                                      reinterpret_cast<void **>(&g_s12.queue));
        if (g_s12.queue == nullptr)
        { Log("[synth] the private D3D12 device would not create a graphics queue"); return false; }
    }
    else
    {
        reshade::api::command_queue *rq = rt->get_command_queue();
        g_s12.dev = rdev != nullptr
            ? reinterpret_cast<ID3D12Device *>(static_cast<uintptr_t>(rdev->get_native())) : nullptr;
        g_s12.queue = rq != nullptr
            ? reinterpret_cast<ID3D12CommandQueue *>(static_cast<uintptr_t>(rq->get_native())) : nullptr;
        if (g_s12.dev == nullptr || g_s12.queue == nullptr)
        {
            Log("[synth] the effect runtime reports no native D3D12 device or queue, so there "
                "is nothing to run an evaluate on. The game is unaffected.");
            return false;
        }
    }
    Log("[synth] %s is %p and its graphics queue is %p", whose,
        static_cast<void *>(g_s12.dev), static_cast<void *>(g_s12.queue));

    // The same question the mirror asks of the device it creates, asked here of
    // the device it was handed. RenoDX asks it of every command list before
    // forwarding to NGX, so an unwrapped device is the case NGX callers prefer,
    // and the answer is logged either way rather than presumed. SafeCreateDevice
    // has already asked it on the shared path, so this is the other one.
    // unwrap is honoured here for the same reason SafeCreateDevice honours it:
    // it is the knob a user is told to turn when unwrapping is what breaks their
    // driver, and a knob that only reaches one of two devices is worse than no
    // knob at all.
    if (!g_s12.shared && g_cfg.unwrap != 0) UnwrapReShadeProxy(&g_s12.dev);

    // The scan first, because a game that has DLSS has already loaded NGX and the
    // module it loaded is the one to use. Only when nothing is there is the
    // driver's own loader brought in -- which is the ordinary case here, since
    // the synthetic contract exists for games that ship no DLSS at all.
    HMODULE ngx = FindNgxLoader();
    if (ngx == nullptr && EnsureNgxLoaderLoaded()) ngx = FindNgxLoader();
    if (ngx == nullptr)
    {
        Log("[synth] no module exports the NGX D3D12 entry points, and the driver's own "
            "loader could not be brought in either, so there is no NGX to run a feature on.");
        return false;
    }

    auto init_ext = reinterpret_cast<PFN_Init_Ext>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init_Ext"));
    auto init_project = reinterpret_cast<PFN_Init_ProjectID>(
        GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init_ProjectID"));
    g_s12.alloc_params = reinterpret_cast<PFN_AllocateParameters>(
        GetProcAddress(ngx, "NVSDK_NGX_D3D12_AllocateParameters"));
    g_s12.create_feature = reinterpret_cast<PFN_D3D12CreateFeature>(
        GetProcAddress(ngx, "NVSDK_NGX_D3D12_CreateFeature"));
    g_s12.eval_feature = reinterpret_cast<PFN_D3D12EvaluateFeature>(
        GetProcAddress(ngx, "NVSDK_NGX_D3D12_EvaluateFeature"));
    // Not required: a driver that exports no ReleaseFeature costs a leaked
    // feature on a resolution change, which is not a reason to refuse a session.
    g_s12.release_feature = reinterpret_cast<PFN_D3D12ReleaseFeature>(
        GetProcAddress(ngx, "NVSDK_NGX_D3D12_ReleaseFeature"));
    if ((init_ext == nullptr && init_project == nullptr) || g_s12.alloc_params == nullptr ||
        g_s12.create_feature == nullptr || g_s12.eval_feature == nullptr)
    { Log("[synth] required NGX D3D12 exports missing"); return false; }

    // Said before the first call it could affect, so a log that ends inside NGX
    // still carries the premise the session was opened on. Only where it is the
    // premise: a device this add-on created a moment ago cannot be carrying
    // somebody else's session, so on the shared path there is nothing assumed.
    if (!g_s12.shared)
        Log("[synth] ASSUMED, NOT MEASURED: that NGX accepts a second D3D12 session on a "
            "device the game may already have one on. Only the D3D11 NGX entry points are "
            "hooked here, so a D3D12 game's own DLSS is invisible to the arming gate and "
            "cannot be refused on. This path is meant for a game that ships no DLSS, which "
            "has no first session; if this game does ship DLSS, that premise is untested.");

    wchar_t exe_dir[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_dir, MAX_PATH);
    if (wchar_t *ed = wcsrchr(exe_dir, (wchar_t)92)) *(ed + 1) = 0;
    Log("[synth] NGX data path: %ls", exe_dir);

    g_ngx_init_in_flight = true;
    Breadcrumb(g_s12.shared ? "initialising NGX on the private D3D12 device"
                            : "initialising NGX on the game's D3D12 device");
    DWORD code = 0;
    bool inited = false, faulted = false;
    // Same negotiation and the same rule as the mirror's: a refusal is NGX
    // declining a version constant and the next one is tried; a fault means NGX
    // died inside itself, and calling back in is what turned a disabled add-on
    // into a killed process on Final Fantasy VII Remake. Stop at the first one.
    for (int ver = 0x13; ver <= 0x16 && !inited && !faulted; ++ver)
    {
        if (init_ext != nullptr)
        {
            NVSDK_NGX_Result r = SafeInitExt(init_ext, 0x1000000ULL, exe_dir, g_s12.dev, ver, &code);
            if (code != 0)
            { Log("[synth] Init_Ext(version=0x%02X) raised 0x%08X", ver, code); faulted = true; }
            else
            {
                Log("[synth] Init_Ext(version=0x%02X) -> %s", ver, r == NGX_SUCCESS ? "ok" : "refused");
                if (r == NGX_SUCCESS) inited = true;
            }
        }
        if (!inited && !faulted && init_project != nullptr)
        {
            NVSDK_NGX_Result r = SafeInitProjectID(init_project,
                                                   "a0f57b54-1daf-4934-90ae-c4035c19df04",
                                                   exe_dir, g_s12.dev, ver, &code);
            if (code != 0)
            { Log("[synth] Init_ProjectID(version=0x%02X) raised 0x%08X", ver, code); faulted = true; }
            else
            {
                Log("[synth] Init_ProjectID(version=0x%02X) -> %s", ver, r == NGX_SUCCESS ? "ok" : "refused");
                if (r == NGX_SUCCESS) inited = true;
            }
        }
    }
    g_ngx_init_in_flight = false;

    if (faulted)
    {
        Log("[synth] NGX raised an exception opening a D3D12 session on %s, so nothing "
            "further is attempted. The game keeps rendering.", whose);
        return false;
    }
    if (!inited)
    { Log("[synth] no NGX D3D12 init entry point was accepted by this driver"); return false; }

    NVSDK_NGX_Result rp = SafeAllocParams(g_s12.alloc_params, &g_s12.params, &code);
    if (code != 0 || rp != NGX_SUCCESS || g_s12.params == nullptr)
    {
        Log("[synth] NGX would not allocate a parameter block: result 0x%08X, raised 0x%08X",
            rp, code);
        return false;
    }

    for (int i = 0; i < Synth12::kFrames; ++i)
        g_s12.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          __uuidof(ID3D12CommandAllocator),
                                          reinterpret_cast<void **>(&g_s12.alloc[i]));
    if (g_s12.alloc[0] != nullptr)
        g_s12.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_s12.alloc[0], nullptr,
                                     __uuidof(ID3D12GraphicsCommandList),
                                     reinterpret_cast<void **>(&g_s12.list));
    if (g_s12.list != nullptr) g_s12.list->Close();
    g_s12.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                           reinterpret_cast<void **>(&g_s12.fence));
    g_s12.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_s12.list == nullptr || g_s12.fence == nullptr || g_s12.event == nullptr)
    { Log("[synth] could not create the command list, fence or event"); return false; }

    // The list is this add-on's own and not ReShade's, and that is not a
    // preference either. ReShade's command_list_impl caches the root signature
    // and the descriptor heaps it last set and skips re-setting them when they
    // have not changed; NGX sets both directly on the underlying list, which
    // that cache cannot see. Recording the evaluate into ReShade's list would
    // leave its next effect pass running under NGX's root signature, silently.
    Log("[synth] D3D12 session ready on %s. %s", whose, g_s12.shared
        ? "The frame does not start on this device, so every texture NGX reads is "
          "created shareable here and imported by the API the game renders with."
        : "Nothing is shared between devices, so there is no NT handle, no "
          "cross-API fence and no format conversion.");
    g_s12.ready = true;
    return true;
}

// One texture NGX will read or write. On the game's own D3D12 device that is an
// ordinary committed resource; on the shared path it also needs an NT handle,
// and ALLOW_SIMULTANEOUS_ACCESS with it -- the importing API performs no D3D12
// state transitions, so the texture has to be usable while it sits in COMMON.
// That is the same pair of flags the mirror's own shared textures carry, for the
// same reason.
static bool Synth12MakeTexture(int slot, UINT w, UINT h, DXGI_FORMAT fmt, bool uav)
{
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
    rd.Flags            = (uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                               : D3D12_RESOURCE_FLAG_NONE) |
                          (g_s12.shared ? D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS
                                        : D3D12_RESOURCE_FLAG_NONE);

    HRESULT hr = g_s12.dev->CreateCommittedResource(&hp,
        g_s12.shared ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource),
        reinterpret_cast<void **>(&g_s12.tex[slot]));
    if (SUCCEEDED(hr) && g_s12.shared)
        hr = g_s12.dev->CreateSharedHandle(g_s12.tex[slot], nullptr, GENERIC_ALL, nullptr,
                                           &g_s12.share[slot]);
    if (FAILED(hr))
    {
        Log("[synth] could not create%s the %ux%u %s texture for %s: 0x%08X", 
            g_s12.shared ? " or share" : "", w, h, FormatName(fmt), kSlotName[slot], hr);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Unpacking a D24 depth aspect, on the D3D12 side
//
// The Vulkan mirror carries the game's depth aspect out through a buffer and
// back into the shared texture. For VK_FORMAT_D24_UNORM_S8_UINT the Vulkan
// specification says exactly what those bytes are, so nothing here is a guess
// about how a driver packs them -- there is no freedom left to guess about:
//
//   copies.adoc, "Depth/Stencil Aspect Copy": when copying between a buffer and
//   the depth or stencil aspect of an image the data in the buffer is laid out
//   as separate planes, addressed as a different format according to the aspect,
//   and the depth aspect of VK_FORMAT_D24_UNORM_S8_UINT is addressed as
//   VK_FORMAT_X8_D24_UNORM_PACK32.
//
//   formats.adoc: that is a 32-bit packed format, first component named in the
//   most significant bits, so X is bits 31..24 and D is bits 23..0, in host byte
//   order -- little-endian on x86, which is the same order the D3D12 GPU reads
//   the word back in.
//
//   fundamentals.adoc: an unsigned normalised value of b bits is c / (2^b - 1),
//   so 24 bits is c / 16777215.
//
// So the shared texture is declared R32_UINT, the transport is untouched -- the
// depth aspect is four bytes per texel exactly as D32_SFLOAT is, so the staging
// buffer, its size and both copies stay as they are -- and this pass turns those
// words into the R32_FLOAT depth NGX reads.
//
// The mask is mandatory rather than tidy: the top 8 bits carry no defined value
// at all, and unmasked they scale depth by up to 256, which puts every
// reprojection on noise.
//
// The divide is written as a divide. float(c) is exact for c <= 2^24-1 and a
// divide is correctly rounded, where the reciprocal-multiply an optimiser folds
// in can land a ULP away from the hardware UNORM conversion the game's own DLSS
// sees. At 8x8 it costs nothing.
//
// The "opaque" clause in formats.adoc is not a counter-argument: it governs how
// the IMAGE is stored, which is tiled memory nothing here touches, not the
// buffer payload of a copy, which the copies chapter pins.
static bool Synth12MakeDepthUnpack(UINT w, UINT h)
{
    // The one texture on this device the other API never sees: not shared, no NT
    // handle, not in the import loop. NGX reads it and nothing else touches it,
    // which is why it can carry ALLOW_UNORDERED_ACCESS without also carrying
    // ALLOW_SIMULTANEOUS_ACCESS.
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
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    HRESULT hr = g_s12.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource),
        reinterpret_cast<void **>(&g_s12.depth_cvt));
    if (FAILED(hr))
    {
        Log("[synth] the %ux%u R32_FLOAT texture the depth unpack writes into could not "
            "be created: 0x%08X", w, h, hr);
        return false;
    }

    // The same skeleton as the D3D11 depth blit in bridge.inc -- same numthreads,
    // same bounds guard -- with the two view types and the one expression that
    // the packing needs. The D3D11 path never needs the arithmetic because it
    // reads the game's depth through an R24_UNORM_X8_TYPELESS view and the
    // hardware does the divide; there is no such view over an R32_UINT texture,
    // because R24_UNORM_X8_TYPELESS belongs to the R24G8 family and not to R32.
    static const char kSrc[] =
        "Texture2D<uint>    src : register(t0);\n"
        "RWTexture2D<float> dst : register(u0);\n"
        "[numthreads(8,8,1)]\n"
        "void main(uint3 id : SV_DispatchThreadID)\n"
        "{\n"
        "    uint w, h; dst.GetDimensions(w, h);\n"
        "    if (id.x < w && id.y < h)\n"
        "        dst[id.xy] = (src[id.xy] & 0x00FFFFFFu) / 16777215.0;\n"
        "}\n";

    PFN_D3DCompile_ compile = LoadSystemD3DCompiler();
    if (compile == nullptr) { Log("[synth] d3dcompiler_47.dll unavailable"); return false; }

    // D3DCOMPILE_IEEE_STRICTNESS, spelled out because d3dcompiler.h is not
    // included here -- this add-on declares the one entry point it uses and
    // nothing else. Without it fxc folds the divide into a multiply by the
    // rounded reciprocal of 16777215, which can land a ULP off the hardware
    // UNORM conversion; with it the DXBC carries a real div. Verified by
    // compiling this exact source with fxc /Gis and reading the disassembly.
    static const UINT kIeeeStrictness = 1u << 13;

    ID3DBlob *code = nullptr, *err = nullptr;
    hr = compile(kSrc, sizeof(kSrc) - 1, "depthunpack", nullptr, nullptr,
                 "main", "cs_5_0", kIeeeStrictness, 0, &code, &err);
    if (FAILED(hr) || code == nullptr)
    {
        Log("[synth] the depth unpack shader would not compile: %s",
            err != nullptr ? static_cast<const char *>(err->GetBufferPointer()) : "");
        if (err != nullptr) err->Release();
        return false;
    }
    if (err != nullptr) err->Release();

    // One table, two ranges: t0 is the packed depth, u0 is the float it becomes.
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors                    = 1;
    ranges[0].BaseShaderRegister                = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors                    = 1;
    ranges[1].BaseShaderRegister                = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;

    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 2;
    param.DescriptorTable.pDescriptorRanges   = ranges;
    param.ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 1;
    rsd.pParameters   = &param;
    rsd.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    // d3d12.dll is already loaded -- Synth12Open took it -- so this asks the
    // module rather than taking a second reference, and it stays a GetProcAddress
    // rather than a static import for the same reason everything else here is:
    // the add-on has to load on a machine with no D3D12 at all.
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    auto serialize = d3d12 != nullptr
        ? reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(
              GetProcAddress(d3d12, "D3D12SerializeRootSignature"))
        : nullptr;
    if (serialize == nullptr)
    {
        Log("[synth] d3d12.dll exports no D3D12SerializeRootSignature, so the depth unpack "
            "has no root signature");
        code->Release();
        return false;
    }

    ID3DBlob *rsblob = nullptr, *rserr = nullptr;
    hr = serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsblob, &rserr);
    if (FAILED(hr) || rsblob == nullptr)
    {
        Log("[synth] the depth unpack root signature would not serialize 0x%08X: %s", hr,
            rserr != nullptr ? static_cast<const char *>(rserr->GetBufferPointer()) : "");
        if (rserr != nullptr) rserr->Release();
        code->Release();
        return false;
    }
    if (rserr != nullptr) rserr->Release();

    hr = g_s12.dev->CreateRootSignature(0, rsblob->GetBufferPointer(), rsblob->GetBufferSize(),
                                        __uuidof(ID3D12RootSignature),
                                        reinterpret_cast<void **>(&g_s12.cvt_rs));
    rsblob->Release();
    if (FAILED(hr))
    {
        Log("[synth] CreateRootSignature failed 0x%08X", hr);
        code->Release();
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature     = g_s12.cvt_rs;
    pd.CS.pShaderBytecode = code->GetBufferPointer();
    pd.CS.BytecodeLength  = code->GetBufferSize();
    hr = g_s12.dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState),
                                               reinterpret_cast<void **>(&g_s12.cvt_pso));
    code->Release();
    if (FAILED(hr)) { Log("[synth] CreateComputePipelineState failed 0x%08X", hr); return false; }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = g_s12.dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap),
                                         reinterpret_cast<void **>(&g_s12.cvt_heap));
    if (FAILED(hr)) { Log("[synth] CreateDescriptorHeap failed 0x%08X", hr); return false; }

    // Written once, here, because both textures live exactly as long as the heap
    // does -- Synth12ReleaseTextures frees all three together.
    const UINT inc = g_s12.dev->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_s12.cvt_heap->GetCPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format                    = DXGI_FORMAT_R32_UINT;
    sd.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels       = 1;
    g_s12.dev->CreateShaderResourceView(g_s12.tex[SLOT_DEPTH], &sd, cpu);

    cpu.ptr += inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format        = DXGI_FORMAT_R32_FLOAT;
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_s12.dev->CreateUnorderedAccessView(g_s12.depth_cvt, nullptr, &ud, cpu);

    Log("[synth] depth unpack shader ready (cs_5_0)");
    return true;
}

// Creating the feature. Every create key comes off the synthetic contract rather
// than off anything measured a second time here: SynthBuild set them and
// SynthSelfCheck proved they are there, and reading them back through the block
// is what makes that proof cover this path too.
// out_w/out_h size the Output slot alone. The synthetic callers pass w,h -- their
// contract is 1:1 by construction -- and the Vulkan mirror passes the game's own
// OutWidth/OutHeight, which is larger whenever the game is upscaling.
// depth_packed defaults false, so the two synthetic call sites are untouched and
// keep the R32_FLOAT depth slot they have always had. Only the Vulkan mirror
// passes true, and only when the game's depth is D24_UNORM_S8_UINT.
static bool Synth12Build(const SynthParams *sp, UINT w, UINT h, UINT out_w, UINT out_h,
                         DXGI_FORMAT back_fmt, bool depth_packed = false)
{
    Synth12ReleaseTextures();
    g_s12.w = w; g_s12.h = h; g_s12.fmt = back_fmt;
    g_s12.out_w = out_w; g_s12.out_h = out_h;
    g_s12.depth_packed = depth_packed;

    // Colour and output on every transport; depth and motion vectors as well
    // when the frame has to cross into this device, because then they are not
    // the game's resources any more but copies of them.
    const DXGI_FORMAT slot_fmt[SLOT_COUNT] = {
        TypedFormat(back_fmt),
        Synth12UavFormat(back_fmt),
        // R32_FLOAT rather than any depth format: a depth-stencil resource is
        // not in the kernel's shared-format list, and NGX reads depth through
        // an ordinary shader resource view anyway. R16G16_FLOAT is what both
        // shipped motion estimation shaders declare and what the arming gate
        // already refused anything else on, so no conversion is implied here.
        //
        // R32_UINT instead when the mirror says the depth aspect arrives packed:
        // the same four bytes per texel and the same copy, declared as what they
        // actually are so Synth12MakeDepthUnpack can read them as integers.
        depth_packed ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R32_FLOAT,
        DXGI_FORMAT_R16G16_FLOAT,
    };
    const int slots = g_s12.shared ? SLOT_COUNT : (SLOT_OUTPUT + 1);
    for (int i = 0; i < slots; ++i)
        if (!Synth12MakeTexture(i, i == SLOT_OUTPUT ? out_w : w, i == SLOT_OUTPUT ? out_h : h,
                                slot_fmt[i], i == SLOT_OUTPUT))
        { Synth12ReleaseTextures(); return false; }

    if (depth_packed && !Synth12MakeDepthUnpack(w, h))
    { Synth12ReleaseTextures(); return false; }

    Log("[synth] colour in %s, output %s -- the frame is copied into the first and the "
        "result is copied back out of the second, because NGX reads one as a shader "
        "resource and writes the other through a UAV in the same call.",
        FormatName(slot_fmt[SLOT_COLOR]), FormatName(slot_fmt[SLOT_OUTPUT]));

    // What the driver will actually allow with these formats, asked before NGX is
    // asked to build anything with them. The D3D11 mirror has printed this since
    // Escape from Tarkov failed on exactly this and said so only from the other
    // add-on's side, as "output format 1 is not a supported typed codec format" --
    // by which point the bridge looked healthy and the fault looked like ours.
    // The synthetic path never had it, and a Vulkan game reaching this code
    // brings formats no D3D11 game ever has: Baldur's Gate 3 arrives with
    // R11G11B10_FLOAT, which is packed and has no alpha.
    for (int i = 0; i < SLOT_COUNT; ++i)
    {
        if (g_s12.tex[i] == nullptr) continue;
        const D3D12_RESOURCE_DESC rd = g_s12.tex[i]->GetDesc();
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {};
        fs.Format = rd.Format;
        if (FAILED(g_s12.dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                                                  &fs, sizeof(fs))))
        {
            Log("[synth]   %-6s fmt %u: the driver would not answer CheckFeatureSupport",
                kSlotName[i], rd.Format);
            continue;
        }
        const bool sample = (fs.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) != 0;
        const bool uav    = (fs.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0;
        const bool load   = (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0;
        const bool store  = (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
        Log("[synth]   %-6s fmt %u %llux%u flags=0x%X: sample %s, typed UAV %s, "
            "UAV load %s, UAV store %s", kSlotName[i], rd.Format, rd.Width, rd.Height,
            rd.Flags, sample ? "yes" : "NO", uav ? "yes" : "NO",
            load ? "yes" : "NO", store ? "yes" : "NO");
        // Said where it is decisive rather than left to be inferred from four
        // yes/no words: DLSS writes its result through a typed UAV store, so a
        // format without one cannot be an Output whatever else it supports.
        if (i == SLOT_OUTPUT && !store)
            Log("[synth]   *** Output is %s and this driver has no typed UAV store for it. "
                "DLSS writes its result through one, so the feature cannot be built with "
                "this format. ***", FormatName(rd.Format));
        if (i == SLOT_OUTPUT && !(rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
            Log("[synth]   *** Output was created without ALLOW_UNORDERED_ACCESS ***");
    }

    Synth12CopyU(sp, g_s12.params, "Width");
    Synth12CopyU(sp, g_s12.params, "Height");
    Synth12CopyU(sp, g_s12.params, "OutWidth");
    Synth12CopyU(sp, g_s12.params, "OutHeight");
    Synth12CopyU(sp, g_s12.params, "PerfQualityValue");
    Synth12CopyU(sp, g_s12.params, "DLSS.Feature.Create.Flags");
    Synth12CopyU(sp, g_s12.params, "DLSS.Enable.Output.Subrects");
    g_s12.params->Set("CreationNodeMask", 1u);
    g_s12.params->Set("VisibilityNodeMask", 1u);
    g_s12.params->Set("RTXValue", 0);

    {
        // Read back out of the block that will be handed to NGX, so the line
        // states what was set rather than what was intended.
        unsigned int used = 0;
        g_s12.params->Get("DLSS.Feature.Create.Flags", &used);
        LogCreateFlags("the synthetic contract's create", used);
        // The other of the two places a create-flags value reaches NGX. The
        // mirror publishes its own in BridgeBuildResources; this transport never
        // goes through that function, so without this line the panel would show
        // whatever the mirror last sent -- or nothing at all in a session that
        // never had a mirror.
        g_panel_flags_from = kFlagsFromLabel[FLAGS_SYNTH];
        InterlockedExchange(&g_panel_flags, static_cast<LONG>(used));
    }

    // Every failure below this line gives the textures back before it returns.
    // Nothing retries after one -- the caller clears ready -- so what is left
    // behind is left for the life of the process, and on the shared path that is
    // four committed resources and four NT handles rather than only VRAM.
    if (!Synth12Begin())
    { Log("[synth] could not start a command list"); Synth12ReleaseTextures(); return false; }
    DWORD code = 0;
    Breadcrumb("creating the D3D12 DLSS feature for the synthetic contract");
    NVSDK_NGX_Result rf = SafeCreateFeature(g_s12.create_feature, g_s12.list,
                                            g_s12.params, &g_s12.feature, &code);
    const UINT64 v = Synth12End();

    // A one-off, so it is waited out rather than pipelined -- but on ReShade's
    // present thread, so not for long enough to read as a hang.
    if (g_s12.fence->GetCompletedValue() < v)
    {
        g_s12.fence->SetEventOnCompletion(v, g_s12.event);
        if (WaitForSingleObject(g_s12.event, 4000) != WAIT_OBJECT_0)
        {
            // The one failure that does NOT hand the textures back. Four seconds
            // without a fence signal means the GPU never finished the create, so
            // the textures it was recorded against may still be in flight, and
            // freeing those is the use after free the wait exists to prevent.
            // Leaking them is the cheaper of the two mistakes.
            Log("[synth] feature creation did not complete within 4 s; giving up. The "
                "textures it was recorded against are left allocated rather than freed "
                "underneath a GPU that has not finished with them.");
            return false;
        }
    }

    if (code != 0)
    {
        // What was observed, first. SafeCreateFeature runs under CaptureFault, so
        // the faulting address is known and the module that owns it can be named.
        // 0xE06D7363 is an MSVC C++ throw and its address is always inside
        // KERNELBASE's RaiseException, which names the messenger and not the
        // sender -- so the module line matters more here than the code does.
        wchar_t who[MAX_PATH + 64] = {};
        Warn("creating the synthetic DLSS feature raised exception 0x%08X inside NGX.", code);
        if (NameFaultOwner(who, _countof(who)))
            Log("[synth]   it faulted in %ls", who);
        Log("[synth]   the create parameters are the six lines above this one, and the "
            "format support for each texture is the four lines above those. An Output "
            "format with no typed UAV store is the failure this has been before.");
        // Offered as something to try, not as the cause. A reporter on 2026-08-31
        // made every nvngx_*.dll on their machine the same version and the create
        // still faulted, so a mismatch is one explanation among several.
        Log("[synth]   NGX loads its feature snippets from the game folder before the "
            "driver's own, so an nvngx_*.dll dropped in there is worth removing as a "
            "test. It is not established as the cause.");
        // Cleared before the release so nothing calls ReleaseFeature on a handle
        // NGX faulted while producing.
        g_s12.feature = nullptr;
        Synth12ReleaseTextures();
        return false;
    }
    if (rf != NGX_SUCCESS || g_s12.feature == nullptr)
    {
        Log("[synth] CreateFeature failed with result 0x%08X, %s", rf, NgxResultName(rf));
        g_s12.feature = nullptr;
        Synth12ReleaseTextures();
        return false;
    }

    Log("[synth] feature ready on %s: %ux%u -> %ux%u handle=%p",
        g_s12.shared ? "this add-on's own D3D12 device" : "the game's own device",
        w, h, out_w, out_h, static_cast<void *>(g_s12.feature));
    g_s12.need_reset = true;
    return true;
}

// Everything the evaluate reads that is not a texture, carried across from the
// synthetic contract each frame. Both transports run this: the contract and the
// self-check that proves it complete are the same on either, and only the route
// the pixels take differs.
static void Synth12FrameParams(const SynthParams *sp)
{
    Synth12CopyU(sp, g_s12.params, "DLSS.Render.Subrect.Dimensions.Width");
    Synth12CopyU(sp, g_s12.params, "DLSS.Render.Subrect.Dimensions.Height");
    Synth12CopyU(sp, g_s12.params, "DLSS.Input.Color.Subrect.Base.X");
    Synth12CopyU(sp, g_s12.params, "DLSS.Input.Color.Subrect.Base.Y");
    Synth12CopyU(sp, g_s12.params, "DLSS.Input.Depth.Subrect.Base.X");
    Synth12CopyU(sp, g_s12.params, "DLSS.Input.Depth.Subrect.Base.Y");
    Synth12CopyU(sp, g_s12.params, "DLSS.Input.MV.Subrect.Base.X");
    Synth12CopyU(sp, g_s12.params, "DLSS.Input.MV.Subrect.Base.Y");
    Synth12CopyU(sp, g_s12.params, "DLSS.Output.Subrect.Base.X");
    Synth12CopyU(sp, g_s12.params, "DLSS.Output.Subrect.Base.Y");
    Synth12CopyF(sp, g_s12.params, "MV.Scale.X");
    Synth12CopyF(sp, g_s12.params, "MV.Scale.Y");
    Synth12CopyF(sp, g_s12.params, "Jitter.Offset.X");
    Synth12CopyF(sp, g_s12.params, "Jitter.Offset.Y");
    Synth12CopyF(sp, g_s12.params, "Sharpness");
    Synth12CopyF(sp, g_s12.params, "DLSS.Pre.Exposure");
    Synth12CopyF(sp, g_s12.params, "DLSS.Exposure.Scale");

    // ExposureTexture is absent from the contract on purpose and stays absent
    // here: this source supplies none, and setting a null would be supplying one.

    int reset = 0;
    sp->Get("Reset", &reset);
    g_s12.params->Set("Reset", (g_s12.need_reset || reset || g_cfg.reset_every) ? 1 : 0);
    g_s12.need_reset = false;
}

// One frame. The back buffer, ReShade's depth binding and the motion vector
// texture are all resources on this one device, so the whole transport is two
// copies with barriers either side of them.
//
// The states those resources are already in are read off ReShade's own code
// rather than guessed. At reshade_begin_effects the back buffer is in
// render_target: that is the old state runtime.cpp asserts for it in the very
// next thing it does, the implicit back buffer copy at the top of the first
// effect pass. Depth and motion vectors are in shader_resource, which on D3D12
// is PIXEL|NON_PIXEL_SHADER_RESOURCE -- generic_depth's own
// reshade_begin_effects handler leaves the depth there, and it is a built-in
// add-on, so ReShade registers its callback before any external one and it runs
// first. NON_PIXEL is already part of that state, so those two need no barrier
// at all, and not emitting one is strictly safer than emitting one whose old
// state might be wrong: a transition from a state a resource is not in is a
// device removal, not a warning.
static void Synth12Frame(reshade::api::effect_runtime *rt, const SynthParams *sp,
                         ID3D12Resource *back, ID3D12Resource *depth, ID3D12Resource *mv,
                         UINT w, UINT h, DXGI_FORMAT back_fmt)
{
    if (!Synth12Open(rt)) return;

    if (g_s12.feature == nullptr || g_s12.w != w || g_s12.h != h || g_s12.fmt != back_fmt)
    {
        Log("[synth] building the D3D12 feature for %ux%u %s", w, h, FormatName(back_fmt));
        if (!Synth12Build(sp, w, h, w, h, back_fmt)) { g_s12.ready = false; return; }
    }

    // ReShade's own commands for this frame are still sitting in an open list.
    // Submitting them first is what makes queue order the only ordering this
    // needs; there is no fence between the two halves because there are not two
    // queues.
    // ponytail: one extra flush per delivered frame. It costs a submission, not
    // the pipeline bubble the D3D11 path pays for crossing devices. If it ever
    // measures, the alternative is recording into ReShade's own list, which is
    // not safe -- see the note at the end of Synth12Open.
    reshade::api::command_queue *rq = rt->get_command_queue();
    if (rq != nullptr) rq->flush_immediate_command_list();

    if (!Synth12Begin()) return;

    Barrier(g_s12.list, back, D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(g_s12.list, g_s12.tex[SLOT_COLOR], D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COPY_DEST);
    g_s12.list->CopyResource(g_s12.tex[SLOT_COLOR], back);
    Barrier(g_s12.list, g_s12.tex[SLOT_COLOR], D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(g_s12.list, g_s12.tex[SLOT_OUTPUT], D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // The four textures are this frame's, set every frame. Depth and the motion
    // vectors are the game's own resources rather than copies of them, which is
    // the whole difference between this transport and the mirror's.
    g_s12.params->Set("Color", g_s12.tex[SLOT_COLOR]);
    g_s12.params->Set("Output", g_s12.tex[SLOT_OUTPUT]);
    g_s12.params->Set("Depth", depth);
    g_s12.params->Set("MotionVectors", mv);

    Synth12FrameParams(sp);

    DWORD code = 0;
    Breadcrumb("running the D3D12 evaluate for the synthetic contract");
    static const char kNgxMark[] = "ngx evaluate";
    g_s12.list->BeginEvent(1, kNgxMark, sizeof(kNgxMark));
    NVSDK_NGX_Result re = g_cfg.mode != 1
        ? SafeEvaluateGuarded(g_s12.eval_feature, g_s12.list, g_s12.feature, g_s12.params, &code)
        : NGX_SUCCESS;
    g_s12.list->EndEvent();

    Barrier(g_s12.list, g_s12.tex[SLOT_OUTPUT], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    // Only mode 2 writes back, and this is NOT the mirror's three modes. There,
    // mode 1 copies the colour texture home instead of the output one, which is
    // what proves the transport in isolation; here that would need a second
    // barrier pair chosen by an if in the middle of a barrier sequence, which is
    // the exact shape that produces a transition from a state a resource is not
    // in. So on this transport 1 reads the same as 0: the evaluate runs, nothing
    // reaches the game. Say so rather than claim a parity that is not here.
    if (code == 0 && re == NGX_SUCCESS && g_cfg.mode == 2)
    {
        Barrier(g_s12.list, back, D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);
        g_s12.list->CopyResource(back, g_s12.tex[SLOT_OUTPUT]);
        Barrier(g_s12.list, back, D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    else
    {
        // The back buffer still has to go home in the state ReShade left it in,
        // whether or not anything was written into it.
        Barrier(g_s12.list, back, D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    Barrier(g_s12.list, g_s12.tex[SLOT_OUTPUT], D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_COMMON);
    Barrier(g_s12.list, g_s12.tex[SLOT_COLOR], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COMMON);
    Synth12End();

    if (code != 0)
    {
        // An access violation inside NGX means its own state is no longer
        // trustworthy, so this stops rather than calling back in.
        Log("[synth] the D3D12 evaluate raised exception 0x%08X, so this backend stops "
            "here. The game keeps rendering on its own.", code);
        g_s12.ready   = false;
        g_s12.feature = nullptr;
        return;
    }
    if (re != NGX_SUCCESS)
    {
        // Refused cleanly rather than faulted: the call returned, so the next
        // frame may try again.
        Log("[synth] the D3D12 evaluate was refused with result 0x%08X, %s",
            re, NgxResultName(re));
        return;
    }

    const UINT64 n = ++g_s12.frames;
    InterlockedIncrement(&g_frames_delivered);
    if (n <= 1 || (n % 1800) == 0)
        Log("[synth] D3D12 frame %llu delivered (%ux%u)", n, w, h);
}
// The engine's own motion-vector texture, or null when the engine is not the
// source. An accessor rather than a direct read because g_ovk is declared far
// below the evaluate that has to ask.
static ID3D12Resource *SynthOfaVkMvTexture();


// The evaluate, for a session whose textures are all its own. The D3D12 half of
// the Vulkan transport is exactly this: no back buffer arrives on this device
// and none goes home from it, so there is nothing to copy here -- the four
// textures are filled and drained by the other API and this only transitions
// them, runs NGX over them and puts them back in COMMON.
//
// It is a second function rather than a branch inside Synth12Frame because what
// differs is the barrier sequence and nothing else, and a barrier sequence with
// an "if" in the middle of it is the thing that produces a transition from a
// state a resource is not in -- which on D3D12 is a device removal.
static bool Synth12EvaluateShared(const SynthParams *sp)
{
    if (!Synth12Begin()) return false;

    for (int i = 0; i < SLOT_COUNT; ++i)
        Barrier(g_s12.list, g_s12.tex[i], D3D12_RESOURCE_STATE_COMMON,
                i == SLOT_OUTPUT ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                 : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // The packed branch, and nothing on any other path reaches it: depth_cvt is
    // null unless Synth12Build was told the mirror's depth arrives packed. The
    // loop above has already put tex[SLOT_DEPTH] in NON_PIXEL_SHADER_RESOURCE,
    // which is the state the SRV wants, so only the destination transitions here.
    if (g_s12.depth_cvt != nullptr)
    {
        Barrier(g_s12.list, g_s12.depth_cvt, D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap *heaps[] = { g_s12.cvt_heap };
        g_s12.list->SetDescriptorHeaps(1, heaps);
        g_s12.list->SetComputeRootSignature(g_s12.cvt_rs);
        g_s12.list->SetPipelineState(g_s12.cvt_pso);
        g_s12.list->SetComputeRootDescriptorTable(
            0, g_s12.cvt_heap->GetGPUDescriptorHandleForHeapStart());
        g_s12.list->Dispatch((g_s12.w + 7) / 8, (g_s12.h + 7) / 8, 1);
        Barrier(g_s12.list, g_s12.depth_cvt, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    g_s12.params->Set("Color",         g_s12.tex[SLOT_COLOR]);
    g_s12.params->Set("Output",        g_s12.tex[SLOT_OUTPUT]);
    g_s12.params->Set("Depth",         g_s12.depth_cvt != nullptr ? g_s12.depth_cvt
                                                                  : g_s12.tex[SLOT_DEPTH]);
    // The engine's own texture when it is the source, and the transport's
    // otherwise. g_ovk.mv12 is non-null only after SynthOfaVkOpen succeeded, which
    // happens on no other path -- the mirror never opens it, so the D3D11 mirror
    // and the nine titles behind it read the same expression they always did.
    //
    // No copy back into SLOT_MV: NGX reads mv12 directly. It is a D3D12 resource
    // on this device like any other, opened from the engine's D3D11 texture.
    ID3D12Resource *const mv_engine = SynthOfaVkMvTexture();
    g_s12.params->Set("MotionVectors", mv_engine != nullptr ? mv_engine
                                                            : g_s12.tex[SLOT_MV]);
    Synth12FrameParams(sp);

    DWORD code = 0;
    Breadcrumb("running the D3D12 evaluate for the synthetic contract");
    static const char kNgxMark[] = "ngx evaluate";
    g_s12.list->BeginEvent(1, kNgxMark, sizeof(kNgxMark));
    NVSDK_NGX_Result re = g_cfg.mode != 1
        ? SafeEvaluateGuarded(g_s12.eval_feature, g_s12.list, g_s12.feature, g_s12.params, &code)
        : NGX_SUCCESS;
    g_s12.list->EndEvent();

    for (int i = 0; i < SLOT_COUNT; ++i)
        Barrier(g_s12.list, g_s12.tex[i],
                i == SLOT_OUTPUT ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                 : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COMMON);
    if (g_s12.depth_cvt != nullptr)
        Barrier(g_s12.list, g_s12.depth_cvt, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COMMON);
    const UINT64 v = Synth12End();

    if (code != 0)
    {
        // An access violation inside NGX means its own state is no longer
        // trustworthy, so this stops rather than calling back in.
        Log("[synth] the D3D12 evaluate raised exception 0x%08X, so this backend stops "
            "here. The game keeps rendering on its own.", code);
        g_s12.ready   = false;
        g_s12.feature = nullptr;
        return false;
    }
    if (re != NGX_SUCCESS)
    {
        // Refused cleanly rather than faulted: the call returned, so the next
        // frame may try again.
        Log("[synth] the D3D12 evaluate was refused with result 0x%08X, %s",
            re, NgxResultName(re));
        return false;
    }

    // The only thing ordering these two queues, since there is no shared fence
    // between them. The caller has already idled the game's Vulkan queue before
    // this ran; this waits out the D3D12 half before the caller reads the output
    // back. Cheap to say, expensive to run -- see the note below.
    // The budget is the caller's, because who is waiting decides what a safe wait
    // is. On the synthetic path this runs on ReShade's present thread with
    // nothing else parked behind it, and four seconds is a generous ceiling. On
    // the Vulkan mirror the game's own GPU is parked at a vkCmdWaitEvents until
    // this returns, and Windows resets a device that stops for two seconds -- so
    // the mirror sets a shorter one and gives up inside the TDR window.
    const DWORD budget = g_s12.wait_ms != 0 ? g_s12.wait_ms : 4000;
    if (g_s12.fence->GetCompletedValue() < v)
    {
        g_s12.fence->SetEventOnCompletion(v, g_s12.event);
        if (WaitForSingleObject(g_s12.event, budget) != WAIT_OBJECT_0)
        {
            Log("[synth] the D3D12 evaluate did not complete within %lu ms, so this backend "
                "stops rather than read a texture the GPU may still be writing.", budget);
            g_s12.ready = false;
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// The Vulkan transport
//
// A Vulkan game gets the same synthetic contract, the same self-check, the same
// arming gate and the same stand-down. What it cannot get is the game's own
// device: the call a DLSS 5 add-on detours is NVSDK_NGX_D3D12_EvaluateFeature
// and there is no Vulkan entry point to offer it instead, so the evaluate runs
// on a D3D12 device of this add-on's own -- exactly the reason the D3D11 mirror
// has one -- and the frame crosses as four textures created on the D3D12 side
// and imported into the game's VkDevice.
//
// NOTHING IS HOOKED, and that is a finding rather than a preference. ReShade's
// own vkCreateDevice appends VK_KHR_external_memory_win32 to every device it
// creates, as an optional extension: it checks the driver supports it, logs and
// carries on if not. That is the one extension this transport needs.
// VK_KHR_external_memory, VK_KHR_dedicated_allocation and
// VK_KHR_get_memory_requirements2 were all promoted into Vulkan 1.1 core, so on
// any device ReShade itself runs on they are simply there. The peer project
// hooks vkCreateDevice to add that list and to force the timelineSemaphore
// feature on; ReShade already forces timelineSemaphore on unconditionally, for
// its own present-from-compute path. So the hook would buy exactly one thing
// this transport does not use -- VK_KHR_external_semaphore_win32, the
// shared-fence import -- and cost the two hazards the note above HookRemove in
// DllMain names: a second thread through the window where the original bytes are
// back, and a jump left in place across an unload. There is no Vulkan hook to
// remove on detach because there is none to install.
//
// The price of not importing the fence is that the two queues are ordered on the
// CPU: ReShade's queue is idled after the copies in, and the D3D12 fence is
// waited on before the copy out.
// ponytail: a full pipeline bubble per delivered frame. The upgrade is importing
// g_s12.fence as a Vulkan timeline semaphore -- a D3D12 fence and a timeline
// semaphore are the same object, and g_s12.fence would only have to be created
// D3D12_FENCE_FLAG_SHARED -- which needs VK_KHR_external_semaphore_win32, which
// ReShade does not enable, which is what would need the hook. Do it when a
// Vulkan title measures badly enough to be worth that.
//
// Every Vulkan entry point is resolved through vkGetDeviceProcAddr out of a
// LoadLibrary of vulkan-1.dll. There is deliberately no import of it: a D3D11
// game on a machine with no Vulkan runtime has to go on loading this add-on, and
// an import table entry would stop it at the loader before DllMain ran.
//
// The Vulkan declarations below are written out rather than vendored, and that
// is the opposite call from the one at the top of this file. That argument was
// about a vtable: an implicit slot count that nothing states and that has been
// observed to move between ReShade releases. These are plain C structs from a
// published ABI that cannot change -- every driver on the machine is already
// compiled against them -- each one checkable against the specification by a
// reader with it open. The alternative is vendoring vulkan_core.h and the
// vk_video tree it includes, for fourteen entry points.
// ---------------------------------------------------------------------------

// Dispatchable handles are pointers; the rest are 64 bits wide on x64, which is
// the only architecture this add-on builds for.
typedef struct SVkDevice_T        *SVkDevice;
typedef struct SVkCommandBuffer_T *SVkCommandBuffer;
typedef uint64_t                   SVkHandle;   // VkImage, VkDeviceMemory, VkBuffer

static const uint32_t kSVkSuccess               = 0;
static const uint32_t kSVkStMemoryAllocateInfo  = 5;
static const uint32_t kSVkStBufferCreateInfo    = 12;
static const uint32_t kSVkStImageCreateInfo     = 14;
static const uint32_t kSVkStImageMemoryBarrier  = 45;
static const uint32_t kSVkStExternalMemoryImage = 1000072001;   // VK_KHR_external_memory
static const uint32_t kSVkStImportMemoryWin32   = 1000073000;   // VK_KHR_external_memory_win32
static const uint32_t kSVkStMemoryDedicated     = 1000127001;   // promoted into Vulkan 1.1
static const uint32_t kSVkHandleTypeD3D12Res    = 0x40;         // D3D12_RESOURCE_BIT
static const uint32_t kSVkImageType2D           = 1;
static const uint32_t kSVkSampleCount1          = 1;
static const uint32_t kSVkTilingOptimal         = 0;
static const uint32_t kSVkSharingExclusive      = 0;
static const uint32_t kSVkUsageTransferSrc      = 1;
static const uint32_t kSVkUsageTransferDst      = 2;
static const uint32_t kSVkLayoutUndefined       = 0;
static const uint32_t kSVkLayoutGeneral         = 1;
static const uint32_t kSVkLayoutTransferSrc     = 6;
static const uint32_t kSVkLayoutTransferDst     = 7;
static const uint32_t kSVkAspectColor           = 1;
static const uint32_t kSVkAspectDepth           = 2;
static const uint32_t kSVkAccessMemoryRead      = 0x8000;
static const uint32_t kSVkAccessMemoryWrite     = 0x10000;
static const uint32_t kSVkStageAllCommands      = 0x10000;
static const uint32_t kSVkQueueFamilyIgnored    = 0xFFFFFFFFu;

// Added for the Vulkan mirror's park. All Vulkan 1.0 core.
static const uint32_t kSVkStEventCreateInfo     = 10;
static const uint32_t kSVkStMemoryBarrier       = 46;
static const uint32_t kSVkEventSet              = 3;   // VK_EVENT_SET
static const uint32_t kSVkStageHost             = 0x4000;

struct SVkEventCreateInfo { uint32_t sType; const void *pNext; uint32_t flags; };
struct SVkMemoryBarrier   { uint32_t sType; const void *pNext;
                            uint32_t srcAccessMask, dstAccessMask; };

struct SVkExtent3D { uint32_t width, height, depth; };
struct SVkOffset3D { int32_t  x, y, z; };
struct SVkSubresourceLayers { uint32_t aspectMask, mipLevel, baseArrayLayer, layerCount; };
struct SVkSubresourceRange  { uint32_t aspectMask, baseMipLevel, levelCount, baseArrayLayer, layerCount; };

struct SVkExternalMemoryImageCreateInfo
{
    uint32_t    sType;
    const void *pNext;
    uint32_t    handleTypes;
};

struct SVkImageCreateInfo
{
    uint32_t        sType;
    const void     *pNext;
    uint32_t        flags;
    uint32_t        imageType;
    uint32_t        format;
    SVkExtent3D     extent;
    uint32_t        mipLevels;
    uint32_t        arrayLayers;
    uint32_t        samples;
    uint32_t        tiling;
    uint32_t        usage;
    uint32_t        sharingMode;
    uint32_t        queueFamilyIndexCount;
    const uint32_t *pQueueFamilyIndices;
    uint32_t        initialLayout;
};

struct SVkBufferCreateInfo
{
    uint32_t        sType;
    const void     *pNext;
    uint32_t        flags;
    uint64_t        size;
    uint32_t        usage;
    uint32_t        sharingMode;
    uint32_t        queueFamilyIndexCount;
    const uint32_t *pQueueFamilyIndices;
};

struct SVkMemoryRequirements { uint64_t size; uint64_t alignment; uint32_t memoryTypeBits; };

struct SVkMemoryDedicatedAllocateInfo
{
    uint32_t    sType;
    const void *pNext;
    SVkHandle   image;
    SVkHandle   buffer;
};

struct SVkImportMemoryWin32HandleInfoKHR
{
    uint32_t    sType;
    const void *pNext;
    uint32_t    handleType;
    HANDLE      handle;
    LPCWSTR     name;
};

struct SVkMemoryAllocateInfo
{
    uint32_t    sType;
    const void *pNext;
    uint64_t    allocationSize;
    uint32_t    memoryTypeIndex;
};

struct SVkImageMemoryBarrier
{
    uint32_t            sType;
    const void         *pNext;
    uint32_t            srcAccessMask;
    uint32_t            dstAccessMask;
    uint32_t            oldLayout;
    uint32_t            newLayout;
    uint32_t            srcQueueFamilyIndex;
    uint32_t            dstQueueFamilyIndex;
    SVkHandle           image;
    SVkSubresourceRange subresourceRange;
};

struct SVkImageCopy
{
    SVkSubresourceLayers srcSubresource;
    SVkOffset3D          srcOffset;
    SVkSubresourceLayers dstSubresource;
    SVkOffset3D          dstOffset;
    SVkExtent3D          extent;
};

struct SVkBufferImageCopy
{
    uint64_t             bufferOffset;
    uint32_t             bufferRowLength;
    uint32_t             bufferImageHeight;
    SVkSubresourceLayers imageSubresource;
    SVkOffset3D          imageOffset;
    SVkExtent3D          imageExtent;
};

typedef void *  (*PFN_SVkGetDeviceProcAddr)(SVkDevice, const char *);
typedef uint32_t(*PFN_SVkCreateImage)(SVkDevice, const SVkImageCreateInfo *, const void *, SVkHandle *);
typedef void    (*PFN_SVkDestroyImage)(SVkDevice, SVkHandle, const void *);
typedef void    (*PFN_SVkGetImageMemoryRequirements)(SVkDevice, SVkHandle, SVkMemoryRequirements *);
typedef uint32_t(*PFN_SVkAllocateMemory)(SVkDevice, const SVkMemoryAllocateInfo *, const void *, SVkHandle *);
typedef void    (*PFN_SVkFreeMemory)(SVkDevice, SVkHandle, const void *);
typedef uint32_t(*PFN_SVkBindImageMemory)(SVkDevice, SVkHandle, SVkHandle, uint64_t);
typedef uint32_t(*PFN_SVkCreateBuffer)(SVkDevice, const SVkBufferCreateInfo *, const void *, SVkHandle *);
typedef void    (*PFN_SVkDestroyBuffer)(SVkDevice, SVkHandle, const void *);
typedef void    (*PFN_SVkGetBufferMemoryRequirements)(SVkDevice, SVkHandle, SVkMemoryRequirements *);
typedef uint32_t(*PFN_SVkBindBufferMemory)(SVkDevice, SVkHandle, SVkHandle, uint64_t);
typedef void    (*PFN_SVkCmdPipelineBarrier)(SVkCommandBuffer, uint32_t, uint32_t, uint32_t,
                                             uint32_t, const void *, uint32_t, const void *,
                                             uint32_t, const SVkImageMemoryBarrier *);
typedef void    (*PFN_SVkCmdCopyImage)(SVkCommandBuffer, SVkHandle, uint32_t, SVkHandle, uint32_t,
                                       uint32_t, const SVkImageCopy *);
typedef void    (*PFN_SVkCmdCopyImageToBuffer)(SVkCommandBuffer, SVkHandle, uint32_t, SVkHandle,
                                               uint32_t, const SVkBufferImageCopy *);
typedef void    (*PFN_SVkCmdCopyBufferToImage)(SVkCommandBuffer, SVkHandle, SVkHandle, uint32_t,
                                               uint32_t, const SVkBufferImageCopy *);
// The park, and the one wait the mirror's rebuild needs. Core Vulkan 1.0.
typedef uint32_t(*PFN_SVkEvCreate)(SVkDevice, const SVkEventCreateInfo *, const void *, SVkHandle *);
typedef void    (*PFN_SVkEvDestroy)(SVkDevice, SVkHandle, const void *);
typedef uint32_t(*PFN_SVkEvStatus)(SVkDevice, SVkHandle);
typedef uint32_t(*PFN_SVkEvSet)(SVkDevice, SVkHandle);
typedef uint32_t(*PFN_SVkEvReset)(SVkDevice, SVkHandle);
typedef void    (*PFN_SVkCmdSetEvent)(SVkCommandBuffer, SVkHandle, uint32_t);
typedef void    (*PFN_SVkCmdWaitEvents)(SVkCommandBuffer, uint32_t, const SVkHandle *,
                                        uint32_t, uint32_t,
                                        uint32_t, const SVkMemoryBarrier *,
                                        uint32_t, const void *,
                                        uint32_t, const SVkImageMemoryBarrier *);
typedef uint32_t(*PFN_SVkDeviceWaitIdle)(SVkDevice);

struct SynthVk
{
    bool      tried;
    bool      ready;
    bool      laid_out;      // the four images have been taken out of UNDEFINED
    // Whether VK_KHR_push_descriptor is enabled on this device. Resolved rather
    // than assumed, and never called from here: it is ReShade's own
    // command_list::push_descriptors that needs it, and that function checks its
    // dispatch table, asserts in a debug build and RETURNS DOING NOTHING in a
    // release one -- a dispatch with no descriptors bound, which is silent wrong
    // pixels rather than an error. The same resolve-is-the-proof argument the
    // vkGetMemoryWin32HandleKHR gate below makes, for the same reason.
    bool      have_push_desc;
    HMODULE   lib;
    SVkDevice dev;

    PFN_SVkCreateImage                 CreateImage;
    PFN_SVkDestroyImage                DestroyImage;
    PFN_SVkGetImageMemoryRequirements  GetImageMemoryRequirements;
    PFN_SVkAllocateMemory              AllocateMemory;
    PFN_SVkFreeMemory                  FreeMemory;
    PFN_SVkBindImageMemory             BindImageMemory;
    PFN_SVkCreateBuffer                CreateBuffer;
    PFN_SVkDestroyBuffer               DestroyBuffer;
    PFN_SVkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_SVkBindBufferMemory            BindBufferMemory;
    PFN_SVkCmdPipelineBarrier          CmdPipelineBarrier;
    PFN_SVkCmdCopyImage                CmdCopyImage;
    PFN_SVkCmdCopyImageToBuffer        CmdCopyImageToBuffer;
    PFN_SVkCmdCopyBufferToImage        CmdCopyBufferToImage;
    PFN_SVkEvCreate                    EvCreate;
    PFN_SVkEvDestroy                   EvDestroy;
    PFN_SVkEvStatus                    EvStatus;
    PFN_SVkEvSet                       EvSet;
    PFN_SVkEvReset                     EvReset;
    PFN_SVkCmdSetEvent                 CmdSetEvent;
    PFN_SVkCmdWaitEvents               CmdWaitEvents;
    PFN_SVkDeviceWaitIdle              DeviceWaitIdle;

    // The same four slots the D3D12 side created, seen from Vulkan.
    SVkHandle img[SLOT_COUNT];
    SVkHandle mem[SLOT_COUNT];

    // Depth cannot be copied image to image into anything: vkCmdCopyImage and
    // vkCmdBlitImage both require the two formats to match exactly when either
    // side is a depth format, and the shared texture on the D3D12 side is
    // R32_FLOAT because a depth-stencil resource is not shareable. A copy
    // through a buffer carries no such rule -- the depth aspect of a 32-bit
    // float depth image is written out as one float per texel, which is exactly
    // what the R32_FLOAT texture takes back in -- so this is the one place a
    // staging allocation is needed.
    //
    // The SYNTHETIC transport no longer uses it: it samples the depth in a
    // compute pass instead, which is what let it stop demanding a 32-bit float
    // copy source. This stays for the VULKAN MIRROR, which is inside an NGX
    // detour with no ReShade command_list to dispatch through and only a raw
    // VkCommandBuffer, and which therefore still carries the copy path. It no
    // longer refuses a depth aspect that is not 32-bit float: the depth aspect of
    // D24_UNORM_S8_UINT is four bytes per texel too, so it travels through this
    // same buffer untouched and Synth12MakeDepthUnpack unpacks it on the far
    // side. What is still refused is a SIXTEEN-bit depth aspect, which is two
    // bytes per texel and would need this allocation and its texture resized.
    SVkHandle buf, buf_mem;

    UINT64 frames;
};

static SynthVk g_svk;

// DXGI to Vulkan, over the formats the four shared textures can actually be.
// Anything else returns VK_FORMAT_UNDEFINED and the caller refuses by name,
// rather than importing a texture whose bits nothing agrees about.
static uint32_t SynthVkFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:        return 37;   // VK_FORMAT_R8G8B8A8_UNORM
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   return 43;   // VK_FORMAT_R8G8B8A8_SRGB
    case DXGI_FORMAT_B8G8R8A8_UNORM:        return 44;   // VK_FORMAT_B8G8R8A8_UNORM
    // Vulkan has no BGRX. The two are the same four bytes and the fourth is
    // ignored on both sides, so this is a spelling difference, not a conversion.
    case DXGI_FORMAT_B8G8R8X8_UNORM:        return 44;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:   return 50;   // VK_FORMAT_B8G8R8A8_SRGB
    case DXGI_FORMAT_R10G10B10A2_UNORM:     return 64;   // VK_FORMAT_A2B10G10R10_UNORM_PACK32
    case DXGI_FORMAT_R16G16_FLOAT:          return 83;   // VK_FORMAT_R16G16_SFLOAT
    case DXGI_FORMAT_R16G16B16A16_FLOAT:    return 97;   // VK_FORMAT_R16G16B16A16_SFLOAT
    case DXGI_FORMAT_R32_FLOAT:             return 100;  // VK_FORMAT_R32_SFLOAT
    // Only the mirror's packed depth slot. Same four bytes as the line above and
    // the same copy; the difference is only that these are integers until
    // Synth12MakeDepthUnpack's pass has run over them.
    case DXGI_FORMAT_R32_UINT:              return 98;   // VK_FORMAT_R32_UINT
    case DXGI_FORMAT_R11G11B10_FLOAT:       return 122;  // VK_FORMAT_B10G11R11_UFLOAT_PACK32
    default:                                return 0;    // VK_FORMAT_UNDEFINED
    }
}

// A barrier on one of THIS add-on's images and never on one of the game's.
// ReShade's own command_list::barrier is what handles the game's resources: it
// reads the image's aspect mask out of the private data it keeps per VkImage,
// which for a depth image is not COLOR -- and which for an image ReShade never
// created is a null it goes on to dereference.
//
// Deliberately over-broad on stages and access. One copy per frame does not
// repay a precise dependency, and a narrow one that is wrong is a race that
// shows up as a frame of stale pixels rather than as an error.
// aspect defaults to COLOR, which is every image this add-on creates. The Vulkan
// mirror is the one caller that passes something else: it barriers the GAME's
// images, and the game's depth image is DEPTH. It cannot use ReShade's own
// barrier for them the way SynthVkFrame does, because inside an NGX detour there
// is no ReShade command_list -- only the game's raw VkCommandBuffer.
static void SynthVkBarrier(SVkCommandBuffer cb, SVkHandle img, uint32_t from, uint32_t to,
                           uint32_t aspect = kSVkAspectColor)
{
    SVkImageMemoryBarrier b = {};
    b.sType         = kSVkStImageMemoryBarrier;
    b.srcAccessMask = kSVkAccessMemoryRead | kSVkAccessMemoryWrite;
    b.dstAccessMask = kSVkAccessMemoryRead | kSVkAccessMemoryWrite;
    b.oldLayout     = from;
    b.newLayout     = to;
    // Not a queue family ownership transfer. These images are
    // VK_SHARING_MODE_EXCLUSIVE and are only ever touched from the one queue
    // ReShade renders effects on; the other API's access to them is separated by
    // the CPU wait, not by an acquire and release pair.
    b.srcQueueFamilyIndex = kSVkQueueFamilyIgnored;
    b.dstQueueFamilyIndex = kSVkQueueFamilyIgnored;
    b.image               = img;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    g_svk.CmdPipelineBarrier(cb, kSVkStageAllCommands, kSVkStageAllCommands, 0,
                             0, nullptr, 0, nullptr, 1, &b);
}

// dw/dh are the destination's own size, and zero means "the same as the source",
// which is every caller on the synthetic path -- it created both images itself
// and knows they match. The mirror does not: the sizes come off the game's
// parameter block, so a contract whose Output is smaller than the feature
// declares truncates the copy rather than producing undefined behaviour.
static void SynthVkCopyImage(SVkCommandBuffer cb, SVkHandle src, uint32_t src_layout,
                             SVkHandle dst, uint32_t dst_layout, UINT w, UINT h,
                             UINT dw = 0, UINT dh = 0)
{
    if (dw != 0 && dw < w) w = dw;
    if (dh != 0 && dh < h) h = dh;

    SVkImageCopy c = {};
    c.srcSubresource.aspectMask = kSVkAspectColor;
    c.srcSubresource.layerCount = 1;
    c.dstSubresource.aspectMask = kSVkAspectColor;
    c.dstSubresource.layerCount = 1;
    c.extent.width  = w;
    c.extent.height = h;
    c.extent.depth  = 1;
    g_svk.CmdCopyImage(cb, src, src_layout, dst, dst_layout, 1, &c);
}

static void SynthVkReleaseImages()
{
    // Gated on the entry points rather than on ready, because the one caller
    // that most needs this is a half-finished import -- which sets ready false
    // on its way out and would otherwise leave every image it did create behind.
    if (g_svk.dev == nullptr || g_svk.DestroyImage == nullptr) return;
    for (int i = 0; i < SLOT_COUNT; ++i)
    {
        if (g_svk.img[i] != 0) { g_svk.DestroyImage(g_svk.dev, g_svk.img[i], nullptr); g_svk.img[i] = 0; }
        if (g_svk.mem[i] != 0) { g_svk.FreeMemory(g_svk.dev, g_svk.mem[i], nullptr);   g_svk.mem[i] = 0; }
    }
    if (g_svk.buf     != 0) { g_svk.DestroyBuffer(g_svk.dev, g_svk.buf, nullptr);  g_svk.buf     = 0; }
    if (g_svk.buf_mem != 0) { g_svk.FreeMemory(g_svk.dev, g_svk.buf_mem, nullptr); g_svk.buf_mem = 0; }
    g_svk.laid_out = false;
}

// Resolve everything, and refuse by name on the first thing that is not there.
// Runs once and a failure is never retried, the same rule the D3D12 session open
// follows.
static bool SynthVkOpen(reshade::api::effect_runtime *rt)
{
    if (g_svk.tried) return g_svk.ready;
    g_svk.tried = true;

    Breadcrumb("opening the Vulkan side of the synthetic transport");
    reshade::api::device *rdev = rt->get_device();
    g_svk.dev = rdev != nullptr
        ? reinterpret_cast<SVkDevice>(static_cast<uintptr_t>(rdev->get_native())) : nullptr;
    if (g_svk.dev == nullptr)
    { Log("[synth] the effect runtime reports no native VkDevice"); return false; }

    // Loaded, never linked. An import of vulkan-1.dll would stop this add-on at
    // the loader in every D3D11 game on a machine with no Vulkan runtime, which
    // is a plausible description of most of the nine titles the mirror runs in.
    g_svk.lib = LoadLibraryW(L"vulkan-1.dll");
    if (g_svk.lib == nullptr)
    {
        Log("[synth] vulkan-1.dll is not loadable in this process, so no Vulkan entry "
            "point can be resolved and this transport refuses. The game is unaffected.");
        return false;
    }
    auto get_proc = reinterpret_cast<PFN_SVkGetDeviceProcAddr>(
        GetProcAddress(g_svk.lib, "vkGetDeviceProcAddr"));
    if (get_proc == nullptr)
    { Log("[synth] vulkan-1.dll exports no vkGetDeviceProcAddr"); return false; }

    // Counted rather than written out, so the line below states the number that
    // was resolved and goes on being true when a fifteenth is added here.
    int resolved = 0;
    #define SYNTH_VK_GET(member, name)                                                     \
        g_svk.member = reinterpret_cast<PFN_SVk##member>(get_proc(g_svk.dev, name));       \
        if (g_svk.member == nullptr)                                                       \
        {                                                                                  \
            Log("[synth] this Vulkan device does not resolve %s, so the transport "        \
                "refuses. Everything asked for here is core Vulkan, so a miss means the "  \
                "device was created without it rather than that it is unsupported.", name);\
            return false;                                                                  \
        }                                                                                  \
        ++resolved;
    SYNTH_VK_GET(CreateImage,                 "vkCreateImage")
    SYNTH_VK_GET(DestroyImage,                "vkDestroyImage")
    SYNTH_VK_GET(GetImageMemoryRequirements,  "vkGetImageMemoryRequirements")
    SYNTH_VK_GET(AllocateMemory,              "vkAllocateMemory")
    SYNTH_VK_GET(FreeMemory,                  "vkFreeMemory")
    SYNTH_VK_GET(BindImageMemory,             "vkBindImageMemory")
    SYNTH_VK_GET(CreateBuffer,                "vkCreateBuffer")
    SYNTH_VK_GET(DestroyBuffer,               "vkDestroyBuffer")
    SYNTH_VK_GET(GetBufferMemoryRequirements, "vkGetBufferMemoryRequirements")
    SYNTH_VK_GET(BindBufferMemory,            "vkBindBufferMemory")
    SYNTH_VK_GET(CmdPipelineBarrier,          "vkCmdPipelineBarrier")
    SYNTH_VK_GET(CmdCopyImage,                "vkCmdCopyImage")
    SYNTH_VK_GET(CmdCopyImageToBuffer,        "vkCmdCopyImageToBuffer")
    SYNTH_VK_GET(CmdCopyBufferToImage,        "vkCmdCopyBufferToImage")
    // The Vulkan mirror's park, and the one wait its rebuild needs. Resolved on
    // both transports rather than only the mirror's: all eight are Vulkan 1.0
    // core, so a device that resolves the fourteen above resolves these too, and
    // one resolve list is one thing to keep in step instead of two.
    SYNTH_VK_GET(EvCreate,                    "vkCreateEvent")
    SYNTH_VK_GET(EvDestroy,                   "vkDestroyEvent")
    SYNTH_VK_GET(EvStatus,                    "vkGetEventStatus")
    SYNTH_VK_GET(EvSet,                       "vkSetEvent")
    SYNTH_VK_GET(EvReset,                     "vkResetEvent")
    SYNTH_VK_GET(CmdSetEvent,                 "vkCmdSetEvent")
    SYNTH_VK_GET(CmdWaitEvents,               "vkCmdWaitEvents")
    SYNTH_VK_GET(DeviceWaitIdle,              "vkDeviceWaitIdle")
    #undef SYNTH_VK_GET

    // Optional, and the only optional one: the depth conversion pass binds its
    // two descriptors with vkCmdPushDescriptorSet through ReShade, and ReShade
    // asks for VK_KHR_push_descriptor on every device it creates but as an
    // optional extension -- so a driver without it is left without it. Resolved
    // here and reported by SynthDepthCsOpen, never called from this file.
    g_svk.have_push_desc = get_proc(g_svk.dev, "vkCmdPushDescriptorSetKHR") != nullptr ||
                           get_proc(g_svk.dev, "vkCmdPushDescriptorSet")    != nullptr;

    // The one extension gate, and the only one there is: everything above is
    // Vulkan 1.1 core, and the timeline semaphore feature the peer project hooks
    // vkCreateDevice to force on is not used here at all.
    //
    // It is asked as a question rather than assumed, because the two structures
    // the import is built from -- VkExternalMemoryImageCreateInfo and
    // VkImportMemoryWin32HandleInfoKHR -- are pNext chain entries, and chaining
    // one from an extension the device did not enable is undefined rather than
    // an error: a driver may ignore the chain and hand back an image bound to
    // ordinary local memory that shares nothing with D3D12, which is wrong
    // pixels and no message anywhere. vkGetMemoryWin32HandleKHR is a command of
    // that same extension, and a device command whose extension was not enabled
    // resolves to null -- so it is resolved and never called. The resolution is
    // the proof; vkAllocateMemory's result below is the check on the proof.
    //
    // UNVERIFIED: the loader is specified to return null here, and some loader
    // versions have been reported returning a trampoline for an extension that
    // is supported but not enabled. Treated as a strong hint and not as the only
    // guard, which is why the allocation result is tested as well.
    if (get_proc(g_svk.dev, "vkGetMemoryWin32HandleKHR") == nullptr)
    {
        Log("[synth] VK_KHR_external_memory_win32 is not enabled on this device -- "
            "vkGetMemoryWin32HandleKHR does not resolve -- so a D3D12 texture cannot be "
            "imported and this transport refuses rather than chain a structure from an "
            "extension that is not there. ReShade asks for that extension on every device "
            "it creates, but as an optional one, so a driver without it is left without it.");
        return false;
    }

    Log("[synth] Vulkan side ready: VkDevice %p, VK_KHR_external_memory_win32 enabled, "
        "VK_KHR_push_descriptor %s, %d entry points resolved out of vulkan-1.dll with no "
        "import of it.", static_cast<void *>(g_svk.dev),
        g_svk.have_push_desc ? "enabled" : "NOT enabled", resolved);
    g_svk.ready = true;
    return true;
}

// Import the four D3D12 textures, and make the staging buffer depth travels
// through. Runs only after Synth12Build has created them and their NT handles.
// Every size comes off the D3D12 texture that was actually created, not off a
// pair of arguments: Synth12Build sizes Output on its own now, so one w,h could
// only ever be right for three of the four slots.
static bool SynthVkImport()
{
    bool said_memory = false;
    for (int i = 0; i < SLOT_COUNT; ++i)
    {
        // Read back off the texture that was actually created rather than
        // recomputed from back_fmt, so a slot whose format was decided elsewhere
        // cannot be imported as something else.
        const D3D12_RESOURCE_DESC rd = g_s12.tex[i]->GetDesc();
        const UINT w = static_cast<UINT>(rd.Width);
        const UINT h = rd.Height;
        const uint32_t vkfmt = SynthVkFormat(rd.Format);
        if (vkfmt == 0)
        {
            Log("[synth] %s is %s on the D3D12 side and this add-on has no Vulkan format "
                "for it, so the import refuses rather than guess one.",
                kSlotName[i], FormatName(rd.Format));
            return false;
        }

        SVkExternalMemoryImageCreateInfo ext = {};
        ext.sType       = kSVkStExternalMemoryImage;
        ext.handleTypes = kSVkHandleTypeD3D12Res;

        SVkImageCreateInfo ici = {};
        ici.sType         = kSVkStImageCreateInfo;
        ici.pNext         = &ext;
        ici.imageType     = kSVkImageType2D;
        ici.format        = vkfmt;
        ici.extent.width  = w;
        ici.extent.height = h;
        ici.extent.depth  = 1;
        ici.mipLevels     = 1;
        ici.arrayLayers   = 1;
        ici.samples       = kSVkSampleCount1;
        ici.tiling        = kSVkTilingOptimal;
        // Transfer only. Nothing on the Vulkan side samples these or writes them
        // from a shader, and asking for STORAGE on a BGRA image is a creation
        // failure on drivers that do not support that pairing -- a refusal
        // earned by a usage flag nothing needs.
        ici.usage         = kSVkUsageTransferSrc | kSVkUsageTransferDst;
        ici.sharingMode   = kSVkSharingExclusive;
        ici.initialLayout = kSVkLayoutUndefined;

        uint32_t r = g_svk.CreateImage(g_svk.dev, &ici, nullptr, &g_svk.img[i]);
        if (r != kSVkSuccess)
        {
            Log("[synth] vkCreateImage refused the %ux%u %s image for %s with VkResult %d",
                w, h, FormatName(rd.Format), kSlotName[i], static_cast<int>(r));
            return false;
        }

        SVkMemoryRequirements req = {};
        g_svk.GetImageMemoryRequirements(g_svk.dev, g_svk.img[i], &req);

        // ponytail: the lowest allowed memory type. ReShade exposes no
        // VkPhysicalDevice -- it keeps that handle private and get_native
        // answers for the device, the queue and the swapchain only -- so
        // vkGetPhysicalDeviceMemoryProperties cannot be called and the types
        // cannot be inspected. Every bit in memoryTypeBits is a type the driver
        // will accept for this image, so this is correct by the specification
        // and at worst slow, and the driver has already narrowed the set to what
        // imported D3D12 memory can live in. The bits and the choice are logged
        // because a report is the only thing that can settle it on a vendor this
        // has not run on.
        uint32_t type_index = 0;
        for (uint32_t b = 0; b < 32; ++b)
            if (req.memoryTypeBits & (1u << b)) { type_index = b; break; }
        if (!said_memory)
        {
            said_memory = true;
            Log("[synth] imported memory: the driver allows types 0x%08X for these images "
                "and the lowest is taken, index %u. UNVERIFIED on AMD and on Intel -- if "
                "the import fails or the picture is wrong on one of those, this line is "
                "the first thing to compare.", req.memoryTypeBits, type_index);
        }

        // Mandatory for an imported D3D12 resource: the allocation is that one
        // resource, and nothing else can be suballocated out of it.
        //
        // ASSUMED, NOT MEASURED, and stated here because it is the one pNext
        // entry above that has no resolve behind it. VkMemoryDedicatedAllocateInfo
        // is core in Vulkan 1.1; on a 1.0 device it belongs to
        // VK_KHR_dedicated_allocation, which ReShade does not ask for and which
        // has no device command to resolve the way vkGetMemoryWin32HandleKHR
        // proves the win32 extension. What stands in for a resolve is that
        // VK_KHR_external_memory_win32 requires VK_KHR_external_memory, and that
        // one is core in 1.1 as well and is not on ReShade's list either -- so a
        // device that got this far is a 1.1 device or the extension gate above
        // would already have refused it. If a driver is ever reported importing
        // memory that shares nothing, this premise is the first thing to test.
        SVkMemoryDedicatedAllocateInfo ded = {};
        ded.sType = kSVkStMemoryDedicated;
        ded.image = g_svk.img[i];

        SVkImportMemoryWin32HandleInfoKHR imp = {};
        imp.sType      = kSVkStImportMemoryWin32;
        imp.pNext      = &ded;
        imp.handleType = kSVkHandleTypeD3D12Res;
        // Duplicated by the driver rather than consumed, so this side still owns
        // the handle and Synth12ReleaseTextures still has to close it.
        imp.handle     = g_s12.share[i];

        SVkMemoryAllocateInfo mai = {};
        mai.sType           = kSVkStMemoryAllocateInfo;
        mai.pNext           = &imp;
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = type_index;

        r = g_svk.AllocateMemory(g_svk.dev, &mai, nullptr, &g_svk.mem[i]);
        if (r != kSVkSuccess)
        {
            Log("[synth] vkAllocateMemory refused the D3D12 shared handle for %s with "
                "VkResult %d. The likeliest cause is that the D3D12 device this add-on "
                "created is on a different GPU from the one the game renders with, which "
                "nothing here can see: there is no VkPhysicalDevice to read a LUID off.",
                kSlotName[i], static_cast<int>(r));
            return false;
        }
        r = g_svk.BindImageMemory(g_svk.dev, g_svk.img[i], g_svk.mem[i], 0);
        if (r != kSVkSuccess)
        {
            Log("[synth] vkBindImageMemory failed for %s with VkResult %d",
                kSlotName[i], static_cast<int>(r));
            return false;
        }
    }

    // Sized off the depth slot itself, which on the mirror is the game's render
    // size rather than anything the colour slots are.
    const D3D12_RESOURCE_DESC dd = g_s12.tex[SLOT_DEPTH]->GetDesc();
    SVkBufferCreateInfo bci = {};
    bci.sType       = kSVkStBufferCreateInfo;
    // One 32-bit word per texel either way: the depth aspect of D32_SFLOAT and
    // the depth aspect of D24_UNORM_S8_UINT -- which the spec addresses as
    // X8_D24_UNORM_PACK32 -- are both four bytes wide, so the packed case needs
    // nothing from this size math.
    bci.size        = static_cast<uint64_t>(dd.Width) * dd.Height * 4;
    bci.usage       = kSVkUsageTransferSrc | kSVkUsageTransferDst;
    bci.sharingMode = kSVkSharingExclusive;
    if (g_svk.CreateBuffer(g_svk.dev, &bci, nullptr, &g_svk.buf) != kSVkSuccess)
    {
        Log("[synth] vkCreateBuffer refused the %llu-byte depth staging buffer", bci.size);
        return false;
    }

    SVkMemoryRequirements breq = {};
    g_svk.GetBufferMemoryRequirements(g_svk.dev, g_svk.buf, &breq);
    // Same choice and the same reasoning as above, and here it is not even a
    // heuristic: this buffer is not imported, so any type the driver allows is
    // as correct as any other.
    uint32_t btype = 0;
    for (uint32_t b = 0; b < 32; ++b)
        if (breq.memoryTypeBits & (1u << b)) { btype = b; break; }

    SVkMemoryAllocateInfo bmai = {};
    bmai.sType           = kSVkStMemoryAllocateInfo;
    bmai.allocationSize  = breq.size;
    bmai.memoryTypeIndex = btype;
    if (g_svk.AllocateMemory(g_svk.dev, &bmai, nullptr, &g_svk.buf_mem) != kSVkSuccess ||
        g_svk.BindBufferMemory(g_svk.dev, g_svk.buf, g_svk.buf_mem, 0) != kSVkSuccess)
    {
        Log("[synth] the depth staging buffer could not be backed with memory");
        return false;
    }

    Log("[synth] %d D3D12 textures imported into the game's VkDevice, and a %llu-byte "
        "buffer made for the depth aspect to travel through.",
        static_cast<int>(SLOT_COUNT), breq.size);
    return true;
}

// ---------------------------------------------------------------------------
// The depth conversion pass
//
// This is what unblocks the synthetic Vulkan path, and it clears exactly two of
// the three obstacles Baldur's Gate 3 reported on 2026-08-31.
//
//   FORMAT.  The transport carried depth out through a buffer and back in as
//            R32_FLOAT, which is exact for a 32-bit float depth aspect and for
//            nothing else -- a D24 aspect copies out as a packed unnormalised
//            integer. A SAMPLE does not care: texelFetch on a depth-aspect view
//            returns the driver-normalised float for D16, D24S8 and D32 alike,
//            which is raw depth exactly as NGX wants it. No linearisation, no
//            near or far plane, and reversed-Z still carried by the
//            DepthInverted create flag as it already was.
//
//   USAGE.   ReShade's DEPTH semantic on Vulkan is bound to generic_depth's own
//            backup texture, created shader_resource | copy_dest with no
//            copy_source, so no transfer command may read it. shader_resource
//            IS VK_IMAGE_USAGE_SAMPLED_BIT: a copy cannot be issued, a sample
//            can.
//
// The SIZE obstacle is NOT cleared and deliberately is not. This pass is one
// texel in, one texel out, same size, no filtering and no scaling. It could
// rescale and make the refusal disappear; depth resampled to a size the game
// never rendered is invented data, and a game whose depth is smaller than its
// back buffer is running its own upscaler -- which is the Vulkan MIRROR's case
// by definition, and the mirror is the right path there.
//
// Through ReShade's add-on API rather than raw Vulkan, and that is not laziness
// for its own sake: ReShade is the only party that knows each image's aspect
// mask, which for the depth image is DEPTH and not COLOR, and a default
// resource_view_desc makes it derive both the format and the aspect from the
// image it already has private data for. It is also the only party that can
// create a descriptor set on the game's device without this add-on carrying a
// descriptor pool. push_descriptors deletes the pool, the per-slot sets and the
// whole "update a set that may still be in flight" problem before any of them
// exist.
//
// The pass writes into a texture of ITS OWN rather than straight into the
// imported D3D12 depth texture, and one full-resolution R32 image copy carries
// it the rest of the way. That is one extra copy -- about 24 MB at 3840x1600,
// under a tenth of a millisecond -- bought for three things: the destination is
// a resource ReShade created and therefore certainly has private data for,
// whereas an image this add-on made with its own vkCreateImage is registered
// with ReShade only if that call happened to travel down its layer; the
// imported image does not need VK_IMAGE_USAGE_STORAGE_BIT, which on imported
// D3D12 memory is a creation flag nothing here can test; and the D3D12 texture
// behind it does not need ALLOW_UNORDERED_ACCESS either. Three untestable
// premises traded for one copy.
// ---------------------------------------------------------------------------

#include "depth_convert_spv.h"

// The blob is data, and the one failure mode of a data file is that it stopped
// being the thing its header describes. Both numbers are in that header.
static_assert(sizeof(kDepthConvertSpv) == 1640,
              "depth_convert_spv.h is not the 1640-byte module its header describes");
static_assert(sizeof(kDepthConvertSpv) / sizeof(kDepthConvertSpv[0]) == 410,
              "depth_convert_spv.h is not the 410-word module its header describes");

struct SynthDepthCs
{
    bool tried;
    bool ready;
    reshade::api::device *dev;

    reshade::api::pipeline_layout layout;
    reshade::api::pipeline        pso;
    reshade::api::sampler         samp;

    // Ours, and the only thing the dispatch writes. R32_FLOAT at the game's
    // render size, and it never leaves copy_source except for the dispatch.
    reshade::api::resource      dst;
    reshade::api::resource_view dst_uav;
    UINT                        w, h;

    // The view over whatever the DEPTH semantic is bound to, cached because the
    // binding almost never changes, and retired one frame late because the
    // command buffer that names it has not been submitted yet when it is
    // replaced. The transport idles the queue once per frame, so a view retired
    // at the top of frame N+1 is a view no submitted work still names.
    uint64_t                    src_res;
    reshade::api::resource_view src_srv;
    reshade::api::resource_view retire_srv;

    UINT64 frames;
};

static SynthDepthCs g_dcs;

// Built once. A failure is latched and never retried, the same rule Synth12Open
// and SynthVkOpen follow, and every branch that returns false names its own
// reason -- there is no path out of here that is silently off.
static bool SynthDepthCsOpen(reshade::api::device *dev)
{
    using namespace reshade::api;

    if (g_dcs.tried) return g_dcs.ready;
    g_dcs.tried = true;
    g_dcs.dev   = dev;

    if (dev == nullptr) return false;

    if (!dev->check_capability(device_caps::compute_shader))
    {
        Log("[synth] this device reports no compute support, so the depth conversion "
            "cannot be built and the synthetic contract has no depth to hand NGX.");
        return false;
    }
    if (dev->get_api() != device_api::vulkan)
    {
        // The same host code would serve D3D12 with the cs_5_0 blob bridge.inc
        // already compiles at runtime, and that is the upgrade path. It is not
        // built, because nothing on the D3D12 synthetic backend has been
        // measured asking for it and a second blob is a second thing to keep in
        // step.
        // ponytail: Vulkan only. Add the DXBC arm when a D3D12 title reports the
        // depth refusal this clears.
        Log("[synth] the depth conversion pass carries a SPIR-V module only, so it can be "
            "built on Vulkan and not on this runtime.");
        return false;
    }
    if (!g_svk.have_push_desc)
    {
        Log("[synth] VK_KHR_push_descriptor is not enabled on this device, so the depth "
            "conversion cannot bind its two descriptors. ReShade asks for that extension "
            "on every device it creates, but as an optional one, so a driver without it is "
            "left without it -- and ReShade's own push_descriptors then records nothing at "
            "all rather than failing, which would be silent wrong depth.");
        return false;
    }
    if (kDepthConvertSpv[0] != 0x07230203u)
    {
        Log("[synth] DEFECT: depth_convert_spv.h does not begin with the SPIR-V magic "
            "number, so the array is not a module. Nothing is dispatched.");
        return false;
    }

    // Point, because texelFetch reads no filter state at all -- the sampler
    // exists only because the module declares a combined image sampler. Clamp
    // for the same reason: nothing outside the image is ever addressed.
    sampler_desc sd = {};
    sd.filter    = filter_mode::min_mag_mip_point;
    sd.address_u = texture_address_mode::clamp;
    sd.address_v = texture_address_mode::clamp;
    sd.address_w = texture_address_mode::clamp;
    if (!dev->create_sampler(sd, &g_dcs.samp))
    {
        Log("[synth] the depth conversion sampler could not be created.");
        return false;
    }

    // One set with two bindings, matching the module: binding 0 the combined
    // image sampler, binding 1 the r32f storage image. Both in ONE parameter,
    // because ReShade maps one parameter to one descriptor set and the module
    // puts both bindings in set 0.
    static const descriptor_range ranges[2] = {
        { /*binding*/ 0, /*dx_register_index*/ 0, /*dx_register_space*/ 0, /*count*/ 1,
          /*visibility*/ shader_stage::compute, /*array_size*/ 1,
          descriptor_type::sampler_with_resource_view },
        { 1, 0, 0, 1, shader_stage::compute, 1, descriptor_type::unordered_access_view },
    };

    pipeline_layout_param params[2] = {};
    params[0].type                    = pipeline_layout_param_type::push_descriptors_with_ranges;
    params[0].descriptor_table.count  = 2;
    params[0].descriptor_table.ranges = ranges;
    // A push constant parameter consumes no descriptor set, so the descriptors
    // above stay in set 0 whichever order these two are written in.
    params[1].type                      = pipeline_layout_param_type::push_constants;
    params[1].push_constants.binding    = 0;   // offset, in 32-bit values
    params[1].push_constants.count      = 2;   // uvec2 extent
    params[1].push_constants.visibility = shader_stage::compute;

    if (!dev->create_pipeline_layout(2, params, &g_dcs.layout))
    {
        Log("[synth] the depth conversion pipeline layout could not be created.");
        dev->destroy_sampler(g_dcs.samp);
        g_dcs.samp = sampler { 0 };
        return false;
    }

    shader_desc cs = {};
    cs.code      = kDepthConvertSpv;
    cs.code_size = sizeof(kDepthConvertSpv);

    pipeline_subobject sub = {};
    sub.type  = pipeline_subobject_type::compute_shader;
    sub.count = 1;
    sub.data  = &cs;

    if (!dev->create_pipeline(g_dcs.layout, 1, &sub, &g_dcs.pso))
    {
        Log("[synth] the depth conversion pipeline could not be created from the %u-byte "
            "SPIR-V module. The module is the one depth_convert_spv.h describes; a refusal "
            "here is the driver's, and vkCreateComputePipelines does not report why.",
            static_cast<unsigned>(sizeof(kDepthConvertSpv)));
        dev->destroy_pipeline_layout(g_dcs.layout);
        dev->destroy_sampler(g_dcs.samp);
        g_dcs.layout = pipeline_layout { 0 };
        g_dcs.samp   = sampler { 0 };
        return false;
    }

    Log("[synth] depth conversion built: one 8x8 compute pass that samples a DEPTH-aspect "
        "view and stores R32_FLOAT, so the depth binding no longer has to be 32-bit float "
        "and no longer has to be a copy source. It is still required to be the size of the "
        "back buffer, and that is not something this pass may fix.");
    g_dcs.ready = true;
    return true;
}

static void SynthDepthCsRelease()
{
    if (g_dcs.dev == nullptr) return;
    if (g_dcs.retire_srv.handle != 0)
    { g_dcs.dev->destroy_resource_view(g_dcs.retire_srv); g_dcs.retire_srv = reshade::api::resource_view { 0 }; }
    if (g_dcs.src_srv.handle != 0)
    { g_dcs.dev->destroy_resource_view(g_dcs.src_srv); g_dcs.src_srv = reshade::api::resource_view { 0 }; }
    if (g_dcs.dst_uav.handle != 0)
    { g_dcs.dev->destroy_resource_view(g_dcs.dst_uav); g_dcs.dst_uav = reshade::api::resource_view { 0 }; }
    if (g_dcs.dst.handle != 0)
    { g_dcs.dev->destroy_resource(g_dcs.dst); g_dcs.dst = reshade::api::resource { 0 }; }
    g_dcs.src_res = 0;
    g_dcs.w = 0;
    g_dcs.h = 0;
}

// The destination, sized to the frame. Created in copy_source and left there
// between frames, so every frame's barrier pair is the same two states -- a
// barrier from a state a resource is not in is the one mistake this class of
// code makes.
static bool SynthDepthCsSize(UINT w, UINT h)
{
    using namespace reshade::api;

    if (g_dcs.dst.handle != 0 && g_dcs.w == w && g_dcs.h == h) return true;

    if (g_dcs.dst_uav.handle != 0)
    { g_dcs.dev->destroy_resource_view(g_dcs.dst_uav); g_dcs.dst_uav = resource_view { 0 }; }
    if (g_dcs.dst.handle != 0)
    { g_dcs.dev->destroy_resource(g_dcs.dst); g_dcs.dst = resource { 0 }; }

    const resource_desc rd(w, h, 1, 1, format::r32_float, 1, memory_heap::default_,
                           resource_usage::unordered_access | resource_usage::copy_source);
    if (!g_dcs.dev->create_resource(rd, nullptr, resource_usage::copy_source, &g_dcs.dst))
    {
        Log("[synth] the %ux%u R32_FLOAT texture the depth conversion writes into could not "
            "be created.", w, h);
        return false;
    }
    if (!g_dcs.dev->create_resource_view(g_dcs.dst, resource_usage::unordered_access,
                                         resource_view_desc(format::r32_float), &g_dcs.dst_uav))
    {
        Log("[synth] no unordered access view could be made over the depth conversion's own "
            "R32_FLOAT texture.");
        g_dcs.dev->destroy_resource(g_dcs.dst);
        g_dcs.dst = resource { 0 };
        return false;
    }
    g_dcs.w = w;
    g_dcs.h = h;
    return true;
}

// One frame. Records into the caller's command list and returns the texture the
// result is in, or a null handle with a reason already logged.
//
// No layout transition on the SOURCE, and that is deliberate rather than an
// omission: generic_depth's own reshade_begin_effects handler leaves the depth
// binding in shader_resource, which on Vulkan is SHADER_READ_ONLY_OPTIMAL --
// exactly the layout push_descriptors declares for a combined image sampler.
// Transitioning it would be transitioning it to the state it is already in. The
// same-state barrier below is there for the memory dependency alone, so the
// copy generic_depth made is visible to this read. The peer project dispatches
// its equivalent with a barrier on the destination and none on the source.
static reshade::api::resource SynthDepthCsRun(reshade::api::command_list *cmd,
                                              reshade::api::resource depth, UINT w, UINT h)
{
    using namespace reshade::api;

    if (!g_dcs.ready || cmd == nullptr) return resource { 0 };
    if (!SynthDepthCsSize(w, h)) { g_dcs.ready = false; return resource { 0 }; }

    // Last frame's view, retired now. The transport idles the queue once per
    // frame, so nothing submitted still names it.
    if (g_dcs.retire_srv.handle != 0)
    {
        g_dcs.dev->destroy_resource_view(g_dcs.retire_srv);
        g_dcs.retire_srv = resource_view { 0 };
    }

    if (g_dcs.src_srv.handle == 0 || g_dcs.src_res != depth.handle)
    {
        g_dcs.retire_srv = g_dcs.src_srv;
        g_dcs.src_srv    = resource_view { 0 };
        g_dcs.src_res    = 0;

        // A default-constructed desc, and the default is the point: ReShade then
        // takes the format off the image's own create info and derives the
        // aspect mask from it -- DEPTH for a depth format, with STENCIL removed
        // because the usage says shader_resource. Naming a format here would be
        // this add-on guessing at an aspect ReShade already knows.
        if (!g_dcs.dev->create_resource_view(depth, resource_usage::shader_resource,
                                             resource_view_desc(), &g_dcs.src_srv))
        {
            static bool said;
            if (!said)
            {
                said = true;
                Log("[synth] no shader resource view could be made over the depth binding, "
                    "so the depth conversion cannot read it. On Vulkan that binding is "
                    "generic_depth's own backup texture and is created shader_resource, so "
                    "a refusal here is a surprise rather than the expected case.");
            }
            return resource { 0 };
        }
        g_dcs.src_res = depth.handle;
    }

    cmd->barrier(depth, resource_usage::shader_resource, resource_usage::shader_resource);
    cmd->barrier(g_dcs.dst, resource_usage::copy_source, resource_usage::unordered_access);

    cmd->bind_pipeline(pipeline_stage::compute_shader, g_dcs.pso);

    sampler_with_resource_view swv = {};
    swv.sampler = g_dcs.samp;
    swv.view    = g_dcs.src_srv;

    descriptor_table_update u = {};
    u.binding     = 0;
    u.count       = 1;
    u.type        = descriptor_type::sampler_with_resource_view;
    u.descriptors = &swv;
    cmd->push_descriptors(shader_stage::compute, g_dcs.layout, 0, u);

    u.binding     = 1;
    u.type        = descriptor_type::unordered_access_view;
    u.descriptors = &g_dcs.dst_uav;
    cmd->push_descriptors(shader_stage::compute, g_dcs.layout, 0, u);

    // The module bounds-checks against this rather than against imageSize, so it
    // is the one value that must be right or the edge of the picture is written
    // from a texel that was never fetched.
    const uint32_t extent[2] = { w, h };
    cmd->push_constants(shader_stage::compute, g_dcs.layout, 1, 0, 2, extent);

    // 8 by 8, which is what the module declares.
    cmd->dispatch((w + 7) / 8, (h + 7) / 8, 1);

    cmd->barrier(g_dcs.dst, resource_usage::unordered_access, resource_usage::copy_source);

    if (++g_dcs.frames == 1)
        Log("[synth] depth conversion: first dispatch, %ux%u, %u by %u groups.",
            w, h, (w + 7) / 8, (h + 7) / 8);
    return g_dcs.dst;
}

// The driver's optical flow engine, which on this transport runs on a private
// D3D11 device sharing this file's own D3D12 textures. Declared here and defined
// beside the rest of the optical flow code, because that is where OfaFrameOn is
// and this is the one caller that lives above it.
static bool SynthOfaVkOpen(UINT w, UINT h, DXGI_FORMAT back_fmt);
static bool SynthOfaVkFrame();
// Whether the engine, and not a ReShade effect, is this session's motion vector
// source. True once it has opened AND once it has given up, because a session
// that gave up stops delivering rather than swapping estimator.
static bool SynthOfaVkIsSource();
// Dropped and allowed to open again when the shared textures it aliases are
// replaced, which is what a resolution or format change does.
static void SynthOfaVkRebuild();

// One frame.
//
// The states the game's resources are in at reshade_begin_effects are the same
// facts the D3D12 transport reads, and they come off ReShade's own code rather
// than off an assumption: the back buffer is in render_target -- the old state
// runtime.cpp itself names for it in the implicit back buffer copy at the top of
// the first effect pass -- and depth and motion vectors are in shader_resource,
// which is where generic_depth's own reshade_begin_effects handler leaves the
// depth, and it is a built-in add-on whose callback runs before any external one.
//
// Unlike the D3D12 transport the back buffer and the motion vectors do have to
// be transitioned here, because Vulkan reads an image in a transfer only from
// TRANSFER_SRC_OPTIMAL or GENERAL and SHADER_READ_ONLY_OPTIMAL is neither.
// ReShade's own barrier is what does it, and that is the whole reason to use it:
// it is the only thing that knows each image's aspect mask, and getting that
// wrong copies the wrong plane.
//
// The DEPTH binding is the exception and no longer moves at all: it is read by a
// compute pass that samples it, and shader_resource is already the layout a
// sampled read wants. See SynthDepthCsRun.
static void SynthVkFrame(reshade::api::effect_runtime *rt, reshade::api::command_list *cmd,
                         const SynthParams *sp, reshade::api::resource back,
                         reshade::api::resource depth, reshade::api::resource mv,
                         UINT w, UINT h, DXGI_FORMAT back_fmt)
{
    using reshade::api::resource_usage;

    if (!Synth12Open(rt)) return;
    if (!SynthVkOpen(rt)) return;

    // Fetched above the rebuild rather than below it, because the rebuild is the
    // one thing here that needs the queue before it needs a command buffer.
    reshade::api::command_queue *rq = rt->get_command_queue();
    if (rq == nullptr) return;

    if (g_s12.feature == nullptr || g_s12.w != w || g_s12.h != h || g_s12.fmt != back_fmt)
    {
        Log("[synth] building the D3D12 feature and its Vulkan import for %ux%u %s",
            w, h, FormatName(back_fmt));
        // Idled first, and this is not belt and braces. The only wait this
        // transport performs sits in the middle of a frame -- everything after
        // it, the copy out of the output image included, is recorded and
        // submitted with nothing waiting on it -- so at the top of the next
        // frame last frame's command buffer may still be executing and still
        // naming these images. Destroying a VkImage a submitted command buffer
        // names is undefined, and a resolution change is exactly when it would
        // happen.
        rq->wait_idle();
        // The images before the textures they are backed by: releasing a D3D12
        // resource that an imported VkDeviceMemory still names is a use after
        // free neither API has any way to report.
        SynthVkReleaseImages();
        // The optical flow engine's own two textures, which are sized to the
        // shape Synth12Build is about to change, and the private device they
        // were created on. It opens again on the next frame, at the new size.
        SynthOfaVkRebuild();
        // The depth conversion's own texture and its two views. Safe here and
        // nowhere else in this function: the queue was just idled, so nothing
        // submitted still names them. The pipeline, the layout and the sampler
        // survive -- none of them depends on the size.
        SynthDepthCsRelease();
        if (!Synth12Build(sp, w, h, w, h, back_fmt)) { g_s12.ready = false; return; }
        if (!SynthVkImport()) { SynthVkReleaseImages(); g_svk.ready = false; return; }
    }

    // Built on the first frame that gets this far and never retried. A failure
    // stops the transport rather than delivering a frame with no depth in it,
    // and every branch inside has already named its own reason.
    if (!SynthDepthCsOpen(rt->get_device()))
    {
        Log("[synth] the depth conversion pass could not be built, so this transport stops. "
            "The game keeps rendering on its own.");
        g_svk.ready = false;
        return;
    }

    // The motion vector source, decided once. The driver's own optical flow
    // engine if it will open -- it creates its own two textures and needs only
    // the private D3D12 device to open them on -- and one of the three ReShade
    // providers otherwise. Once it has
    // opened, or once it has stopped, no ReShade motion-vector texture is read
    // again on this transport: changing estimator under a feature whose temporal
    // history was built from the other one is the defect this source was added
    // to remove, reintroduced.
    SynthOfaVkOpen(w, h, back_fmt);
    const bool ofa_mv = SynthOfaVkIsSource();

    // The arming gate now lets a session through on the PROMISE of the engine --
    // see the Vulkan branch in OnSynthBeginEffects -- so this frame can arrive
    // with the engine not open and no ReShade texture behind mv either. There is
    // then nothing to copy and nothing to evaluate over: mv.handle is zero, and
    // recording vkCmdCopyImage from VK_NULL_HANDLE into ReShade's own command
    // buffer is undefined behaviour in the game's queue, not a dropped frame.
    //
    // Stand down instead. The engine gets its next chance on the next frame if it
    // has not given up; if it has, g_ovk.stopped closes the gate above and this
    // session is over.
    if (!ofa_mv && mv.handle == 0)
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            Log("[synth] the driver's optical flow engine has not opened and there is no "
                "ReShade motion-vector texture either, so nothing is delivered until one "
                "of the two arrives. The log above says why the engine did not open.");
        }
        return;
    }

    cmd->barrier(back, resource_usage::render_target, resource_usage::copy_source);
    if (!ofa_mv)
        cmd->barrier(mv, resource_usage::shader_resource, resource_usage::copy_source);

    // Depth, sampled rather than copied, into a texture of this add-on's own.
    // Recorded before the raw Vulkan copies below so that everything ReShade
    // records and everything this file records stay in two contiguous runs
    // rather than interleaved -- they land in the same VkCommandBuffer either
    // way, and one order is easier to read in a capture than two.
    const reshade::api::resource dconv = SynthDepthCsRun(cmd, depth, w, h);
    if (dconv.handle == 0)
    {
        // Put the two textures back where ReShade left them before leaving, or
        // the next thing to touch them transitions from a state they are not in.
        cmd->barrier(back, resource_usage::copy_source, resource_usage::render_target);
        if (!ofa_mv)
            cmd->barrier(mv, resource_usage::copy_source, resource_usage::shader_resource);
        return;
    }

    SVkCommandBuffer cb =
        reinterpret_cast<SVkCommandBuffer>(static_cast<uintptr_t>(cmd->get_native()));

    for (int i = 0; i < SLOT_COUNT; ++i)
    {
        // Out of UNDEFINED once, and a GENERAL to GENERAL barrier on every frame
        // after that -- which is not a no-op. It is the memory dependency that
        // makes what the other API wrote visible here and the other way round,
        // and with no shared fence between the two APIs it is the only one there
        // is on this side.
        SynthVkBarrier(cb, g_svk.img[i],
                       g_svk.laid_out ? kSVkLayoutGeneral : kSVkLayoutUndefined,
                       kSVkLayoutGeneral);
    }
    g_svk.laid_out = true;

    SynthVkCopyImage(cb, static_cast<SVkHandle>(back.handle), kSVkLayoutTransferSrc,
                     g_svk.img[SLOT_COLOR], kSVkLayoutGeneral, w, h);
    // Skipped entirely when the driver's engine is the source: the evaluate then
    // reads the engine's own texture instead of this one, so copying a ReShade
    // effect's vectors in would be work nothing reads -- and on a frame the
    // engine refuses, would be the estimator swap this transport refuses to
    // make.
    if (!ofa_mv)
        SynthVkCopyImage(cb, static_cast<SVkHandle>(mv.handle), kSVkLayoutTransferSrc,
                         g_svk.img[SLOT_MV], kSVkLayoutGeneral, w, h);

    // The converted depth, which is now an ordinary R32_FLOAT colour image on
    // both sides, so it travels as an ordinary image copy. The buffer round trip
    // this replaces existed only because a DEPTH aspect cannot be copied into a
    // colour image; nothing samples an aspect any more by the time we get here.
    SynthVkCopyImage(cb, static_cast<SVkHandle>(dconv.handle), kSVkLayoutTransferSrc,
                     g_svk.img[SLOT_DEPTH], kSVkLayoutGeneral, w, h);

    // The whole of the CPU synchronisation on this side, and the reason there is
    // no fence in this transport. wait_idle flushes ReShade's immediate command
    // list and then waits the queue out, so everything above has finished on the
    // GPU before the D3D12 evaluate below reads any of it.
    rq->wait_idle();

    // The optical flow engine copies the colour that has just crossed into its
    // own texture on the D3D12 side, reads it through the private D3D11 device,
    // and writes the vectors the evaluate below hands to NGX. It has to sit here
    // and nowhere else: above the wait the colour is not there yet, and below the
    // evaluate it would be a frame late.
    //
    // A frame the engine does not deliver -- the first one, which has no
    // previous frame to compare against, or one whose execute was refused -- is
    // not delivered at all. That is the same rule the D3D11 path follows and for
    // the same reason: the alternative is handing DLSS somebody else's vectors
    // for one frame.
    const bool mv_ready = !ofa_mv || SynthOfaVkFrame();

    // The mirror sets this to its own 900 ms and nothing resets it, so state the
    // budget here rather than inheriting whichever path ran last: nothing is
    // parked behind this one, and an overrun sets g_s12.ready false for good.
    g_s12.wait_ms = 0;
    const bool ok = mv_ready && Synth12EvaluateShared(sp);

    // Read again, and this is not defensive. The flush inside wait_idle advances
    // ReShade's immediate command list to its next command buffer and begins
    // recording into that one; the handle taken before it has been closed and
    // submitted, and recording into it now would be recording into nothing.
    cb = reinterpret_cast<SVkCommandBuffer>(static_cast<uintptr_t>(cmd->get_native()));

    if (ok && g_cfg.mode == 2)
    {
        cmd->barrier(back, resource_usage::copy_source, resource_usage::copy_dest);
        SynthVkBarrier(cb, g_svk.img[SLOT_OUTPUT], kSVkLayoutGeneral, kSVkLayoutGeneral);
        SynthVkCopyImage(cb, g_svk.img[SLOT_OUTPUT], kSVkLayoutGeneral,
                         static_cast<SVkHandle>(back.handle), kSVkLayoutTransferDst, w, h);
        cmd->barrier(back, resource_usage::copy_dest, resource_usage::render_target);
    }
    else
    {
        // The back buffer goes home in the state ReShade left it in whether or
        // not anything was written into it. Only mode 2 writes back here, and
        // mode 1 reads the same as mode 0 -- not the mirror's three modes; see
        // the note on the same branch in Synth12Frame.
        cmd->barrier(back, resource_usage::copy_source, resource_usage::render_target);
    }
    // Depth is not here: it was never moved out of shader_resource. Nor are the
    // motion vectors when the driver's engine is the source, because then that
    // texture was never transitioned either.
    if (!ofa_mv)
        cmd->barrier(mv, resource_usage::copy_source, resource_usage::shader_resource);

    if (!ok) return;

    const UINT64 n = ++g_svk.frames;
    InterlockedIncrement(&g_frames_delivered);
    if (n <= 1 || (n % 1800) == 0)
        Log("[synth] Vulkan frame %llu delivered (%ux%u)", n, w, h);
}

// ---------------------------------------------------------------------------
// Arming and the latch
// ---------------------------------------------------------------------------

static bool  g_synth_delivering;
static LONG  g_synth_frames;
static bool  g_synth_armed_said;
static bool  g_synth_shape_said;


// The counters as they stood when the source latch was last released, and the
// release bookkeeping beside them.
//
// g_eval_count and g_create_count are never zeroed -- ReportIdle re-hooks the
// host executable when both read zero past sixty seconds, which is the action
// recorded as killing The Elder Scrolls Online, and g_frames_delivered is the
// one cumulative liveness signal the whole add-on has. So the arming gate is
// given a baseline instead: "has the game used DLSS SINCE we handed the frame
// back", which is the question that test was always trying to ask. Zero for the
// whole life of every session that never releases -- every D3D11 session and
// every session that never mirrored -- so the gate is byte-identical to what
// shipped until the moment a release happens.
//
// Written only by SynthReleaseLatch, on the present thread, inside g_bridge_cs.
static volatile LONG g_eval_base;
static volatile LONG g_create_base;
static volatile LONG g_release_n;      // episodes released this session

// Stage 1's latch, seen from this side. The game's own DLSS wins from any state
// at any time, so this asks only whether it has arrived and never argues about
// it. The two are never both live: this runs before anything else in the
// delivery path, and the mirror sets SRC_MIRROR in the evaluate detour before it
// reaches BridgeFrame at all.
static void SynthStandDown()
{
    if (!g_synth_delivering) return;
    g_synth_delivering = false;

    EnterCriticalSection(&g_bridge_cs);
    // The feature standing in g_bridge was created for the synthetic contract --
    // DLAA at back buffer size, over ReShade's depth and a third-party shader's
    // motion vectors. The game's first frame may well be the same shape, in
    // which case nothing in BridgeFrameInner would notice and the mirror would
    // run the game's textures through somebody else's feature. Clearing
    // frame_ready forces the rebuild; need_reset throws away a temporal history
    // accumulated from motion vectors the game did not produce.
    g_bridge.frame_ready = false;
    g_bridge.need_reset  = true;
    // The D3D12 session's undo is NOT repeated here. The Vulkan mirror's evaluate
    // detour already does it -- same two fields, same critical section --
    // immediately before it calls VkMirrorFrame, which is the only moment it is
    // correct at. This runs on ReShade's present thread, strictly after that
    // detour has released g_bridge_cs, so by the time it could zero the width
    // the mirror has already built; all a second copy achieved was to un-satisfy
    // the build guard behind the mirror's back and buy a rebuild nobody asked
    // for. need_reset is left alone here for the same reason.
    g_s12.need_reset = true;
    LeaveCriticalSection(&g_bridge_cs);

    // What this says is what happens, which is not what it used to say. The
    // synthetic feature is not discarded here: nothing in this function frees an
    // NGX feature or a texture. The release lives in Synth12Build, so it happens
    // when -- and only when -- a later mirrored frame actually reaches a rebuild.
    // Every path that refuses the mirror before then leaves the feature standing
    // for the rest of the process, and a line claiming otherwise is how a leak
    // gets looked for in the wrong place.
    // Not "for good" any more, and the word had to go rather than be softened:
    // the source latch can now be released when the game's own DLSS goes quiet,
    // so a sentence promising the synthetic contract is finished for the session
    // would be contradicted by the next line the same log prints.
    Log("[synth] the game called DLSS itself, so the synthetic contract stands down "
        "after %ld frame%s. It arms again only if the game's own DLSS goes quiet. The "
        "feature it built is released by the next mirrored frame that rebuilds, not here.",
        g_synth_frames, g_synth_frames == 1 ? "" : "s");
}

// Has the game used DLSS since the latch was last handed back? Against the
// BASELINE, not against zero -- see g_eval_base. Both latches increment their
// counter before they write SRC_MIRROR, so SRC_MIRROR strictly implies a
// non-zero count: a release that reset only g_source would be refused forever
// and deliver nothing, which is the reported bug wearing a different panel
// string.
//
// A function rather than two lines inline, so the arming gate and the check
// below read the same test. Putting it back to "!= 0" is the one edit that
// would silently restore the bug, and it now fails an assertion at attach
// instead of shipping.
static bool SynthCountsMovedSince(LONG creates, LONG evals, LONG create_base, LONG eval_base)
{
    return creates != create_base || evals != eval_base;
}

static bool SynthGameUsedDlssSinceBase()
{
    return SynthCountsMovedSince(InterlockedCompareExchange(&g_create_count, 0, 0),
                                 InterlockedCompareExchange(&g_eval_count, 0, 0),
                                 g_create_base, g_eval_base);
}

// Run once at attach. The failure mode being guarded is not a crash: it is the
// un-latch shipping while the game still delivers nothing, which looks exactly
// like working code until somebody toggles DLSS.
static void SynthLatchSelfCheck()
{
    // Shipped behaviour, baselines zero: any use of DLSS refuses arming.
    assert(!SynthCountsMovedSince(0, 0, 0, 0));
    assert( SynthCountsMovedSince(1, 0, 0, 0));
    assert( SynthCountsMovedSince(0, 1, 0, 0));
    // After a release, the counts that stood at release time must NOT refuse --
    // this is the assertion that fails if the test goes back to "!= 0".
    assert(!SynthCountsMovedSince(7, 400, 7, 400));
    // And the game coming back must refuse again.
    assert( SynthCountsMovedSince(7, 401, 7, 400));
    assert( SynthCountsMovedSince(8, 400, 7, 400));
}

// Every condition, each read off something rather than assumed, and the caller
// says which one refused.
//
// The depth readback is not among them, and that is deliberate twice over.
// SynthProbeDepth stands aside whenever ReShade hands over the game's own
// depth-stencil texture, because D3D11 copies such a resource only whole -- which
// is the common case, not an edge one, so requiring a successful readback would
// mean never arming in the configuration that works. Nor can it disprove, which
// this comment used to claim: the readback runs in OnSynthBeginEffects AFTER the
// delivery decision, so depth_read is zero every time this function is asked, and
// the clause that refused a flat buffer could never fire. It has been removed
// rather than left to read as a test that runs. A bound, right-sized, provably
// flat buffer arms; the report, which does run after the readback, is the only
// place "flat" is said.
//
// Not fixed by reordering: moving the probe above the gate would stall the GPU
// every frame, and carrying the verdict forward in a static risks refusing a good
// buffer on a stale reading -- a regression on an untested path, on the last
// release.
// ignore_mv skips the four motion-vector shape tests at the end and nothing
// else. It exists for one caller: the pre-gate that decides whether to run the
// driver's optical flow engine at all, which is asked BEFORE there is a motion
// vector texture because it is what produces one. Without it every mirror
// session in the nine shipped titles would pay 1.6 ms a frame building a flow
// field nobody reads -- and Skyrim SE spent five seconds with depth "present but
// NOT BOUND" before arming, which is 300 frames of it. Every other caller passes
// false and gets exactly the function that was here.
static const char *SynthArmRefusal(const SynthFacts *f, bool ignore_mv)
{
    if (g_cfg.synth_after <= 0)
        return "synth_after=0 in dlss5-bridge.cfg";
    if (g_cfg.source != CFG_SRC_SYNTH && g_cfg.source != CFG_SRC_AUTO)
        return "source is not synth or auto";
    if (g_cfg.stage < 3)
        return "stage is below 3, so no evaluate is run at all";

    // Not "no evaluate": a game that created a feature has DLSS and has simply
    // not drawn with it yet, and taking the field in front of it would be taken
    // back one frame later.
    if (SynthGameUsedDlssSinceBase())
        return "the game has already used DLSS itself";

    // Only for the FIRST arming. g_hook_time is restarted whenever a new NGX
    // module is hooked, and on the synthetic path this add-on loads the driver's
    // NGX loader itself -- so arming caused new modules to be hooked, which
    // restarted the clock, which un-armed it one frame later. Skyrim Special
    // Edition on 2026-08-30: feature created, frame 1 delivered, and then "the
    // configured delay has not elapsed" for the rest of the session.
    //
    // What the delay is for is deciding whether the game has DLSS of its own,
    // and that question is settled once. What ends a synthetic session is the
    // mirror latch, not a timer.
    if (InterlockedCompareExchange(&g_source, 0, 0) != SRC_SYNTH &&
        GetTickCount64() - g_hook_time < static_cast<ULONGLONG>(g_cfg.synth_after) * 1000)
        return "the configured delay has not elapsed";

    if (!f->depth_bound)     return "no depth buffer is bound";
    if (f->depth_samples != 1) return "the depth buffer is multisampled";
    // The same test the motion vectors get below, and it was missing here. NGX
    // takes depth at the render resolution, and the render resolution this
    // contract declares is the back buffer -- ReShade.fxh declares DepthBufferTex
    // at back buffer size, but the binding is not always that texture: generic
    // depth hands over the game's own depth-stencil directly whenever it has no
    // copy to give, and that one is whatever size the game renders at. Handing
    // NGX a depth surface of a size no key in the contract states is a broken
    // contract that only NGX can refuse, one evaluate at a time.
    if (f->depth_w != f->back_w || f->depth_h != f->back_h)
        return "the depth buffer is not the size of the back buffer";


    // Two transport questions rather than contract ones, and the only ones there
    // are, so they are asked here where every other refusal is said.
    //
    // WHAT USED TO BE HERE, and is not any more: a refusal for a Vulkan depth
    // buffer that was not 32-bit float, and a refusal for a Vulkan depth binding
    // that was not a copy source. Both existed because the transport carried
    // depth out through a staging buffer, which is exact for a float depth
    // aspect and for nothing else, and which no transfer may issue at all
    // against an image with no VK_IMAGE_USAGE_TRANSFER_SRC_BIT. The synthetic
    // transport now SAMPLES the depth in a compute pass instead -- see
    // SynthDepthCsRun -- and a sampled depth-aspect texel is a normalised float
    // whatever the storage format, from an image whose usage says
    // shader_resource. So the format question has no answer to refuse on, and
    // the usage question moved from copy_source to shader_resource, which is
    // what generic_depth's backup texture is created with.
    //
    // The MOTION VECTOR copy_source test stays exactly as it was: that texture
    // is still copied, not sampled.
    if (f->device_api == static_cast<uint32_t>(reshade::api::device_api::vulkan))
    {
        const uint32_t copy_src = static_cast<uint32_t>(reshade::api::resource_usage::copy_source);
        const uint32_t shader_r = static_cast<uint32_t>(reshade::api::resource_usage::shader_resource);
        if ((f->depth_usage & shader_r) == 0)
            return "the Vulkan depth binding was not created as a shader resource, so the "
                   "compute pass that reads it cannot sample it -- on Vulkan that binding "
                   "is ReShade's own depth backup texture, which generic_depth creates "
                   "shader_resource and copy_dest, so this is not the expected case";
    }

    if (!ignore_mv)
    {
        if (!f->mv_bound)        return "no motion vector texture is bound";
        // BELOW the bound test, and that is the whole point of where it sits.
        // Above it, this fired on a texture that was declared and never bound --
        // mv_usage is zero because there was no view to read it from, not because
        // the texture was created without copy_source -- and it beat the true
        // sentence one line up. A user with no motion-vector shader enabled was
        // told the creation flags of a texture that had never been created.
        //
        // mv_which 0 is the driver's own optical flow engine -- kSynthMv[0], the
        // row with no ReShade texture behind it -- whose vectors never travel
        // through a Vulkan transfer: they are written into the shared D3D12
        // texture from the other side.
        //
        // Both the device_api test and the constant are restated here rather than
        // reused: copy_src above is scoped to the block this moved out of, and
        // dropping the Vulkan guard would refuse every D3D11 session, where
        // SynthBound::usage is deliberately left zero. That is the nine shipped
        // titles.
        if (f->device_api == static_cast<uint32_t>(reshade::api::device_api::vulkan) &&
            f->mv_which != 0 &&
            (f->mv_usage &
             static_cast<uint32_t>(reshade::api::resource_usage::copy_source)) == 0)
            return "the Vulkan motion vector texture was not created as a copy source, so "
                   "no transfer may read it";
        if (f->mv_samples != 1)  return "the motion vectors are multisampled";
        if (f->mv_fmt != DXGI_FORMAT_R16G16_FLOAT) return "the motion vectors are not R16G16_FLOAT";
        if (f->mv_w != f->back_w || f->mv_h != f->back_h)
            return "the motion vectors are not the size of the back buffer";
    }

    if (f->def_value[SYNTH_DEF_UPSIDE_DOWN])
        return "the depth buffer is upside down and NGX has no way to be told";
    if (f->def_value[SYNTH_DEF_LOGARITHMIC])
        return "the depth encoding is logarithmic and NGX has no way to be told";

    return nullptr;
}

// ---------------------------------------------------------------------------
// The driver's own optical flow engine
//
// Why this exists: Skyrim Special Edition, 2026-08-30. The synthetic contract
// was built, the feature was created, frames were delivered, and fine detail
// dissolved along the direction of movement -- worse slowly than quickly. Five
// observations settled the cause by experiment: all four mv_sign combinations
// smeared, so not the sign; mode=1 was clean, so not the transport;
// reset_every=1 was clean, so it is the temporal accumulation; worse slowly, so
// the sub-pixel regime; the trail ran ALONG the movement, so under-estimated
// rather than reversed. vort_Shaders estimates at mip 1, its finest pass is
// PS_Motion1, there is no PS_Motion0, and below one pixel it reports ~0.
//
// NVOFA never reports zero. Measured on this machine at 1920x1080, MEDIUM,
// "exactly zero: 0.00%" at every step above 0.10 px, and at a true 0.10 px it
// answers 0.156 at grid 2. That is the whole reason to build this.
//
// D3D11 ONLY. The function list below was recovered empirically for
// NvOFAPICreateInstanceD3D11 and for nothing else: the D3D12 and Vulkan entry
// points are different exports with different, unrecovered slot tables and
// different accepted API versions. On those two runtimes the kSynthMv[1..]
// ReShade providers are the only ones, which is what they already were.
//
// NOT MEASURED and not claimed anywhere below: rotation and zoom against real
// game content, transparency, per-object motion, and temporal hints -- every
// number quoted here was taken with disableTemporalHints = 1 over rigid warps of
// a synthetic pattern.
// ---------------------------------------------------------------------------

typedef long NvOfStatus;
static const NvOfStatus kOfaSuccess = 0;      // NV_OF_SUCCESS
static const NvOfStatus kOfaRaised  = 0x7FFFFFFF;   // our own marker, as NGX_EXCEPTION_MARKER is

// Plain C structs from a published ABI, hand-written for the same reason the
// Vulkan structs next door are: there is no lib to link and no header worth
// vendoring for five exports. This layout is additionally confirmed by
// experiment on this machine -- breaking one field made the driver name that
// exact field back through nvOFGetLastError.
struct NvOfInitParams
{
    uint32_t width;               //  0
    uint32_t height;              //  4
    uint32_t outGridSize;         //  8   1, 2 or 4
    uint32_t hintGridSize;        // 12   same value; unread with hints disabled
    uint32_t mode;                // 16   1 = NV_OF_MODE_OPTICALFLOW
    uint32_t perfLevel;           // 20   5 SLOW, 10 MEDIUM, 20 FAST
    uint32_t enableExternalHints; // 24
    uint32_t enableOutputCost;    // 28
    void    *hPrivData;           // 32
    uint32_t disparityRange;      // 40
    uint32_t enableRoi;           // 44
};

struct NvOfExecuteInputParams
{
    void    *inputFrame;           //  0  the CURRENT frame
    void    *referenceFrame;       //  8  the PREVIOUS frame
    void    *externalHints;        // 16
    uint32_t disableTemporalHints; // 24
    uint32_t padding;              // 28
    void    *hPrivData;            // 32
    uint32_t padding2;             // 40
    uint32_t numRois;              // 44
    void    *roiData;              // 48
};

struct NvOfExecuteOutputParams { void *outputBuffer; void *outputCostBuffer; void *hPrivData; };

// The same discipline as the PanelImGui asserts: a miscounted pad is a compile
// error here rather than a call through the wrong offset at runtime.
static_assert(sizeof(NvOfInitParams) == 48 && offsetof(NvOfInitParams, hPrivData) == 32,
              "NvOfInitParams does not match the layout measured against the driver");
static_assert(sizeof(NvOfExecuteInputParams) == 56 &&
              offsetof(NvOfExecuteInputParams, numRois) == 44,
              "NvOfExecuteInputParams does not match the layout measured against the driver");

typedef NvOfStatus (__stdcall *PFN_OfaCreateInstance)(uint32_t, void *);
typedef NvOfStatus (__stdcall *PFN_OfaCreateD3D11)(ID3D11Device *, ID3D11DeviceContext *, void **);
typedef NvOfStatus (__stdcall *PFN_OfaInit)(void *, const NvOfInitParams *);
typedef NvOfStatus (__stdcall *PFN_OfaRegister)(void *, ID3D11Resource *, void **);
typedef NvOfStatus (__stdcall *PFN_OfaUnregister)(void *);
typedef NvOfStatus (__stdcall *PFN_OfaExecute)(void *, const NvOfExecuteInputParams *,
                                               NvOfExecuteOutputParams *);
typedef NvOfStatus (__stdcall *PFN_OfaDestroy)(void *);
typedef NvOfStatus (__stdcall *PFN_OfaLastError)(void *, char *, uint32_t *);

static const char *OfaStatusName(NvOfStatus s)
{
    switch (s)
    {
    case 0:  return "SUCCESS";
    case 1:  return "INVALID_PTR";
    case 2:  return "INVALID_PARAM";
    case 3:  return "INVALID_CALL";
    case 4:  return "INVALID_VERSION";
    case 5:  return "OUT_OF_MEMORY";
    case 6:  return "NOT_INITIALIZED";
    case 7:  return "UNSUPPORTED_FEATURE";
    case 8:  return "GENERIC";
    case 9:  return "OF_NOT_AVAILABLE";
    case 10: return "UNSUPPORTED_DEVICE";
    case 11: return "DEVICE_DOES_NOT_EXIST";
    // A width or height the engine will not take comes back as 0x80004005,
    // which is E_FAIL and not an NV_OF_STATUS at all. Every test below is
    // != kOfaSuccess for exactly that reason, and this names the case rather
    // than reporting it as an enum value nobody can look up.
    case static_cast<NvOfStatus>(0x80004005): return "E_FAIL, not an NV_OF_STATUS";
    default: return "a status this build has no name for";
    }
}

// 5, 10 and 20 are NVOFA's own values; anything else was refused by the config
// validator, so the default arm can only be reached if that check is changed.
static const char *SynthPerfName(int p)
{
    return p == 5 ? "SLOW" : p == 10 ? "MEDIUM" : p == 20 ? "FAST" : "?";
}

struct SynthOfa
{
    bool    tried;      // the first open was attempted; a failed first open is never retried
    bool    ready;      // dll, instance, session, textures, shaders all in hand
    bool    stopped;    // gave up after being ready; no ReShade provider is substituted
    bool    primed;     // src[prev] holds a real frame, so an execute is possible
    bool    first_said;
    HMODULE lib;
    void   *slot[64];
    void   *session;
    UINT    w, h, grid;
    int     perf;       // the NV_OF_PERF_LEVEL this session was opened with
    DXGI_FORMAT back_fmt;               // the shape the session was opened for
    // The device every texture below was created on, compared rather than
    // assumed. A game that re-creates its D3D11 device -- ReShade answers that
    // with a fresh effect runtime and this add-on keeps running -- would
    // otherwise leave a CopyResource between two devices, which is undefined
    // rather than an error. Held as a bare pointer and never dereferenced: the
    // textures below hold their own references, so the address cannot be reused
    // by a second device while this session exists, and the comparison is exact.
    ID3D11Device *dev;
    void   *reg_src[2], *reg_flow;      // registration handles
    ID3D11Texture2D *copy, *src[2], *flow, *mv;
    ID3D11ShaderResourceView  *copy_srv, *flow_srv;
    ID3D11UnorderedAccessView *src_uav[2], *mv_uav;
    ID3D11ComputeShader *luma_cs, *flow_cs;
    int    cur;         // which of src[] this frame writes
    int    fails;       // consecutive execute failures
    bool   checked;     // the self-check has run for this shape
    int    quiet;       // frames seen with no motion anywhere, for the self-check
    UINT64 frames;
};

static SynthOfa g_ofa;

// Each driver entry point gets its own guarded wrapper, for the reason the NGX
// wrappers in dlss5-bridge.cpp were given theirs: these are undocumented
// exports reached through signatures recovered from disassembly, and a wrong
// guess has to land in the log rather than take the game down. Each is its own
// function with no C++ objects, so __try is legal and no unwinding is needed.
static NvOfStatus OfaSafeCreateInstance(PFN_OfaCreateInstance fn, uint32_t ver, void *slots)
{
    __try { return fn(ver, slots); }
    __except (CaptureFault(GetExceptionInformation())) { return kOfaRaised; }
}

static NvOfStatus OfaSafeCreateSession(PFN_OfaCreateD3D11 fn, ID3D11Device *d,
                                       ID3D11DeviceContext *c, void **out)
{
    __try { return fn(d, c, out); }
    __except (CaptureFault(GetExceptionInformation())) { return kOfaRaised; }
}

static NvOfStatus OfaSafeInit(PFN_OfaInit fn, void *s, const NvOfInitParams *p)
{
    __try { return fn(s, p); }
    __except (CaptureFault(GetExceptionInformation())) { return kOfaRaised; }
}

static NvOfStatus OfaSafeRegister(PFN_OfaRegister fn, void *s, ID3D11Resource *r, void **out)
{
    __try { return fn(s, r, out); }
    __except (CaptureFault(GetExceptionInformation())) { return kOfaRaised; }
}

static NvOfStatus OfaSafeUnregister(PFN_OfaUnregister fn, void *h)
{
    __try { return fn(h); }
    __except (CaptureFault(GetExceptionInformation())) { return kOfaRaised; }
}

static NvOfStatus OfaSafeDestroy(PFN_OfaDestroy fn, void *s)
{
    __try { return fn(s); }
    __except (CaptureFault(GetExceptionInformation())) { return kOfaRaised; }
}

static NvOfStatus OfaSafeExecute(PFN_OfaExecute fn, void *s, const NvOfExecuteInputParams *in,
                                 NvOfExecuteOutputParams *out)
{
    __try { return fn(s, in, out); }
    __except (CaptureFault(GetExceptionInformation())) { return kOfaRaised; }
}

// The driver's own account of what it refused, which is what makes an nvOFInit
// failure actionable: it names the offending field. Empty when the call has
// nothing to say or is not safe to make.
static void OfaLastError(char *dst, size_t cap)
{
    dst[0] = '\0';
    if (g_ofa.session == nullptr || g_ofa.slot[8] == nullptr) return;
    uint32_t n = static_cast<uint32_t>(cap);
    auto fn = reinterpret_cast<PFN_OfaLastError>(g_ofa.slot[8]);
    __try { fn(g_ofa.session, dst, &n); }
    __except (CaptureFault(GetExceptionInformation())) { dst[0] = '\0'; }
    // Nothing in the recovered signature promises a terminator, and every caller
    // hands the result straight to a %s.
    dst[cap - 1] = '\0';
}

// Unregister before release, on every path including a half-built open: the
// driver holds these handles and releasing the texture under one is a use after
// free inside the display driver rather than in this process.
//
// lib, slot and the two compiled shaders are deliberately left alone. They are
// shape-independent, and recompiling two compute shaders on a resolution change
// is a D3DCompile for nothing.
static void OfaClose()
{
    auto unreg = reinterpret_cast<PFN_OfaUnregister>(g_ofa.slot[5]);
    if (unreg != nullptr)
    {
        for (int i = 0; i < 2; ++i)
            if (g_ofa.reg_src[i] != nullptr) OfaSafeUnregister(unreg, g_ofa.reg_src[i]);
        if (g_ofa.reg_flow != nullptr) OfaSafeUnregister(unreg, g_ofa.reg_flow);
    }
    g_ofa.reg_src[0] = g_ofa.reg_src[1] = g_ofa.reg_flow = nullptr;

    if (g_ofa.copy_srv != nullptr) { g_ofa.copy_srv->Release(); g_ofa.copy_srv = nullptr; }
    if (g_ofa.flow_srv != nullptr) { g_ofa.flow_srv->Release(); g_ofa.flow_srv = nullptr; }
    if (g_ofa.mv_uav   != nullptr) { g_ofa.mv_uav->Release();   g_ofa.mv_uav   = nullptr; }
    for (int i = 0; i < 2; ++i)
        if (g_ofa.src_uav[i] != nullptr) { g_ofa.src_uav[i]->Release(); g_ofa.src_uav[i] = nullptr; }

    if (g_ofa.copy != nullptr) { g_ofa.copy->Release(); g_ofa.copy = nullptr; }
    for (int i = 0; i < 2; ++i)
        if (g_ofa.src[i] != nullptr) { g_ofa.src[i]->Release(); g_ofa.src[i] = nullptr; }
    if (g_ofa.flow != nullptr) { g_ofa.flow->Release(); g_ofa.flow = nullptr; }
    if (g_ofa.mv   != nullptr) { g_ofa.mv->Release();   g_ofa.mv   = nullptr; }

    if (g_ofa.session != nullptr)
    {
        auto destroy = reinterpret_cast<PFN_OfaDestroy>(g_ofa.slot[7]);
        if (destroy != nullptr) OfaSafeDestroy(destroy, g_ofa.session);
        g_ofa.session = nullptr;
    }

    g_ofa.ready   = false;
    g_ofa.primed  = false;
    g_ofa.checked = false;
    g_ofa.fails   = 0;
    g_ofa.quiet   = 0;
    g_ofa.cur     = 0;
    g_ofa.dev     = nullptr;
}

// There is no teardown at DLL_PROCESS_DETACH, and that is deliberate rather than
// an oversight: g_s12 and g_svk have none either, for the same reasons. Releasing
// GPU objects from DllMain under the loader lock is a hazard, most games never
// reach detach at all, and the process is exiting. OfaClose on a shape change is
// the only teardown that exists.

static bool OfaMakeShader(ID3D11Device *dev11, const char *src, const char *name,
                          ID3D11ComputeShader **out)
{
    if (*out != nullptr) return true;

    PFN_D3DCompile_ compile = LoadSystemD3DCompiler();
    if (compile == nullptr) { Log("[synth] optical flow: d3dcompiler_47.dll is unavailable, so the flow "
                                     "conversion shader cannot be built"); return false; }

    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = compile(src, strlen(src), name, nullptr, nullptr, "main", "cs_5_0", 0, 0,
                         &code, &err);
    if (FAILED(hr) || code == nullptr)
    {
        Log("[synth] optical flow shader compile failed 0x%08X: %s", hr,
            err != nullptr ? static_cast<const char *>(err->GetBufferPointer()) : "");
        if (err != nullptr) err->Release();
        return false;
    }
    if (err != nullptr) err->Release();

    hr = dev11->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, out);
    code->Release();
    if (FAILED(hr))
    { Log("[synth] optical flow CreateComputeShader failed 0x%08X", hr); return false; }
    return true;
}

// Grey rather than colour: the estimator matches on structure and a single
// channel is what it reduces to anyway. Reinhard rather than saturate, because
// an R16G16B16A16_FLOAT back buffer clips every highlight to white under
// saturate and the estimator then has nothing to lock onto in exactly the bright
// regions that carry the most detail. Reinhard is monotone, so nothing it
// matches on is reordered.
static const char kOfaLumaSrc[] =
    "Texture2D<float4>   src : register(t0);\n"
    "RWTexture2D<float4> dst : register(u0);\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 id : SV_DispatchThreadID)\n"
    "{\n"
    "    uint w, h; dst.GetDimensions(w, h);\n"
    "    if (id.x >= w || id.y >= h) return;\n"
    "    float3 c = src[id.xy].rgb;\n"
    "    float  l = dot(c, float3(0.2126, 0.7152, 0.0722));\n"
    "    l = l / (l + 1.0);\n"
    "    dst[id.xy] = float4(l, l, l, 1.0);\n"
    "}\n";

// Three things here are load-bearing and each one is a measurement.
//
//  1/32   the output is S10.5 fixed point, so the stored value is pixels x 32.
//  /(w,h) the contract stores UV and MV.Scale multiplies by (W,H) to get back to
//         pixels, so the round trip is exact.
//  no sign flip: with inputFrame = this frame and referenceFrame = the previous
//         one, the vector already points from the current pixel back to where
//         that content was, which is verbatim the DLSS definition. Measured over
//         1,166,400 vectors, 100% within 0.5 px, both axes.
//
// The reconstruction filter is NEAREST, not bilinear, and that is the one place
// this departs from what the transport note first sketched. The disocclusion
// band is 12-16 px wide in pixels at every grid, and the values inside it
// correspond to no surface in the scene. Bilinear across that band manufactures
// more such values and spreads it wider; nearest quantises but never invents a
// vector no cell measured. Where the field is smooth it varies far more slowly
// than one grid cell and both filters land inside the 0.1 px noise floor.
//
// id.x * fw / w is the exact integer divide whenever fw*grid == w, which is
// every resolution in practice; the min keeps the sample inside the texture at
// the ones where it is not.
static const char kOfaFlowSrc[] =
    "Texture2D<int2>     flow : register(t0);\n"
    "RWTexture2D<float2> dst  : register(u0);\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 id : SV_DispatchThreadID)\n"
    "{\n"
    "    uint w, h, fw, fh;\n"
    "    dst.GetDimensions(w, h);\n"
    "    flow.GetDimensions(fw, fh);\n"
    "    if (id.x >= w || id.y >= h) return;\n"
    "    uint2 c = min(uint2(id.x * fw / w, id.y * fh / h), uint2(fw - 1, fh - 1));\n"
    "    dst[id.xy] = float2(flow[c]) * (1.0 / 32.0) / float2(w, h);\n"
    "}\n";

static ID3D11Texture2D *OfaMakeTexture(ID3D11Device *dev11, UINT w, UINT h, DXGI_FORMAT fmt,
                                       UINT bind, const char *what)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = fmt; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = bind;

    ID3D11Texture2D *t = nullptr;
    const HRESULT hr = dev11->CreateTexture2D(&td, nullptr, &t);
    if (FAILED(hr))
    {
        Log("[synth] optical flow: could not create the %ux%u %s texture for %s: 0x%08X",
            w, h, FormatName(fmt), what, hr);
        return nullptr;
    }
    return t;
}
// The private D3D11 device the Vulkan transport runs the engine on, or null when
// there is none. Declared here because OfaOpen has to name which device it opened
// on and g_ovk is defined far below it.
static ID3D11Device *SynthOfaVkDevice();


static bool OfaOpen(ID3D11Device *dev11, ID3D11DeviceContext *ctx, UINT w, UINT h,
                    DXGI_FORMAT back_fmt)
{
    // Before anything that can fail, exactly as Synth12Open and SynthVkOpen set
    // theirs: a first open that failed is never retried.
    g_ofa.tried = true;
    g_ofa.w = w; g_ofa.h = h; g_ofa.back_fmt = back_fmt; g_ofa.dev = dev11;
    g_ofa.grid = static_cast<UINT>(g_cfg.ofa_grid);

    if (g_ofa.lib == nullptr)
    {
        g_ofa.lib = LoadLibraryW(L"nvofapi64.dll");
        if (g_ofa.lib == nullptr)
        {
            Log("[synth] optical flow: nvofapi64.dll is not loadable in this process. It is "
                "installed with the display driver, so this means no NVIDIA driver here "
                "rather than a driver too old.");
            return false;
        }
        auto create = reinterpret_cast<PFN_OfaCreateInstance>(
            GetProcAddress(g_ofa.lib, "NvOFAPICreateInstanceD3D11"));
        if (create == nullptr)
        {
            Log("[synth] optical flow: nvofapi64.dll exports no NvOFAPICreateInstanceD3D11.");
            return false;
        }
        // 0x20 is the public 2.0 layout, which is the one the structs above were
        // written to and confirmed against on this machine. The D3D11 entry was
        // measured to accept 0x10 through 0x50.
        const NvOfStatus r = OfaSafeCreateInstance(create, 0x20, g_ofa.slot);
        if (r != kOfaSuccess)
        {
            Log("[synth] optical flow: this driver refused API version 0x20 with status %ld "
                "(%s). The D3D11 entry accepts 0x10 through 0x50 on the driver this was "
                "measured against.", r, OfaStatusName(r));
            memset(g_ofa.slot, 0, sizeof(g_ofa.slot));
            return false;
        }
    }

    // Asked once, before a texture exists. BGRA8 typed UAV store is not in
    // D3D11's guaranteed set, and without it the conversion writes nothing and
    // the engine is handed a blank frame -- which looks like bad flow rather
    // than like a missing feature.
    {
        D3D11_FEATURE_DATA_FORMAT_SUPPORT2 f2 = {};
        f2.InFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        if (FAILED(dev11->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &f2, sizeof(f2))) ||
            (f2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0)
        {
            Log("[synth] optical flow: this device cannot store to a B8G8R8A8_UNORM "
                "unordered access view, so the colour frame cannot be converted into the "
                "only 4-channel format the flow engine accepts.");
            return false;
        }
    }

    if (!OfaMakeShader(dev11, kOfaLumaSrc, "ofaluma", &g_ofa.luma_cs)) return false;
    if (!OfaMakeShader(dev11, kOfaFlowSrc, "ofaflow", &g_ofa.flow_cs)) return false;

    const UINT fw = (w + g_ofa.grid - 1) / g_ofa.grid;
    const UINT fh = (h + g_ofa.grid - 1) / g_ofa.grid;

    // The back buffer is not guaranteed to carry BIND_SHADER_RESOURCE -- a DXGI
    // swapchain only promises a render target view -- so a compute shader cannot
    // read it directly. CopyResource into a texture we created with the same
    // format always works.
    g_ofa.copy = OfaMakeTexture(dev11, w, h, TypedFormat(back_fmt),
                                D3D11_BIND_SHADER_RESOURCE, "the colour copy");
    // BGRA8 because the engine's legal input list is BGRA8, NV12 and R8_UNORM,
    // and Skyrim SE presents R8G8B8A8_UNORM. SHADER_RESOURCE alongside the UAV
    // because that is the binding the registration was proven with in the
    // harness, and it costs nothing.
    for (int i = 0; i < 2; ++i)
        g_ofa.src[i] = OfaMakeTexture(dev11, w, h, DXGI_FORMAT_B8G8R8A8_UNORM,
                                      D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE,
                                      i == 0 ? "the current frame" : "the previous frame");
    g_ofa.flow = OfaMakeTexture(dev11, fw, fh, DXGI_FORMAT_R16G16_SINT,
                                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                "the flow field");
    // Exactly the shape SynthArmRefusal already demands of a motion vector
    // texture, so the gate needs no new rule. That is the whole point of
    // converting here rather than handing NGX the engine's own output.
    g_ofa.mv = OfaMakeTexture(dev11, w, h, DXGI_FORMAT_R16G16_FLOAT,
                              D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE,
                              "the motion vectors");
    if (g_ofa.copy == nullptr || g_ofa.src[0] == nullptr || g_ofa.src[1] == nullptr ||
        g_ofa.flow == nullptr || g_ofa.mv == nullptr)
    { OfaClose(); return false; }

    HRESULT hr = dev11->CreateShaderResourceView(g_ofa.copy, nullptr, &g_ofa.copy_srv);
    if (SUCCEEDED(hr)) hr = dev11->CreateShaderResourceView(g_ofa.flow, nullptr, &g_ofa.flow_srv);
    for (int i = 0; i < 2 && SUCCEEDED(hr); ++i)
        hr = dev11->CreateUnorderedAccessView(g_ofa.src[i], nullptr, &g_ofa.src_uav[i]);
    if (SUCCEEDED(hr)) hr = dev11->CreateUnorderedAccessView(g_ofa.mv, nullptr, &g_ofa.mv_uav);
    if (FAILED(hr))
    {
        Log("[synth] optical flow: a view over one of the four textures could not be "
            "created: 0x%08X", hr);
        OfaClose();
        return false;
    }

    LogVideoMemory("before opening the optical flow session");

    auto create_ses = reinterpret_cast<PFN_OfaCreateD3D11>(g_ofa.slot[0]);
    NvOfStatus r = OfaSafeCreateSession(create_ses, dev11, ctx, &g_ofa.session);
    if (r != kOfaSuccess || g_ofa.session == nullptr)
    {
        // The real case this covers: nvofapi64.dll is present because an NVIDIA
        // GPU is installed, while the game renders on the iGPU.
        Log("[synth] optical flow: the driver will not open a session on this D3D11 device: "
            "status %ld (%s). This is what a game rendering on an adapter other than the "
            "NVIDIA one looks like, which happens on a hybrid laptop.", r, OfaStatusName(r));
        g_ofa.session = nullptr;
        OfaClose();
        return false;
    }

    NvOfInitParams ip;
    memset(&ip, 0, sizeof(ip));
    ip.width = w; ip.height = h;
    ip.outGridSize = g_ofa.grid;
    ip.hintGridSize = g_ofa.grid;      // unread with hints disabled; set to match
    ip.mode = 1;                       // NV_OF_MODE_OPTICALFLOW
    // From the config, validated there to 5, 10 or 20 -- NVOFA's own
    // NV_OF_PERF_LEVEL values, so a number quoted in a report reads against
    // NVIDIA's own table. FAST by default because it is the only level that
    // fits a frame at 4K: measured at 3840x1600 grid 2, FAST costs 1.6 ms
    // where MEDIUM costs 6.2 and SLOW 11.5. The slower levels estimate more
    // accurately, which is the knob to reach for when fine detail drifts.
    ip.perfLevel = static_cast<uint32_t>(g_cfg.ofa_perf);
    g_ofa.perf   = g_cfg.ofa_perf;
    auto init = reinterpret_cast<PFN_OfaInit>(g_ofa.slot[1]);
    r = OfaSafeInit(init, g_ofa.session, &ip);
    if (r != kOfaSuccess)
    {
        // Tested against SUCCESS rather than switched on the enum, because a
        // width or height error comes back as E_FAIL, which is not one.
        char msg[512];
        OfaLastError(msg, sizeof(msg));
        Log("[synth] optical flow: nvOFInit refused %ux%u at grid %u with 0x%08lX (%s). %s",
            w, h, g_ofa.grid, static_cast<unsigned long>(r), OfaStatusName(r), msg);
        if (r == 5)   // NV_OF_ERR_OUT_OF_MEMORY
            Log("[synth]   Grid 2 needs about 109 MB driver-side at 3840x1600; grid 4 needs "
                "about 63 MB. Try ofa_grid=4 in dlss5-bridge.cfg.");
        OfaClose();
        return false;
    }

    auto reg = reinterpret_cast<PFN_OfaRegister>(g_ofa.slot[4]);
    NvOfStatus r0 = OfaSafeRegister(reg, g_ofa.session, g_ofa.src[0],  &g_ofa.reg_src[0]);
    NvOfStatus r1 = OfaSafeRegister(reg, g_ofa.session, g_ofa.src[1],  &g_ofa.reg_src[1]);
    NvOfStatus r2 = OfaSafeRegister(reg, g_ofa.session, g_ofa.flow,    &g_ofa.reg_flow);
    if (r0 != kOfaSuccess || r1 != kOfaSuccess || r2 != kOfaSuccess ||
        g_ofa.reg_src[0] == nullptr || g_ofa.reg_src[1] == nullptr || g_ofa.reg_flow == nullptr)
    {
        char msg[512];
        OfaLastError(msg, sizeof(msg));
        Log("[synth] optical flow: registering the three buffers was refused (%ld, %ld, %ld). %s",
            r0, r1, r2, msg);
        OfaClose();
        return false;
    }

    g_ofa.ready   = true;
    g_ofa.stopped = false;

    // Which device, said rather than assumed. This named "the game's own D3D11
    // device" unconditionally, and on the Vulkan transport that is false: the
    // game has no D3D11 device there and the session opens on a private one this
    // add-on created. The two are told apart by the pointer, because g_ovk.dev is
    // non-null only when this file made it.
    Log("[synth] optical flow: nvofapi64.dll loaded, instance at API 0x20, session open on "
        "%s. Grid %u, perf %s, flow %ux%u R16G16_SINT -> %ux%u "
        "R16G16_FLOAT. This is kSynthMv[0] and the three ReShade motion-vector names are "
        "not consulted while it is running.",
        (SynthOfaVkDevice() != nullptr && SynthOfaVkDevice() == dev11)
            ? "a private D3D11 device this add-on created, because the game is on Vulkan"
            : "the game's own D3D11 device",
        g_ofa.grid, SynthPerfName(g_ofa.perf), fw, fh, w, h);
    Log("[synth] optical flow conversion shaders ready (cs_5_0)");
    LogVideoMemory("after opening the optical flow session");
    return true;
}

// One compute dispatch, with the game's own compute bindings saved and put back
// exactly as BridgeBlitDepth does it. This runs on the immediate context in the
// middle of the game's frame. Two separate calls rather than one block spanning
// both dispatches, because nvOFExecute runs between them on the same context.
static void OfaDispatch(ID3D11DeviceContext *ctx, ID3D11ComputeShader *cs,
                        ID3D11ShaderResourceView *srv, ID3D11UnorderedAccessView *uav,
                        UINT w, UINT h)
{
    ID3D11ComputeShader       *old_cs  = nullptr;
    ID3D11ShaderResourceView  *old_srv = nullptr;
    ID3D11UnorderedAccessView *old_uav = nullptr;
    ctx->CSGetShader(&old_cs, nullptr, nullptr);
    ctx->CSGetShaderResources(0, 1, &old_srv);
    ctx->CSGetUnorderedAccessViews(0, 1, &old_uav);

    UINT keep = static_cast<UINT>(-1);
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, &keep);
    ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

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
}

// The one runnable check this stage leaves behind, and it ships enabled. It
// checks the arithmetic rather than the plumbing -- the plumbing announces its
// own failures above -- by recomputing what the conversion shader should have
// written from the flow texture the DRIVER actually filled, not from anything
// this add-on assumed.
//
// It runs once per shape, and only on a frame whose flow field contains a
// non-zero vector: a frame with no motion proves nothing, because 0 == 0 passes
// a shader that has the scale wrong, the grid ratio wrong, or is not running at
// all.
static void OfaSelfCheck(ID3D11Device *dev11, ID3D11DeviceContext *ctx)
{
    // The first attempt is taken immediately, because the frame right after
    // priming is the one most likely to have motion in it. Every attempt after
    // that waits for a multiple of sixteen: each one stages and maps about 30 MB
    // and blocks until the GPU has caught up, and a game that opens on a static
    // menu would otherwise take that stall on a hundred frames in a row with
    // nothing in the log to attribute the hitch to.
    if (g_ofa.quiet > 0 && (g_ofa.frames & 15) != 0) return;

    // Three assertions off GetDesc alone, which cost nothing and catch a
    // mis-sized texture before a single texel is read.
    D3D11_TEXTURE2D_DESC fd = {}, md = {};
    g_ofa.flow->GetDesc(&fd);
    g_ofa.mv->GetDesc(&md);
    const UINT want_fw = (g_ofa.w + g_ofa.grid - 1) / g_ofa.grid;
    const UINT want_fh = (g_ofa.h + g_ofa.grid - 1) / g_ofa.grid;
    if (fd.Width != want_fw || fd.Height != want_fh || fd.Format != DXGI_FORMAT_R16G16_SINT ||
        md.Width != g_ofa.w || md.Height != g_ofa.h || md.Format != DXGI_FORMAT_R16G16_FLOAT ||
        g_ofa.flow == reinterpret_cast<ID3D11Texture2D *>(g_ofa.mv))
    {
        g_ofa.checked = true;
        Log("[synth] DEFECT: the optical flow textures are not the shape this build claims. "
            "Flow is %ux%u fmt %u, expected %ux%u R16G16_SINT; motion vectors are %ux%u "
            "fmt %u, expected %ux%u R16G16_FLOAT.",
            fd.Width, fd.Height, fd.Format, want_fw, want_fh,
            md.Width, md.Height, md.Format, g_ofa.w, g_ofa.h);
        return;
    }

    D3D11_TEXTURE2D_DESC sd = fd;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D *fs = nullptr, *ms = nullptr;
    if (FAILED(dev11->CreateTexture2D(&sd, nullptr, &fs))) return;
    sd = md;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    if (FAILED(dev11->CreateTexture2D(&sd, nullptr, &ms))) { fs->Release(); return; }

    ctx->CopyResource(fs, g_ofa.flow);
    ctx->CopyResource(ms, g_ofa.mv);

    D3D11_MAPPED_SUBRESOURCE fm = {}, mm = {};
    if (FAILED(ctx->Map(fs, 0, D3D11_MAP_READ, 0, &fm)) ||
        FAILED(ctx->Map(ms, 0, D3D11_MAP_READ, 0, &mm)))
    { fs->Release(); ms->Release(); return; }

    int   sampled = 0, bad = 0, nonzero = 0;
    float worst = 0.0f;
    int   bx = 0, by = 0, bfx = 0, bfy = 0;
    float bex = 0.0f, bey = 0.0f, bax = 0.0f, bay = 0.0f;

    for (UINT cy = 0; cy < fd.Height; cy += 16)
    {
        const int16_t *frow = reinterpret_cast<const int16_t *>(
            static_cast<const unsigned char *>(fm.pData) + static_cast<size_t>(cy) * fm.RowPitch);
        const UINT py = cy * g_ofa.grid + g_ofa.grid / 2;
        if (py >= md.Height) continue;
        const unsigned short *mrow = reinterpret_cast<const unsigned short *>(
            static_cast<const unsigned char *>(mm.pData) + static_cast<size_t>(py) * mm.RowPitch);

        for (UINT cx = 0; cx < fd.Width; cx += 16)
        {
            const UINT px = cx * g_ofa.grid + g_ofa.grid / 2;
            if (px >= md.Width) continue;

            const int fx = frow[cx * 2], fy = frow[cx * 2 + 1];
            if (fx != 0 || fy != 0) ++nonzero;

            // expected: what the shader should have written, computed from the
            // flow texture the driver actually filled.
            const float ex = static_cast<float>(fx) * (1.0f / 32.0f) / static_cast<float>(g_ofa.w);
            const float ey = static_cast<float>(fy) * (1.0f / 32.0f) / static_cast<float>(g_ofa.h);
            const float ax = DirectX::PackedVector::XMConvertHalfToFloat(mrow[px * 2]);
            const float ay = DirectX::PackedVector::XMConvertHalfToFloat(mrow[px * 2 + 1]);

            // Compared in PIXELS, which is the unit a reader of the log can act
            // on. 0.02 px is generous against half-float rounding -- about 5e-4
            // px at these magnitudes -- and still catches every way this can be
            // wrong: a missing /32 errs by 32x, a wrong grid ratio picks another
            // cell's vector entirely, a Y flip errs by twice the value, and a
            // shader that never ran leaves zeros.
            const float dx = fabsf(ax - ex) * static_cast<float>(g_ofa.w);
            const float dy = fabsf(ay - ey) * static_cast<float>(g_ofa.h);
            const float d  = dx > dy ? dx : dy;
            ++sampled;
            if (d > worst)
            {
                worst = d;
                bx = static_cast<int>(cx); by = static_cast<int>(cy);
                bfx = fx; bfy = fy;
                bex = ex * static_cast<float>(g_ofa.w); bey = ey * static_cast<float>(g_ofa.h);
                bax = ax * static_cast<float>(g_ofa.w); bay = ay * static_cast<float>(g_ofa.h);
            }
            if (d > 0.02f) ++bad;
        }
    }

    ctx->Unmap(fs, 0);
    ctx->Unmap(ms, 0);
    fs->Release();
    ms->Release();

    if (nonzero == 0)
    {
        // A still frame disproves nothing, so this is not a verdict. Same voice
        // as the depth report's "unverified".
        // Eight attempts, one immediately and the rest sixteen frames apart, so
        // the window is still about 120 frames wide -- the count is of what was
        // actually looked at, because that is the number a reader can check.
        if (++g_ofa.quiet >= 8)
        {
            g_ofa.checked = true;
            Log("[synth] optical flow self-check: 8 checks spread over the first 120 or so "
                "frames found no non-zero vector anywhere, so the conversion could not be "
                "checked. Nothing was disproved.");
        }
        return;
    }

    g_ofa.checked = true;
    if (bad == 0)
        Log("[synth] optical flow self-check: %d cells sampled, flow %ux%u R16G16_SINT -> "
            "%ux%u R16G16_FLOAT, worst disagreement %.3f px. The conversion, the 1/32 scale "
            "and the grid mapping are what this build claims they are.",
            sampled, fd.Width, fd.Height, g_ofa.w, g_ofa.h, worst);
    else
        // Delivery continues. This is a defect report about this add-on, and
        // refusing to deliver would hide the numbers a report needs. The text
        // names the likely cause by construction, because it prints both numbers
        // in pixels rather than a verdict: a factor of exactly 32 is the missing
        // scale.
        Log("[synth] DEFECT: the optical flow conversion disagrees with its own input at "
            "cell (%d,%d): flow (%+d,%+d) is %+.3f,%+.3f px, and the motion vector texture "
            "holds %+.3f,%+.3f px. %d of %d cells sampled disagree by more than 0.02 px. "
            "The image will smear; this is a defect in this add-on and not in the estimator.",
            bx, by, bfx, bfy, bex, bey, bax, bay, bad, sampled);
}

// One frame. Returns true when out holds a motion vector texture NGX may read.
//
// The copy, both dispatches and the execute are timed together rather than the
// execute alone, because all of it is new cost -- and because nvOFExecute blocks
// the calling thread, which here is ReShade's present thread, so this is CPU
// time inside the game's frame rather than merely GPU time. The one-off session
// open is outside the bracket; see the note at the QueryPerformanceCounter.
// The body, over a colour texture and a device the caller supplies. Two callers:
// OfaFrame below, which is the D3D11 game's own device and its back buffer, and
// SynthOfaVkFrame, which is a private D3D11 device and the D3D12 shared texture
// the Vulkan frame was already copied into. Extracted rather than duplicated --
// everything here except where the two pointers come from is the same code, and
// a second copy of an nvOFExecute call with its own failure counting is exactly
// the thing that drifts.
//
// dev11, ctx and src are BORROWED. This function releases none of them.
static bool OfaFrameOn(ID3D11Device *dev11, ID3D11DeviceContext *ctx,
                       ID3D11Texture2D *src, DXGI_FORMAT src_fmt,
                       UINT w, UINT h, SynthBound *out)
{
    SynthBound blank = {};
    *out = blank;
    if (dev11 == nullptr || ctx == nullptr || src == nullptr) return false;

    LARGE_INTEGER t0, t1;
    bool delivered  = false;
    bool want_check = false;

    // A resolution change, a format change, or an ofa_grid or ofa_perf edit
    // rebuilds the session in place. tried is not consulted here: it governs the
    // first open only, and a failed re-open is recorded by stopped instead.
    //
    // perf was absent from this test until the panel gained a control for it.
    // NV_OF_PERF_LEVEL is only read at nvOFInit, so editing ofa_perf mid-session
    // was accepted by the parser, passed the validator, was reported as a config
    // change -- and estimated at the old level for the rest of the session, with
    // the log still printing the old name. One clause, and the teardown below is
    // the one ofa_grid already proved.
    if (g_ofa.ready && (g_ofa.w != w || g_ofa.h != h || g_ofa.back_fmt != src_fmt ||
                        g_ofa.dev != dev11 ||
                        g_ofa.grid != static_cast<UINT>(g_cfg.ofa_grid) ||
                        g_ofa.perf != g_cfg.ofa_perf))
    {
        Log("[synth] optical flow: the back buffer became %ux%u %s at grid %d perf %s on "
            "device %p, so the session is rebuilt.", w, h, FormatName(src_fmt),
            g_cfg.ofa_grid, SynthPerfName(g_cfg.ofa_perf),
            static_cast<void *>(dev11));
        OfaClose();
        if (!OfaOpen(dev11, ctx, w, h, src_fmt))
        {
            Log("[synth] optical flow: the session could not be rebuilt for the new size, "
                "so this stops.");
            g_ofa.stopped = true;
        }
    }
    else if (!g_ofa.ready && !g_ofa.tried)
    {
        OfaOpen(dev11, ctx, w, h, src_fmt);
    }

    // Nothing below ran, so nothing below is charged for. Counting a frame here
    // would put an "optical flow" clause on the timing line of a session that
    // fell back to a ReShade provider and never ran the engine at all, which is
    // the one thing that line exists to be able to deny.
    if (!g_ofa.ready) return false;

    // Started here rather than at the top of the function, so a one-off session
    // open -- two shader compiles, five textures and an nvOFInit -- is not
    // averaged into the per-frame figure it would otherwise dominate. What is
    // measured from here is the copy, both dispatches and the execute: all of
    // it new cost, and all of it on ReShade's present thread.
    QueryPerformanceCounter(&t0);

    // The colour frame, converted into the only 4-channel format the engine
    // takes. One copy and one dispatch; the copy is ~0.03 ms of bandwidth at
    // 3840x1600 against 1.6 ms of execute, which is why there is one path here
    // and not a second that tries an SRV over the back buffer first.
    ctx->CopyResource(g_ofa.copy, src);
    OfaDispatch(ctx, g_ofa.luma_cs, g_ofa.copy_srv, g_ofa.src_uav[g_ofa.cur], w, h);

    if (!g_ofa.primed)
    {
        g_ofa.primed = true;
        g_ofa.cur ^= 1;
        if (!g_ofa.first_said)
        {
            g_ofa.first_said = true;
            Log("[synth] optical flow: the first frame has no previous frame to compare "
                "against, so nothing is delivered on it. Flow starts on the next one.");
        }
        goto done;
    }

    {
        NvOfExecuteInputParams  in;
        NvOfExecuteOutputParams eo;
        memset(&in, 0, sizeof(in));
        memset(&eo, 0, sizeof(eo));
        in.inputFrame           = g_ofa.reg_src[g_ofa.cur];       // the CURRENT frame
        in.referenceFrame       = g_ofa.reg_src[g_ofa.cur ^ 1];   // the PREVIOUS frame
        in.disableTemporalHints = 1;   // what every measurement for this was taken with
        eo.outputBuffer         = g_ofa.reg_flow;

        auto exec = reinterpret_cast<PFN_OfaExecute>(g_ofa.slot[6]);
        const NvOfStatus r = OfaSafeExecute(exec, g_ofa.session, &in, &eo);
        ++g_ofa.frames;

        if (r == kOfaRaised)
        {
            // Calling back into a component that faulted is what turned a
            // disabled add-on into a killed process on Final Fantasy VII Remake.
            Log("[synth] optical flow: nvOFExecute raised an exception, so this stops here. "
                "The game keeps rendering on its own.");
            g_ofa.ready   = false;
            g_ofa.stopped = true;
            goto done;
        }
        if (r != kOfaSuccess)
        {
            if (g_ofa.fails == 0 || g_ofa.fails == 2)
            {
                char msg[512];
                OfaLastError(msg, sizeof(msg));
                Log("[synth] optical flow: execute %llu refused with status %ld (%s). %s",
                    g_ofa.frames, r, OfaStatusName(r), msg);
            }
            // This frame is not delivered and the game's own frame goes to screen
            // untouched. primed and the previous frame are kept, so the next
            // frame retries.
            if (++g_ofa.fails >= 3)
            {
                Log("[synth] optical flow: three consecutive failures, so this stops. No "
                    "ReShade motion-vector texture is substituted: the temporal history "
                    "DLSS has accumulated was built from these vectors and swapping the "
                    "estimator under it is the defect this path was rebuilt to avoid.");
                g_ofa.ready   = false;
                g_ofa.stopped = true;
            }
            goto done;
        }

        g_ofa.fails = 0;
        OfaDispatch(ctx, g_ofa.flow_cs, g_ofa.flow_srv, g_ofa.mv_uav, w, h);
        // Wanted, not run: the check stages and maps about 30 MB and blocks
        // until the GPU catches up, and inside the bracket below that stall
        // would be charged to the per-frame flow figure -- the same error the
        // bracket was moved down the function to avoid for the session open.
        want_check = !g_ofa.checked;

        // No AddRef, and for the reason written over the Get in SynthParams:
        // SynthOfa owns mv for the life of the shape and nothing downstream
        // releases what it takes out of the contract.
        out->res     = reshade::api::resource { reinterpret_cast<uintptr_t>(g_ofa.mv) };
        out->tex11   = nullptr;
        out->w       = w;
        out->h       = h;
        out->fmt     = DXGI_FORMAT_R16G16_FLOAT;
        out->samples = 1;
        out->usage   = 0;

        // Advanced HERE and not at the exit, and the difference is not
        // housekeeping. On a refused execute this is deliberately skipped, so
        // the next frame overwrites src[cur] and keeps src[cur^1] -- which is
        // the input of the last execute that SUCCEEDED, and therefore the last
        // frame DLSS was actually given. A refused frame is not delivered, so
        // DLSS's history is still at that same frame, and the vector it needs is
        // the one spanning the whole gap rather than the last hop of it.
        // Advancing unconditionally would hand a two-frame gap a one-frame
        // vector and under-reproject the history by exactly one frame, which is
        // the smear this path was rebuilt to remove, reintroduced on the
        // recovery frame.
        g_ofa.cur ^= 1;
        delivered = true;
    }

done:
    QueryPerformanceCounter(&t1);
    g_bridge.ofa_ticks += (t1.QuadPart - t0.QuadPart);
    ++g_bridge.ofa_frames;

    // Outside the bracket on purpose. It runs at most a handful of times in a
    // session and it blocks on the GPU; charging it to the per-frame figure
    // would make the first report of a working session read three times its
    // real cost.
    if (want_check) OfaSelfCheck(dev11, ctx);

    // TimingTick is reached only through BridgeFrame, so a session that runs the
    // engine every frame and never delivers one would spend this and never
    // report it -- and a blocking call in the game's present thread that no line
    // in the log accounts for is precisely what this measurement exists to make
    // impossible. Said here when 600 flow frames have gone by with not one
    // delivered frame to carry the figure.
    if (g_bridge.ofa_frames >= 600 && g_bridge.timed_frames == 0)
    {
        // Asked for here rather than read off g_bridge.qpf: that field is filled
        // by TimingTick, and TimingTick not having run is the whole condition
        // this branch is testing for.
        LARGE_INTEGER qpf;
        QueryPerformanceFrequency(&qpf);
        Log("[synth] optical flow: %.2f ms/frame over %llu frames, and not one of them "
            "reached the bridge's own frame path -- so nothing else in this log accounts "
            "for that time. It is spent on ReShade's present thread.",
            1000.0 * double(g_bridge.ofa_ticks) / double(qpf.QuadPart)
                   / double(g_bridge.ofa_frames),
            g_bridge.ofa_frames);
        g_bridge.ofa_ticks  = 0;
        g_bridge.ofa_frames = 0;
    }

    return delivered;
}

// The D3D11 game's own device and its own back buffer, resolved off the runtime.
// Everything this does beyond that is in OfaFrameOn.
static bool OfaFrame(reshade::api::effect_runtime *rt, reshade::api::command_list *cmd,
                     reshade::api::resource_view rtv, UINT w, UINT h, SynthBound *out)
{
    SynthBound blank = {};
    *out = blank;

    auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(
        static_cast<uintptr_t>(cmd->get_native()));
    if (ctx == nullptr) return false;

    SynthBound back = {};
    SynthResolveView(rt->get_device(), static_cast<uint32_t>(reshade::api::device_api::d3d11),
                     rtv, &back);
    if (back.tex11 == nullptr) return false;
    if (back.w != w || back.h != h) { back.tex11->Release(); return false; }

    ID3D11Device *dev11 = nullptr;
    ctx->GetDevice(&dev11);
    if (dev11 == nullptr) { back.tex11->Release(); return false; }

    const bool delivered = OfaFrameOn(dev11, ctx, back.tex11, back.fmt, w, h, out);

    dev11->Release();
    back.tex11->Release();
    return delivered;
}

// ---------------------------------------------------------------------------
// The driver's optical flow engine on the VULKAN transport
//
// WHAT THE AUDIT ASKED FOR AND WHAT THIS IS. The audit chose NVOFA -- the
// driver's own optical flow engine -- as the motion vector source for the
// synthetic Vulkan path, in place of a third-party ReShade shader whose
// estimator smeared fine detail along the direction of movement in Skyrim on
// 2026-08-30. That choice is what is implemented here. What is NOT implemented
// is the route the audit picked to reach it: a second NVOFA session on the
// private D3D12 device, at API 0x30, with the two-argument constructor, the
// four-argument register, a zeroed 192-byte init block, mandatory fence points
// on both execute structs, two new D3D12 root signatures and pipeline state
// objects, a descriptor heap, and a session that faults on every teardown order
// tried and therefore has to be leaked.
//
// The reason is the instruction this work was given: if you find yourself
// writing a second copy of something that already exists, the seam is in the
// wrong place. That route is a second copy of OfaOpen, OfaClose, OfaFrame,
// OfaSelfCheck and the whole status/failure/rebuild discipline around them,
// written against six function signatures recovered from one driver build on
// one GPU, none of which can be exercised on the only Vulkan title available to
// test with. It would be the largest untested block in this file.
//
// The seam this was built on turned out not to be the seam the mirror uses, and
// that is why none of it works on Vulkan. The reasoning was: a D3D12 texture on
// a SHARED heap with ALLOW_SIMULTANEOUS_ACCESS, and an ID3D11Texture2D alias of
// it opened through the NT handle -- MakeSharedPair in bridge.inc, in every
// session. MakeSharedPair does try that direction first. It has never once
// succeeded: on 2026-08-31 every D3D11 log on this machine shows all four
// textures refused with the same 0x80070057, every format, followed by
// "trying the other direction", and the mirror running on attempt B -- created
// on D3D11, opened on D3D12. The claim above was read off the function name
// rather than off its log.
//
// Attempt B is not available here: these textures are imported into the game's
// Vulkan device through the D3D12 NT handle and NGX evaluates on the D3D12
// device, so they must be D3D12-created. That is the whole story of why the
// engine runs on D3D11 and not on Vulkan, and it is stated in the README's
// known limits rather than fixed, on a final release. The four
// textures this transport already creates are exactly that: shared heap,
// simultaneous access, NT handle, because the Vulkan side imports them. So the
// colour that has just crossed from Vulkan into g_s12.tex[SLOT_COLOR] can be
// read by a D3D11 device without anything new being created, and the vectors
// can be written back into g_s12.tex[SLOT_MV] the same way.
//
// What that costs is one private D3D11 device with no swapchain, which is the
// same object the private D3D12 device already is and is safe for the same
// reason: ReShade creates a runtime on a swapchain, and there is no swapchain
// here. What it buys is that the session, the init parameters, the register
// calls, the two compute shaders, the arming, the failure counting, the
// rebuild-on-resize and the known-answer self check are the code that is
// already proven working, at the API version -- 0x20 -- whose 48-byte
// NV_OF_INIT_PARAMS layout was confirmed field by field against the driver.
//
// If a measurement ever shows the extra device or the CPU barrier costing more
// than it is worth, the audit's route (b) is the upgrade and every signature it
// needs is recorded in the specification. This is not a claim that route (b) is
// wrong; it is a claim that it is not yet earned.
//
// KNOWN CEILING, and it is real. The engine cannot open until the four shared
// textures exist, and those are created by Synth12Build, which needs the
// contract, which the arming gate has to have passed -- and the arming gate
// wants a motion vector texture. So the FIRST arming of a Vulkan session still
// needs one of the three ReShade motion-vector providers to be installed and
// bound; from the frame after that the engine supplies the vectors and no
// ReShade texture is consulted again. The audit's own route (b) places the
// engine at the top of Synth12EvaluateShared and therefore has the identical
// bootstrap, so this is a property of where the engine sits and not of how it
// is reached.
// ponytail: the first Vulkan arming needs a ReShade motion-vector provider.
// Breaking that needs the engine to own a fifth shared texture and a fifth
// Vulkan import so it can run before the contract exists -- and its vectors
// would then be one frame old, which is the defect this source replaces. Revisit
// if a Vulkan title with no motion-vector shader is ever reported.
// ---------------------------------------------------------------------------

typedef HRESULT (WINAPI *PFN_D3D11CreateDevice_)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                 const D3D_FEATURE_LEVEL *, UINT, UINT,
                                                 ID3D11Device **, D3D_FEATURE_LEVEL *,
                                                 ID3D11DeviceContext **);

struct SynthOfaVk
{
    bool tried;
    bool ready;
    bool stopped;          // a barrier that never cleared; never retried
    HMODULE lib;

    ID3D11Device        *dev;
    ID3D11DeviceContext *ctx;
    // The engine's OWN colour and motion-vector textures, created on this private
    // D3D11 device and opened on the private D3D12 one -- not aliases of the four
    // g_s12 textures, which is what this used to be and why it never worked.
    //
    // That crossing runs D3D11 -> D3D12, which is the direction this driver
    // accepts. The other one, D3D12 -> D3D11, is refused for every texture and
    // every format: the mirror's MakeSharedPair tries it first in every session
    // and falls back 16 times out of 16. Aliasing the g_s12 textures needed the
    // refused direction, and nothing made that necessary -- those four have to be
    // D3D12-created because the game's Vulkan device imports them, but the
    // engine's two are imported into nothing.
    //
    // Colour is copied in from g_s12.tex[SLOT_COLOR] on the D3D12 side each
    // frame. The vectors need no copy out: the evaluate hands NGX mv12 directly.
    ID3D11Texture2D     *color;
    ID3D11Texture2D     *mvout;
    ID3D12Resource      *color12;
    ID3D12Resource      *mv12;
    HANDLE               hcolor;
    HANDLE               hmv;
    // The only synchronisation between this private D3D11 device and the private
    // D3D12 one, and it is a CPU barrier rather than a shared fence. That is
    // enough here and nowhere else: this runs between the caller's
    // rq->wait_idle() and the D3D12 evaluate, and the previous frame's D3D12
    // work was waited to completion on the CPU at the end of that frame -- so at
    // the moment this executes there is no GPU work in flight on either device
    // that names either texture. A shared fence would be the upgrade the day
    // this transport stops being fully CPU-serialised.
    ID3D11Query         *done;

    UINT        w, h;
    DXGI_FORMAT fmt;
    UINT64      frames;
};

static SynthOfaVk g_ovk;

static ID3D12Resource *SynthOfaVkMvTexture() { return g_ovk.mv12; }
static ID3D11Device   *SynthOfaVkDevice()    { return g_ovk.dev; }

static bool SynthOfaVkIsSource() { return g_ovk.ready || g_ovk.stopped; }

static void SynthOfaVkClose()
{
    // D3D12 aliases first, then the NT handles, then the D3D11 textures that own
    // the memory. Releasing the owner while the other device still holds a view
    // of it is the one ordering that is wrong here.
    if (g_ovk.color12 != nullptr) { g_ovk.color12->Release(); g_ovk.color12 = nullptr; }
    if (g_ovk.mv12    != nullptr) { g_ovk.mv12->Release();    g_ovk.mv12    = nullptr; }
    if (g_ovk.hcolor  != nullptr) { CloseHandle(g_ovk.hcolor); g_ovk.hcolor = nullptr; }
    if (g_ovk.hmv     != nullptr) { CloseHandle(g_ovk.hmv);    g_ovk.hmv    = nullptr; }
    if (g_ovk.color != nullptr) { g_ovk.color->Release(); g_ovk.color = nullptr; }
    if (g_ovk.mvout != nullptr) { g_ovk.mvout->Release(); g_ovk.mvout = nullptr; }
    if (g_ovk.done  != nullptr) { g_ovk.done->Release();  g_ovk.done  = nullptr; }
    if (g_ovk.ctx   != nullptr) { g_ovk.ctx->Release();   g_ovk.ctx   = nullptr; }
    if (g_ovk.dev   != nullptr) { g_ovk.dev->Release();   g_ovk.dev   = nullptr; }
    g_ovk.ready = false;
}

// Torn down and allowed to open again, which is what a resolution change needs:
// the two textures this reads and writes are ALIASES of g_s12.tex[SLOT_COLOR]
// and [SLOT_MV], and Synth12Build releases those and creates new ones. An alias
// of a released D3D12 resource is a use after free that neither API reports.
// stopped is not cleared: a session that gave up gave up.
static void SynthOfaVkRebuild()
{
    if (!g_ovk.tried) return;
    // The optical flow session's own textures live on the device this is about
    // to release, so it goes first.
    if (g_ofa.dev != nullptr && g_ofa.dev == g_ovk.dev) OfaClose();
    SynthOfaVkClose();
    g_ovk.tried = g_ovk.stopped;
}

// One shot. A failure is latched and never retried, and every branch names its
// own reason -- a session that falls back to a ReShade provider has to be able
// to say why, because that estimator is the one that smears.
// The engine's own shared texture, created on the private D3D11 device and opened
// on the private D3D12 one. D3D11 -> D3D12: the direction this driver accepts.
//
// Transcribed from MakeSharedPair's attempt B in bridge.inc rather than called,
// deliberately. That function writes g_bridge.tex11[i], tex12[i] and shared[i] by
// slot index and is load-bearing for nine shipped titles; borrowing it for a
// fifth and sixth texture would put this file's failures into the mirror's state.
// Thirty lines is cheaper than that coupling.
//
// Measured on this machine 2026-08-31: R11G11B10_FLOAT, R16G16_FLOAT,
// B8G8R8A8_UNORM and R16G16_SINT all cross with and without the UAV bind and
// arrive carrying ALLOW_SIMULTANEOUS_ACCESS (flags 0x24), which is what makes the
// COMMON -> X -> COMMON barrier pairs around the copy correct. R32G32_FLOAT is
// refused at CreateTexture2D -- the kernel's shared-format list is narrower than
// DXGI's, the same note bridge.inc already carries.
static bool SynthOfaShare(UINT w, UINT h, DXGI_FORMAT fmt, bool uav,
                          ID3D11Texture2D **out11, ID3D12Resource **out12,
                          HANDLE *out_handle, const char *what)
{
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

    HRESULT hr = g_ovk.dev->CreateTexture2D(&td, nullptr, out11);
    if (FAILED(hr))
    {
        Log("[synth] optical flow: the engine's %s texture could not be created on the "
            "private D3D11 device, 0x%08X %s %ux%u.", what, hr, FormatName(fmt), w, h);
        return false;
    }

    IDXGIResource1 *dxgi_res = nullptr;
    hr = (*out11)->QueryInterface(__uuidof(IDXGIResource1),
                                  reinterpret_cast<void **>(&dxgi_res));
    if (SUCCEEDED(hr))
    {
        hr = dxgi_res->CreateSharedHandle(nullptr,
                                          DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                          nullptr, out_handle);
        dxgi_res->Release();
    }
    if (SUCCEEDED(hr))
        hr = g_s12.dev->OpenSharedHandle(*out_handle, __uuidof(ID3D12Resource),
                                         reinterpret_cast<void **>(out12));
    if (FAILED(hr))
    {
        Log("[synth] optical flow: the engine's %s texture was created but could not be "
            "opened on the private D3D12 device, 0x%08X %s.", what, hr, FormatName(fmt));
        if (*out11 != nullptr) { (*out11)->Release(); *out11 = nullptr; }
        if (*out_handle != nullptr) { CloseHandle(*out_handle); *out_handle = nullptr; }
        return false;
    }
    return true;
}

static bool SynthOfaVkOpen(UINT w, UINT h, DXGI_FORMAT back_fmt)
{
    if (g_ovk.tried) return g_ovk.ready;
    g_ovk.tried = true;
    g_ovk.w = w; g_ovk.h = h;
    // The TYPED spelling, because that is the format g_s12.tex[SLOT_COLOR] was
    // actually created with and therefore the format the alias has. Handing
    // OfaFrameOn anything else makes its own rebuild test fire on every frame.
    g_ovk.fmt = TypedFormat(back_fmt);

    if (g_cfg.ofa_grid == 0)
    {
        Log("[synth] optical flow: ofa_grid=0 in dlss5-bridge.cfg, so the driver's "
            "optical flow engine is not used on this transport either and the motion "
            "vectors come from a ReShade effect.");
        return false;
    }
    if (g_s12.dev == nullptr)
    {
        Log("[synth] optical flow: the private D3D12 device is not open, so there is "
            "nothing to open the engine's own textures on.");
        return false;
    }

    // Loaded, never linked, for the same reason vulkan-1.dll and d3d12.dll are:
    // this add-on must still load in a process with none of them.
    g_ovk.lib = LoadLibraryW(L"d3d11.dll");
    auto create = g_ovk.lib != nullptr
        ? reinterpret_cast<PFN_D3D11CreateDevice_>(GetProcAddress(g_ovk.lib, "D3D11CreateDevice"))
        : nullptr;
    if (create == nullptr)
    {
        Log("[synth] optical flow: d3d11.dll is not present or exports no D3D11CreateDevice, "
            "so the engine cannot be given a device to run on. The motion vectors come from "
            "a ReShade effect.");
        return false;
    }

    // ponytail: the default adapter, exactly as the private D3D12 device above
    // takes it and for the same reason -- ReShade exposes no VkPhysicalDevice,
    // so there is no LUID to match. Both private devices therefore land on the
    // same adapter as each other, which is what the shared handle below needs;
    // whether that is the adapter the game renders on is the question the
    // Vulkan memory import already answers by failing.
    const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 1,
                        D3D11_SDK_VERSION, &g_ovk.dev, nullptr, &g_ovk.ctx);
    if (FAILED(hr) || g_ovk.dev == nullptr || g_ovk.ctx == nullptr)
    {
        Log("[synth] optical flow: D3D11CreateDevice failed 0x%08X, so there is no device "
            "to open an optical flow session on.", hr);
        SynthOfaVkClose();
        return false;
    }

    // The engine's own two textures, created here and opened on the private D3D12
    // device -- see SynthOfaShare for why this direction and not the other.
    //
    // The colour takes the same typed format the transport's own colour texture
    // has, so the per-frame CopyResource is a straight copy with no conversion.
    // The vectors are R16G16_FLOAT, which is what the conversion writes and what
    // NGX reads, and they need the UAV bind because the conversion shader writes
    // them through an unordered access view.
    if (!SynthOfaShare(w, h, g_ovk.fmt, /*uav=*/false,
                       &g_ovk.color, &g_ovk.color12, &g_ovk.hcolor, "colour") ||
        !SynthOfaShare(w, h, DXGI_FORMAT_R16G16_FLOAT, /*uav=*/true,
                       &g_ovk.mvout, &g_ovk.mv12, &g_ovk.hmv, "motion vector"))
    {
        SynthOfaVkClose();
        return false;
    }

    D3D11_QUERY_DESC qd = {};
    qd.Query = D3D11_QUERY_EVENT;
    if (FAILED(g_ovk.dev->CreateQuery(&qd, &g_ovk.done)) || g_ovk.done == nullptr)
    {
        Log("[synth] optical flow: the private D3D11 device would not create an event "
            "query, and without one there is nothing to wait on before the D3D12 evaluate "
            "reads what this wrote.");
        SynthOfaVkClose();
        return false;
    }

    // The session itself: the same OfaOpen the D3D11 path uses, at the same API
    // version, with the same 48-byte NV_OF_INIT_PARAMS whose layout was
    // confirmed against the driver field by field.
    if (!OfaOpen(g_ovk.dev, g_ovk.ctx, w, h, g_ovk.fmt))
    {
        Log("[synth] optical flow: the session did not open on the private D3D11 device -- "
            "the reason is the line above this one. The motion vectors come from a ReShade "
            "effect, and that estimator is what produced the smearing this source was added "
            "to fix, so a report of melting detail from here is expected rather than new.");
        SynthOfaVkClose();
        return false;
    }

    g_ovk.ready = true;
    Log("[synth] optical flow: running on a private D3D11 device that shares this "
        "transport's own colour and motion vector textures with the private D3D12 one. "
        "Nothing new is allocated for it: those two textures are already on a shared heap "
        "with simultaneous access because the Vulkan side imports them. From the next "
        "frame the vectors come from the driver and no ReShade motion-vector texture is "
        "read again.");
    return true;
}

// One frame, run between the caller's queue idle and the D3D12 evaluate.
// Returns true when g_s12.tex[SLOT_MV] now holds this frame's vectors.
static bool SynthOfaVkFrame()
{
    if (!g_ovk.ready) return false;

    // Fill the engine's own colour texture from the transport's, on the D3D12
    // side, and WAIT for it on the CPU before the D3D11 read below.
    //
    // This did not exist while g_ovk.color was an alias of the same memory. Now
    // that the engine owns its texture, the copy is the only thing that makes it
    // this frame's colour -- and the wait is the only thing that orders it before
    // the engine reads it. Skipping the wait is the one failure this transport
    // cannot report: the flow would be computed against a stale or torn frame,
    // giving subtly wrong vectors and ghosting, with nothing in the log and
    // OfaSelfCheck still passing, because that checks the conversion against the
    // flow texture the driver filled and not whether the colour was current.
    //
    // The barriers are COMMON -> COPY -> COMMON on both sides. Correct by the
    // resources' own declared flags: both carry ALLOW_SIMULTANEOUS_ACCESS, the
    // transport's because Synth12MakeTexture sets it and the engine's because it
    // survives the D3D11 -> D3D12 crossing, measured as flags 0x24.
    if (g_ovk.color12 != nullptr && g_s12.tex[SLOT_COLOR] != nullptr)
    {
        if (!Synth12Begin())
        {
            Log("[synth] optical flow: no command list to copy this frame's colour into "
                "the engine's texture, so nothing is delivered this frame.");
            return false;
        }
        D3D12_RESOURCE_BARRIER rb[2] = {};
        for (int i = 0; i < 2; ++i)
        {
            rb[i].Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            rb[i].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            rb[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        rb[0].Transition.pResource   = g_s12.tex[SLOT_COLOR];
        rb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        rb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        rb[1].Transition.pResource   = g_ovk.color12;
        rb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        rb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        g_s12.list->ResourceBarrier(2, rb);

        g_s12.list->CopyResource(g_ovk.color12, g_s12.tex[SLOT_COLOR]);

        for (int i = 0; i < 2; ++i)
        {
            const D3D12_RESOURCE_STATES was = rb[i].Transition.StateAfter;
            rb[i].Transition.StateBefore = was;
            rb[i].Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
        }
        g_s12.list->ResourceBarrier(2, rb);

        const UINT64 v = Synth12End();
        if (g_s12.fence->GetCompletedValue() < v)
        {
            g_s12.fence->SetEventOnCompletion(v, g_s12.event);
            if (WaitForSingleObject(g_s12.event, 4000) != WAIT_OBJECT_0)
            {
                Log("[synth] optical flow: the colour copy into the engine's texture did "
                    "not complete within 4 s, so the engine is not run on it. Nothing is "
                    "delivered this frame rather than flow computed on a stale frame.");
                return false;
            }
        }
    }

    SynthBound mv = {};
    if (!OfaFrameOn(g_ovk.dev, g_ovk.ctx, g_ovk.color, g_ovk.fmt, g_ovk.w, g_ovk.h, &mv))
    {
        // Not delivered: the first frame has no previous one, or the execute was
        // refused. Either way the shared texture is left holding what the last
        // successful frame put there rather than being filled from somewhere
        // else -- swapping estimator under a running feature is the defect this
        // whole source exists to avoid -- and the caller does not deliver.
        if (!g_ofa.ready && !g_ovk.stopped)
        {
            g_ovk.stopped = true;
            Log("[synth] optical flow: the session on the private D3D11 device has stopped, "
                "so this transport stops delivering rather than change estimator underneath "
                "a feature whose temporal history was built from these vectors.");
        }
        return false;
    }

    g_ovk.ctx->CopyResource(g_ovk.mvout, g_ofa.mv);

    // The CPU barrier. See the note on the query in the struct: this is the only
    // thing ordering the two private devices, and it is enough because both
    // halves of this transport are already serialised on this thread.
    g_ovk.ctx->End(g_ovk.done);
    g_ovk.ctx->Flush();
    BOOL fin = FALSE;
    const ULONGLONG deadline = GetTickCount64() + 200;
    while (g_ovk.ctx->GetData(g_ovk.done, &fin, sizeof(fin), 0) != S_OK)
    {
        if (GetTickCount64() > deadline)
        {
            Log("[synth] optical flow: the private D3D11 device did not finish writing the "
                "motion vectors within 200 ms, so this transport stops rather than let the "
                "D3D12 evaluate read a texture that is still being written.");
            g_ovk.stopped = true;
            // Before the device goes, not after: the optical flow session's five
            // textures and its handle live on THIS device, and SynthOfaVkClose
            // nulls g_ovk.dev -- after which SynthOfaVkRebuild's "same device"
            // test can never match again and the session would be left holding
            // that device alive for the life of the process.
            if (g_ofa.dev != nullptr && g_ofa.dev == g_ovk.dev) OfaClose();
            SynthOfaVkClose();
            return false;
        }
        SwitchToThread();
    }

    const UINT64 n = ++g_ovk.frames;
    if (n == 1)
        Log("[synth] optical flow: first frame of driver-computed motion vectors written "
            "into the shared texture the D3D12 evaluate reads, %ux%u R16G16_FLOAT.",
            g_ovk.w, g_ovk.h);
    return true;
}

// Runs on every frame, above the gates that hold the stage-2 report down to one
// per change: a report is news and a frame is not.
static void SynthMaybeDeliver(reshade::api::effect_runtime *rt,
                              reshade::api::command_list *cmd,
                              reshade::api::resource_view rtv,
                              const SynthFacts *f,
                              const SynthBound *depth, const SynthBound *mv)
{
    // First, and before any test that could let this run.
    if (InterlockedCompareExchange(&g_source, 0, 0) == SRC_MIRROR) { SynthStandDown(); return; }

    // The config, on every synthetic transport rather than only on D3D11, and
    // ABOVE every test that reads it. BridgeFrameInner's call is reached from
    // the mirror detour and from the D3D11 arm at the bottom of this function,
    // so a D3D12 or Vulkan session -- and any session that has not armed yet,
    // which with the shipped synth_after=0 is every session -- read this file
    // once at attach and never again. Below SynthArmRefusal this call could
    // never lift the refusal that was keeping it from being reached: ticking
    // the panel's own "add DLAA" box wrote synth_after=10 into a file nothing
    // would then read, for the rest of the session. Rate limited inside; on the
    // D3D11 transport BridgeFrameInner's own call finds the gate closed and
    // does nothing.
    //
    // Under g_bridge_cs like the mirror's own call, so g_cfg keeps one writer at
    // a time. The mirror returned one line above, so this section is never taken
    // from here on the path the nine shipped titles use.
    EnterCriticalSection(&g_bridge_cs);
    CfgReload();
    LeaveCriticalSection(&g_bridge_cs);

    const char *why = SynthArmRefusal(f, false);
    // Split, and the motion-vector half asks about the ReShade providers only.
    //
    // Row 0 of kSynthMv is the driver's own optical flow engine, which has no
    // ReShade texture behind it: the Vulkan branch that selects it leaves
    // mv->res null ON PURPOSE, because on that transport the vectors are already
    // in the shared D3D12 texture and a ReShade resource handle there would be a
    // trap rather than a value. Compounded with the depth test, this refused
    // every frame after the one that opened the engine -- so that branch has
    // shipped without ever executing. mv_which != 0 is the sound discriminator:
    // kSynthMvReShadeFirst is 1, so no ReShade provider can occupy row 0.
    if (why == nullptr && depth->res.handle == 0)
        why = "the depth binding did not resolve to a texture";
    if (why == nullptr && mv->res.handle == 0 && f->mv_which != 0)
        why = "the motion vector binding did not resolve to a texture";
    if (why != nullptr)
    {
        // SynthArmRefusal derives this sentence and nothing was saying it, so a
        // user who set synth_after and got nothing had every test in
        // SynthArmRefusal to guess between -- and a report whose verdict can
        // read "every precondition holds"
        // while a different test -- the delay, or the game's own DLSS -- is what
        // is holding it.
        //
        // Only once the user has opted in: synth_after=0 is the shipped default
        // and its refusal is the file saying it is switched off, which is not
        // news in the nine titles that will never turn it on.
        //
        // On change rather than once, because the reason moves as the session
        // does. The compare is on the pointer: every string here is a literal
        // returned from that one function, so identity is the reason and the
        // text is only how it reads.
        static const char *said;
        if (g_cfg.synth_after > 0 && why != said)
        {
            said = why;
            Log("[synth] not delivering: %s.", why);
        }
        return;
    }

    SynthBound back = {};
    SynthResolveView(rt->get_device(), f->device_api, rtv, &back);
    if (back.res.handle == 0) return;

    // The report measured against ReShade's screenshot size; the contract is
    // built from the texture that is actually handed over. Where the two
    // disagree the motion vectors were checked against the wrong number, so the
    // shape was never really verified and there is nothing to deliver from.
    if (back.w != f->back_w || back.h != f->back_h || back.samples != 1)
    {
        if (!g_synth_shape_said)
        {
            g_synth_shape_said = true;
            Log("[synth] not delivering: the render target ReShade supplies is %ux%u "
                "with %u sample%s, and the back buffer everything else was measured "
                "against is %ux%u. The motion vectors were checked against the second "
                "of those, so their shape is unverified.",
                back.w, back.h, back.samples,
                back.samples == 1 ? "" : "s", f->back_w, f->back_h);
        }
        if (back.tex11 != nullptr) back.tex11->Release();
        return;
    }

    // The same question SynthArmRefusal asks of depth and motion vectors, asked
    // of the render target here because this is where it is first resolved. The
    // Vulkan transport copies out of it and back into it, and both directions
    // need the corresponding usage on the image ReShade created.
    if (f->device_api == static_cast<uint32_t>(reshade::api::device_api::vulkan) &&
        (back.usage & static_cast<uint32_t>(reshade::api::resource_usage::copy_source |
                                            reshade::api::resource_usage::copy_dest)) !=
        static_cast<uint32_t>(reshade::api::resource_usage::copy_source |
                              reshade::api::resource_usage::copy_dest))
    {
        static bool said;
        if (!said)
        {
            said = true;
            Log("[synth] not delivering: the render target ReShade supplies has usage "
                "0x%08X, and a Vulkan transfer needs it to be both a copy source and a "
                "copy destination. Nothing is copied out of an image that was not "
                "created to be copied out of.", back.usage);
        }
        if (back.tex11 != nullptr) back.tex11->Release();
        return;
    }

    // Only the D3D11 transport needs a device context; the D3D12 one records
    // into a command list of its own. Resolved before the latch is taken so a
    // runtime that hands over neither refuses without claiming the source.
    auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(
        static_cast<uintptr_t>(cmd->get_native()));
    const bool on_d3d11 =
        f->device_api == static_cast<uint32_t>(reshade::api::device_api::d3d11);
    const bool on_vulkan =
        f->device_api == static_cast<uint32_t>(reshade::api::device_api::vulkan);
    if (on_d3d11 && ctx == nullptr) { if (back.tex11 != nullptr) back.tex11->Release(); return; }

    // Only from SRC_NONE. The mirror is never overwritten from here; the check
    // at the top already returned, and this closes the window between them.
    if (InterlockedCompareExchange(&g_source, SRC_SYNTH, SRC_NONE) == SRC_MIRROR)
    { if (back.tex11 != nullptr) back.tex11->Release(); return; }

    const unsigned int flags = SynthCreateFlags(back.fmt, f->def_value[SYNTH_DEF_REVERSED]);
    if (!g_synth_armed_said)
    {
        g_synth_armed_said = true;
        // "since this add-on started" is not always the window, and the counter is
        // not D3D11's alone -- the Vulkan create increments it too. After a source
        // latch release the question is what the game has done SINCE the release,
        // which is the case that path exists for.
        Log("[synth] arming. The configured %d s delay has passed, no NGX create and no "
            "evaluate has come from the game since %s, source=%s allows it, and every "
            "precondition in the report above holds.",
            g_cfg.synth_after,
            InterlockedCompareExchange(&g_release_n, 0, 0) != 0
                ? "the source latch was released" : "this add-on started",
            kCfgSourceName[g_cfg.source]);
        // Which transport, read off the device rather than off the build. Both
        // deliver the same contract; they differ in how many devices the frame
        // touches, which is the first thing a report needs to say.
        Log("[synth]   transport: %s.", on_d3d11
            ? "D3D11 -- the frame is copied onto this add-on's own D3D12 device and the "
              "evaluate runs there"
            : on_vulkan
                ? "Vulkan -- four textures created on this add-on's own D3D12 device are "
                  "imported into the game's VkDevice, the frame is copied into them there, "
                  "and the evaluate runs on the D3D12 side"
                : "D3D12 -- the evaluate runs on the game's own device, over the game's own "
                  "depth and motion vectors, with no copy between devices");
        Log("[synth]   contract: %ux%u -> %ux%u, preset 5 (DLAA), create flags %u, "
            "MV.Scale %.1f,%.1f, jitter 0,0.",
            back.w, back.h, back.w, back.h, flags,
            static_cast<double>(g_cfg.mv_sign_x != 0 ? g_cfg.mv_sign_x
                                : (f->mv_which >= 0 && kSynthMv[f->mv_which].sign_x != 0
                                       ? kSynthMv[f->mv_which].sign_x : 1)) * back.w,
            static_cast<double>(g_cfg.mv_sign_y != 0 ? g_cfg.mv_sign_y
                                : (f->mv_which >= 0 && kSynthMv[f->mv_which].sign_y != 0
                                       ? kSynthMv[f->mv_which].sign_y : 1)) * back.h);
        // Both sizes on one line, once per session, so a field report shows the
        // flow field AND the texture NGX was handed and proves the upsample
        // happened rather than being assumed. That is the ambiguity -- "the
        // transport is right and the flow is bad" against "the transport is
        // wrong" -- which cost this project the vort_Shaders diagnosis.
        // g_ofa.grid != 0 as well as mv_which == 0, and the second test is not
        // redundant any more. It used to be: mv_which 0 meant the engine had
        // already opened, because that was the only way to select row 0. The
        // Vulkan arming gate now selects it on the PROMISE of the engine -- so
        // this can run before OfaOpen has set grid, and the two divisions below
        // crashed the game with an integer divide by zero on the first session
        // that armed without a ReShade shader, 2026-08-31.
        if (f->mv_which == 0 && g_ofa.grid != 0)
        {
            Log("[synth]   motion vectors: NVIDIA optical flow, grid %u, perf %s. Flow "
                "%ux%u R16G16_SINT -> NGX %ux%u R16G16_FLOAT, scaled by 1/32 into UV.",
                g_ofa.grid, SynthPerfName(g_ofa.perf),
                (g_ofa.w + g_ofa.grid - 1) / g_ofa.grid,
                (g_ofa.h + g_ofa.grid - 1) / g_ofa.grid, g_ofa.w, g_ofa.h);
            Log("[synth]   direction: %+d,%+d, measured on this machine and not a "
                "provider's claim.",
                g_cfg.mv_sign_x != 0 ? g_cfg.mv_sign_x : kSynthMv[0].sign_x,
                g_cfg.mv_sign_y != 0 ? g_cfg.mv_sign_y : kSynthMv[0].sign_y);
        }
        else if (f->mv_which == 0)
        {
            Log("[synth]   motion vectors: the driver's optical flow engine, which has not "
                "opened yet -- this session armed on the expectation of it. The flow field "
                "sizes are reported by the line that opens it, on one of the next frames.");
        }
        Log("[synth]   IsHDR is %s because the back buffer is %s; DepthInverted is %s "
            "because RESHADE_DEPTH_INPUT_IS_REVERSED is %d.",
            (flags & 0x01) ? "set" : "clear", FormatName(back.fmt),
            (flags & 0x08) ? "set" : "clear", f->def_value[SYNTH_DEF_REVERSED]);

        // A known defect, stated rather than hidden, because it is the first
        // thing that would be blamed on the sign knob. This runs at
        // reshade_begin_effects, which fires BEFORE any technique; the provider
        // writes its texture from inside its own technique. So the vectors read
        // here were computed during the previous frame's effect chain, and DLSS
        // is handed this frame's colour against last frame's motion.
        //
        // Not fixed by moving to reshade_finish_effects, which is why it is
        // still here: all three transports read the resource states that hold at
        // begin_effects -- back buffer in render_target, depth and motion
        // vectors in shader_resource, the last of those left there by
        // generic_depth's own begin_effects handler. Those are not the states
        // after the chain has run, and on D3D12 a barrier from a state a
        // resource is not in is a device removal rather than a warning. The move
        // needs the states re-derived for the new point, not a changed enum.
        if (f->mv_which == 0)
            // Not one frame old on this provider, and the difference is worth
            // stating rather than leaving a reader to infer it from the note
            // below being absent: the flow is computed here, at this same point
            // in this same frame, from this frame's back buffer against the one
            // captured at the same point last frame. Colour and motion are the
            // same two frames.
            Log("[synth]   note: the motion vectors are computed here, from this frame's "
                "back buffer against the previous frame's, so they are not one frame "
                "behind the colour the way a ReShade effect's are.");
        else
            Log("[synth]   note: the motion vectors are one frame old. This delivers at "
                "reshade_begin_effects, before any technique runs, and the provider writes its "
                "texture from inside its own technique.");
    }
    g_synth_delivering = true;

    SynthParams sp;
    SynthBuild(&sp, back.w, back.h, back.fmt, back.res, depth->res, mv->res,
               f->def_value[SYNTH_DEF_REVERSED], f->mv_which);
    // mv_which 0 is the driver's optical flow engine, whose vectors reach NGX
    // through the shared D3D12 texture rather than through this contract.
    SynthSelfCheck(&sp, f->mv_which == 0);

    // The same two steps the game's evaluate detour takes, in the same order and
    // under the same section: opening the session and running the frame are one
    // unit, or a source that took the lock between them finds session_ready true
    // and every texture still null.
    EnterCriticalSection(&g_bridge_cs);
    // Asked again here, and this is the line that makes the latch a latch. The
    // compare above it is outside the section, and everything between the two --
    // the arming log with its file write, the build, the self-check -- is long
    // enough for the game's first DLSS call to land in the middle of it. The
    // evaluate detour publishes SRC_MIRROR before it takes this section and does
    // its undo inside it, so inside the section there are only two states and
    // both are correct: either the detour has not arrived, and it will find
    // prev_source SRC_SYNTH and throw away whatever this frame builds, or it has
    // already run, and this frame would evaluate the game's own freshly built
    // feature under a contract describing somebody else's textures -- delivering
    // one wrong image into the back buffer and a temporal history to match.
    // Left delivering rather than stood down here: the compare at the top is
    // what says so, on the next frame, with the log line that goes with it.
    if (InterlockedCompareExchange(&g_source, 0, 0) == SRC_MIRROR)
    {
        LeaveCriticalSection(&g_bridge_cs);
        if (back.tex11 != nullptr) back.tex11->Release();
        return;
    }
    // The seam is exactly here and nowhere else: everything above this line is
    // the same contract, the same self-check, the same latch and the same
    // report, and everything below is the transport that carries it.
    if (on_d3d11)
    {
        if (g_cfg.stage >= 1 && InterlockedCompareExchange(&g_probe_done, 1, 0) == 0)
        {
            ID3D11Device *dev = nullptr;
            ctx->GetDevice(&dev);
            if (dev != nullptr) { BridgeInitSession(dev, ctx); dev->Release(); }
        }
        BridgeFrame(ctx, &sp);
    }
    else if (on_vulkan)
    {
        // Same rule as the D3D12 arm below: SynthArmRefusal has already refused
        // anything under stage 3, and neither of these transports has a step to
        // isolate at 1 or 2 the way the mirror's cross-device copies do.
        SynthVkFrame(rt, cmd, &sp, back.res, depth->res, mv->res,
                     back.w, back.h, back.fmt);
    }
    else if (g_cfg.stage >= 3)
    {
        // stage is the same switch it is on the mirror, and the D3D12 transport
        // has nothing below 3 to do: there are no cross-device copies to run on
        // their own, so 1 and 2 have nothing to isolate here.
        Synth12Frame(rt, &sp,
                     reinterpret_cast<ID3D12Resource *>(static_cast<uintptr_t>(back.res.handle)),
                     reinterpret_cast<ID3D12Resource *>(static_cast<uintptr_t>(depth->res.handle)),
                     reinterpret_cast<ID3D12Resource *>(static_cast<uintptr_t>(mv->res.handle)),
                     back.w, back.h, back.fmt);
    }
    LeaveCriticalSection(&g_bridge_cs);

    ++g_synth_frames;
    if (back.tex11 != nullptr) back.tex11->Release();
}

// ---------------------------------------------------------------------------
// The three callbacks
// ---------------------------------------------------------------------------

static uint64_t g_synth_signature;
static bool     g_synth_reported;
static ULONGLONG g_synth_last_probe;
static ULONGLONG g_synth_last_print;
// SynthVerdictKey of the last report actually printed.
//
// This was SynthArmRefusal's return pointer, which was wrong twice over and is
// recorded here because the mistake is an easy one to make again. That function
// short-circuits on configuration before it looks at a single measured fact, so
// under the shipped synth_after=0 it returns the same literal all session and
// the comparison was constant-true -- an unconditional thirty-second gag on a
// report that is supposed to speak the moment the story changes. And it is not
// the verdict anyway: SynthReport computes its own from depth_ok/mv_ok/aside and
// never calls it, so the two could diverge completely.
static uint32_t g_synth_last_verdict;

// The last measurement, kept so the status panel can put it back through
// SynthArmRefusal rather than carry a second copy of those tests that would
// drift from this one. Published from the frame path and read from ReShade's UI
// thread with no lock: SynthFacts is all plain integers and holds no pointer, so
// the worst a reader can see is a mix of two frames' numbers -- which for a
// panel is one frame of a stale digit, and never a pointer to anything.
static SynthFacts g_synth_panel_facts;

// stage=0 has to remain an off switch that stops everything, and this file is
// new enough to be the thing that quietly broke it. Nothing here creates a
// device or opens a session, so the only cost to guard is the work itself --
// guarded at the top of every entry point rather than at registration alone, so
// editing the config mid-session still turns it off.
static bool SynthEnabled()
{
    return g_cfg.stage != 0 && g_cfg.source != CFG_SRC_OFF;
}

static void SynthForget()
{
    g_synth_reported     = false;
    g_synth_signature    = 0;
    g_synth_last_probe   = 0;
    g_synth_last_print   = 0;
    g_synth_last_verdict = 0;
}

static void OnSynthInitEffectRuntime(reshade::api::effect_runtime *)
{
    // A new runtime has its own texture list, so whatever was reported belonged
    // to something else.
    SynthForget();
}

static void OnSynthReloadedEffects(reshade::api::effect_runtime *)
{
    // Every effect_texture_variable handle ReShade handed out before this is now
    // a pointer into a freed vector. Nothing is cached across frames here for
    // exactly that reason; this only forces the answer to be stated again,
    // because a reload is the most likely moment for it to have changed.
    SynthForget();
}

// The ReShade motion-vector providers. Lifted out of OnSynthBeginEffects
// unchanged so the driver's optical flow engine can be tried first and this can
// be the else -- the loop and its reasoning are the code that was already here.
//
// The first name that exists is the answer only if it is also bound. Marty's
// qUINT_of.fx and his current iMMERSE Launchpad are routinely installed side
// by side, and the older file declares texMotionVectors at index 0 whether
// or not its technique is on -- so stopping at the first declaration reports
// "present but NOT BOUND" over a Launchpad that is running and bound at
// index 1. A declaration is kept as the fallback so "not present at all"
// stays a distinct answer.
//
// From kSynthMvReShadeFirst: row 0 of that table is the driver's own optical
// flow engine, this add-on creates the texture it writes into, and there is no
// name to hand find_texture_variable. See the note on the table.
static void OnSynthMvWalk(reshade::api::effect_runtime *rt, reshade::api::device *dev,
                          SynthFacts *f, SynthBound *mv_b)
{
    reshade::api::resource_view mv_srv = { 0 };
    for (int i = kSynthMvReShadeFirst; i < static_cast<int>(_countof(kSynthMv)); ++i)
    {
        reshade::api::resource_view v = { 0 };
        if (!SynthLookup(rt, kSynthMv[i].name, &v)) continue;
        if (f->mv_which < 0) { f->mv_which = i; mv_srv = v; }
        if (v.handle != 0)   { f->mv_which = i; mv_srv = v; break; }
    }
    f->mv_bound = (mv_srv.handle != 0) ? 1 : 0;
    SynthResolveView(dev, f->device_api, mv_srv, mv_b);
}

// The third parameter is the render target ReShade is about to apply its effects
// to, which on a D3D11 device is a plain ID3D11RenderTargetView over the back
// buffer. That is the only colour surface this add-on can reach, so it is both
// the Color the synthetic contract reads and the Output it writes back into.
// The fourth is the same target viewed as sRGB and describes no different
// resource, so nothing here wants it.
// ---------------------------------------------------------------------------
// Releasing the source latch
//
// The mirror latch is taken by the render thread the first time the game calls
// NGX, and until now nothing ever put it back. That was right while the game's
// DLSS was a fixed property of the session and wrong the moment a user can
// toggle it in a settings menu: turning DLSS off left g_source at SRC_MIRROR
// with nothing to mirror, and the synthetic path could not re-arm because
// arming requires SRC_NONE. Both paths silent, the panel still saying Mirror.
// Reported on Baldur's Gate 3, 2026-08-31.
//
// Detection is g_eval_count standing still. It is the only signal there is: no
// *_ReleaseFeature entry point is hooked anywhere in the tree -- the Vulkan set
// is Evaluate/Evaluate_C/Create/Create1 -- and adding one means a new patch site
// in the machinery nine shipped D3D11 titles run on, for a signal that is
// ambiguous anyway, because a resolution change is a release followed at once by
// a create.
//
// This runs on the present thread only, which is already the only writer of
// SRC_SYNTH, so it is also the only writer of SRC_NONE. The render thread stays
// the only writer of SRC_MIRROR.
// ---------------------------------------------------------------------------

static void SynthReleaseLatch(ULONGLONG quiet_ms)
{
    // Before anything is written. g_s12.ready and g_svk.ready are one-way behind
    // their tried flags, and the Vulkan mirror shares both with the synthetic
    // transport -- so a transport the mirror killed is dead for the process.
    // Releasing into that hands the frame to something that cannot deliver: the
    // panel would flip to "Synthetic" and still show nothing, which is the same
    // bug one layer down. tried && !ready is the test, not !ready: a transport
    // that was never opened may still open.
    const char *dead = g_bridge.disabled               ? "D3D11 bridge"
                     : (g_s12.tried && !g_s12.ready) ? "private D3D12"
                     : (g_svk.tried && !g_svk.ready) ? "Vulkan import" : nullptr;
    if (dead != nullptr)
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            Log("[synth] the game's own DLSS has gone quiet, but the %s session failed "
                "earlier this run and is not retried, so releasing the latch would hand "
                "the frame to a transport that cannot deliver. The latch stays with the "
                "mirror and the game keeps rendering on its own.", dead);
        }
        return;
    }

    bool released = false;
    LONG n = 0;

    EnterCriticalSection(&g_bridge_cs);
    const LONG evals   = InterlockedCompareExchange(&g_eval_count, 0, 0);
    const LONG creates = InterlockedCompareExchange(&g_create_count, 0, 0);
    g_eval_base   = evals;
    g_create_base = creates;

    // The mirror's own SRC_SYNTH -> SRC_MIRROR undo, run in the other direction:
    // a zero width forces one ordered rebuild on whichever side builds next,
    // because a feature built for the game's contract is not a feature for a
    // DLAA contract over ReShade's depth even when the shapes happen to match.
    g_s12.w = 0;
    g_s12.need_reset = true;

    // And the D3D11 side, which is the same two fields the mirror's own handover
    // uses in the other direction. Without them a D3D11 game whose DLSS is
    // switched off keeps the feature the mirror built for the game's contract and
    // the synthetic path arms onto it: a feature made for one contract, fed the
    // other. Clearing frame_ready forces the rebuild; need_reset throws away a
    // temporal history accumulated from vectors the new source did not produce.
    g_bridge.frame_ready = false;
    g_bridge.need_reset  = true;

    // CAS and not an exchange, so this can only ever release the mirror.
    if (InterlockedCompareExchange(&g_source, SRC_NONE, SRC_MIRROR) == SRC_MIRROR)
    {
        // The one window there is: the increment at the top of the evaluate
        // detour happens before its latch write, so an evaluate that raced this
        // has already moved the counter. Put the latch back and say nothing --
        // both interleavings end at SRC_MIRROR, and the zeroed width costs that
        // frame the same rebuild the mirror's own handover would have cost.
        if (InterlockedCompareExchange(&g_eval_count, 0, 0) != evals)
            InterlockedExchange(&g_source, SRC_MIRROR);
        else
        {
            released = true;
            n = ++g_release_n;
        }
    }
    LeaveCriticalSection(&g_bridge_cs);

    // Outside the section: no file I/O under a lock the render thread holds
    // across GPU work.
    if (!released) return;
    if (n <= 3)
        Log("[synth] the game's own DLSS has not evaluated for %llu ms, so it is "
            "switched off rather than between frames. The source latch is released: the "
            "synthetic contract may arm again, and the game's next evaluate takes the "
            "mirror straight back.", quiet_ms);
    else if (n == 4)
        Log("[synth] the source latch has been released four times this session; further "
            "releases are not reported. The status panel shows which source is running now.");
}

static void OnSynthBeginEffects(reshade::api::effect_runtime *rt,
                                reshade::api::command_list   *cmd,
                                reshade::api::resource_view   rtv,
                                reshade::api::resource_view)
{
    // Said at most once, and from here because this is the first place in the
    // process where every add-on has certainly finished loading. See the note on
    // the function: it warns, it does not prevent.
    WarnIfOldCopyLoaded();

    if (!SynthEnabled() || rt == nullptr || cmd == nullptr) return;

    // The only per-frame present-thread code this add-on has, so it is the only
    // place the question can be asked: the panel draw runs only while the
    // overlay is open, and there is no timer thread by design. Everything BELOW
    // the mirror return measures the frame; this measures the game, which is the
    // one thing that can end a mirror session.
    {
        static LONG      last_evals;
        static ULONGLONG quiet_since;
        static LONG      quiet_ticks;
        const LONG       evals = InterlockedCompareExchange(&g_eval_count, 0, 0);
        const ULONGLONG  now   = GetTickCount64();

        if (evals != last_evals || quiet_since == 0)
        { last_evals = evals; quiet_since = now; quiet_ticks = 0; }
        else ++quiet_ticks;

        // Five seconds AND thirty observed presents. The wall clock alone is not
        // enough: five seconds of a game at three frames a second is a loading
        // screen, and thirty ticks means the game is really presenting before
        // the silence is believed. Five seconds is also 3.5x the worker's whole
        // bounded window (kVkmIdleMs), so by construction the worker is parked on
        // its wake event before anything here writes.
        //
        // The config pair repeats the first two tests of SynthArmRefusal on
        // purpose: without it a mirror-only session -- the shipped synth_after=0
        // -- would release, gain nothing, and pay a rebuild hitch after every
        // loading screen. If those tests ever move, this is the other copy.
        if (InterlockedCompareExchange(&g_source, 0, 0) == SRC_MIRROR &&
            now - quiet_since >= 5000 && quiet_ticks >= 30 &&
            g_cfg.synth_after > 0 &&
            (g_cfg.source == CFG_SRC_SYNTH || g_cfg.source == CFG_SRC_AUTO) &&
            g_reshade_api >= kSynthApiFloor)
        {
            // D3D11 as well as Vulkan. It was Vulkan-only when this was written,
            // and the first D3D11 user to switch DLSS off in a settings menu found
            // what that meant: the panel kept saying Mirror and nothing was
            // delivered, because arming needs SRC_NONE and nothing put it back.
            // The undo is the same two fields on either transport, and the nine
            // shipped titles cannot reach this at all -- it needs synth_after
            // above 0, and 0 is the shipped default.
            reshade::api::device *d = rt->get_device();
            const bool releasable =
                d != nullptr &&
                (d->get_api() == reshade::api::device_api::vulkan ||
                 d->get_api() == reshade::api::device_api::d3d11);
            if (releasable)
                SynthReleaseLatch(now - quiet_since);
            quiet_since = now; quiet_ticks = 0;   // one attempt per window
        }
    }

    // While the mirror owns the session nothing this function measures can arm
    // anything -- every report during it is a true answer to a question no longer
    // being asked. The latch is not permanent any more: SynthReleaseLatch hands
    // the session back when the game's own DLSS goes quiet, and the probe resumes
    // with it, because this returns on the state rather than on a one-time flag. Baldur's Gate 3 on 2026-08-31 printed six of them in forty seconds
    // AFTER "the synthetic contract stands down for good", each one honest and
    // each one noise: the depth binding really does alternate as generic_depth
    // swaps its backup, so keying the rate limit on the verdict cannot help when
    // it is the verdict that is oscillating. The fix is not a longer floor, it
    // is not asking.
    if (InterlockedCompareExchange(&g_source, 0, 0) == SRC_MIRROR) return;

    SynthFacts f = {};
    f.api      = g_reshade_api;
    f.mv_which = -1;
    f.depth_which = -1;

    if (f.api < kSynthApiFloor)
    {
        // Said once and then never again: below the floor no effect_runtime call
        // is safe, so there is nothing further to measure and nothing that can
        // change.
        if (!g_synth_reported) { SynthReport(&f); g_synth_reported = true; }
        return;
    }

    reshade::api::device *dev = rt->get_device();
    f.device_api = (dev != nullptr) ? static_cast<uint32_t>(dev->get_api()) : 0u;
    if (!SynthApiHasBackend(f.device_api))
    {
        if (!g_synth_reported) { SynthReport(&f); g_synth_reported = true; }
        return;
    }

    rt->get_screenshot_width_and_height(&f.back_w, &f.back_h);
    SynthReadDefs(rt, &f);

    // Two name lookups per frame, three when the first motion-vector name is
    // absent or declared without a binding.
    // Each is a walk of ReShade's texture list. That is the whole per-frame cost
    // of this file during an ordinary mirror session.
    // Same shape as the motion-vector walk below and for the same reason: two
    // effects can each declare a ": DEPTH" texture while only one of them is
    // bound, so a declaration is kept only as the fallback answer.
    reshade::api::resource_view depth_srv = { 0 };
    for (int i = 0; i < static_cast<int>(_countof(kSynthDepthNames)); ++i)
    {
        reshade::api::resource_view v = { 0 };
        if (!SynthLookup(rt, kSynthDepthNames[i], &v)) continue;
        if (f.depth_which < 0) { f.depth_which = i; depth_srv = v; }
        if (v.handle != 0)     { f.depth_which = i; depth_srv = v; break; }
    }
    f.depth_found = (f.depth_which >= 0) ? 1 : 0;
    f.depth_bound = (depth_srv.handle != 0) ? 1 : 0;

    // Moved above the motion-vector walk, unchanged otherwise. The optical flow
    // pre-gate below asks SynthArmRefusal, and every test it does not skip is a
    // depth test, so the depth facts have to be filled before it is asked.
    SynthBound depth_b = {};
    SynthResolveView(dev, f.device_api, depth_srv, &depth_b);

    f.depth_w = depth_b.w; f.depth_h = depth_b.h;
    f.depth_fmt = depth_b.fmt; f.depth_samples = depth_b.samples;
    f.depth_is_dsv = depth_b.is_dsv;
    f.depth_usage  = depth_b.usage;

    // The driver's own optical flow engine first, and it is not consulted in a
    // session that would not deliver anyway: the pre-gate is every arming test
    // except the four about the motion vector texture, which is the one thing it
    // is about to produce. Without it every mirror session in the nine shipped
    // titles would pay 1.6 ms a frame building a flow field nobody reads.
    //
    // Once it is running the three ReShade names are not consulted again --
    // including on a frame whose execute failed, and including after it has
    // given up for good. Swapping the estimator under a running DLSS feature
    // would hand it a temporal history accumulated from one estimator and a new
    // frame from another, which is the same class of defect as the smear this
    // whole path was rebuilt to fix. g_ofa.stopped is what says so; ready alone
    // cannot, because it is also false before the first open.
    // (ready || !tried) is the whole of "may this be attempted". It says three
    // things at once and each one is a case: running, so keep going; never
    // attempted, so open it; attempted once and failed, so never again -- the
    // same rule Synth12Open and SynthVkOpen follow. stopped is a different
    // question and is asked below, because a first open that failed must still
    // fall back to a ReShade provider while a session that gave up must not.
    // Asked before the two branches below rather than inside either, because
    // neither of them can answer it. With a session running and ofa_grid edited
    // to 0 the pre-gate refuses on ofa_grid, the fallback refuses on ready, and
    // the result is no motion vectors from anywhere and no line saying why --
    // synthesis stops for the rest of the session on a documented off switch.
    // tried is cleared with it so setting the key back to 1, 2 or 4 opens the
    // session again: that flag is there to stop a failed open being retried
    // every frame, not to make a user's own edit one-way.
    if (g_ofa.ready && g_cfg.ofa_grid == 0)
    {
        Log("[synth] optical flow: ofa_grid=0 in dlss5-bridge.cfg, so the session is "
            "closed and the motion vectors come from a ReShade effect again.");
        OfaClose();
        g_ofa.tried = false;
    }

    SynthBound mv_b = {};
    if (f.device_api == static_cast<uint32_t>(reshade::api::device_api::d3d11) &&
        g_cfg.ofa_grid != 0 && (g_ofa.ready || !g_ofa.tried) &&
        SynthArmRefusal(&f, /*ignore_mv=*/true) == nullptr &&
        OfaFrame(rt, cmd, rtv, f.back_w, f.back_h, &mv_b))
    {
        f.mv_which = 0;
        f.mv_bound = 1;
    }
    // g_ovk.ready OR still allowed to try, which is what breaks the circle.
    //
    // The engine opens inside SynthVkFrame, and SynthVkFrame is only reached once
    // the arming gate has passed, and the gate wanted motion vectors -- which the
    // engine is what supplies. So on a machine with no ReShade motion-vector
    // shader installed, nothing ever armed and the engine never got a first
    // chance: the user had to enable a shader once purely to open the door, then
    // remove it. Measured on Baldur's Gate 3, 2026-08-31.
    //
    // Requiring only that it is ALLOWED to try lets the first frames build the
    // transport and open the engine. Those frames deliver nothing -- the first
    // execute has no previous frame to compare against, which is already this
    // path's rule -- and from the next one the vectors are real. If the engine
    // then refuses to open, SynthOfaVkIsSource stays false and SynthVkFrame
    // stands the transport down rather than evaluating over an unwritten texture;
    // that branch already existed and is what makes this safe to try.
    //
    // ofa_grid=0 is still an off switch, and a session that STOPPED is not
    // retried -- g_ovk.stopped is one-way and is what tried collapses to.
    else if (f.device_api == static_cast<uint32_t>(reshade::api::device_api::vulkan) &&
             g_cfg.ofa_grid != 0 && !g_ovk.stopped &&
             (g_ovk.ready || !g_ovk.tried) &&
             SynthArmRefusal(&f, /*ignore_mv=*/true) == nullptr)
    {
        // The Vulkan transport's own session, which opened inside SynthVkFrame
        // once the shared textures existed. Its vectors are already where the
        // evaluate reads them, so there is no ReShade texture to look up and
        // none is looked up: the walk below is not run and the three provider
        // names are not consulted again for the rest of the session.
        //
        // res stays null on purpose. On this transport the contract's
        // MotionVectors handle is never dereferenced -- SynthVkFrame skips the
        // copy and Synth12EvaluateShared sets the key to g_s12.tex[SLOT_MV]
        // itself -- and putting a D3D12 pointer into a field every other line in
        // this file reads as a ReShade Vulkan resource would be a trap rather
        // than a value.
        f.mv_which = 0;
        f.mv_bound = 1;
        mv_b.w       = f.back_w;
        mv_b.h       = f.back_h;
        mv_b.fmt     = DXGI_FORMAT_R16G16_FLOAT;
        mv_b.samples = 1;
        mv_b.usage   = 0;
    }
    else if (!g_ofa.ready && !g_ofa.stopped)
    {
        // Said once, and only after the same pre-gate the engine itself is held
        // behind, so a mirror session that never wanted synthesis never prints
        // it. Which of the three reasons applies is read off the same values the
        // branch above tested, not off a second lookup.
        static bool said;
        if (!said && SynthArmRefusal(&f, /*ignore_mv=*/true) == nullptr)
        {
            said = true;
            if (f.device_api == static_cast<uint32_t>(reshade::api::device_api::vulkan))
                Log("[synth] optical flow: this runtime is on Vulkan, where the engine runs "
                    "on a private D3D11 device that shares this transport's own textures -- "
                    "and those textures do not exist until the first frame is built, which "
                    "needs a contract, which needs this gate to pass. So the FIRST arming "
                    "still needs a ReShade motion-vector texture; from the frame after it "
                    "the driver supplies the vectors and no ReShade texture is read again.");
            else if (f.device_api != static_cast<uint32_t>(reshade::api::device_api::d3d11))
                Log("[synth] optical flow: this runtime is on %s, and the optical flow "
                    "function list this add-on recovered is the D3D11 one. The motion "
                    "vectors come from a ReShade effect.", SynthApiName(f.device_api));
            else if (g_cfg.ofa_grid == 0)
                Log("[synth] optical flow: ofa_grid=0 in dlss5-bridge.cfg, so the "
                    "driver's optical flow engine is not used and the motion vectors come "
                    "from a ReShade effect.");
            else
                Log("[synth] optical flow: the session did not open -- the reason is the "
                    "line above this one. Falling back to the motion-vector texture a "
                    "ReShade effect provides. That estimator is what produced the smearing "
                    "this path was rebuilt to fix, so a report of melting detail from here "
                    "is expected rather than new.");
        }
        OnSynthMvWalk(rt, dev, &f, &mv_b);
    }

    f.mv_w = mv_b.w; f.mv_h = mv_b.h;
    f.mv_fmt = mv_b.fmt; f.mv_samples = mv_b.samples;
    f.mv_usage = mv_b.usage;

    // Above the gates below, and that is the whole reason it sits here rather
    // than beside the report it depends on: those hold the report down to one
    // per change, and a frame is not news. Both textures are still held, and
    // neither is AddRef'd again on the way into the parameter block.
    // Published here rather than after the depth readback below, so the panel
    // and the delivery decision are looking at one measurement: this is the
    // exact struct SynthArmRefusal is about to be asked about, and the panel
    // asks the same function the same question.
    g_synth_panel_facts    = f;

    SynthMaybeDeliver(rt, cmd, rtv, &f, &depth_b, &mv_b);

    if (mv_b.tex11 != nullptr) mv_b.tex11->Release();
    ID3D11Texture2D *depth_tex = depth_b.tex11;

    // The readback stalls the GPU, so it is not part of what is measured every
    // frame. It runs only when everything cheap already differs from the last
    // report, which means a depth buffer that goes flat without any other fact
    // changing -- a cutscene on a flat backdrop, say -- is not noticed until
    // something else moves. Widening that needs a real sampling policy, not a
    // shorter interval.
    const uint64_t sig = SynthSignature(&f);
    if (g_synth_reported && sig == g_synth_signature) { if (depth_tex) depth_tex->Release(); return; }

    // A game whose depth selection flaps between candidates would otherwise
    // change the signature every frame and stall on every frame with it.
    // Two floors, not one. The five-second floor is how often the facts may be
    // re-measured; the thirty-second one is how often the SAME VERDICT may be
    // re-printed. They are different questions, and running them together
    // reprinted a twenty-line report every fifteen seconds in Baldur's Gate 3 --
    // because generic_depth swaps its backup between frames, depth_usage
    // oscillates between 0x0 and 0x4C0, and the signature covers every byte of
    // SynthFacts, so it flipped forever while the story never changed.
    //
    // The second floor is keyed on the verdict and on nothing else, so a report
    // whose meaning changed prints immediately no matter how recent the last one
    // was. That is the whole claim, and the first attempt at it did not hold it:
    // see g_synth_last_verdict.
    const ULONGLONG now = GetTickCount64();
    if (g_synth_reported && now - g_synth_last_probe < 5000)
    {
        if (depth_tex) depth_tex->Release();
        return;
    }
    g_synth_last_probe = now;

    // The readback happens before the print decision, not after it, because the
    // verdict is not known until it has. depth_read and depth_varies are what
    // separate "flat" from "ok", and a rate limit that ran first would compare
    // two verdicts that both still said "unread" and hold a real change back.
    if (depth_tex != nullptr)
    {
        SynthProbeDepth(cmd, depth_tex, &f);
        depth_tex->Release();
    }

    // Now the verdict exists, so "has anything the report SAYS changed?" is a
    // comparison. Same verdict inside thirty seconds: stay quiet. Anything else:
    // print at once, however recently the last one went out.
    const uint32_t verdict = SynthVerdictKey(&f);
    if (g_synth_reported && now - g_synth_last_print < 30000 &&
        verdict == g_synth_last_verdict)
        return;

    g_synth_last_print   = now;
    g_synth_last_verdict = verdict;

    SynthReport(&f);
    // The signature stored is the one taken before the readback, not a fresh one
    // taken after. Recomputing here folds depth_lo and depth_hi into the stored
    // value while the next frame's comparison still has them at zero, so the two
    // can never match and the report fires again every five seconds for the rest
    // of the session.
    g_synth_signature = sig;
    g_synth_reported  = true;
}

// Registration is a plain GetProcAddress from the same module the add-on
// registration came from, matching what RegisterWithReShade already does. The
// SDK's own register_addon is not used and neither is its register_event:
// ReShadeRegisterEvent resolves the owning add-on from the callback's address,
// which lands in this module, which is registered.
typedef void (*PFN_ReShadeRegisterEvent)(reshade::addon_event, void *);


static void SynthRegisterEvents()
{
    if (!SynthEnabled())
    {
        Log("[synth] not registering for ReShade events: %s.",
            g_cfg.stage == 0 ? "stage=0" : "source=off");
        return;
    }
    if (g_reshade_dll == nullptr) return;

    auto reg = reinterpret_cast<PFN_ReShadeRegisterEvent>(
        GetProcAddress(g_reshade_dll, "ReShadeRegisterEvent"));
    if (reg == nullptr)
    {
        Log("[synth] this ReShade exports no ReShadeRegisterEvent, so no feasibility "
            "report can be produced. The D3D11 mirror is unaffected.");
        return;
    }

    reg(reshade::addon_event::init_effect_runtime,    reinterpret_cast<void *>(&OnSynthInitEffectRuntime));
    reg(reshade::addon_event::reshade_reloaded_effects, reinterpret_cast<void *>(&OnSynthReloadedEffects));
    reg(reshade::addon_event::reshade_begin_effects,  reinterpret_cast<void *>(&OnSynthBeginEffects));


    // Both numbers, because "registered" alone does not say whether the calls
    // this file needs are the ones that will be made. ReShadeUnregisterAddon
    // drops these again at detach, so there is no unregister here.
    SynthLatchSelfCheck();
    Log("[synth] registered for ReShade effect events at add-on API %u (synth floor %u).",
        g_reshade_api, kSynthApiFloor);
}

// ---------------------------------------------------------------------------
// The status panel
//
// There are several paths now -- mirror or synthetic, D3D11 or D3D12 or Vulkan
// -- and until this existed the only way to know which one a session was on was
// to find and read the log. That made "which of these are you on" the first
// question on every support report. This answers it before anyone has to ask.
//
// Read only, and deliberately. The config file stays the single source of truth
// for every value shown; a second way to set one is a second thing that can
// disagree with the first. Every line here says what IS, and where it came from.
//
// It lives at the end of this file rather than in one of its own because
// everything it reads that is not in g_bridge is file-static here: SynthFacts,
// SynthArmRefusal, SynthApiName. The refusal sentence in particular is reused
// rather than restated -- a second copy of those tests would drift from the one
// the delivery path actually applies, and the panel would then contradict the
// log it exists to replace.
// ---------------------------------------------------------------------------

// ReShade hands its ImGui entry points to an add-on as a flat struct of function
// pointers, keyed by the ImGui version the add-on asks for. This is not the
// effect_runtime situation the note at the top of this file argues about. There
// the slot count was implicit, undocumented, and had moved between ReShade 5.9
// and 6.0, so the only defence was letting the compiler count. Here the layout
// is named by a number passed in and validated before anything comes back, the
// historical layouts are kept forever -- imgui_function_table.cpp still serves
// the 18600 table 6.0.0 shipped -- and a wrong number is a clean refusal rather
// than a jump into the wrong slot.
//
// Pinned to 19000 rather than to the newest, 19250. ReShade serves 19250 from
// every build and 19000 from every build except "Release Signed", so 19000 is
// the table that exists on ReShade 6.0.0 through 6.8.0 while 19250 exists on
// 6.7.0 and newer only. 6.0.0 is the floor the rest of this file already
// targets. A panel whose whole reason for being is to answer a support question
// must not be the first thing missing on the build the report came from.
static const uint32_t kPanelImGuiVersion = 19000;

// Six members are named and the rest is padding, because six are all this panel
// calls. Every one takes const char *, bool *, int * or int, returns void, bool
// or const char *, and touches no ImGui type -- no ImVec2, no ImVec4, no flag
// typedef -- which is what makes hand-declaring them exact rather than
// approximate. SmallButton is deliberately absent: there is no Save, no Reload
// and no Reset to defaults, because a radio click IS the save and CfgReload IS
// the reload. BeginDisabled is deliberately absent too: a control that cannot
// take effect is printed as text instead of drawn greyed, which a support report
// can be copied from and which keeps 150 slots of padding out of this struct.
//
// Each ordinal is written exactly once, in the enum, and every pad length is
// derived from it -- so pad arithmetic cannot be got wrong, only an ordinal can,
// and the offsetof asserts below cannot catch that because they compare two
// numbers derived from the same belief. The monotonicity assert is what catches
// a transposed pair, a duplicated constant, or an ordinal past the end.
//
// Ordinals counted from imgui_function_table_19000, which has 412 members. That
// file has exactly two commits in ReShade's history -- 8c49c67 (2024-01-06) and
// af334eb (2024-10-27) -- and the member list is identical across both: 412
// names in the same order, with only type spellings changed, none of them below
// ordinal 297. No widget ordinal has moved in any shipped ReShade 6.x. Re-count
// against source/imgui_function_table_19000.hpp only if this list changes.
// Truncating after the last one used is safe because this struct is only ever
// read through a pointer ReShade owns and never allocated.
enum : size_t {
    kOrdGetVersion      =   3,
    kOrdSameLine        =  85,
    kOrdTextUnformatted = 106,
    kOrdSeparatorText   = 113,
    kOrdCheckbox        = 118,
    kOrdRadioButton2    = 122,
    kOrdTableMembers    = 412,
};

struct PanelImGui
{
    const void *pad_a[kOrdGetVersion - 1];                             // 1..2
    const char *(*GetVersion)();                                       // 3
    const void *pad_b[kOrdSameLine - kOrdGetVersion - 1];              // 4..84
    void (*SameLine)(float, float);                                    // 85
    const void *pad_c[kOrdTextUnformatted - kOrdSameLine - 1];         // 86..105
    void (*TextUnformatted)(const char *, const char *);               // 106
    const void *pad_d[kOrdSeparatorText - kOrdTextUnformatted - 1];    // 107..112
    void (*SeparatorText)(const char *);                               // 113
    const void *pad_e[kOrdCheckbox - kOrdSeparatorText - 1];           // 114..117
    bool (*Checkbox)(const char *, bool *);                            // 118
    const void *pad_f[kOrdRadioButton2 - kOrdCheckbox - 1];            // 119..121
    bool (*RadioButton2)(const char *, int *, int);                    // 122
};

static_assert(offsetof(PanelImGui, GetVersion)      == (kOrdGetVersion      - 1) * sizeof(void *),
              "GetVersion is ordinal 3 of imgui_function_table_19000");
static_assert(offsetof(PanelImGui, SameLine)        == (kOrdSameLine        - 1) * sizeof(void *),
              "SameLine is ordinal 85 of imgui_function_table_19000");
static_assert(offsetof(PanelImGui, TextUnformatted) == (kOrdTextUnformatted - 1) * sizeof(void *),
              "TextUnformatted is ordinal 106 of imgui_function_table_19000");
static_assert(offsetof(PanelImGui, SeparatorText)   == (kOrdSeparatorText   - 1) * sizeof(void *),
              "SeparatorText is ordinal 113 of imgui_function_table_19000");
static_assert(offsetof(PanelImGui, Checkbox)        == (kOrdCheckbox        - 1) * sizeof(void *),
              "Checkbox is ordinal 118 of imgui_function_table_19000");
static_assert(offsetof(PanelImGui, RadioButton2)    == (kOrdRadioButton2    - 1) * sizeof(void *),
              "RadioButton2 is ordinal 122 of imgui_function_table_19000");

static_assert(kOrdGetVersion < kOrdSameLine && kOrdSameLine < kOrdTextUnformatted &&
              kOrdTextUnformatted < kOrdSeparatorText && kOrdSeparatorText < kOrdCheckbox &&
              kOrdCheckbox < kOrdRadioButton2 && kOrdRadioButton2 <= kOrdTableMembers,
              "panel ordinals must be strictly increasing and inside the 412-slot table");

// Null until a table has been obtained, and nothing registers the overlay
// without one. Every call in the draw goes through this pointer, so registering
// with it null would fault on the first widget.
static const PanelImGui *g_panel_gui;

// ReShade's table exports TextV, not Text: every formatting entry point takes a
// va_list, so there is no varargs Text to call. This is the route, not a
// workaround for one.
static void PanelLine(_Printf_format_string_ const char *fmt, ...)
{
    char b[640];
    va_list a;
    va_start(a, fmt);
    _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a);
    va_end(a);
    g_panel_gui->TextUnformatted(b, nullptr);
}

// ImGui derives a widget's identity from its label, so every option string has
// to be unique across the whole window -- "auto" appears in three groups. The
// ## suffix is ImGui's own way to say that: everything after it is identity and
// is not displayed.
static const char *kPanelSrcNames[]  = { "auto##src", "mirror only##src",
                                         "synthetic only##src" };
static const int   kPanelSrcValues[] = { CFG_SRC_AUTO, CFG_SRC_MIRROR, CFG_SRC_SYNTH };
static const char *kPanelGridNames[] = { "off (worse)##grid", "1 finest##grid",
                                         "2 recommended##grid", "4 cheapest##grid" };
static const int   kPanelGridValues[]= { 0, 1, 2, 4 };
static const char *kPanelPerfNames[] = { "SLOW##perf", "MEDIUM##perf", "FAST##perf" };
static const int   kPanelPerfValues[]= { 5, 10, 20 };
static const char *kPanelMvxNames[]  = { "auto##mvx", "+1##mvx", "-1##mvx" };
static const char *kPanelMvyNames[]  = { "auto##mvy", "+1##mvy", "-1##mvy" };
static const int   kPanelMvValues[]  = { 0, 1, -1 };

// The only place SameLine and RadioButton2 are called. v is always a local
// snapshot, never &g_cfg.something: RadioButton2 writes through the pointer it
// is given, and pointing it at g_cfg would make the panel a second writer of
// the struct CfgReload owns -- a second source of truth in one line.
static bool PanelRadioRow(const char *label, int *v, const char *const *names,
                          const int *values, int n)
{
    const PanelImGui *ui = g_panel_gui;
    ui->TextUnformatted(label, nullptr);
    bool hit = false;
    for (int i = 0; i < n; ++i)
    {
        ui->SameLine(0.0f, -1.0f);          // ImGui's own defaults for both
        if (ui->RadioButton2(names[i], v, values[i])) hit = true;
    }
    return hit;
}

// One outstanding write, and it is a receipt rather than a setting: nothing in
// this add-on reads it to decide behaviour, it feeds one line of text, and it
// clears itself the moment g_cfg agrees. There is nowhere else for a second copy
// of a setting to live, because an immediate-mode radio drawn from a per-draw
// snapshot holds no state between frames.
static struct {
    const char *key;
    char        word[16];
    int         value;
    ULONGLONG   tick;
    bool        ok;
    char        why[160];
} g_panel_receipt;

// Frames put into the game's output, and when that count last moved. Cumulative
// totals cannot tell a session working now from one that stopped ten minutes
// ago, which is the first thing an "it stopped working" report needs. Display
// only; nothing reads either to decide anything.
static LONG      g_panel_last_out;
static ULONGLONG g_panel_last_out_tick;

// The diagnostics disclosure. A file-static, not a config key: it decides
// nothing but whether the last block draws, and a seventh setting in the file
// would be a second thing to keep in step for no gain.
static bool g_panel_diag;

// Formats the value itself and hands CfgSetKey a string. word is the file's own
// spelling for source=, which is a word rather than a number.
static void PanelSet(const char *key, int value, const char *word)
{
    g_panel_receipt.key   = key;
    g_panel_receipt.value = value;
    g_panel_receipt.tick  = GetTickCount64();
    g_panel_receipt.why[0] = 0;
    if (word != nullptr)
        strcpy_s(g_panel_receipt.word, sizeof(g_panel_receipt.word), word);
    else
        _snprintf_s(g_panel_receipt.word, sizeof(g_panel_receipt.word), _TRUNCATE, "%d", value);
    g_panel_receipt.ok = CfgSetKey(key, g_panel_receipt.word,
                                   g_panel_receipt.why, sizeof(g_panel_receipt.why));
}

// Three separate facts, tested in this order: the write returned, g_cfg agrees,
// and -- for the two flow settings -- the engine agrees, which the session line
// above the control already reports off g_ofa rather than off g_cfg, so the
// two-stage adoption is visible without any machinery here.
//
// g_cfg_last_read is the exact discriminator between "the validator rejected it"
// and "nothing has read the file yet": CfgReload sets it unconditionally once
// past its gate, so a value still zero seconds after a write means nothing
// called CfgReload at all. That is the honest answer for stage=0, for
// source=off, and for a paused game.
static void PanelReceipt(const char *key, int live)
{
    if (g_panel_receipt.key == nullptr || strcmp(g_panel_receipt.key, key) != 0) return;

    if (!g_panel_receipt.ok)
    {
        PanelLine("Not saved: %s.", g_panel_receipt.why);
        return;
    }
    if (live == g_panel_receipt.value) { g_panel_receipt.key = nullptr; return; }

    const ULONGLONG now = GetTickCount64();
    if (now - g_panel_receipt.tick <= 2000)
        PanelLine("Saved. Applies on the next frame.");
    else if (g_cfg_last_read == 0)
        PanelLine("Saved, not read yet: no frame has been through.");
    else
        PanelLine("%s=%s written but not adopted; the log says why.",
                  key, g_panel_receipt.word);
}

// The one line that makes the ten file-only keys safe to leave file-only: the
// escape hatches stay out of reach of a casual click, but they can no longer
// silently corrupt a bug report. Silent when nothing is off its default, because
// absence is the good state.
static void PanelNonDefault(char *out, size_t n)
{
    const struct { const char *k; int v, d; } t[] = {
        { "stage",       g_cfg.stage,       3 },
        { "mode",        g_cfg.mode,        2 },
        { "skip_game",   g_cfg.skip_game,   1 },
        { "flags",       g_cfg.flags,      -1 },
        { "subrects",    g_cfg.subrects,    1 },
        { "reset_every", g_cfg.reset_every, 0 },
        { "pixels",      g_cfg.pixels,      0 },
        { "dred",        g_cfg.dred,        1 },
        { "skip_exe",    g_cfg.skip_exe,    1 },
        { "unwrap",      g_cfg.unwrap,      1 },
    };
    out[0] = 0;
    for (size_t i = 0; i < _countof(t); ++i)
    {
        if (t[i].v == t[i].d) continue;
        char one[48];
        _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%s=%d",
                    out[0] != 0 ? ", " : "", t[i].k, t[i].v);
        strncat_s(out, n, one, _TRUNCATE);
    }
    if (g_cfg.source == CFG_SRC_OFF)
        strncat_s(out, n, out[0] != 0 ? ", source=off" : "source=off", _TRUNCATE);
}

// ReShade opens and closes the window around this, so nothing here may open one
// of its own; it draws widgets into the window that is already begun. It runs on
// the game's presenting thread, between ImGui::NewFrame and ImGui::Render, while
// the render thread is free to mutate everything it reads.
//
// So every value is snapshotted once, at the top, and nothing below reads a
// global twice: a panel that read g_source twice could print a mirror session's
// contract under a synthetic heading. No lock is taken -- g_bridge_cs and
// g_hook_cs are both held across GPU work by the render thread, and blocking
// present on either would turn a diagnostic into a stall -- and no resource
// pointer is dereferenced. The two pointers read at all, g_bridge.feature and
// g_s12.feature, are compared against null and never followed.
static void OnPanelDraw(reshade::api::effect_runtime *rt)
{
    const PanelImGui *ui = g_panel_gui;
    if (ui == nullptr) return;

    const LONG src     = InterlockedCompareExchange(&g_source, 0, 0);
    const LONG releases = InterlockedCompareExchange(&g_release_n, 0, 0);
    const LONG creates = InterlockedCompareExchange(&g_create_count, 0, 0);
    const LONG evals   = InterlockedCompareExchange(&g_eval_count, 0, 0);
    const LONG out     = InterlockedCompareExchange(&g_frames_delivered, 0, 0);
    const LONG flags   = InterlockedCompareExchange(&g_panel_flags, 0, 0);

    // A whole-struct copy, so SynthArmRefusal below is asked about one frame's
    // measurement rather than about fields collected across several.
    const SynthFacts facts = g_synth_panel_facts;

    const bool   b_session  = g_bridge.session_ready;
    const bool   b_disabled = g_bridge.disabled;
    const bool   b_frame    = g_bridge.frame_ready;
    const bool   b_feature  = g_bridge.feature != nullptr;
    const UINT   b_w  = g_bridge.width,     b_h  = g_bridge.height;
    const UINT   b_ow = g_bridge.out_width, b_oh = g_bridge.out_height;
    const UINT   b_rw = g_bridge.render_w,  b_rh = g_bridge.render_h;
    const UINT   b_nw = g_bridge.ngx_out_w, b_nh = g_bridge.ngx_out_h;

    const bool   s_tried   = g_s12.tried;
    const bool   s_ready   = g_s12.ready;
    const bool   s_shared  = g_s12.shared;
    const bool   s_feature = g_s12.feature != nullptr;
    const UINT   s_w = g_s12.w, s_h = g_s12.h;
    // Read separately, because they are not the same numbers. The synthetic
    // callers pass w,h for both -- DLAA, no upscale -- so a panel written
    // against that path printed the render size twice and was right by
    // coincidence. The Vulkan mirror carries the game's own ratio, and printed
    // "2560x1067 in, 2560x1067 out" over a log line saying 2560x1067 -> 3840x1600.
    const UINT   s_ow = g_s12.out_w, s_oh = g_s12.out_h;

    // The config as it stands, and the flow engine as it is actually running.
    // All plain scalars, no pointer, exactly the argument SynthFacts already
    // makes above: the worst a reader can see is a mix of two frames' numbers.
    // The two are deliberately separate -- g_cfg is the file, g_ofa is the
    // engine, and a setting written this frame is in the first and not yet in
    // the second, which is a state the panel has to be able to show.
    const int  c_source = g_cfg.source, c_after = g_cfg.synth_after;
    const int  c_grid   = g_cfg.ofa_grid, c_perf = g_cfg.ofa_perf;
    const int  c_mvx    = g_cfg.mv_sign_x, c_mvy = g_cfg.mv_sign_y;
    const UINT o_w = g_ofa.w, o_h = g_ofa.h, o_grid = g_ofa.grid;
    const int  o_perf   = g_ofa.perf;
    // The frequency is asked of the OS rather than read from g_bridge, which sets
    // it only inside TimingTick -- the D3D11 bridge's per-frame timing, which
    // never runs on a Vulkan session. The ticks and the frame count ARE written
    // there, by OfaFrameOn, which both transports share: so on Vulkan the panel
    // said "not measured yet" over a cost the log was printing in the same
    // second. A process-constant is not something to inherit from another path's
    // start-up.
    LARGE_INTEGER f_freq = {};
    QueryPerformanceFrequency(&f_freq);
    const LONGLONG f_qpf = f_freq.QuadPart, f_ticks = g_bridge.ofa_ticks;
    const UINT64   f_frames = g_bridge.ofa_frames;

    // What the widgets are bound to. Never &g_cfg.anything: RadioButton2 and
    // Checkbox write through the pointer they are given, and the panel writes
    // the file, not the struct.
    int  w_source = c_source, w_grid = c_grid, w_perf = c_perf;
    int  w_mvx = c_mvx, w_mvy = c_mvy;
    bool w_after = c_after > 0;

    // Whether there is a file to write at all. When the folder was never
    // writable CfgWriteDefault failed at attach, CfgReload has been returning on
    // a failed open ever since, and this session is on built-in defaults -- so
    // every control group is replaced by its text form and the build section
    // says why. One GetFileAttributesA per draw, and only while the overlay is
    // open.
    char cfg_path[MAX_PATH];
    CfgPath(cfg_path);
    const bool cfg_writable = GetFileAttributesA(cfg_path) != INVALID_FILE_ATTRIBUTES;

    // Asked of the runtime rather than taken off the last measurement, because
    // the measurement only happens when synthesis is switched on -- and the
    // backend is the one line this panel has to be able to state in a session
    // that has never enabled it. Guarded by the same API floor
    // SynthRegisterEvents checks: below that, effect_runtime's vtable is not the
    // one the vendored headers describe and get_device is a different slot.
    uint32_t dev_api = 0;
    bool     dev_api_known = false;
    if (rt != nullptr && g_reshade_api >= kSynthApiFloor)
    {
        reshade::api::device *dev = rt->get_device();
        if (dev != nullptr)
        {
            dev_api = static_cast<uint32_t>(dev->get_api());
            dev_api_known = true;
        }
    }

    // Which of the two feature blocks holds this session's numbers, and the test
    // is on the TRANSPORT rather than on the source. It used to read
    // src == SRC_SYNTH, which quietly meant "D3D11 mirror" and was true of the
    // mirror only there: the Vulkan mirror runs through Synth12Open,
    // Synth12Build and Synth12EvaluateShared and never writes a byte of
    // g_bridge, so a working mirror read the D3D11 block, found it empty, and
    // printed "No contract yet" and "D3D12 session: not opened yet" over its own
    // log saying it had built one and was delivering frames. This panel exists
    // to tell the user which situation they are in; contradicting the log is the
    // one thing it must not do.
    const bool on_s12 = dev_api_known &&
                        dev_api != static_cast<uint32_t>(reshade::api::device_api::d3d11) &&
                        (src == SRC_SYNTH || src == SRC_MIRROR);

    // Whether the driver's optical flow engine can run here at all. The function
    // list this add-on recovers from nvofapi64 is the D3D11 one, and on Vulkan the
    // synthetic path opens it on a private D3D11 device that aliases the two
    // shared textures -- see SynthOfaVkOpen -- so it runs there too, and ofa_grid
    // and ofa_perf are live controls on both. Only D3D12 is not wired.
    //
    // This read D3D11 alone, which made a Vulkan session print "the driver's
    // optical flow engine" as its motion-vector provider and then, three lines
    // later, that the engine could not run there -- while hiding the cost line
    // and both controls that were working.
    // From the struct that belongs to THIS transport. The engine has two sessions
    // -- g_ofa is the D3D11 one, g_ovk the private D3D11 device the Vulkan path
    // aliases the shared textures onto -- and this read only the first. On Vulkan
    // that made the panel print "Optical flow opens on the first synthetic frame"
    // for the rest of the session, over a g_ovk that had already tried and
    // failed, while the log said exactly what went wrong. The panel's whole job
    // is to say which situation the session is in.
    const bool on_vk_ofa = dev_api_known &&
                           dev_api == static_cast<uint32_t>(reshade::api::device_api::vulkan);
    const bool o_ready   = on_vk_ofa ? g_ovk.ready   : g_ofa.ready;
    const bool o_stopped = on_vk_ofa ? g_ovk.stopped : g_ofa.stopped;
    const bool o_tried   = on_vk_ofa ? g_ovk.tried   : g_ofa.tried;

    const bool ofa_here = dev_api_known &&
                          (dev_api == static_cast<uint32_t>(reshade::api::device_api::d3d11) ||
                           dev_api == static_cast<uint32_t>(reshade::api::device_api::vulkan));

    ui->SeparatorText("source");
    if (src == SRC_MIRROR)
    {
        // Which of the two it is, from the predicate the delivery path itself uses.
        // This said "forwarded untouched" unconditionally, which is false under the
        // shipped skip_game=1: there the game's own evaluate is suppressed because
        // this add-on's result replaces it.
        if (BridgeWillDeliver())
            PanelLine("Mirror. The game's own DLSS is suppressed, because this add-on's "
                      "result replaces it (skip_game=1). %ld creates, %ld evaluates.",
                      creates, evals);
        else
            PanelLine("Mirror. The game's own DLSS, forwarded untouched. %ld creates, "
                      "%ld evaluates.", creates, evals);
        PanelLine("Full quality: the engine's own jitter, motion vectors and upscaling "
                  "ratio.");
    }
    else if (src == SRC_SYNTH)
    {
        // "has no DLSS" and "its DLSS is switched off" are different sentences
        // and the user can tell them apart; after a release only the second one
        // is true.
        PanelLine(releases > 0
            ? "Synthetic. The game's own DLSS is switched off, so this add-on builds one."
            : "Synthetic. The game has no DLSS, so this add-on builds one.");
        PanelLine("Reconstructed from the finished frame, so below what a game's own DLSS "
                  "gives.");
        PanelLine("DLAA: same resolution in and out. It stabilises, it does not upscale.");
    }
    else
    {
        // The one sentence the delivery path itself refuses on, asked here of a
        // copy of the same measurement. Restating those tests in this file is
        // exactly the drift the panel exists to remove.
        // It returns null when every precondition holds and the delivery still
        // bailed later -- a render target of a size nothing was measured against,
        // or a Vulkan image that cannot be copied. Those are logged and they
        // persist, so the panel must not print "(null)" for the rest of the
        // session.
        const char *why = SynthArmRefusal(&facts, false);
        // Without this the panel says "not running" and leaves the reason to a
        // log the user is not reading: the game's own DLSS is off, and turning
        // it back on is the whole fix.
        if (releases > 0)
            PanelLine("The game's own DLSS is switched off right now; it was on earlier "
                      "this session. Turning it back on takes the mirror back immediately.");
        PanelLine("Not running: %s.",
                  why != nullptr ? why : "the frame was refused later; the log says why");
    }

    // The two launch settings, next to the line that reports what they decided.
    // Live whenever no source holds the session -- before the first DLSS call, and
    // again after SynthReleaseLatch hands it back. While one holds it, a source
    // change can only be a request for the next launch. off is not offered --
    // SynthRegisterEvents refuses outright on source=off, so the click could not
    // be undone without restarting the game, and the line below names the file.
    //
    // With no writable file there is no control to draw at all, so the reason
    // stands in their place rather than being buried in diagnostics.
    if (!cfg_writable)
    {
        PanelLine("No dlss5-bridge.cfg beside this add-on: nothing here can be saved.");
    }
    else if (src == SRC_NONE)
    {
        if (PanelRadioRow("Source", &w_source, kPanelSrcNames, kPanelSrcValues, 3))
            PanelSet("source", w_source, kCfgSourceName[w_source]);
        PanelLine("auto: the game's own DLSS first, synthetic only if it never asks.");
        PanelLine("Switch off entirely with source=off in dlss5-bridge.cfg.");
        PanelReceipt("source", c_source);

        // A checkbox, not a seconds spinner: the number's only job is "long
        // enough to be sure the game has no DLSS of its own". The seconds stay a
        // file edit, and the line below prints whatever the file holds.
        if (ui->Checkbox("Add DLAA to a game that has no DLSS of its own", &w_after))
            PanelSet("synth_after", w_after ? 10 : 0, nullptr);
        if (c_after > 0)
            PanelLine("Waits %d s for the game's own DLSS, then builds one.", c_after);
        PanelReceipt("synth_after", c_after);
    }
    else
    {
        PanelLine("dlss5-bridge.cfg: source=%s, synth_after=%d. Change it there, "
                  "before launch.", kCfgSourceName[c_source], c_after);
    }

    // Never on the mirror: optical flow does not run there. Drawn before the
    // source has latched too, because that is the state a first-time user is in
    // and the settings still apply to it.
    if (src == SRC_SYNTH || src == SRC_NONE)
    {
        ui->SeparatorText("motion vectors");

        // Which of the two providers is running is the largest single
        // determinant of image quality on this path, so it is stated with its
        // quality rather than as a neutral name. The drift line is drawn only on
        // the engine: the shader path has its own, worse symptom named above it.
        if (src == SRC_SYNTH)
        {
            if (facts.mv_which == 0)
            {
                PanelLine("Motion vectors: the driver's optical flow engine. Sub-pixel "
                          "accurate.");
                PanelLine("Fine detail still drifts slightly under very slow motion. No "
                          "setting removes it.");
            }
            else if (facts.mv_which > 0 &&
                     facts.mv_which < static_cast<int>(_countof(kSynthMv)))
            {
                PanelLine("Motion vectors: the ReShade shader %s -- the lower-quality "
                          "source.", kSynthMv[facts.mv_which].name);
                // The half-resolution, no-sub-pixel mechanism was measured of
                // vort_Motion.fx and of nothing else; kSynthMv[2] and [3] are a
                // texture name each, with no estimator behind them this add-on
                // has measured. So the symptom is said for all three and the
                // mechanism only where it was seen.
                if (facts.mv_which == 1)
                    PanelLine("Half resolution, no sub-pixel: fine detail smears under "
                              "slow camera motion.");
                else
                    PanelLine("Fine detail smears under slow camera motion.");
            }
            else
                PanelLine("No motion-vector source found.");
        }

        // The engine, not the file. A setting written this frame is in g_cfg and
        // not yet here, and this line is what makes that two-stage adoption
        // visible without any machinery for it.
        if (!ofa_here)
        {
            if (dev_api_known)
                PanelLine("The driver's optical flow engine is not wired on a D3D12 "
                          "runtime, so it cannot run here.");
        }
        else if (o_ready)
            PanelLine("Optical flow: grid %u, %s, %ux%u vectors.",
                      o_grid, SynthPerfName(o_perf),
                      o_grid != 0 ? (o_w + o_grid - 1) / o_grid : 0,
                      o_grid != 0 ? (o_h + o_grid - 1) / o_grid : 0);
        else if (c_grid == 0)
            PanelLine("Optical flow off: ofa_grid=0. Set detail to 2 to use the driver's "
                      "engine.");
        else if (o_stopped || o_tried)
            PanelLine("Optical flow did not run this session; the log says why.");
        else
            PanelLine("Optical flow opens on the first synthetic frame.");

        // TimingTick zeroes both counters every 600 frames, so this is a rolling
        // window and the line says so rather than implying a lifetime average.
        if (ofa_here && o_ready)
        {
            if (f_frames != 0 && f_qpf != 0)
                PanelLine("Cost: %.2f ms per frame, over the last %llu frames.",
                          1000.0 * double(f_ticks) / double(f_qpf) / double(f_frames),
                          f_frames);
            else
                PanelLine("Cost: not measured yet.");
        }

        // Radios rather than a slider: every legal value is whitelisted in
        // CfgReload, and a slider would let a user select ofa_grid=3, which the
        // validator silently rejects -- the panel would then show a value the
        // add-on is not using.
        if (ofa_here && cfg_writable)
        {
            if (PanelRadioRow("Detail", &w_grid, kPanelGridNames, kPanelGridValues, 4))
                PanelSet("ofa_grid", w_grid, nullptr);
            if (PanelRadioRow("Effort", &w_perf, kPanelPerfNames, kPanelPerfValues, 3))
                PanelSet("ofa_perf", w_perf, nullptr);
            // The two settings multiply, so neither can be judged alone and one
            // help line is shared. Nothing measured in a real session says a user
            // should move either: FAST and MEDIUM were indistinguishable, and the
            // drift they would be reached for is structural.
            PanelLine("Defaults are right for almost everyone. Changing either reopens the "
                      "flow session.");
            PanelReceipt("ofa_grid", c_grid);
            PanelReceipt("ofa_perf", c_perf);
        }
        else if (ofa_here)
        {
            // Only reachable with no writable file, which is also the only way
            // g_cfg can be anything but the built-in defaults -- so naming the
            // file here would contradict the line in the source section that
            // says there is none.
            PanelLine("Built-in defaults: ofa_grid=%d, ofa_perf=%s.",
                      c_grid, SynthPerfName(c_perf));
        }

        // The forced sign is applied whatever the provider is, index 0 included,
        // and the engine's direction is a measurement rather than a convention --
        // so while it is running there is no control, only the pair it resolved
        // to and which of the two it is.
        if (o_ready)
        {
            PanelLine("Motion vector direction: %+d,%+d, %s.",
                      c_mvx != 0 ? c_mvx : kSynthMv[0].sign_x,
                      c_mvy != 0 ? c_mvy : kSynthMv[0].sign_y,
                      (c_mvx != 0 || c_mvy != 0)
                          ? "forced by mv_sign_x/mv_sign_y in dlss5-bridge.cfg"
                          : "measured");
        }
        else if (cfg_writable)
        {
            if (PanelRadioRow("Flip X", &w_mvx, kPanelMvxNames, kPanelMvValues, 3))
                PanelSet("mv_sign_x", w_mvx, nullptr);
            if (PanelRadioRow("Flip Y", &w_mvy, kPanelMvyNames, kPanelMvValues, 3))
                PanelSet("mv_sign_y", w_mvy, nullptr);
            PanelLine("Leave on auto. Flip an axis if the picture smears that way.");
            PanelReceipt("mv_sign_x", c_mvx);
            PanelReceipt("mv_sign_y", c_mvy);
        }
        else
        {
            PanelLine("Built-in defaults: mv_sign_x=%d, mv_sign_y=%d.", c_mvx, c_mvy);
        }
    }

    ui->SeparatorText("session");
    // Asked of the runtime rather than taken off the last measurement, because
    // the measurement only happens when synthesis is switched on. Before the
    // latch there is no transport to name, so that half is not printed.
    if (!dev_api_known)
        PanelLine("Runtime: unknown (add-on API %u).", g_reshade_api);
    else if (src == SRC_NONE)
        PanelLine("Runtime: %s.", SynthApiName(dev_api));
    else
        PanelLine("Runtime: %s. Transport: %s.", SynthApiName(dev_api),
                  dev_api == static_cast<uint32_t>(reshade::api::device_api::d3d11)
                      ? "D3D11 to this add-on's D3D12 device"
                      : dev_api == static_cast<uint32_t>(reshade::api::device_api::vulkan)
                          ? "Vulkan, imported D3D12 textures"
                          : "D3D12, no copy");

    if (on_s12 && s_w != 0)
        PanelLine("%ux%u in, %ux%u out.%s", s_w, s_h,
                  s_ow != 0 ? s_ow : s_w, s_oh != 0 ? s_oh : s_h,
                  (s_ow == s_w && s_oh == s_h) ? " No upscale: DLAA." : "");
    else if (!on_s12 && b_frame)
        PanelLine("%ux%u in, %ux%u out; %ux%u rendered into %ux%u.",
                  b_w, b_h, b_ow, b_oh, b_rw, b_rh, b_nw, b_nh);
    else
        PanelLine("No contract yet.");

    if (on_s12)
        PanelLine("D3D12 session: %s, feature %s.",
                  s_ready ? (s_shared ? "open on this add-on's own device"
                                      : "open on the game's own device")
                          : s_tried ? "failed, not retried" : "not opened yet",
                  s_feature ? "created" : "none");
    else
        PanelLine("D3D12 session: %s, feature %s.",
                  b_disabled ? "failed, not retried"
                             : b_session ? "open on this add-on's own device"
                                         : "not opened yet",
                  b_feature ? "created" : "none");

    {
        // Whether frames are still arriving, not only how many have. Two
        // file-statics, updated here and read nowhere else.
        const ULONGLONG now_t = GetTickCount64();
        if (out != g_panel_last_out) { g_panel_last_out = out; g_panel_last_out_tick = now_t; }
        if (out == 0)
            PanelLine("No frames delivered yet.");
        else if (now_t - g_panel_last_out_tick <= 2000)
            PanelLine("%ld frames delivered, still arriving.", out);
        else
            PanelLine("%ld delivered, none for %llu s.",
                      out, (now_t - g_panel_last_out_tick) / 1000);
    }

    // Not gated: this one describes the picture the user is looking at right
    // now, and it is silent when nothing is off its default.
    char nondefault[384];
    PanelNonDefault(nondefault, sizeof(nondefault));
    if (nondefault[0] != 0)
        PanelLine("Non-default keys: %s. Image problems may be these.", nondefault);

    // Everything a bug report needs and a running session does not. A checkbox
    // rather than a header, because Checkbox is already one of the six ordinals.
    ui->Checkbox("Diagnostics", &g_panel_diag);
    if (g_panel_diag)
    {
        PanelLine("dlss5-bridge %s, built %s, add-on API %u.",
                  BRIDGE_VERSION, __DATE__, g_reshade_api);
        if (flags >= 0)
        {
            char names[192];
            CreateFlagNames(names, sizeof(names), static_cast<unsigned int>(flags));
            PanelLine("Create flags %ld:%s", flags, names);
        }
        PanelLine("Log: %s", g_log_path);
        if (cfg_writable)
            PanelLine("Config: %s", cfg_path);
        if (g_panel_addons[0] != 0)
        {
            // Straight through: TextUnformatted takes the embedded newlines, so
            // there is nothing to split and no per-line loop to get wrong. This
            // add-on matches its own *.addon* scan, so the list always holds at
            // least itself and this is where a user sees whether the DLSS 5
            // add-on is present beside it.
            PanelLine("Add-ons in this folder:");
            ui->TextUnformatted(g_panel_addons, nullptr);
        }
        if (g_panel_dlss5[0] != 0)
        {
            PanelLine("[RenoDX.DLSS5]:");
            ui->TextUnformatted(g_panel_dlss5, nullptr);
        }
    }
}

typedef const void *(*PFN_ReShadeGetImGuiFunctionTable)(uint32_t);
typedef void (*PFN_ReShadeRegisterOverlay)(const char *,
                                           void (*)(reshade::api::effect_runtime *));

// Both exports are resolved from the same module the add-on registration came
// from, the way SynthRegisterEvents resolves ReShadeRegisterEvent. The callback
// address is what ReShade uses to work out which add-on owns the overlay, so
// this has to run after RegisterWithReShade succeeded -- which it does, from
// DllMain, beside the event registration.
//
// Nothing unregisters: ReShadeUnregisterAddon drops the overlay along with the
// events at detach.
static void PanelRegister()
{
    if (g_reshade_dll == nullptr) return;

    auto get_table = reinterpret_cast<PFN_ReShadeGetImGuiFunctionTable>(
        GetProcAddress(g_reshade_dll, "ReShadeGetImGuiFunctionTable"));
    auto reg = reinterpret_cast<PFN_ReShadeRegisterOverlay>(
        GetProcAddress(g_reshade_dll, "ReShadeRegisterOverlay"));

    // Not present, rather than not enabled: both exports live inside ReShade's
    // own GUI build switch, so a ReShade compiled without the overlay exports
    // neither and there is nothing here to fix.
    if (get_table == nullptr || reg == nullptr)
    {
        Log("[panel] this ReShade exports no %s, so it has no ImGui overlay to draw a "
            "status panel into. Everything else is unaffected.",
            get_table == nullptr ? "ReShadeGetImGuiFunctionTable" : "ReShadeRegisterOverlay");
        return;
    }

    // Refused cleanly, rather than faulted: ReShade validates the number, names
    // it in its own log, and returns nothing. Registering the overlay anyway
    // would fault on the first widget, because every call in the draw goes
    // through this pointer -- so the null check is what gates registration, and
    // not registering is the whole of the defence.
    g_panel_gui = static_cast<const PanelImGui *>(get_table(kPanelImGuiVersion));
    if (g_panel_gui == nullptr)
    {
        Log("[panel] this ReShade does not serve the ImGui %u function table, so no status "
            "panel is registered and its settings can only be changed in "
            "dlss5-bridge.cfg -- ReShade.log names the version it refused. The signed "
            "public build is the one that does this: it serves only the newest table. "
            "Everything else in this add-on is unaffected.", kPanelImGuiVersion);
        return;
    }

    // Until this panel gained controls it only read through the table. Checkbox
    // and RadioButton2 write through a bool * and an int *, so a table base that
    // is not the layout this struct describes would corrupt memory rather than
    // print nonsense. Ordinal 3 is the cheapest thing that says the base is
    // right. Not compared against a literal: the 19000 table forwards to
    // whatever ImGui ReShade was built with, so the string tracks ReShade's
    // build and not the table number -- only "it looks like a version" is
    // checkable here.
    const char *imgui_ver = g_panel_gui->GetVersion();
    if (imgui_ver == nullptr || *imgui_ver < '0' || *imgui_ver > '9')
    {
        Log("[panel] the ImGui %u function table's version entry returned %s, which is not "
            "a version string, so the table is not the layout this panel expects and no "
            "status panel is registered. Everything else in this add-on is unaffected.",
            kPanelImGuiVersion, imgui_ver != nullptr ? imgui_ver : "nothing");
        g_panel_gui = nullptr;
        return;
    }

    reg("DLSS 5 Bridge", &OnPanelDraw);
    Log("[panel] status panel registered in ReShade's overlay. Asked for the ImGui %u "
        "function table; ReShade reports its own ImGui as %s.",
        kPanelImGuiVersion, g_panel_gui->GetVersion());
}

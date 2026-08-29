// D3D11 -> D3D12 NGX bridge.
//
// The game drives DLSS through NVSDK_NGX_D3D11_EvaluateFeature_C. That call is
// intercepted and forwarded untouched, then this bridge reproduces the same
// contract on a second NGX session running on its own D3D12 device. RenoDX's
// DLSS 5 add-on detours the D3D12 entry points, so the D3D12 evaluate is where
// its neural-rendering pass is inserted.
//
// Per frame:
//   1. copy the game's Color / Depth / MotionVectors into shared textures
//   2. signal a shared fence on the D3D11 immediate context
//   3. wait on it from the D3D12 queue, run the D3D12 evaluate, signal back
//   4. wait on the D3D11 context, copy the result into the game's Output
//
// The textures are created on the D3D12 side with D3D12_HEAP_FLAG_SHARED and
// opened as D3D11 aliases, because the game's own textures carry MiscFlags = 0
// and cannot be shared directly.

#pragma once

enum { SLOT_COLOR = 0, SLOT_OUTPUT, SLOT_DEPTH, SLOT_MV, SLOT_COUNT };

static const char *kSlotKey[SLOT_COUNT]  = { "Color", "Output", "Depth", "MotionVectors" };
static const char *kSlotName[SLOT_COUNT] = { "Color", "Output", "Depth", "MV" };

struct Bridge
{
    bool disabled;          // set after a hard failure; never retried
    bool session_ready;     // device, queue, fences, NGX session
    bool frame_ready;       // shared textures and NGX feature match the game
    bool msaa_reported;     // the MSAA notice is said once per spell of it
    int  consecutive_fails;

    ID3D12Device              *dev12;
    ID3D12CommandQueue        *queue;
    ID3D12GraphicsCommandList *list;

    // A command allocator must not be reset while the GPU is still executing the
    // commands recorded in it -- doing so recycles that memory underneath the
    // GPU and it goes on to execute garbage. One allocator per frame in flight,
    // each remembering the fence value that retires it, so none is ever reused
    // early. The command list itself is safe to reset as soon as it is submitted.
    static const int           kFrames = 3;
    ID3D12CommandAllocator    *alloc[kFrames];
    UINT64                     alloc_fence[kFrames];
    int                        frame_slot;
    HANDLE                     fence_event;
    ID3D12Fence               *fence12;
    ID3D11Fence               *fence11;
    ID3D11DeviceContext4      *ctx4;

    // Engines that drive D3D11 from more than one thread turn on multithread
    // protection. The runtime then serialises individual calls, but a sequence
    // of them still needs holding as a unit or another thread can interleave
    // into the middle of the copy / signal / copy-back run.
    ID3D11Multithread         *mt;
    UINT64                     fence_value;

    PFN_D3D12CreateFeature   create_feature;
    PFN_D3D12EvaluateFeature eval_feature;
    PFN_D3D12ReleaseFeature  release_feature;
    PFN_AllocateParameters   alloc_params;

    NVSDK_NGX_Parameter *params;
    NVSDK_NGX_Handle    *feature;

    ID3D12Resource  *tex12[SLOT_COUNT];
    ID3D11Texture2D *tex11[SLOT_COUNT];
    HANDLE           shared[SLOT_COUNT];

    // Identity of the game textures this set was built for. BG3 recreates its
    // render targets mid-session, and Color/Output also swap every frame, so
    // the descriptor rather than the pointer is what has to match.
    // Texture sizes, which in an upscaling mode differ between input and output.
    UINT        width, height;          // Color/Depth/MV texture size
    UINT        out_width, out_height;  // Output texture size
    // What the game told NGX about the actual rendered area, which is smaller
    // than the texture whenever DLSS is upscaling rather than running as DLAA.
    UINT        render_w, render_h;
    UINT        ngx_out_w, ngx_out_h;
    DXGI_FORMAT fmt[SLOT_COUNT];

    bool   need_reset;      // NGX Reset flag for the first frame of a new set
    UINT64 frames_done;

    // Timing. The point is to separate two very different costs: work this code
    // does on the CPU, versus the GPU pipeline bubbles the cross-API fences
    // create. A small CPU figure next to a long frame interval means the cost is
    // the synchronisation, not anything the bridge computes.
    LONGLONG qpf;
    LONGLONG cpu_ticks;
    LONGLONG span_start;
    LONGLONG last_entry;
    UINT64   timed_frames;

    // The game's depth is R24G8_TYPELESS and D3D11 will not create a shared
    // texture in that format, so the shared copy is R32_FLOAT and a compute pass
    // converts into it. CopyResource cannot: the two formats are in different
    // typeless families.
    bool                       depth_converted;
    ID3D11ComputeShader       *depth_cs;
    ID3D11UnorderedAccessView *depth_uav;   // on the shared R32_FLOAT texture
    ID3D11UnorderedAccessView *mv_uav;      // the MV conversion pass writes the shared copy through it
    ID3D11ShaderResourceView  *depth_srv;   // on the game's depth
    ID3D11Resource            *depth_src;   // held so its pointer cannot be recycled

    // Some games hand NGX motion vectors in a format the driver will not share
    // -- R32G32_FLOAT, for one, is absent from the kernel's shared-format list,
    // and CreateTexture2D then rejects it outright. The shared MV copy is
    // R16G16_FLOAT, the DLSS-recommended motion-vector format, and a compute
    // pass converts into it, the same way depth is converted.
    bool                       mv_converted;
    ID3D11ComputeShader       *mv_cs;
    ID3D11ShaderResourceView  *mv_srv;      // on the game's MV
    ID3D11Resource            *mv_src;      // held so its pointer cannot be recycled

    // Last resort if even that fails: a zero-filled D3D12 texture completes the
    // NGX contract but leaves DLSS blind to disocclusion, which shows up as the
    // previous scene smearing into the new one.
    bool depth_placeholder;
};

static Bridge g_bridge;

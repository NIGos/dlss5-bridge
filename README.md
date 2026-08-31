# DLSS 5 Bridge

> ## v1.3.0 — the biggest release, and the last
>
> **Vulkan games are supported.** A Vulkan game's own DLSS contract is mirrored
> onto a private D3D12 session, so neural rendering runs over the game's own
> upscale at its own quality preset, with the engine's own jitter and motion
> vectors. Nothing reconstructed. On by default; `vk_mirror=0` turns it off.
>
> **Motion vectors come straight from the NVIDIA driver.** The optical flow engine
> in `nvofapi64.dll` is driven directly, so no ReShade motion-vector shader has to
> be installed. Works on D3D11 and on Vulkan; see Known limits for the different
> route each one takes.
>
> **A game with no DLSS at all can be given one.** The two above add up to a DLSS
> contract built from ReShade's depth and the driver's motion vectors, handed to
> the add-on as though the game had produced it. `synth_after`.
>
> All three are NVIDIA-only. The Vulkan mirror is on by default — hooking those
> entry points does nothing in a DirectX game, which has none — and the other two
> wait for `synth_after` and `ofa_grid` in the config file, which explains itself.
>
> **This is also the last feature release.** The DLSS 5 add-on now supports D3D11
> and D3D9 directly, which was this bridge's original reason to exist — for those
> games, use that add-on on its own; it is the one that gets updated. There is no
> active development after this. The repository stays up and stays MIT, the source
> is complete and builds from a clean copy with the command below, and issues can
> be opened and will be read — but assume nothing is fixed unless someone picks it
> up. Fork it freely.

A ReShade add-on that lets a DLSS 5 Neural Rendering add-on — which only hooks
DirectX 12 — run inside a game that renders with DirectX 11.

Tested on three titles, unevenly: Skyrim Special Edition on D3D11, for the synthetic
contract and the driver's optical flow, over extended play; Baldur's Gate 3 on
Vulkan, for the mirror and for the synthetic contract with the optical flow engine;
and Red Dead Redemption 2 on Vulkan, for the mirror over the game's own DLSS through
mode changes made mid-session. Nothing here is specific to any of them, but nothing
else has been tried.

## What it does

A DLSS 5 add-on works by detouring `NVSDK_NGX_D3D12_CreateFeature` and
`NVSDK_NGX_D3D12_EvaluateFeature` and inserting its neural-rendering pass into
them. A D3D11 game never calls those functions, so the add-on sits idle forever
showing "waiting for game DLSS".

This bridge intercepts the game's own `NVSDK_NGX_D3D11_EvaluateFeature_C`,
forwards it untouched, and then reproduces the same DLSS contract on a second
NGX session running on its own D3D12 device. That D3D12 evaluate is a genuine
NGX call, so the DLSS 5 add-on detours it and does its work. The result is
copied back into the game's own output texture.

The DLSS 5 add-on is not modified or patched in any way. It simply starts
receiving the calls it was always waiting for.

Per frame:

1. copy the game's Color and MotionVectors into shared textures
2. convert the game's depth into a shared `R32_FLOAT` texture with a compute
   shader — `CopyResource` cannot, the formats are in different typeless
   families. Which view format is legal depends on the game's depth format, so
   it is read from the texture rather than assumed
3. signal a fence shared between the D3D11 and D3D12 queues
4. run the D3D12 evaluate, which is where the DLSS 5 add-on inserts itself
5. signal back, and copy the result into the game's output

Every size, offset and scalar is read from the game's own NGX parameter block
and forwarded verbatim, so upscaling presets work as well as DLAA.

## Requirements

In the game folder, alongside the game executable:

| File | Where from |
| --- | --- |
| `dxgi.dll` — ReShade 6.0+ **with add-on support** | reshade.me, full version |
| a DLSS 5 Neural Rendering ReShade add-on | its own author |
| `nvngx_dlssnr.dll` | shipped with that add-on |
| `dlss5-bridge.addon64` | this package |

The DLSS 5 add-on's own neural-rendering toggle has to be enabled, either in
its ReShade overlay panel or in `ReShade.ini`.

The bridge itself needs a D3D11 game with native DLSS, a GPU and driver that
support D3D12, and `ID3D11Device5` for cross-API shared fences.

## Install

Drop `dlss5-bridge.addon64` next to ReShade. On first run it writes
`dlss5-bridge.cfg` with working defaults; nothing needs configuring. That file
explains itself — the two keys that decide whether anything happens are at the top
with a sentence each, and the rest is grouped by what it does.

**Upgrading from 1.0.x:** the add-on was called `dlss5-dx11-bridge.addon64` and
its settings file `dlss5-dx11-bridge.cfg`. **Delete the old `.addon64`.** ReShade
loads every add-on it finds, so leaving it there puts two copies of this in the
process, both writing over the same NGX entry points; the log says so if it
happens, but it cannot prevent it. The old `.cfg` is read as-is — settings carry
over with nothing to do — and can be deleted once the new one has been written.

To remove it, delete the file.

Nothing on disk is patched. The only writes to foreign code are 14 bytes at each
of three function entry points in every module that exports the NGX D3D11 API —
six such modules in Baldur's Gate 3, twelve at most — in memory, restored around
every call. `vk_mirror=1` adds four more per module.

## Configuration

`dlss5-bridge.cfg` is re-read while the game runs, so values can be
changed without restarting. Changes that only take effect on a new NGX feature
trigger a rebuild automatically.

| Key | Default | Meaning |
| --- | --- | --- |
| `stage` | 3 | How much of the bridge runs. `0` fully inert, `1` the input copies only, `2` also the depth conversion, `3` everything. Useful for isolating a problem: if `stage=0` still misbehaves, the bridge is not the cause. |
| `mode` | 2 | `0` never writes to the game, `1` transport only with no DLSS, `2` the full path. |
| `skip_game` | 1 | Do not forward the game's own DLSS evaluate. Its result is overwritten anyway, so running it is wasted work. Suppressed only while the bridge is healthy and already delivering. |
| `flags` | -1 | `DLSS.Feature.Create.Flags` for the bridge's feature. `-1` copies the game's own value. Any other value forces one, except `107`, which was this add-on's default before 1.0.16 and is treated as unset — use `108` to force that bit pattern deliberately. |
| `subrects` | 1 | Fallback for `DLSS.Enable.Output.Subrects`, used only when the game does not set one of its own. |
| `reset_every` | 0 | `1` forces the NGX Reset flag every frame, discarding temporal history. Diagnostic only. |
| `pixels` | 0 | `1` reads pixels back to the CPU for debugging. Stalls the GPU hard. |
| `vk_mirror` | 1 | `1` hooks the `NVSDK_NGX_VULKAN` entry points as well, and mirrors a **Vulkan** game's own DLSS contract onto the private D3D12 session. On by default — hooking them does nothing in a process that has none, which is every DirectX game — and read once at launch, not re-read while the game runs: it decides whether four more foreign entry points get a fourteen-byte jump written into them, which is a launch-time decision. Needs a Vulkan game with DLSS of its own, ReShade attached to the Vulkan runtime, and a game whose NGX depth aspect is four bytes per texel — `D32_SFLOAT`, `D32_SFLOAT_S8_UINT` or `D24_UNORM_S8_UINT`. A 16-bit depth aspect, and every other case, refuses by name in the log and leaves the game's own DLSS on screen. `stage` still applies: `2` runs everything but the copy back into the game's Output, `3` is the whole path. |
| `source` | `auto` | Which contract source may hold the session. `auto` lets the game's own DLSS win and falls back to the synthetic contract; `mirror` and `synth` pin one; `off` disables both. Read at launch and re-read every second. |
| `synth_after` | 0 | Seconds to wait, with no NGX call from the game, before the synthetic contract may arm. `0` — the shipped default — leaves the synthetic path off entirely. |
| `mv_sign_x` | 1 | Forces the X sign of the motion vectors instead of using the provider's documented convention. `1` keeps it, `-1` flips it. For diagnosing a provider whose sign this add-on has wrong. |
| `mv_sign_y` | 1 | The same for Y. |
| `probe` | 0 | `1` runs a standalone NGX D3D12 probe at attach and logs what the driver reports, then continues normally. A diagnostic; it changes nothing about how frames are handled. |
| `ofa_grid` | 2 | Output grid size for the driver's own optical flow engine, which supplies motion vectors on the synthetic path. `0` switches it off and falls back to a ReShade motion-estimation shader; `1`, `2` and `4` select the grid. Only read when the synthetic path is armed (`synth_after` above 0) — the mirror path never touches it. On a **Vulkan** runtime the engine runs on a private D3D11 device, over two textures of its own that the transport copies into and reads out of — see Known limits for why it cannot alias the transport's; on D3D12 the engine is not wired and the motion vectors come from a ReShade shader. Measured on an RTX 5090 at 3840x1600: grid 2 uses 109 MB, grid 4 uses 63 MB at roughly twice the error, and grid 1 buys nothing measurable over grid 2. Frame cost is not quoted here: the figures taken in a standalone harness and the one the add-on's own log reports measure different things and have never been reconciled, and the status panel reports the live one for the session in front of you. Any other value is ignored. |
| `ofa_perf` | 20 | `NV_OF_PERF_LEVEL` for that engine: `5` SLOW, `10` MEDIUM, `20` FAST — NVIDIA's own numbers, so a report quoting this can be read against their table directly. The slower levels estimate sub-pixel motion more accurately and are the knob to reach for when fine detail drifts or shimmers under very slow camera motion; at FAST the residual error is 0.03-0.09 px and systematic. Cost multiplies with `ofa_grid`, and SLOW is several times FAST at either grid. Numbers are not quoted here for the reason given in the `ofa_grid` row; the status panel reports the live cost for the session in front of you. Changing it closes and reopens the flow session. Any other value is ignored. |

## Status panel

Open ReShade's overlay and there is a **DLSS 5 Bridge** window. It answers,
without anyone having to find a log file, the question every support report
starts with: which of these paths is this session actually on.

- the source: mirror, synthetic, or neither yet, and how that was decided. When
  nothing is running it names the one precondition that refused — the same
  sentence the delivery path itself refuses on, not a second copy of it
- the backend: which API ReShade's effect runtime is on, and the transport that
  follows from it
- the contract: input and output dimensions, and the create flags with their
  bits spelled out
- motion vectors, on the synthetic path: which provider is producing them by
  name, whether the optical flow session is open and at what grid and effort,
  and what it measured as costing per frame
- session state: whether the D3D12 session is open, whether a feature exists,
  frames delivered, and whether frames are still arriving rather than only how
  many have
- every `*.addon*` beside this one and whether each is loaded, plus whatever
  `[RenoDX.DLSS5]` section `ReShade.ini` holds
- the build, the log path, the config path, and any diagnostic key that is off
  its default, so a screenshot identifies the session exactly

### Settings in the panel

Six keys have controls: `source` (`auto` / `mirror only` / `synthetic only`),
`synth_after` (as a checkbox writing `10` or `0`), `ofa_grid`, `ofa_perf`,
`mv_sign_x` and `mv_sign_y`.

`dlss5-bridge.cfg` is still the only place a value is set. The panel does
not hold a copy of any setting: a click writes that one line of the file, and
the file is re-read from the frame path exactly as it is after a text-editor
edit — same parser, same validators, same change log, same rebuild. So there is
one source of truth and one writer, a value the validator rejects simply is not
adopted, and a hand edit made while the panel is open wins with no merge rule to
document. Every other byte of the file is left alone, comments included.

The dividing line for what gets a control is not simple against advanced, it is
whether the key takes effect on a running session. `dred` and `unwrap` are read
at device creation, so a click on them would do nothing. `pixels` is re-read
every frame but only acts on frames 2 through 4, so it is a launch-time setting
in effect; `skip_exe` is re-read on every module scan, but a module it rejected
stays rejected. `stage`, `mode`, `skip_game`, `flags`, `subrects`,
`reset_every` and `source=off` stay file-only for a different reason: they are
diagnostic escape hatches that exist because a specific title broke, and a
wrongly-set one produces a bug report that describes the setting rather than the
bug. The panel names any of them that is off its default instead, in one line.

`source` and `synth_after` are offered whenever no source holds the session:
before the first DLSS call, and again after a Vulkan game's own DLSS has been
quiet long enough for the latch to be released. While a source holds it the panel
prints their values and says they apply to the next launch. The motion-vector sign controls are
offered only while the optical flow engine is *not* running, because its
direction is measured on the machine rather than assumed, and forcing a sign
there would invert vectors that are already correct.

There is no Save button, no Reload and no Reset to defaults: a click is the
save, the per-frame re-read is the reload, and deleting the file restores the
defaults on the next launch.

The panel asks ReShade for the ImGui 19000 function table, which every ReShade
from 6.0.0 to 6.8.0 serves. Two builds have no panel and say so in the log: one
compiled without its overlay, which exports nothing to register into, and
ReShade's own "Release Signed" build, which serves only the newest table. Both
refuse cleanly and nothing else in the add-on is affected.

## Log

`dlss5-bridge.log` records the contract read from the game, which
resource-sharing direction the driver accepted, the result of every NGX call,
and a timing line every 600 frames:

```
[bridge] 600 frames: bridge CPU 0.84 ms/frame | frame interval 16.00 ms (62.5 fps) | spread 5.74-29.93 ms | bridge is 5% of the frame | d3d12 43200/43202 (2 behind)
```

- **bridge CPU** is time spent inside this add-on, mostly waiting on the GPU
  rather than working. Read it next to the frame interval, not on its own.
- **spread** is the widest and narrowest gap between consecutive frames in the
  window. The average hides it, and it is what a driver-side frame generator
  responds to.
- **d3d12 N/M** is how far the D3D12 side is running behind. One to a few is
  ordinary pipelining. A gap that grows while the log then stops is the
  transport stalling; a small gap before a log stops dead means it is not.

## Performance

- The transport costs nothing measurable. With the D3D12 device, queue and
  allocators created but the evaluate disabled (`stage=2`), frame time matches
  the add-on being fully inert (`stage=0`).
- CPU time inside the add-on is well under a millisecond per frame. The rest is
  the neural pass on the GPU.
- How much that costs depends on scene, resolution, GPU and the DLSS 5 add-on's
  own settings, and varies enough between areas of one game that a single figure
  would mislead.

To measure it where you play: set `stage=0`, stand still, read a timing line;
set `stage=3`, do not move, read another. The file is re-read while the game
runs, so both come from one spot in one session.

## Related

[dlss5-d3d12-fix](https://github.com/NIGos/dlss5-d3d12-fix) fixes a different
failure of the same add-on: a DirectX 12 game whose DLSS output carries a mip
chain, which that add-on requires to be single-mip and silently refuses. If the
panel says STANDBY/FAILED rather than waiting for the game's DLSS, that is the
one to use.

## Building

Windows SDK and MSVC. No external dependencies; the ReShade add-on API is
reached through `GetProcAddress` and the NGX interfaces are declared inline.

From the `src` folder. `bridge.h`, `bridge.inc`, `synth.inc`, `vkmirror.inc`
and `depth_convert_spv.h` are pulled in by the `.cpp` and are not compiled
separately. `depth_convert.comp` is not on the build path at all: `cl` sees only
the generated array, and regenerating it needs `glslangValidator`, which this
build does not require and this machine does not have.

```
rc /nologo version.rc
cl /nologo /LD /EHsc /O2 /MT /std:c++17 dlss5-bridge.cpp ^
   /link /OUT:dlss5-bridge.addon64 version.res kernel32.lib user32.lib advapi32.lib
```

The version lives in two places that have to stay in step: `BRIDGE_VERSION` in
the `.cpp`, and the numbers in `version.rc`. The first is what the log prints,
the second is what ReShade's overlay shows.

## Reporting a problem

A screenshot of the status panel above answers the first question on its own.
For anything past that, post `dlss5-bridge.log`. It is written to answer the
usual questions without a conversation:

- the exact build, with its compile date
- the host executable and Windows version
- **which of NVIDIA's model files are present next to the add-on**, and every
  `*.addon*` in the folder — the most common cause of "it does nothing" is a
  missing `nvngx_dlssnr.dll` or no DLSS 5 add-on at all
- **which `d3d11.dll` the process is using** — a wrapper in the game folder
  (ENB, a proxy) rather than the one in System32
- every other ReShade add-on in the folder, so conflicts are visible
- the GPU and driver
- the NGX capabilities this GPU will agree to. `SuperSamplingDenoising.Available`
  is reported among them, but it describes Ray Reconstruction rather than
  neural rendering, so a `0` there does not by itself mean the feature is
  unavailable
- **every module exporting the NGX D3D11 API, and which of them were hooked** —
  one line per layer, with the entry-point addresses
- if none were found, every loaded module exposing NGX or Streamline
- if they were hooked but nobody called them within 60 seconds, an explicit
  note saying so — that is a different problem from failing to hook, and the
  log distinguishes them
- whether `sl.interposer.dll` is in the process. Streamline does reach this
  add-on — it links NVIDIA's NGX D3D11 client and calls the same entry points on
  the feature snippet — but the calls then come from Streamline rather than from
  the game, which is worth knowing when reading the parameter block

## Confirmed working

Reported by users, across seven unrelated engines:

| Title | Engine | DLSS from |
| --- | --- | --- |
| **Baldur's Gate 3** | Divinity 4.0 | the game |
| **Final Fantasy XIV Online** | in-house | the game |
| **The Legend of Heroes: Trails beyond the Horizon** | Falcom | the game — needed both fixes in 1.0.4 and 1.0.5, and is the reason they exist |
| **Tainted Grail: Fall of Avalon** | Unity | the game |
| **7 Days to Die** | Unity | the game |
| **Skyrim Special Edition** | Creation | a DLSS injector mod |
| **Fallout 4** | Creation | a DLSS injector mod |
| **S.T.A.L.K.E.R. Anomaly** | X-Ray | an upscaler injector mod (SSS24) |
| **Assetto Corsa** | kunOS | Custom Shaders Patch (Preview 338 or later) |

The last four matter for a second reason: they show the bridge picks up DLSS
that another mod provides, not only DLSS built into the game. Those setups reach
NGX by a different route — the mod links NGX statically and calls the feature
snippet directly, rather than through the driver's loader — which is why every
module exporting the API is hooked rather than one chosen by guesswork.

Nothing here targets a particular game. Every module exporting the NGX D3D11
API is hooked, and every size, format and offset is read from the parameter
block the caller passes. Where it has failed so far
it has been because something was hardcoded from the one game it was written
against — see 1.0.4 — so reports from new titles are useful even when they work.

## Third-party code

The ReShade add-on API headers under `src/reshade/` are Copyright 2014 Patrick Mours,
BSD 3-clause. Their licence text is in [src/reshade/LICENSE.md](src/reshade/LICENSE.md)
and must travel with any binary this project distributes.

The Vulkan mirror in `vkmirror.inc` carries no borrowed code, but two of its
ideas came from reading [AlanBacker/dlss5-vk-bridge](https://github.com/AlanBacker/dlss5-vk-bridge),
MIT, which is itself a Vulkan port of this project: creating the shared textures
on D3D12 and importing *those* into the game's `VkDevice` rather than importing
the game's own images, and parking the game's command buffer on a `VkEvent` pair
so the D3D12 evaluate can complete between the copies in and the copy back.
Three defects of theirs are deliberately not reproduced and are named in the
comments where each would have gone: a `DllMain` with no `DLL_PROCESS_DETACH`
branch, a park whose worst case exceeds the two-second Windows TDR, and the
non-keys `"Pre.Exposure"` / `"Exposure.Scale"`.

Two files of theirs *are* in this repository, under their licence: the depth
conversion shader the synthetic Vulkan path dispatches. `depth_convert.comp` and
the generated SPIR-V array in `depth_convert_spv.h` are their
`src/depth_convert.comp` and `src/depth_convert_spv.h`, verbatim, MIT, with the
full dual copyright — Alan Z, Vulkan port; NIGos, original DX11 bridge — carried
in the header of the array. They are taken rather than written because this
machine has no Vulkan SDK, no `glslangValidator`, no `shaderc` and no
`dxcompiler.dll`: a shader written here could be neither compiled nor validated,
and a hand-assembled SPIR-V module nothing on this machine can check is not a
safer artifact than a `glslang`-generated one its author compiled and ran
`spirv-val` over. A wrong word in a SPIR-V module is not a compile error; it is
the driver faulting inside its own shader compiler and reading as a crash caused
by this add-on. The array's header carries the regeneration command, the exact
word and byte counts and a SHA-256, and two `static_assert`s in `synth.inc` fail
the build if the file stops being that module.

Nothing else in this repository is third-party.

## Known limits

- **A game that drives DLSS from an exposure texture is refused by default.** The
  mirror does not carry that texture across, and delivering without it would be
  wrong brightness rather than a broken frame — the worse failure, because it looks
  like a decision somebody made. Red Dead Redemption 2 is such a game. To run
  anyway and let DLSS compute its own exposure, set `flags` to the game's own value
  with `AutoExposure` (`0x40`) added; the log line names the value when it refuses.
  Note that the arithmetically obvious result is often `107`, which this add-on
  treats as unset — Red Dead Redemption 2 declares `43`, so the value to set is
  `75`. On that title the result was reported as looking correct.
- The game's DLSS runs once and the bridge's runs once; with `skip_game=1` only
  the bridge's does. There is no path that avoids a second NGX session.
- Every fix is verified on one GPU — the mirror on Baldur's Gate 3 and Red Dead Redemption 2, the synthetic contract and the optical flow on Skyrim Special Edition (D3D11) and on Baldur's Gate 3 (Vulkan). The other titles in
  the table above are user reports, so a change that works here can still be
  wrong somewhere else.
- Resolution changes and DLSS preset changes are handled by rebuilding, but
  alt-tab and exclusive-fullscreen transitions are not specifically handled.
- Verbose logging is always on.
- The Vulkan mirror (`vk_mirror=1`) carries the 32-bit float depth aspect
  (`D32_SFLOAT`, `D32_SFLOAT_S8_UINT`) and the packed 24-bit one
  (`D24_UNORM_S8_UINT`) and no other. The packed case needs nothing new from the
  transport: the Vulkan specification addresses that depth aspect as
  `X8_D24_UNORM_PACK32`, one 32-bit word per texel with the depth in the low 24
  bits, so the same buffer and the same two copies carry it and a D3D12 compute
  pass masks and divides it into the `R32_FLOAT` the feature reads. A **16-bit**
  depth aspect still refuses by name, and for a different reason: it copies two
  bytes per texel, which the staging buffer and its texture are both sized wrong
  for.
- Only one NGX call runs at a time in the process. The mirror evaluates on a
  worker thread while the game evaluates on its own render thread, and until
  1.3.0 nothing kept the two apart. Red Dead Redemption 2 faults on that; other
  Vulkan titles were winning the race rather than being safe from it, which is
  why this is listed as a property of the design and not as a fixed bug. The cost
  is that a mirrored frame's evaluate can now wait for the game's, inside the
  same 1.2 s budget the park already had.

- The Vulkan mirror runs one frame in flight, so it costs one pipeline bubble
  per frame. It has been run against two Vulkan titles on one GPU — several
  sessions and roughly 25000 mirrored frames of Baldur's Gate 3, and Red Dead
  Redemption 2 through mode changes made mid-session — and no third has been
  tried.
- On the synthetic **Vulkan** path the driver's optical flow engine works, and the
  route it takes is worth recording because the obvious one does not. The engine
  needs a colour it can read and a motion-vector texture it can write, on a D3D11
  device. It used to try opening `ID3D11Texture2D` aliases of the two shared D3D12
  textures the transport already creates, through
  `ID3D11Device1::OpenSharedResource1`, and that call is refused with `E_INVALIDARG`
  (`0x80070057`) on the GPU and driver this was developed on. The same refusal is in
  every D3D11 session log: the mirror's `MakeSharedPair` tries that direction first,
  is refused for every texture and every format, and succeeds only by creating on
  D3D11 and opening on D3D12 — the line reading `via D3D11->D3D12`. So the engine now
  creates its own two textures the way the mirror does, in the direction this driver
  accepts, and the transport copies each frame's colour into the engine's texture on
  the D3D12 side under a CPU fence wait. NGX is handed the engine's motion-vector
  texture directly; there is no copy back.
- Arming on Vulkan no longer needs a ReShade motion-vector shader. The gate wanted
  motion vectors before it would build the transport, and the engine that supplies
  them opens inside the transport — so a machine with no such shader installed
  never armed and the engine never got a first chance. The gate now accepts a
  session on the expectation of the engine; the first frames build the transport
  and open it, deliver nothing, and the vectors are real from the next one. If the
  engine then refuses to open, the transport stands down rather than evaluating
  over an unwritten texture.
- The synthetic **Vulkan** path reads depth with a compute pass that samples it,
  so the depth binding no longer has to be 32-bit float and no longer has to be
  a copy source. What it still has to be is *the size of the back buffer*, and
  that is not a limitation anything here may remove: a game whose depth is
  smaller renders at one resolution and presents at another, which means it is
  running its own upscaler — and a game running its own DLSS is the mirror's
  case, not this one. Depth resampled to a size the game never rendered is
  invented data. So in Baldur's Gate 3 with its own DLSS **on**, the synthetic
  path still refuses, and now says only `the depth buffer is not the size of the
  back buffer`.
- That compute pass needs `VK_KHR_push_descriptor`. ReShade asks for it on every
  device it creates, but as an optional extension, so a driver without it gets a
  named refusal rather than a silent wrong picture.
If anything goes wrong the bridge disables itself and the game renders on its
own; it never leaves a broken frame on screen deliberately.

# DLSS 5 Bridge

**DLSS 5 Neural Rendering for DirectX 11 games, Vulkan games, and, as an
option, games that have no DLSS at all, through NVIDIA Optical Flow, at lower
quality.**

A ReShade add-on. The DLSS 5 neural rendering add-on only works where a game
runs DLSS on DirectX 12. This bridge gives it that: it mirrors a DirectX 11 or
Vulkan game's own DLSS onto a private DirectX 12 session, and for a game
without DLSS it can build a substitute one from ReShade's depth and the
driver's motion vectors. Nothing in the game is modified.

This bridge does not do neural rendering itself. It needs the separate **DLSS 5
Neural Rendering add-on** (RenoDX's `renodx-dlss5.addon64`), distributed in
[its Discord channel](https://discord.com/channels/1408098019194310818/1542647972695904317),
together with its `nvngx_dlssnr.dll`. The bridge only gives that add-on a
place to work.

If it is useful to you, you can help cover the AI tooling used in its
development:

[![Support on Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/nigos)

Releases and their notes: [github.com/NIGos/dlss5-bridge/releases](https://github.com/NIGos/dlss5-bridge/releases).

**Status.** Newer builds of the DLSS 5 add-on reach DirectX 11 and DirectX 9
on their own; the builds that do not still need this bridge, and it is kept
working for them. Issues are read and fixed, and releases continue. Every
release is run through [ngxGym](https://github.com/NIGos/ngxGym) on D3D11 and
Vulkan before it is published.

## What it does

The DLSS 5 add-on is not modified. It receives genuine NGX D3D12 calls on a
private D3D12 device, and its result is copied back into the game's output.
Three routes, chosen automatically:

| Route | Game | Contract |
| --- | --- | --- |
| **D3D11 bridge** | DirectX 11 with DLSS | The game's own, mirrored per frame: Color, Depth and MotionVectors copied into shared textures, evaluated on D3D12, copied back. Every size, offset and scalar comes from the game's parameter block. |
| **Vulkan mirror** | Vulkan with DLSS | The game's own, mirrored the same way through imported D3D12 textures. `vk_mirror=1`, the default. |
| **Substitute contract** | Any game with no DLSS, or with its DLSS switched off | DLAA at back-buffer size, built from ReShade's depth and the NVIDIA driver's optical flow. `synth=1`, off by default. |

The game's own DLSS always wins. The substitute takes the session only when
the game is not asking, and hands it back on the game's next call.

The substitute is a real DLSS feature fed approximated inputs, and it shows:
text softens and dense foliage smears. It is an option, not a default.

## Requirements

An NVIDIA GPU and driver with D3D12 support. In the game folder, beside the
executable:

| File | From |
| --- | --- |
| `dxgi.dll` — ReShade 6.0 or newer **with add-on support** | reshade.me, full version |
| A DLSS 5 Neural Rendering add-on (`renodx-dlss5.addon64`) | [its Discord channel](https://discord.com/channels/1408098019194310818/1542647972695904317). Any add-on that hooks the NGX D3D12 entry points is driven the same way |
| `nvngx_dlssnr.dll` | shipped with that add-on |
| `dlss5-bridge.addon64` | this project |
| `nvngx_dlss.dll` 3.1.13 or newer | the game, if it has DLSS. **Required only with `synth=1`**, and then also in a game without DLSS: the driver store carries no super-resolution snippet, and the substitute is DLAA, which older snippets accept and degrade. Below 3.x the bridge refuses and names the version. |

The neural add-on's own toggle has to be on, in its panel or in `ReShade.ini`.

**The neural add-on's build matters, and its version number does not identify
one.** Two builds of 2026-08-28 declare the same version; the newer one needs to
see this add-on's D3D12 device through ReShade's proxy, and underneath it
reports active and writes nothing. The log prints a SHA-256 for every add-on
beside this one, says whether that build has been measured here, and keeps the
proxy for a build that needs it.

## Install

Copy `dlss5-bridge.addon64` next to ReShade. On first run it writes
`dlss5-bridge.cfg` with working defaults; nothing needs configuring. To remove
the add-on, delete the file.

The settings file's first line is the version that wrote it. A different version
replaces the file with its own defaults on first run and says so in the log;
the same version never touches it. `# dlss5-bridge keep` as the first line
keeps a file across versions.

**Games without DLSS.** The substitute contract needs three files beside the
game's executable that such a game does not bring: the DLSS 5 add-on and its
`nvngx_dlssnr.dll`, and a **`nvngx_dlss.dll` of version 3.1.13 or newer**,
copied by hand from any game that has DLSS. The NVIDIA driver does not supply
that file, and NGX looks for it only in the executable's folder. Then turn on
the panel switch, or set `synth=1`. If the file is missing, the panel and the
log say so and name it.

**Upgrading from 1.1.0 or earlier:** the files were called
`dlss5-dx11-bridge.addon64` and `dlss5-dx11-bridge.cfg`. Delete the old
`.addon64` — ReShade loads both, and the older one ends up on screen. The old
`.cfg` is read where no current one exists.

Nothing on disk is patched. In memory, 14 bytes at each of three entry points
in every module exporting the NGX D3D11 API are replaced and restored around
each call; `vk_mirror=1` adds four entry points per module.

## Configuration

`dlss5-bridge.cfg` is re-read about once a second, so most keys take effect
without a restart. While a Vulkan mirror session holds the session the file is
not re-read until the game's DLSS goes quiet.

| Key | Default | Meaning |
| --- | --- | --- |
| `vk_mirror` | 1 | Hook the Vulkan NGX entry points and mirror a Vulkan game's own DLSS. Read once at launch. Depth aspects carried: `D32_SFLOAT`, `D32_SFLOAT_S8_UINT`, `D24_UNORM_S8_UINT`; a 16-bit aspect is refused by name. |
| `synth` | 0 | Allow the substitute contract, for a game with no DLSS and for a game whose DLSS is switched off. One switch in the panel. |
| `synth_after` | 0 | With `synth=1`, seconds of silence from the game before a substitute is built for a game that has never asked. `0` is the default 10 s. A delay, not an opt-in. |
| `source` | `auto` | `auto` lets the game's DLSS win and falls back to the substitute; `mirror` and `synth` pin one; `off` disables both. |
| `ofa_grid` | 2 | Grid of the driver's optical flow engine on the substitute path: `1`, `2`, `4`, or `0` to use a ReShade motion-vector shader instead. |
| `ofa_perf` | 20 | Optical flow effort, NVIDIA's own values: `5` slow, `10` medium, `20` fast. |
| `mv_sign_x`, `mv_sign_y` | 0 | Force the motion-vector sign (`1`, `-1`); `0` uses the provider's convention or the engine's measurement. Diagnostic. |
| `vk_present` | 0 | How the substitute's result reaches a Vulkan back buffer: `0` copies where the image allows it and draws it otherwise, `1` copies only, `2` draws always. |
| `vk_sync` | 0 | How the substitute's Vulkan transport orders itself against the private D3D12 device: `0` parks the game's queue on a Vulkan event while a worker thread runs the optical flow and the evaluate, `1` waits the queue idle on the CPU instead, `2` pipelines. `1` costs a full CPU-GPU serialisation every frame and is the fallback if the park misbehaves on a driver. `2` runs one frame's evaluate while the game renders the next, so the wait the game's queue reaches is on work that started a frame earlier rather than on work that starts when it gets there -- at the price of showing the result one present later, which is about 17 ms at 60 fps and 33 at 30, and one more Output texture. That texture is the back buffer's own size and format, so it is width x height x bytes per texel: 8.3 MB at 1920x1080 and 24.6 MB at 3840x1600 for a 4-byte format, double each on an RGBA16F back buffer. |
| `stage` | 3 | How much runs: `0` inert, `1` input copies, `2` plus depth conversion, `3` everything. Below `3` the substitute does not evaluate; below `2` the Vulkan mirror records nothing. |
| `mode` | 2 | `0` never writes to the game, `1` transport only, `2` the full path. |
| `skip_game` | 1 | Skip the game's own DLSS evaluate while the bridge is delivering; its result would be overwritten. |
| `flags` | -1 | `DLSS.Feature.Create.Flags`. `-1` copies the game's value. `107` is treated as unset (an old default); use `108` to force that pattern. |
| `subrects` | 1 | Fallback for `DLSS.Enable.Output.Subrects` when the game sets none. D3D11 bridge only. |
| `reset_every` | 0 | `1` sets the NGX Reset flag every frame. Diagnostic. |
| `pixels` | 0 | `1` reads pixels back to the CPU on frames 2 to 4. Diagnostic; stalls the GPU. |
| `dred` | 1 | Ask D3D12 to record what the GPU was executing, so a device reset can be explained. Read when the session opens. |
| `skip_exe` | 1 | `1` hooks the executable's own NGX exports only if no library exports them within a minute, so a game's image is not patched at startup. `0` hooks at once, `2` never. |
| `unwrap` | 1 | Hand NGX the D3D12 device underneath ReShade's proxy. `0` keeps the proxy. A neural add-on build measured to need the proxy, and ReShade loaded as `d3d12.dll`, both override `1` automatically -- so on those `0` is already in force and changes nothing. `2` strips the proxy anyway, which is the value to try when a session that should work does not. |
| `ngx_loader` | 0 | What to do about an NVIDIA driver whose NGX loader drives neural rendering into a snippet that faults (32.0.16.1664 and 1686 with `nvngx_dlssnr.dll` 310.8.0.0): `0` closes that route in memory at attach, so the DLSS 5 add-on drives the feature itself as it did on 32.0.16.1656; `1` leaves the driver alone; `2` loads the previous driver's loader instead, if the driver store still holds one. Applies only when a snippet measured to fault is beside the game. |
| `shape` | 0 | `1` builds the mirrored feature at the render region the game declares on each evaluate (`DLSS.Render.Subrect.Dimensions`) rather than at the size it created the feature with, and rebuilds when that region changes. For a game that creates one size and renders a smaller region inside it (Phantasy Star Online 2, #8). Diagnostic until measured. |
| `stall_test` | 0 | Holds the private D3D12 queue for this many milliseconds, once, at the 60th submission, to exercise the stall path. Diagnostic. |
| `unwrap_list` | 0 | `1` hands NGX the command list underneath ReShade's proxy. Diagnostic. |
| `probe` | 0 | `1` runs a standalone NGX D3D12 probe at attach and logs the result. Diagnostic. |
| `hash_out` | 1 | Once per feature build, 60 frames in, read the input and the output back and log a hash of the output, the mean of each channel of both, and the brightness ratio out/in. One readback per build; `0` disables. D3D11 bridge only. |

## Status panel

ReShade's overlay has a **DLSS 5 Bridge** window. It shows which route holds
the session and why, the contract in use, the motion-vector source on the
substitute path, and whether frames are still arriving. One switch, **Replace
DLSS when the game isn't using its own**, writes `synth`; Detail, Speed and
Direction write the optical flow and sign keys. A **Details** checkbox adds
what a bug report needs: build, transport, create flags, file paths, every
add-on in the folder and the neural add-on's `ReShade.ini` section.

The panel writes single lines into `dlss5-bridge.cfg` and reads the file back
like any other edit. There is no save, reload or reset: deleting the file
restores the defaults on the next launch.

The panel needs ReShade 6.0.0 to 6.8.0. A ReShade built without its overlay,
or the "Release Signed" build, has no panel; the log says so and nothing else
is affected.

## Log

`dlss5-bridge.log` beside the add-on records the environment, every add-on and
NVIDIA model file in the folder with its SHA-256, the contract read from the
game, the presentation (output format, colour space, HDR or SDR contract, the
exposure texture's value), every NGX result, a cumulative delivered-frame count
every 600 frames, on the D3D11 bridge the brightness the output came back at
relative to the input, and a timing line:

```
[bridge] 600 frames: bridge CPU 0.84 ms/frame | frame interval 16.00 ms (62.5 fps) | spread 5.74-29.93 ms | bridge is 5% of the frame | d3d12 43200/43202 (2 behind)
```

*bridge CPU* is time inside the add-on, mostly waiting on the GPU. *spread* is
the widest and narrowest frame interval in the window. *d3d12 N/M* is how far
the D3D12 side runs behind; a few is ordinary pipelining.

The transport itself costs nothing measurable; the neural pass on the GPU is
the cost, and it depends on scene, resolution, GPU and the neural add-on's
settings. To measure it in place: `stage=0`, read a timing line, `stage=3`,
read another, without moving.

## Reporting a problem

A screenshot of the panel with Details ticked answers the first questions.
Past that, attach `dlss5-bridge.log`. Please name the game, the API, and what
was on screen. Reports from titles not listed below are useful even when
everything works.

## Compatibility

Developed and verified on one GPU, on Baldur's Gate 3 (D3D11 and Vulkan), Red
Dead Redemption 2 (Vulkan) and Skyrim Special Edition (D3D11). Every release is
run through [ngxGym](https://github.com/NIGos/ngxGym), a synthetic DLSS host
that exercises both backends, mode changes, contract faults and the substitute
contract against real NGX, without a game.

NVIDIA drivers 32.0.16.1664 and 1686 route neural rendering (NGX feature 18) into
`nvngx_dlssnr.dll` itself, and the only build of that snippet in circulation,
310.8.0.0, faults inside D3D12 on that route: with the DLSS 5 add-on present the
game terminated or stopped presenting. The add-on closes that route in the
loaded `_nvngx.dll` at attach -- one pointer, in memory, nothing on disk -- and
the DLSS 5 add-on drives the feature as it did on 32.0.16.1656. See `ngx_loader`.

Reported working by users:

| Title | Engine | DLSS from |
| --- | --- | --- |
| Baldur's Gate 3 | Divinity 4.0 | the game |
| Final Fantasy XIV Online | in-house | the game |
| The Legend of Heroes: Trails beyond the Horizon | Falcom | the game |
| Tainted Grail: Fall of Avalon | Unity | the game |
| 7 Days to Die | Unity | the game |
| Skyrim Special Edition | Creation | a DLSS injector mod |
| Fallout 4 | Creation | a DLSS injector mod |
| S.T.A.L.K.E.R. Anomaly | X-Ray | an upscaler injector mod (SSS24) |
| Assetto Corsa | kunOS | Custom Shaders Patch (Preview 338 or later) |

Nothing targets a particular game: every module exporting the NGX API is
hooked, and every size, format and offset is read from the caller's parameter
block. DLSS supplied by a mod is picked up the same way as DLSS built into the
game.

## Known limits

If anything goes wrong the bridge disables itself and the game renders on its
own. It never leaves a broken frame on screen deliberately.

- **Two NGX sessions.** The game's DLSS runs once and the bridge's runs once;
  with `skip_game=1` only the bridge's does. One NGX call runs at a time in the
  process, so a mirrored frame's evaluate can wait for the game's.
- **Exposure texture.** A game that drives DLSS from an exposure texture is
  carried when the texture is a single-component 32-bit float; any other shape
  is refused with its format named. `flags` with `AutoExposure` (`0x40`) added
  lets DLSS compute its own exposure in that case.
- **Substitute before the game's DLSS.** A substitute armed before a game's own
  DLSS first appears faults inside the neural add-on when the game then asks,
  and the mirror stands down for the session. Leave `synth_after` at its
  default in a game that has DLSS; the mirror-first order is the one that works.
- **Substitute depth must be back-buffer sized.** Smaller depth means the game
  is upscaling itself, which is the mirror's case, and resampled depth would be
  invented data. The Vulkan substitute reads depth with a compute pass that
  needs `VK_KHR_push_descriptor`; a driver without it is refused by name.
- **Optical flow on Vulkan** runs on a private D3D11 device with two textures of
  its own, because the driver refuses to open the transport's D3D12 textures as
  D3D11 aliases. On D3D12 runtimes the engine is not wired; motion vectors come
  from a ReShade shader there.
- **Vulkan mirror** runs one frame in flight, one pipeline bubble per frame, and
  parks the game's command buffer on a `VkEvent` pair. That park raises two
  Khronos validation messages per frame by construction
  (`VUID-vkSetEvent-event-09543`, `VUID-vkCmdWaitEvents-srcStageMask-01158`);
  every NVIDIA driver measured accepts it.
- Resolution, preset and display-mode changes rebuild, and so does the game
  creating its DLSS feature again, whatever the shape: the create flags can
  change on their own, as IsHDR does when a game switches HDR on. On Vulkan a
  display change also recreates ReShade's runtime; the mirror and the
  substitute both follow it.
- **A result smaller than the texture holding it** is written at 0,0 and the
  rest of the texture is left alone, which is what the game's own DLSS does
  with the same block. Games padded for dynamic resolution allocate this way;
  the game's own evaluate is not skipped on those frames.
- Verbose logging is always on.

## Related

[dlss5-d3d12-fix](https://github.com/NIGos/dlss5-d3d12-fix) addresses a
different failure of the same neural add-on: a DirectX 12 game whose DLSS output
carries a mip chain. If the add-on's panel says STANDBY/FAILED rather than
waiting for the game's DLSS, that is the one to use.

## Building

Windows SDK and MSVC; no external dependencies. `src\build.cmd` sets up the
MSVC environment and runs the two lines:

```
rc /nologo version.rc
cl /nologo /W4 /O2 /MT /EHsc /std:c++17 /Ireshade /LD dlss5-bridge.cpp version.res ^
   /Fe:dlss5-bridge.addon64 /link /DLL user32.lib advapi32.lib bcrypt.lib
```

`bridge.h`, `bridge.inc`, `synth.inc`, `vkmirror.inc` and `depth_convert_spv.h`
are included by the `.cpp`. The version is in two places that stay in step:
`BRIDGE_VERSION` in the `.cpp` and the numbers in `version.rc`.

## Third-party code

- The ReShade add-on API headers under `src/reshade/` are Copyright 2014 Patrick
  Mours, BSD 3-clause; their licence is in
  [src/reshade/LICENSE.md](src/reshade/LICENSE.md) and travels with every binary.
- `depth_convert.comp` and `depth_convert_spv.h` are from
  [AlanBacker/dlss5-vk-bridge](https://github.com/AlanBacker/dlss5-vk-bridge),
  MIT, verbatim, with their dual copyright in the array's header. Two ideas in
  `vkmirror.inc` come from the same project: creating the shared textures on
  D3D12 and importing them into the game's `VkDevice`, and parking the game's
  command buffer on a `VkEvent` pair.

Everything else is this project's own, under MIT.

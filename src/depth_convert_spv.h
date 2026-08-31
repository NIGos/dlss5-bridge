// SPIR-V for depth_convert.comp: one compute pass that SAMPLES a depth-aspect
// view and STORES the value into an R32_SFLOAT image.
//
// Why a sample and not a copy. On Vulkan, ReShade's DEPTH semantic is bound to
// generic_depth's own backup texture, which is created shader_resource and
// copy_dest and nothing else -- no VK_IMAGE_USAGE_TRANSFER_SRC_BIT -- so no
// transfer command may legally read it. And vkCmdCopyImage and vkCmdBlitImage
// both require the two formats to match exactly when either side is a depth
// format, while the shared texture on the D3D12 side has to be R32_FLOAT
// because a depth-stencil resource is not in the kernel's shared-format list.
// A sampled depth-aspect texel is a normalised float whatever the storage
// format is, which is what bridges both facts at once.
//
// PROVENANCE. Both the GLSL and this array are taken verbatim from
// AlanBacker/dlss5-vk-bridge, src/depth_convert.comp and src/depth_convert_spv.h,
// MIT -- see the notice below and the Third-party section of README.md. They
// are not paraphrased and not regenerated: this machine has no Vulkan SDK, no
// glslangValidator, no shaderc and no dxcompiler.dll, so a shader written here
// could be neither compiled nor validated. A hand-assembled SPIR-V module that
// nothing on this machine can check is not a safer artifact than a
// glslang-generated one the upstream author compiled and ran spirv-val over; a
// wrong word is not a compile error, it is NVIDIA faulting inside its own
// shader compiler and reading as a crash caused by this add-on. So the
// already-validated blob is carried under its licence and credited, and the
// GLSL beside it in depth_convert.comp is the normative statement of what it
// does.
//
//   MIT License
//   Copyright (c) 2026 Alan Z    -- Vulkan port
//   Copyright (c) 2026 NIGos     -- original DLSS 5 DX11 Bridge
//   Permission is hereby granted, free of charge, to any person obtaining a
//   copy of this software and associated documentation files (the "Software"),
//   to deal in the Software without restriction, including without limitation
//   the rights to use, copy, modify, merge, publish, distribute, sublicense,
//   and/or sell copies of the Software, and to permit persons to whom the
//   Software is furnished to do so, subject to the following conditions:
//   The above copyright notice and this permission notice shall be included in
//   all copies or substantial portions of the Software.
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
//   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//   DEALINGS IN THE SOFTWARE.
//
// Upstream header, kept so the regeneration command travels with the array:
//   glslangValidator -V --target-env vulkan1.1 -o depth_convert.spv depth_convert.comp
//   validated with spirv-val.
// 410 words, 1640 bytes, SPIR-V version 1.3, id bound 68.
// SHA-256 of the 1640 bytes: 88c93ccbd041d6723565d94ca40ab6ce15ecd68c3e3b76202cf8d0df407fedb0
//
// THE INTERFACE THIS ARRAY DECLARES, which the host code must match exactly:
//   set 0 binding 0   combined image sampler   texture + sampler over a DEPTH-aspect view
//   set 0 binding 1   storage image, r32f, writeonly
//   push constant     uvec2 extent, at offset 0, 8 bytes
//   local size        8 x 8 x 1
static const unsigned int kDepthConvertSpv[] = {
    0x07230203, 0x00010300, 0x0008000b, 0x00000044, 0x00000000, 0x00020011,
    0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000005,
    0x00000004, 0x6e69616d, 0x00000000, 0x0000000c, 0x00060010, 0x00000004,
    0x00000011, 0x00000008, 0x00000008, 0x00000001, 0x00030003, 0x00000002,
    0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00030005,
    0x00000009, 0x00000070, 0x00080005, 0x0000000c, 0x475f6c67, 0x61626f6c,
    0x766e496c, 0x7461636f, 0x496e6f69, 0x00000044, 0x00030005, 0x00000014,
    0x00004350, 0x00050006, 0x00000014, 0x00000000, 0x65747865, 0x0000746e,
    0x00030005, 0x00000016, 0x00006370, 0x00030005, 0x0000002c, 0x00000064,
    0x00050005, 0x00000030, 0x5f637273, 0x74706564, 0x00000068, 0x00050005,
    0x0000003b, 0x5f747364, 0x66323372, 0x00000000, 0x00040047, 0x0000000c,
    0x0000000b, 0x0000001c, 0x00030047, 0x00000014, 0x00000002, 0x00050048,
    0x00000014, 0x00000000, 0x00000023, 0x00000000, 0x00040047, 0x00000030,
    0x00000021, 0x00000000, 0x00040047, 0x00000030, 0x00000022, 0x00000000,
    0x00030047, 0x0000003b, 0x00000019, 0x00040047, 0x0000003b, 0x00000021,
    0x00000001, 0x00040047, 0x0000003b, 0x00000022, 0x00000000, 0x00040047,
    0x00000043, 0x0000000b, 0x00000019, 0x00020013, 0x00000002, 0x00030021,
    0x00000003, 0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000000,
    0x00040017, 0x00000007, 0x00000006, 0x00000002, 0x00040020, 0x00000008,
    0x00000007, 0x00000007, 0x00040017, 0x0000000a, 0x00000006, 0x00000003,
    0x00040020, 0x0000000b, 0x00000001, 0x0000000a, 0x0004003b, 0x0000000b,
    0x0000000c, 0x00000001, 0x00020014, 0x0000000f, 0x0004002b, 0x00000006,
    0x00000010, 0x00000000, 0x00040020, 0x00000011, 0x00000007, 0x00000006,
    0x0003001e, 0x00000014, 0x00000007, 0x00040020, 0x00000015, 0x00000009,
    0x00000014, 0x0004003b, 0x00000015, 0x00000016, 0x00000009, 0x00040015,
    0x00000017, 0x00000020, 0x00000001, 0x0004002b, 0x00000017, 0x00000018,
    0x00000000, 0x00040020, 0x00000019, 0x00000009, 0x00000006, 0x0004002b,
    0x00000006, 0x00000020, 0x00000001, 0x00030016, 0x0000002a, 0x00000020,
    0x00040020, 0x0000002b, 0x00000007, 0x0000002a, 0x00090019, 0x0000002d,
    0x0000002a, 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000001,
    0x00000000, 0x0003001b, 0x0000002e, 0x0000002d, 0x00040020, 0x0000002f,
    0x00000000, 0x0000002e, 0x0004003b, 0x0000002f, 0x00000030, 0x00000000,
    0x00040017, 0x00000033, 0x00000017, 0x00000002, 0x00040017, 0x00000036,
    0x0000002a, 0x00000004, 0x00090019, 0x00000039, 0x0000002a, 0x00000001,
    0x00000000, 0x00000000, 0x00000000, 0x00000002, 0x00000003, 0x00040020,
    0x0000003a, 0x00000000, 0x00000039, 0x0004003b, 0x0000003a, 0x0000003b,
    0x00000000, 0x0004002b, 0x0000002a, 0x00000040, 0x00000000, 0x0004002b,
    0x00000006, 0x00000042, 0x00000008, 0x0006002c, 0x0000000a, 0x00000043,
    0x00000042, 0x00000042, 0x00000020, 0x00050036, 0x00000002, 0x00000004,
    0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003b, 0x00000008,
    0x00000009, 0x00000007, 0x0004003b, 0x0000002b, 0x0000002c, 0x00000007,
    0x0004003d, 0x0000000a, 0x0000000d, 0x0000000c, 0x0007004f, 0x00000007,
    0x0000000e, 0x0000000d, 0x0000000d, 0x00000000, 0x00000001, 0x0003003e,
    0x00000009, 0x0000000e, 0x00050041, 0x00000011, 0x00000012, 0x00000009,
    0x00000010, 0x0004003d, 0x00000006, 0x00000013, 0x00000012, 0x00060041,
    0x00000019, 0x0000001a, 0x00000016, 0x00000018, 0x00000010, 0x0004003d,
    0x00000006, 0x0000001b, 0x0000001a, 0x000500ae, 0x0000000f, 0x0000001c,
    0x00000013, 0x0000001b, 0x000400a8, 0x0000000f, 0x0000001d, 0x0000001c,
    0x000300f7, 0x0000001f, 0x00000000, 0x000400fa, 0x0000001d, 0x0000001e,
    0x0000001f, 0x000200f8, 0x0000001e, 0x00050041, 0x00000011, 0x00000021,
    0x00000009, 0x00000020, 0x0004003d, 0x00000006, 0x00000022, 0x00000021,
    0x00060041, 0x00000019, 0x00000023, 0x00000016, 0x00000018, 0x00000020,
    0x0004003d, 0x00000006, 0x00000024, 0x00000023, 0x000500ae, 0x0000000f,
    0x00000025, 0x00000022, 0x00000024, 0x000200f9, 0x0000001f, 0x000200f8,
    0x0000001f, 0x000700f5, 0x0000000f, 0x00000026, 0x0000001c, 0x00000005,
    0x00000025, 0x0000001e, 0x000300f7, 0x00000028, 0x00000000, 0x000400fa,
    0x00000026, 0x00000027, 0x00000028, 0x000200f8, 0x00000027, 0x000100fd,
    0x000200f8, 0x00000028, 0x0004003d, 0x0000002e, 0x00000031, 0x00000030,
    0x0004003d, 0x00000007, 0x00000032, 0x00000009, 0x0004007c, 0x00000033,
    0x00000034, 0x00000032, 0x00040064, 0x0000002d, 0x00000035, 0x00000031,
    0x0007005f, 0x00000036, 0x00000037, 0x00000035, 0x00000034, 0x00000002,
    0x00000018, 0x00050051, 0x0000002a, 0x00000038, 0x00000037, 0x00000000,
    0x0003003e, 0x0000002c, 0x00000038, 0x0004003d, 0x00000039, 0x0000003c,
    0x0000003b, 0x0004003d, 0x00000007, 0x0000003d, 0x00000009, 0x0004007c,
    0x00000033, 0x0000003e, 0x0000003d, 0x0004003d, 0x0000002a, 0x0000003f,
    0x0000002c, 0x00070050, 0x00000036, 0x00000041, 0x0000003f, 0x00000040,
    0x00000040, 0x00000040, 0x00040063, 0x0000003c, 0x0000003e, 0x00000041,
    0x000100fd, 0x00010038,
};

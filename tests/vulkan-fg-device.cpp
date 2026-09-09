// CPU-only calls through the production Vulkan create wrappers. No Vulkan,
// D3D, NGX, ReShade runtime or patched function bytes are needed.
#include "../src/dlss5-bridge.cpp"

// Negative control: compile this same test against unmodified pre4 with
// /DBRIDGE_TEST_LEGACY_FG. Supply only the ownership input absent in that version;
// its production forwarding must still fail the mapped-FG expectation.
#ifdef BRIDGE_TEST_LEGACY_FG
#include <unordered_map>
static std::unordered_map<uint64_t, uint64_t> g_vkm_cmd_devices;
#endif

#define EXPECT(condition) do { if (!(condition)) { \
    printf("FAIL line %d: %s\n", __LINE__, #condition); exit(1); } } while (0)

struct RecordedCall {
    int count, layer, feature, nesting;
    void *device, *command;
    NVSDK_NGX_Parameter *params;
    NVSDK_NGX_Handle **out;
};
static RecordedCall g_call;
static NVSDK_NGX_Result g_return_result;
static NVSDK_NGX_Handle g_handle = {42};
static NVSDK_NGX_Handle *g_return_handle;
static SynthParams g_params;

static NVSDK_NGX_Result Record(int layer, void *device, void *command, int feature,
                              NVSDK_NGX_Parameter *params, NVSDK_NGX_Handle **out)
{
    g_call = {g_call.count + 1, layer, feature, g_nest, device, command, params, out};
    if (out != nullptr) *out = g_return_handle;
    return g_return_result;
}

static NVSDK_NGX_Result Legacy0(void *c, int f, NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **o)
{ return Record(0, nullptr, c, f, p, o); }
static NVSDK_NGX_Result Legacy1(void *c, int f, NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **o)
{ return Record(1, nullptr, c, f, p, o); }
static NVSDK_NGX_Result Named0(void *d, void *c, int f, NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **o)
{ return Record(0, d, c, f, p, o); }
static NVSDK_NGX_Result Named1(void *d, void *c, int f, NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **o)
{ return Record(1, d, c, f, p, o); }

static void CheckForward(const char *name, Hook &hook, void *command, int feature,
                         void *expected_device, int expected_layer = 0,
                         NVSDK_NGX_Result result = NGX_SUCCESS,
                         NVSDK_NGX_Handle *handle = &g_handle, bool null_out = false)
{
    printf("CASE: %s\n", name);
    g_call = {};
    g_return_result = result;
    g_return_handle = handle;
    const int nesting = g_nest;
    const LONG creates = g_create_count;
    const bool observed = nesting == 0 && reinterpret_cast<uintptr_t>(command) >= 0x10000;
    // A reused SR handle must lose its old non-SR classification.
    g_other_feature_count = feature == 1 ? 1 : 0;
    g_other_feature[0] = &g_handle;
    g_s12.w = 777;
    g_s12.need_reset = false;
    g_vkm.rebuild_on_game_create = false;
    NVSDK_NGX_Handle *out = nullptr;
    auto **output_arg = null_out ? nullptr : &out;
    const auto actual = ForwardVkCreate(hook, command, feature, &g_params, output_arg);
    EXPECT(actual == result);
    EXPECT(out == (null_out ? nullptr : handle));
    EXPECT(g_call.count == 1 && g_call.layer == expected_layer);
    EXPECT(g_call.device == expected_device && g_call.command == command);
    EXPECT(g_call.feature == feature && g_call.params == &g_params && g_call.out == output_arg);
    EXPECT(g_call.nesting == (nesting != 0 ? nesting : 1) && g_nest == nesting);
    EXPECT(g_create_count == creates + (observed ? 1 : 0));
    const bool recorded = observed && result == NGX_SUCCESS && !null_out && handle != nullptr;
    EXPECT(g_other_feature_count == (feature == 1 ? (recorded ? 0 : 1) : (recorded ? 1 : 0)));
    if (recorded && feature != 1) EXPECT(IsOtherFeature(handle));
    const bool rebuild = observed && feature == 1 && result == NGX_SUCCESS;
    EXPECT(g_s12.w == (rebuild ? 0u : 777u));
    EXPECT(g_s12.need_reset == rebuild && g_vkm.rebuild_on_game_create == rebuild);
    if (rebuild)
        EXPECT(g_vkm.cw == 640 && g_vkm.ch == 360 && g_vkm.cow == 1280 && g_vkm.coh == 720);
    EXPECT(!hook.active); // Never write instructions in the CPU harness.
    puts("PASS");
}

int main()
{
    InitializeCriticalSection(&g_log_cs);
    InitializeCriticalSection(&g_hook_cs);
    InitializeCriticalSection(&g_ngx_cs);
    InitializeCriticalSection(&g_bridge_cs);
    g_ngx_cs_ready = true;
    strcpy_s(g_log_path, "NUL");
    g_params.Set("Width", 640u); g_params.Set("Height", 360u);
    g_params.Set("OutWidth", 1280u); g_params.Set("OutHeight", 720u);
    g_layer_count = 2;
    g_layer[0].mod = reinterpret_cast<HMODULE>(static_cast<uintptr_t>(0x100000));
    g_layer[1].mod = reinterpret_cast<HMODULE>(static_cast<uintptr_t>(0x200000));
    g_layer[0].vk_create.target = reinterpret_cast<BYTE *>(&Legacy0);
    g_layer[0].vk_create1.target = reinterpret_cast<BYTE *>(&Named0);
    g_layer[1].vk_create.target = reinterpret_cast<BYTE *>(&Legacy1);
    g_layer[1].vk_create1.target = reinterpret_cast<BYTE *>(&Named1);
    // Opaque, distinct dispatchable handles; production must never dereference them.
    uint64_t command_a = 0, command_b = 0, unknown = 0, device_a = 0, device_b = 0;
    g_vkm_cmd_devices[reinterpret_cast<uintptr_t>(&command_a)] = reinterpret_cast<uintptr_t>(&device_a);
    g_vkm_cmd_devices[reinterpret_cast<uintptr_t>(&command_b)] = reinterpret_cast<uintptr_t>(&device_b);

    CheckForward("FG command A names device A", g_layer[0].vk_create, &command_a, 11, &device_a);
    CheckForward("FG command B names device B in its own layer", g_layer[1].vk_create, &command_b, 11, &device_b, 1);
    CheckForward("FG command A still names A after B", g_layer[0].vk_create, &command_a, 11, &device_a);
    CheckForward("unknown command retains legacy call", g_layer[0].vk_create, &unknown, 11, nullptr);
    g_vkm_cmd_devices.erase(reinterpret_cast<uintptr_t>(&command_a));
    CheckForward("removed ownership retains legacy call", g_layer[0].vk_create, &command_a, 11, nullptr);
    g_vkm_cmd_devices[reinterpret_cast<uintptr_t>(&command_a)] = reinterpret_cast<uintptr_t>(&device_a);
    g_layer[0].vk_create1.target = nullptr;
    CheckForward("missing sibling cannot borrow another layer's CreateFeature1", g_layer[0].vk_create, &command_a, 11, nullptr);
    g_layer[0].vk_create1.target = reinterpret_cast<BYTE *>(&Named0);
    Hook outside = {}; outside.target = reinterpret_cast<BYTE *>(&Legacy0);
    CheckForward("unregistered hook retains legacy call", outside, &command_a, 11, nullptr);
    g_nest = 1;
    CheckForward("nested FG retains legacy call without double accounting", g_layer[0].vk_create, &command_a, 11, nullptr);
    g_nest = 0;
    CheckForward("SR retains legacy call and invalidates reused non-SR handle", g_layer[0].vk_create, &command_a, 1, nullptr);
    CheckForward("other feature retains legacy call and accounting", g_layer[0].vk_create, &command_a, 2, nullptr);
    CheckForward("invalid command retains legacy call without accounting", g_layer[0].vk_create, nullptr, 11, nullptr);
    const auto failure = static_cast<NVSDK_NGX_Result>(0xBAD00001);
    CheckForward("named failure propagates and does not record returned handle", g_layer[0].vk_create, &command_a, 11, &device_a, 0, failure);
    CheckForward("legacy failure propagates", g_layer[0].vk_create, &unknown, 11, nullptr, 0, failure);
    CheckForward("null returned handle is not recorded", g_layer[0].vk_create, &command_a, 11, &device_a, 0, NGX_SUCCESS, nullptr);
    CheckForward("null output argument is forwarded", g_layer[0].vk_create, &command_a, 11, &device_a, 0, NGX_SUCCESS, &g_handle, true);
    g_ngx_cs_ready = false;
    CheckForward("FG forwarding before NGX lock readiness", g_layer[0].vk_create, &command_b, 11, &device_b);

    puts("CASE: unloading a module clears all seven hooks and preserves its peer");
    Hook *hooks[] = {&g_layer[0].eval, &g_layer[0].eval_c, &g_layer[0].create,
                    &g_layer[0].vk_eval, &g_layer[0].vk_eval_c,
                    &g_layer[0].vk_create, &g_layer[0].vk_create1};
    for (auto *hook : hooks) {
        hook->target = reinterpret_cast<BYTE *>(&Legacy0);
        hook->active = true; // Unload must discard state without patching freed code.
    }
    BYTE peer[sizeof(Layer)]; memcpy(peer, &g_layer[1], sizeof(peer));
    ForgetUnloadedLayer(g_layer[0].mod);
    EXPECT(g_layer[0].mod == nullptr);
    for (const auto *hook : hooks) EXPECT(hook->target == nullptr && !hook->active);
    EXPECT(memcmp(peer, &g_layer[1], sizeof(peer)) == 0);
    puts("PASS");
    // Even an unowned slot with a nonnull stale sibling may not select it.
    g_layer[0].vk_create.target = reinterpret_cast<BYTE *>(&Legacy0);
    g_layer[0].vk_create1.target = reinterpret_cast<BYTE *>(&Named0);
    CheckForward("unowned layer cannot select a stale sibling", g_layer[0].vk_create, &command_a, 11, nullptr);

    g_vkm_cmd_devices.clear();
    DeleteCriticalSection(&g_bridge_cs);
    DeleteCriticalSection(&g_ngx_cs);
    DeleteCriticalSection(&g_hook_cs);
    DeleteCriticalSection(&g_log_cs);
    puts("PASS: production Vulkan FG device routing, fallbacks and handle accounting (CPU only)");
    return 0;
}

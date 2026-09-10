// Production history selection/ownership without a GPU. Real feature creation
// and atlas copy-back are exercised by ngxGym's split-history scenario.
#include "../src/dlss5-bridge.cpp"
#define EXPECT(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static NVSDK_NGX_Handle *released[32];
static unsigned releases;
static NVSDK_NGX_Result RecordRelease(NVSDK_NGX_Handle *feature)
{ released[releases++] = feature; return NGX_SUCCESS; }

int main()
{
    InitializeCriticalSection(&g_log_cs);
    InitializeCriticalSection(&g_bridge_cs);
    strcpy_s(g_log_path, "NUL");
    g_source = SRC_MIRROR;
    NVSDK_NGX_Handle game[9] = {}, private_feature[9] = {};
    g_bridge.release_feature = RecordRelease;
    g_bridge.feature = &private_feature[0];
    g_eval_handle = &game[0];
    EXPECT(BridgeSelectHistory(nullptr));
    EXPECT(g_bridge.histories[0].game == &game[0]);
    EXPECT(g_bridge.histories[0].feature == &private_feature[0]);
    g_bridge.histories[1] = { &game[1], &private_feature[1] };
    g_bridge.frame_ready = g_bridge.session_ready = true;
    g_bridge.frames_done = 1;
    g_bridge.need_reset = false;
    for (unsigned i = 0; i < 100; ++i)
    {
        g_eval_handle = &game[i % 2];
        EXPECT(BridgeSelectHistory(nullptr));
        EXPECT(g_bridge.feature == &private_feature[i % 2]);
        EXPECT(!g_bridge.need_reset);
        EXPECT(BridgeWillDeliver(g_eval_handle));
    }
    EXPECT(!BridgeWillDeliver(&game[2]));
    BridgeReleaseTextures();
    EXPECT(releases == 2 && released[0] != released[1]);
    EXPECT(g_bridge.feature == nullptr && !g_bridge.frame_ready);
    BridgeReleaseTextures();
    EXPECT(releases == 2);
    puts("PASS: independent selection, no peer reset, suppression and unique release");

    g_bridge.histories[0] = { &game[0], &private_feature[0] };
    g_bridge.feature = &private_feature[2]; // A create failure before registration.
    BridgeReleaseTextures();
    EXPECT(releases == 4 && released[2] == &private_feature[0] && released[3] == &private_feature[2]);
    for (unsigned i = 0; i < 8; ++i) g_bridge.histories[i] = { &game[i], &private_feature[i] };
    g_bridge.feature = &private_feature[0];
    g_eval_handle = &game[8];
    EXPECT(!BridgeSelectHistory(nullptr));
    EXPECT(g_bridge.feature == &private_feature[0] && !BridgeWillDeliver(&game[8]));
    BridgeReleaseTextures();
    EXPECT(releases == 12);
    puts("PASS: partial creation cleanup and bounded native fallback");

    SynthParams params;
    params.Set("Width", 640u); params.Set("Height", 360u);
    params.Set("OutWidth", 1280u); params.Set("OutHeight", 720u);
    params.Set("DLSS.Feature.Create.Flags", 15u);
    params.Set("PerfQualityValue", 2u);
    params.Set("DLSS.Hint.Render.Preset.Quality", 3u);
    BridgeNoteCreate(&game[0], &params);
    params.Set("DLSS.Feature.Create.Flags", 7u);
    params.Set("PerfQualityValue", 0u);
    BridgeNoteCreate(&game[1], &params);
    g_eval_handle = &game[0];
    unsigned value = 0;
    EXPECT(BridgeCreateUInt(&params, "DLSS.Feature.Create.Flags", &value) && value == 15);
    EXPECT(BridgeCreateUInt(&params, "PerfQualityValue", &value) && value == 2);
    g_eval_handle = &game[1];
    EXPECT(BridgeCreateUInt(&params, "DLSS.Feature.Create.Flags", &value) && value == 7);
    EXPECT(BridgeCreateUInt(&params, "PerfQualityValue", &value) && value == 0);
    g_bridge.frame_ready = true;
    params.Reset(); // NGX reused an address, with no complete create shape.
    BridgeNoteCreate(&game[1], &params);
    EXPECT(!g_bridge.frame_ready && g_bridge.need_reset);
    unsigned w, h, ow, oh;
    EXPECT(!BridgeCreateShapeOf(&game[1], &w, &h, &ow, &oh));
    EXPECT(!BridgeCreatedUInt("DLSS.Feature.Create.Flags", &value));
    g_eval_handle = &game[0];
    EXPECT(BridgeCreatedUInt("DLSS.Feature.Create.Flags", &value) && value == 15);
    g_source = SRC_SYNTH;
    EXPECT(!BridgeCreatedUInt("DLSS.Feature.Create.Flags", &value));
    puts("PASS: shared parameter block, per-create options, handle reuse and synthetic isolation");
    return 0;
}

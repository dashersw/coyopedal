#include "nam_json.h"
#include "cJSON.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif
namespace {
const cJSON* get(const cJSON* o, const char* k) {
    return cJSON_GetObjectItemCaseSensitive(o, k);
}
bool absent(const cJSON* p) {
    return !p || cJSON_IsNull(p);
}
bool number(const cJSON* o, const char* k, double expected) {
    const auto* p = get(o, k);
    return cJSON_IsNumber(p) && p->valuedouble == expected;
}
bool text(const cJSON* p, const char* expected) {
    return cJSON_IsString(p) && p->valuestring && std::strcmp(p->valuestring, expected) == 0;
}
bool inactive(const cJSON* o, const char* key) {
    const auto* p = get(o, key);
    return absent(p) || cJSON_IsFalse(p) ||
           (cJSON_IsObject(p) && (absent(get(p, "active")) || cJSON_IsFalse(get(p, "active"))));
}
bool topology(const cJSON* m) {
    if (!text(get(m, "architecture"), "WaveNet") || !number(m, "sample_rate", 48000))
        return false;
    const auto* c = get(m, "config");
    const auto* scale = get(c, "head_scale");
    if (!cJSON_IsObject(c) || !absent(get(c, "condition_dsp")) || !absent(get(c, "head")) ||
        !cJSON_IsNumber(scale) || !std::isfinite(scale->valuedouble))
        return false;
    const auto* layers = get(c, "layers");
    if (!cJSON_IsArray(layers) || cJSON_GetArraySize(layers) != 1)
        return false;
    const auto* l = cJSON_GetArrayItem(layers, 0);
    if (!number(l, "input_size", 1) || !number(l, "condition_size", 1) ||
        !number(l, "channels", 8) || !number(l, "bottleneck", 8) || !number(l, "groups_input", 1) ||
        !number(l, "groups_input_mixin", 1))
        return false;
    constexpr int kernels[] = {6, 6, 6,  6,  6, 6, 6, 6, 6, 6, 6, 6,
                               6, 6, 15, 15, 6, 6, 6, 6, 6, 6, 6};
    constexpr int dilations[] = {1,   3,   7, 17, 41, 101, 239, 1,  3,  7,   17, 41,
                                 101, 239, 1, 13, 1,  3,   7,   17, 41, 101, 239};
    const auto* ks = get(l, "kernel_sizes");
    const auto* ds = get(l, "dilations");
    const auto* acts = get(l, "activation");
    const auto* gating = get(l, "gating_mode");
    const auto* secondary = get(l, "secondary_activation");
    if (!cJSON_IsArray(ks) || !cJSON_IsArray(ds) || !cJSON_IsArray(acts) ||
        cJSON_GetArraySize(ks) != 23 || cJSON_GetArraySize(ds) != 23 ||
        cJSON_GetArraySize(acts) != 23)
        return false;
    if (!absent(gating) && (!cJSON_IsArray(gating) || cJSON_GetArraySize(gating) != 23))
        return false;
    if (!absent(secondary) && (!cJSON_IsArray(secondary) || cJSON_GetArraySize(secondary) != 23))
        return false;
    for (int i = 0; i < 23; ++i) {
        const auto* k = cJSON_GetArrayItem(ks, i);
        const auto* d = cJSON_GetArrayItem(ds, i);
        const auto* a = cJSON_GetArrayItem(acts, i);
        const auto* slope = get(a, "negative_slope");
        if (!cJSON_IsNumber(k) || k->valuedouble != kernels[i] || !cJSON_IsNumber(d) ||
            d->valuedouble != dilations[i] || !text(get(a, "type"), "LeakyReLU") ||
            !cJSON_IsNumber(slope) || !std::isfinite(slope->valuedouble) ||
            std::abs(slope->valuedouble - 0.01) > 1e-7)
            return false;
        if (!absent(gating) && !text(cJSON_GetArrayItem(gating, i), "none"))
            return false;
        if (!absent(secondary) && !cJSON_IsNull(cJSON_GetArrayItem(secondary, i)))
            return false;
    }
    if (!(absent(get(l, "gated")) || cJSON_IsFalse(get(l, "gated"))) || !inactive(l, "head1x1") ||
        !absent(get(l, "slimmable")))
        return false;
    const auto* point = get(l, "layer1x1");
    const auto* h = get(l, "head");
    if (!cJSON_IsObject(point) || !cJSON_IsTrue(get(point, "active")) ||
        !number(point, "groups", 1))
        return false;
    if (!number(h, "out_channels", 1) || !number(h, "kernel_size", 16) ||
        !(absent(get(h, "head_dilation")) || number(h, "head_dilation", 1)) ||
        !cJSON_IsTrue(get(h, "bias")))
        return false;
    const char* films[] = {"conv_pre_film",         "conv_post_film",      "input_mixin_pre_film",
                           "input_mixin_post_film", "activation_pre_film", "activation_post_film",
                           "layer1x1_post_film",    "head1x1_post_film"};
    for (const auto* key : films)
        if (!inactive(l, key))
            return false;
    return true;
}
void put(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        p[i] = v >> (8 * i);
}
std::uint32_t crc(const std::uint8_t* p, std::size_t n) {
    std::uint32_t c = 0xffffffff;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; ++i)
            c = (c >> 1) ^ ((c & 1) ? 0xedb88320 : 0);
    }
    return c ^ 0xffffffff;
}
} // namespace
bool pedalboard_parse_nam(const char* json, std::size_t length, std::uint8_t* out,
                          std::size_t capacity, char* error, std::size_t error_capacity) {
    auto fail = [&](const char* message) {
        if (error && error_capacity)
            std::snprintf(error, error_capacity, "%s", message);
        return false;
    };
    if (!json || !out || capacity < 48616 || length == 0 || length > 2 * 1024 * 1024)
        return fail("NAM file too large or output too small");
#if defined(ESP_PLATFORM)
    // Install once; all cJSON nodes use PSRAM rather than the audio engine's SRAM.
    static const bool hooks = [] {
        cJSON_Hooks h{[](size_t n) -> void* {
                          return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                      },
                      heap_caps_free};
        cJSON_InitHooks(&h);
        return true;
    }();
    (void)hooks;
#endif
    const char* end = nullptr;
    cJSON* root = cJSON_ParseWithLengthOpts(json, length, &end, false);
    if (!root)
        return fail("Invalid NAM JSON or insufficient PSRAM");
    struct Cleanup {
        cJSON* p;
        ~Cleanup() {
            cJSON_Delete(p);
        }
    } cleanup{root};
    while (end < json + length && (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t'))
        ++end;
    if (end != json + length)
        return fail("Trailing bytes after NAM JSON");
    const cJSON* model = topology(root) ? root : nullptr;
    if (!model && text(get(root, "architecture"), "SlimmableContainer")) {
        const auto* entries = get(get(root, "config"), "submodels");
        const cJSON* entry;
        cJSON_ArrayForEach(entry, entries) {
            if (topology(get(entry, "model"))) {
                model = get(entry, "model");
                break;
            }
        }
    }
    if (!model)
        return fail("Requires 48 kHz 8-channel A2-Full WaveNet");
    const auto* weights = get(model, "weights");
    if (!cJSON_IsArray(weights) || cJSON_GetArraySize(weights) != 12146)
        return fail("Expected 12146 NAM weights");
    size_t offset = 32;
    const cJSON* weight;
    cJSON_ArrayForEach(weight, weights) {
        if (!cJSON_IsNumber(weight) || !std::isfinite(weight->valuedouble) ||
            std::abs(weight->valuedouble) > std::numeric_limits<float>::max())
            return fail("Non-finite or invalid NAM weight");
        const float value = static_cast<float>(weight->valuedouble);
        std::uint32_t bits;
        std::memcpy(&bits, &value, 4);
        put(out + offset, bits);
        offset += 4;
    }
    std::memset(out, 0, 32);
    std::memcpy(out, "NAMB", 4);
    out[4] = 1;
    out[6] = 2;
    put(out + 8, 48000);
    put(out + 12, 12146);
    put(out + 16, crc(out + 32, 48616 - 32));
    put(out + 20, 32);
    return true;
}

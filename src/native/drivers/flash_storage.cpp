#include "flash_storage.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include "esp_log.h"
#include "esp_partition.h"

namespace {

constexpr char kTag[] = "v1_store";
constexpr esp_partition_subtype_t kModelsSubtype = static_cast<esp_partition_subtype_t>(0x40);
constexpr esp_partition_subtype_t kPresetsSubtype = static_cast<esp_partition_subtype_t>(0x41);
constexpr esp_partition_subtype_t kUserSubtype = static_cast<esp_partition_subtype_t>(0x42);

const esp_partition_t* models{};
const esp_partition_t* presets{};
const esp_partition_t* user_models{};

extern "C" const std::uint8_t kFactoryPresetsStart[] asm("_binary_v1_presets_fallback_start");
extern "C" const std::uint8_t kFactoryPresetsEnd[] asm("_binary_v1_presets_fallback_end");

extern "C" const std::uint8_t kFactoryModelsStart[] asm("_binary_pedalboard_factory_models_start");
extern "C" const std::uint8_t kFactoryModelsEnd[] asm("_binary_pedalboard_factory_models_end");

constexpr std::size_t kCopyChunkSize = 128U;

void discover() {
    if (models == nullptr) {
        models = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, kModelsSubtype, "models");
        presets = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, kPresetsSubtype, "presets");
        user_models =
            esp_partition_find_first(ESP_PARTITION_TYPE_DATA, kUserSubtype, "user_models");
        ESP_LOGI(kTag, "V1 storage: models=%s presets=%s user=%s", models != nullptr ? "yes" : "no",
                 presets != nullptr ? "yes" : "no", user_models != nullptr ? "yes" : "no");
    }
}

// The factory presets are assets/presets.json, embedded in the application and
// mirrored into the presets partition. There is no container around them: what
// is flashed is the file, so `esptool.py read_flash` on this partition gives
// back something a person can read, and the repair below is a byte copy of a
// text file rather than a format this code has to agree with a packer about.
//
// Valid means "could be that file": a JSON object, and small enough to fit. The
// parser is what decides whether it holds presets, and it runs on every boot in
// factory_presets.c -- validating the shape twice, in two languages, is what the
// packed version needed and is exactly what is gone.
bool embedded_presets_valid(const std::uint8_t* const image, const std::size_t size) {
    if (image == nullptr || size < 2U) {
        return false;
    }
    std::size_t at = 0;
    while (at < size &&
           (image[at] == ' ' || image[at] == '\n' || image[at] == '\r' || image[at] == '\t')) {
        ++at;
    }
    return at < size && image[at] == '{';
}

bool partition_matches_embedded_presets(const std::uint8_t* const image, const std::size_t size) {
    std::array<std::uint8_t, kCopyChunkSize> current{};
    for (std::size_t offset = 0; offset < size; offset += current.size()) {
        const std::size_t count = std::min(current.size(), size - offset);
        if (esp_partition_read(presets, offset, current.data(), count) != ESP_OK ||
            std::memcmp(current.data(), image + offset, count) != 0) {
            return false;
        }
    }
    return true;
}

bool ensure_factory_presets() {
    if (presets == nullptr) {
        return false;
    }
    const std::uint8_t* const image = kFactoryPresetsStart;
    const std::size_t size = static_cast<std::size_t>(kFactoryPresetsEnd - kFactoryPresetsStart);
    if (!embedded_presets_valid(image, size) || size > presets->size) {
        ESP_LOGE(kTag, "embedded factory preset image is invalid (%u bytes)",
                 static_cast<unsigned>(size));
        return false;
    }
    if (partition_matches_embedded_presets(image, size)) {
        return true;
    }

    const std::size_t erase_size =
        (size + COYOPEDAL_FLASH_SECTOR - 1U) & ~(COYOPEDAL_FLASH_SECTOR - 1U);
    ESP_LOGW(kTag, "repairing stale factory preset partition from app image");
    if (esp_partition_erase_range(presets, 0U, erase_size) != ESP_OK) {
        ESP_LOGE(kTag, "factory preset partition erase failed");
        return false;
    }

    std::array<std::uint8_t, kCopyChunkSize> chunk{};
    for (std::size_t offset = 0; offset < size; offset += chunk.size()) {
        const std::size_t count = std::min(chunk.size(), size - offset);
        // The embedded image lives in flash. Copy each piece into internal RAM
        // before esp_partition_write temporarily disables the flash cache.
        std::memcpy(chunk.data(), image + offset, count);
        if (esp_partition_write(presets, offset, chunk.data(), count) != ESP_OK) {
            ESP_LOGE(kTag, "factory preset write failed at %u", static_cast<unsigned>(offset));
            return false;
        }
    }
    if (!partition_matches_embedded_presets(image, size)) {
        ESP_LOGE(kTag, "factory preset verification failed");
        return false;
    }
    ESP_LOGI(kTag, "factory preset partition repaired and verified (%u bytes)",
             static_cast<unsigned>(size));
    return true;
}

const esp_partition_t* resolve(const std::uint32_t address, std::uint32_t& offset) {
    discover();
    if (address < COYOPEDAL_FLASH_PRESETS_BASE) {
        offset = address;
        return models;
    }
    if (address >= COYOPEDAL_FLASH_USER_MODELS_BASE &&
        address < COYOPEDAL_FLASH_USER_MODELS_BASE + COYOPEDAL_FLASH_USER_MODELS_SIZE) {
        offset = address - COYOPEDAL_FLASH_USER_MODELS_BASE;
        return user_models;
    }
    if (address >= COYOPEDAL_FLASH_PRESETS_BASE && address < COYOPEDAL_FLASH_PRESETS_END) {
        offset = address - COYOPEDAL_FLASH_PRESETS_BASE;
        return presets;
    }
    return nullptr;
}

} // namespace

extern "C" bool coyopedal_flash_store_init(void) {
    discover();
    return models != nullptr && presets != nullptr && ensure_factory_presets();
}

extern "C" bool coyopedal_flash_store_read(std::uint32_t address, std::uint8_t* const out,
                                           const std::uint32_t count) {
    std::uint32_t offset = 0;
    const esp_partition_t* const partition = resolve(address, offset);
    return partition != nullptr && out != nullptr && offset + count <= partition->size &&
           esp_partition_read(partition, offset, out, count) == ESP_OK;
}

extern "C" bool coyopedal_flash_presets_read(char* const out, const std::uint32_t capacity,
                                             std::uint32_t* const length) {
    discover();
    if (out == nullptr || length == nullptr || capacity == 0U || presets == nullptr) {
        return false;
    }
    const std::uint32_t count = std::min<std::uint32_t>(capacity, presets->size);
    if (esp_partition_read(presets, 0U, out, count) != ESP_OK) {
        return false;
    }
    *length = count;
    return true;
}

extern "C" bool coyopedal_factory_models_read(const std::uint32_t offset, std::uint8_t* const out,
                                              const std::uint32_t count) {
    const auto size = static_cast<std::size_t>(kFactoryModelsEnd - kFactoryModelsStart);
    if (out == nullptr || offset > size || count > size - offset)
        return false;
    std::memcpy(out, kFactoryModelsStart + offset, count);
    return true;
}

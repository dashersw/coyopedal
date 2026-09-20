#ifndef COYOPEDAL_PEDAL_FLASH_STORAGE_H_
#define COYOPEDAL_PEDAL_FLASH_STORAGE_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The flash store behind the model and preset containers.
//
// One flat logical address space, carved into three windows, each of which
// resolve() in the .cpp maps onto a named ESP-IDF data partition. These are NOT
// physical flash offsets -- those live in the generated partition table, and the
// containers must not have to know them. The models window starts at zero.
#define COYOPEDAL_FLASH_PRESETS_BASE 0x400000U
// End of the preset window. Nothing is mapped between here and the user models.
#define COYOPEDAL_FLASH_PRESETS_END 0x500000U
#define COYOPEDAL_FLASH_USER_MODELS_BASE 0x580000U
#define COYOPEDAL_FLASH_USER_MODELS_SIZE 0x80000U

// The flash erase granularity.
#define COYOPEDAL_FLASH_SECTOR 4096U

// Find the data partitions and seed factory presets when necessary.
bool coyopedal_flash_store_init(void);
bool coyopedal_flash_store_read(uint32_t address, uint8_t* out, uint32_t count);

// The factory preset document, whole: assets/presets.json as it was flashed,
// with whatever the erased tail of the partition holds after it. Reads up to
// `capacity` bytes and reports how many in `*length`; the parser in
// preset_json.c stops at the closing brace, which is why the tail can come along
// rather than having to be found here.
//
// Its own call rather than an address in the window above because the document
// has no header to read a length out of first -- the point of keeping it as text
// is that there is nothing in front of it -- so the only way to read it is to
// read all of it.
bool coyopedal_flash_presets_read(char* out, uint32_t capacity, uint32_t* length);
// Factory models are read from the catalogue embedded in the application.
bool coyopedal_factory_models_read(uint32_t offset, uint8_t* out, uint32_t count);

#ifdef __cplusplus
}
#endif

#endif // COYOPEDAL_PEDAL_FLASH_STORAGE_H_

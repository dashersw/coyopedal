/*
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_heap_caps.h"
#include "usb_private.h"
#include "usb/usb_types_ch9.h"

// ----------------------------------------------------- Macros --------------------------------------------------------

#if !CONFIG_IDF_TARGET_LINUX
#include "esp_private/esp_cache_private.h"
#define ALIGN_UP(num, align)    ((align) == 0 ? (num) : (((num) + ((align) - 1)) & ~((align) - 1)))
#endif

// ----------------------- Configs -------------------------

#ifdef CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM      // In esp32p4, the USB-DWC internal DMA can access external RAM
#define DATA_BUFFER_CAPS                     (MALLOC_CAP_DMA | MALLOC_CAP_CACHE_ALIGNED | MALLOC_CAP_SPIRAM)
#else
#define DATA_BUFFER_CAPS                     (MALLOC_CAP_DMA | MALLOC_CAP_CACHE_ALIGNED | MALLOC_CAP_INTERNAL)
#endif

#if CONFIG_IDF_TARGET_ESP32S3
// Reserve the live UAC2 transfer buffers before Wi-Fi, the model and the
// effects fragment internal DMA memory.  The production HD X path uses three
// one-packet capture URBs (192 bytes), two one-packet playback URBs (384
// bytes), and two four-packet feedback URBs (16 bytes).  Keep exact-size DMA
// slots instead of retaining the old four-packet audio experiment's 768- and
// 1536-byte buffers.
#define S3_ISOC_DATA_ALIGN 64
static uint8_t s_isoc_data_feedback[2][64] __attribute__((aligned(S3_ISOC_DATA_ALIGN)));
static uint8_t s_isoc_data_capture[3][192] __attribute__((aligned(S3_ISOC_DATA_ALIGN)));
static uint8_t s_isoc_data_playback[2][384] __attribute__((aligned(S3_ISOC_DATA_ALIGN)));

// The enumeration driver's control URB is the one allocation that decided
// whether this pedal had any audio at all.  It wants
// sizeof(usb_setup_packet_t) + CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE,
// cache-aligned -- 2112 bytes of DMA-capable INTERNAL memory -- and
// usb_host_install() runs after the A2-Full graph has taken 213 KB of the
// 225 KB of internal SRAM and left a largest free block of 2048.  The install
// therefore failed with ESP_ERR_NO_MEM, the interface never enumerated, and the
// board came up with a loaded model, a working panel and silence.
//
// Reserving it statically is the only deterministic answer: the buffer exists
// before the graph plans anything, so the graph allocates around it instead of
// the two racing for the same bytes.  Starting the USB host ahead of the graph
// was tried instead and is wrong -- it takes the contiguous regions the NAM
// bank planner needs and the model then fails to load.
#define S3_CTRL_DATA_SIZE ((8 + CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE + 63) & ~63)
static uint8_t s_ctrl_data[S3_CTRL_DATA_SIZE] __attribute__((aligned(S3_ISOC_DATA_ALIGN)));

typedef struct {
    uint8_t *data;
    size_t size;
    bool used;
} s3_isoc_data_slot_t;

static s3_isoc_data_slot_t s_isoc_data_slots[] = {
    {s_isoc_data_feedback[0], sizeof(s_isoc_data_feedback[0]), false},
    {s_isoc_data_feedback[1], sizeof(s_isoc_data_feedback[1]), false},
    {s_isoc_data_capture[0], sizeof(s_isoc_data_capture[0]), false},
    {s_isoc_data_capture[1], sizeof(s_isoc_data_capture[1]), false},
    {s_isoc_data_capture[2], sizeof(s_isoc_data_capture[2]), false},
    {s_isoc_data_playback[0], sizeof(s_isoc_data_playback[0]), false},
    {s_isoc_data_playback[1], sizeof(s_isoc_data_playback[1]), false},
};

static s3_isoc_data_slot_t s_ctrl_data_slot = {s_ctrl_data, sizeof(s_ctrl_data), false};
static portMUX_TYPE s_isoc_data_pool_lock = portMUX_INITIALIZER_UNLOCKED;

static void *s3_ctrl_data_pool_alloc(size_t size)
{
    void *result = NULL;
    portENTER_CRITICAL(&s_isoc_data_pool_lock);
    if (!s_ctrl_data_slot.used && s_ctrl_data_slot.size >= size) {
        s_ctrl_data_slot.used = true;
        result = s_ctrl_data_slot.data;
    }
    portEXIT_CRITICAL(&s_isoc_data_pool_lock);
    if (result != NULL) {
        memset(result, 0, size);
    }
    return result;
}

static void *s3_isoc_data_pool_alloc(size_t size)
{
    void *result = NULL;
    portENTER_CRITICAL(&s_isoc_data_pool_lock);
    for (size_t index = 0; index < sizeof(s_isoc_data_slots) / sizeof(s_isoc_data_slots[0]); ++index) {
        s3_isoc_data_slot_t *slot = &s_isoc_data_slots[index];
        if (!slot->used && slot->size >= size) {
            slot->used = true;
            result = slot->data;
            break;
        }
    }
    portEXIT_CRITICAL(&s_isoc_data_pool_lock);
    if (result != NULL) {
        memset(result, 0, size);
    }
    return result;
}

static bool s3_isoc_data_pool_free(void *pointer)
{
    if (pointer == NULL) {
        return false;
    }
    bool found = false;
    portENTER_CRITICAL(&s_isoc_data_pool_lock);
    if (pointer == s_ctrl_data_slot.data) {
        s_ctrl_data_slot.used = false;
        portEXIT_CRITICAL(&s_isoc_data_pool_lock);
        return true;
    }
    for (size_t index = 0; index < sizeof(s_isoc_data_slots) / sizeof(s_isoc_data_slots[0]); ++index) {
        s3_isoc_data_slot_t *slot = &s_isoc_data_slots[index];
        if (slot->data == pointer) {
            slot->used = false;
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_isoc_data_pool_lock);
    return found;
}
#endif

urb_t *urb_alloc(size_t data_buffer_size, int num_isoc_packets)
{
    urb_t *urb = heap_caps_calloc(1, sizeof(urb_t) + (sizeof(usb_isoc_packet_desc_t) * num_isoc_packets), USB_HOST_OBJ_CAPS);

#if !CONFIG_IDF_TARGET_LINUX
    // Note for developers: We do not use heap_caps_get_allocated_size() because it is broken with HEAP_POISONING=COMPREHENSIVE
    size_t cache_align = 0;
    esp_cache_get_alignment(DATA_BUFFER_CAPS, &cache_align);
    data_buffer_size = ALIGN_UP(data_buffer_size, cache_align);
#endif

    void *data_buffer = NULL;
#if CONFIG_IDF_TARGET_ESP32S3
    if (num_isoc_packets > 0) {
        data_buffer = s3_isoc_data_pool_alloc(data_buffer_size);
    } else {
        data_buffer = s3_ctrl_data_pool_alloc(data_buffer_size);
    }
#endif
    if (data_buffer == NULL) {
        data_buffer = heap_caps_malloc(data_buffer_size, DATA_BUFFER_CAPS);
    }
    if (urb == NULL || data_buffer == NULL)     {
        goto err;
    }

    // Cast as dummy transfer so that we can assign to const fields
    usb_transfer_dummy_t *dummy_transfer = (usb_transfer_dummy_t *)&urb->transfer;
    dummy_transfer->data_buffer = data_buffer;
    dummy_transfer->data_buffer_size = data_buffer_size;
    dummy_transfer->num_isoc_packets = num_isoc_packets;
    return urb;
err:
    heap_caps_free(urb);
#if CONFIG_IDF_TARGET_ESP32S3
    if (!s3_isoc_data_pool_free(data_buffer))
#endif
    {
        heap_caps_free(data_buffer);
    }
    return NULL;
}

void urb_free(urb_t *urb)
{
    if (urb == NULL) {
        return;
    }
#if CONFIG_IDF_TARGET_ESP32S3
    if (!s3_isoc_data_pool_free(urb->transfer.data_buffer))
#endif
    {
        heap_caps_free(urb->transfer.data_buffer);
    }
    heap_caps_free(urb);
}

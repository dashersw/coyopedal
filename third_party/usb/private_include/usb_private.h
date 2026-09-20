/*
 * SPDX-FileCopyrightText: 2015-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "sdkconfig.h"
#include "esp_heap_caps.h"

// The USB host stack's own bookkeeping -- driver objects, device and endpoint
// objects, client objects, cached descriptors and URB headers -- is touched
// only by the CPU. On this board the A2-Full NAM graph takes 213 KB of the
// 225 KB of internal SRAM, so leaving these small structs in internal memory
// is what made usb_host_install() fail with ESP_ERR_NO_MEM and the pedal come
// up with no audio device at all. They belong in PSRAM; the DWC DMA lists and
// the isochronous data buffers keep their own DMA-capable caps below.
//
// This is safe only because the DWC interrupt is allocated ESP_INTR_FLAG_LOWMED
// rather than ESP_INTR_FLAG_IRAM, so it never runs with the cache disabled.
#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_SPIRAM
#define USB_HOST_OBJ_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define USB_HOST_OBJ_CAPS MALLOC_CAP_DEFAULT
#endif

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/queue.h>
#include "esp_assert.h"
#include "usb/usb_types_ch9.h"
#include "usb/usb_types_stack.h"

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------ Types --------------------------------------------------------

typedef struct {
    uint8_t *data_buffer;
    size_t data_buffer_size;
    int num_bytes;
    int actual_num_bytes;
    uint32_t flags;
    usb_device_handle_t device_handle;
    uint8_t bEndpointAddress;
    usb_transfer_status_t status;
    uint32_t timeout;
    usb_transfer_cb_t callback;
    void *context;
    int num_isoc_packets;
    usb_isoc_packet_desc_t isoc_packet_desc[0];
} usb_transfer_dummy_t;
ESP_STATIC_ASSERT(sizeof(usb_transfer_dummy_t) == sizeof(usb_transfer_t), "usb_transfer_dummy_t does not match usb_transfer_t");

struct urb_s {
    TAILQ_ENTRY(urb_s) tailq_entry;
    // HCD Layer: Handler pointer and variables. Must be initialized to NULL and 0 respectively
    void *hcd_ptr;
    uint32_t hcd_var;
    // Host Lib Layer:
    void *usb_host_client;  // Currently only used when submitted to shared pipes (i.e., Device default pipes)
    bool usb_host_inflight; // Debugging variable, used to prevent re-submitting URBs already inflight
    // Non-control transfers are normally resubmitted to the same endpoint for
    // their entire lifetime. Cache the resolved endpoint and host wrapper so
    // the 1 ms isochronous path does not take the USBH device mutex twice per
    // callback. The key fields preserve the public API's endpoint semantics.
    void *usb_host_cached_device;
    void *usb_host_cached_ep;
    void *usb_host_cached_ep_context;
    uint8_t usb_host_cached_ep_address;
    // Public transfer structure. Must be last due to variable length array
    usb_transfer_t transfer;
};
typedef struct urb_s urb_t;

/**
 * @brief Processing request source
 *
 * Enum to indicate which layer of the USB Host stack requires processing. The main handling loop should then call that
 * layer's processing function (i.e., xxx_process()).
 */
typedef enum {
    USB_PROC_REQ_SOURCE_USBH = 0x01,
    USB_PROC_REQ_SOURCE_HUB = 0x02,
    USB_PROC_REQ_SOURCE_ENUM = 0x03
} usb_proc_req_source_t;

/**
 * @brief Processing request callback
 *
 * Callback function provided to each layer of the USB Host stack so that each layer can request calls to their
 * processing function.
 */
typedef bool (*usb_proc_req_cb_t)(usb_proc_req_source_t source, bool in_isr, void *context);

// --------------------------------------------------- Allocation ------------------------------------------------------

/**
 * @brief Allocate a URB
 *
 * - Data buffer is allocated in DMA capable memory
 * - The constant fields of the URB are also set
 * - The data_buffer field of the URB is set to point to start of the allocated data buffer
 * - The resulting data_buffer_size can be bigger that the requested size. This is to ensure that the data buffer is cache aligned
 *
 * @param[in] data_buffer_size Size of the URB's data buffer
 * @param[in] num_isoc_packets Number of isochronous packet descriptors
 *
 * @return
 *    - urb_t* URB object
 */
urb_t *urb_alloc(size_t data_buffer_size, int num_isoc_packets);

/**
 * @brief Free a URB
 *
 * @param[in] urb URB object
 */
void urb_free(urb_t *urb);

#ifdef __cplusplus
}
#endif

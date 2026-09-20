// Read-only BLE discovery. Firmware writes stay on the authenticated Wi-Fi API.
//
// The whole file is conditional on the controller being in the build
// (CONFIG_BT_ENABLED in sdkconfig.defaults). Without it the two entry points
// still exist and answer "no radio", so maintenance mode reports BLE
// unavailable instead of failing to link.
#include "sdkconfig.h"
#if CONFIG_BT_ENABLED
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include "freertos/semphr.h"
#include <cstring>
extern "C" {
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
}
namespace {
uint8_t address_type;
bool initialized{};
bool running{};
TaskHandle_t host_task_handle{};
const ble_uuid128_t service_uuid = BLE_UUID128_INIT(0x01, 0x00, 0x53, 0x53, 0x64, 0x72, 0x61, 0x6f,
                                                    0x62, 0x6c, 0x61, 0x64, 0x65, 0x70, 0x90, 0x7a);
const ble_uuid128_t info_uuid = BLE_UUID128_INIT(0x02, 0x00, 0x53, 0x53, 0x64, 0x72, 0x61, 0x6f,
                                                 0x62, 0x6c, 0x61, 0x64, 0x65, 0x70, 0x90, 0x7a);
int read_info(uint16_t, uint16_t, ble_gatt_access_ctxt* ctx, void*) {
    if (ctx->op != BLE_GATT_ACCESS_OP_READ_CHR)
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    esp_netif_ip_info_t ip{};
    auto* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif)
        netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (netif)
        esp_netif_get_ip_info(netif, &ip);
    char value[128];
    std::snprintf(value, sizeof value,
                  "{\"device\":\"pedalboard-s3\",\"ip\":\"" IPSTR
                  "\",\"port\":8080,\"ota\":\"/v1/ota\",\"auth\":true}",
                  IP2STR(&ip.ip));
    return os_mbuf_append(ctx->om, value, std::strlen(value)) == 0 ? 0
                                                                   : BLE_ATT_ERR_INSUFFICIENT_RES;
}
ble_gatt_chr_def characteristics[2]{};
ble_gatt_svc_def services[2]{};
void advertise();
int gap_event(ble_gap_event* event, void*) {
    if (event->type == BLE_GAP_EVENT_DISCONNECT || event->type == BLE_GAP_EVENT_ADV_COMPLETE ||
        (event->type == BLE_GAP_EVENT_CONNECT && event->connect.status != 0))
        advertise();
    return 0;
}
void advertise() {
    ble_hs_adv_fields fields{};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = const_cast<ble_uuid128_t*>(&service_uuid);
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    if (ble_gap_adv_set_fields(&fields) != 0)
        return;
    ble_hs_adv_fields response{};
    const char* name = ble_svc_gap_device_name();
    response.name = reinterpret_cast<const uint8_t*>(name);
    response.name_len = std::strlen(name);
    response.name_is_complete = 1;
    if (ble_gap_adv_rsp_set_fields(&response) != 0)
        return;
    ble_gap_adv_params params{};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = 800;
    params.itvl_max = 960;
    const int rc =
        ble_gap_adv_start(address_type, nullptr, BLE_HS_FOREVER, &params, gap_event, nullptr);
    if (rc)
        ESP_LOGW("panel_ble", "advertising failed: %d", rc);
    else
        ESP_LOGI("panel_ble",
                 "Pedalboard S3 advertising; Wi-Fi update address available over GATT");
}
void synced() {
    if (ble_hs_util_ensure_addr(0) == 0 && ble_hs_id_infer_auto(0, &address_type) == 0)
        advertise();
}
void host_task(void*) {
    nimble_port_run();
    vTaskSuspend(nullptr);
}
} // namespace
bool pedalboard_ble_start() {
    if (initialized)
        return running;
    if (nimble_port_init() != ESP_OK)
        return false;
    initialized = true;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("Pedalboard S3");
    characteristics[0].uuid = &info_uuid.u;
    characteristics[0].access_cb = read_info;
    characteristics[0].flags = BLE_GATT_CHR_F_READ;
    services[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    services[0].uuid = &service_uuid.u;
    services[0].characteristics = characteristics;
    if (ble_gatts_count_cfg(services) != 0 || ble_gatts_add_svcs(services) != 0) {
        initialized = nimble_port_deinit() != ESP_OK;
        return false;
    }
    ble_hs_cfg.sync_cb = synced;
    // The stack must be internal, not PSRAM. NimBLE's host task opens its NVS
    // store on sync (ble_store_config_conf_init -> nvs_open NVS_READWRITE), and
    // creating that namespace writes flash -- which disables the cache and
    // asserts esp_task_stack_is_sane_cache_disabled() for any task running on a
    // stack in external RAM. The store is compiled in because
    // CONFIG_BT_NIMBLE_MAX_BONDS is 3 (see sdkconfig.defaults). 4 KiB of
    // internal SRAM is the whole cost.
    running =
        xTaskCreatePinnedToCoreWithCaps(host_task, "panel_ble", 4096, nullptr, 3, &host_task_handle,
                                        0, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == pdPASS;
    if (!running)
        initialized = nimble_port_deinit() != ESP_OK;
    return running;
}

#else

bool pedalboard_ble_start() {
    return false;
}

#endif

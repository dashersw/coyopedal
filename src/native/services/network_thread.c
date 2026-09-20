#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lwip/opt.h"
#include "lwip/sys.h"

sys_thread_t __real_sys_thread_new(const char* name, lwip_thread_fn thread, void* arg,
                                   int stacksize, int prio);

// esp_netif keeps lwIP's TCP/IP thread alive after Wi-Fi deinitialization.
// Its dormant stack must not occupy the internal SRAM needed to reload NAM.
// Preserve the SDK's priority/affinity and leave other lwIP threads unchanged.
sys_thread_t __wrap_sys_thread_new(const char* name, lwip_thread_fn thread, void* arg,
                                   int stacksize, int prio) {
    if (strcmp(name, TCPIP_THREAD_NAME) != 0)
        return __real_sys_thread_new(name, thread, arg, stacksize, prio);
    TaskHandle_t task = NULL;
    if (xTaskCreatePinnedToCoreWithCaps(thread, name, stacksize, arg, prio, &task,
                                        CONFIG_LWIP_TCPIP_TASK_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        return NULL;
    return (sys_thread_t)task;
}

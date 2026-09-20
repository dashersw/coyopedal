#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    COYOPEDAL_UI_USB_UNMOUNTED = 0,
    COYOPEDAL_UI_USB_MOUNTED,
    COYOPEDAL_UI_USB_STREAMING,
    COYOPEDAL_UI_USB_SUSPENDED,
} coyopedal_ui_usb_t;

// Restore controls and presets. Runs from the app's native boot hook, before
// the Gea runtime mounts the JSX tree that reads them.
bool coyopedal_ui_init(void);
void coyopedal_ui_set_usb(coyopedal_ui_usb_t state);
const char* coyopedal_ui_screen_name(void);

#ifdef __cplusplus
}
#endif

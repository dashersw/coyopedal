#pragma once

// Installs the bounded in-memory ESP-IDF log capture without starting Wi-Fi.
// Call this as the first operation in app_main so boot failures are retrievable.
void coyopedal_remote_capture_logs();

// Initialize persistent networking/crypto bookkeeping before NAM; radios stay off.
void coyopedal_remote_prepare_runtime();

// Starts the authenticated maintenance network, discovery responder and HTTP
// service. Returns false when no per-device remote_config.h was provisioned or
// when the radio/server could not be created.
bool coyopedal_remote_start();

// Re-publish the current radio state after the audio UI has initialized. Wi-Fi
// events can arrive while the framebuffer is still being created, so the UI
// performs this explicit final synchronization rather than guessing.
void coyopedal_remote_sync_network_ui();

// True when this boot should come up in maintenance: a mode switch or the end
// of an audio window asked for it. Consumes the request.
bool coyopedal_remote_maintenance_boot_requested();
// An audio boot is a bounded window, not a destination. Arm the unconditional
// return timer as the very first thing an audio boot does, so the board comes
// back to maintenance whether or not the engine, the USB host or the UI ever
// came up. Without this a failed audio start leaves the board with both radios
// off and no way in.
void coyopedal_remote_arm_audio_window();
// Apply what the armed window asked for once the UI is up: ENGAGE switches
// the pedal on (the boot preset recalls bypassed) and MASK <n> selects the
// effect blocks, so a window measures the amp and effects under load.
void coyopedal_remote_audio_window_configure();
// True when this boot was explicitly asked to go to audio. Audio is the default
// boot, but after a crash only an explicit request still goes there.
bool coyopedal_remote_audio_boot_requested();
void coyopedal_remote_mark_audio_boot();

// Mark a freshly written OTA slot valid after the complete audio application
// has initialized successfully.
void coyopedal_remote_validate_running_ota();

// EFFECT PROFILE and EFFECT SOAK persist a bounded test request in RTC memory,
// reboot into the same radio-free audio mode as normal use, then reboot back
// to maintenance with the result. prepare runs before the audio workers are
// created; start runs after the complete audio/UI stack is initialized.
void coyopedal_remote_prepare_requested_diagnostics();
void coyopedal_remote_start_requested_diagnostics();

// Serialize audio/maintenance mode transitions without blocking the UI.
void coyopedal_remote_mode_start();
bool coyopedal_remote_mode_busy();
void coyopedal_remote_toggle_mode();
// Implemented in main/main.cpp: block until BOOT/GPIO0 is released, bounded.
// GPIO0 doubles as the boot strapping pin, so resetting while it is held can
// strap the chip into ROM download mode. Every restart path waits here first.
void coyopedal_mode_button_wait_for_release();
bool coyopedal_remote_audio_mode();

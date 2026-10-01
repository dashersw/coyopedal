#pragma once
#include <cstddef>
#include <cstdint>

#ifdef ESP_PLATFORM
void internal_speaker_init();
void internal_speaker_start();
// Diagnostics only; call from the low-priority audio heartbeat.
void internal_speaker_report();
bool internal_speaker_available();
bool internal_speaker_enabled();
void internal_speaker_set_enabled(bool enabled);
std::uint32_t internal_speaker_revision();
const char* internal_speaker_status();
// Nonblocking fan-out of completed, packed stereo PCM. Never owns the USB ring.
void internal_speaker_submit(const std::uint64_t* frames, std::size_t count);
#else
inline bool internal_speaker_available() {
    return false;
}
inline bool internal_speaker_enabled() {
    return false;
}
inline void internal_speaker_set_enabled(bool) {}
inline std::uint32_t internal_speaker_revision() {
    return 0;
}
inline const char* internal_speaker_status() {
    return "Not available on this device";
}
#endif

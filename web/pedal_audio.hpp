#pragma once

// What the board bridge needs from the audio thread. Everything else it reaches
// through the DSP's own C headers, the same way src/native/ui/board_bridge.cpp
// does on the device.
namespace pedal_audio {

bool init();
void pump();
bool running();
int load_permille();
int input_peak_permille();
int output_peak_permille();
int channel_peak_permille(int channel);
int input_channels();
int input_channel();
void set_input_channel(int channel);
int underruns();

} // namespace pedal_audio

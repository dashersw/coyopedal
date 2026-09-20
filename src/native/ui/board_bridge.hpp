#pragma once
#include <string>
double pbGet(double key);
void pbSet(double key, double value);
std::string pbLabel(double kind, double index);
void pbAction(double action, double index, double value);
void pbPresetName(double action, const std::string& text);
bool pedalboard_panel_maintenance_init();
void pedalboard_panel_network(const char* ssid, const char* detail);

// 1: show transition/maintenance; 3: cancel a failed entry; 4/5: tuner on/off;
// 6/7: pedal bypassed/engaged; 8/9: gate off/on. 4 to 9 log a UI check after
// repainting.
bool pedalboard_panel_mode(unsigned mode);

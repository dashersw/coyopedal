#include "board_bridge.hpp"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "panel_presets.hpp"
#include "remote_service.h"
#include "model_catalog.h"
#include "board_ui.h"
#include "control.h"
#include "audio/effects.h"
#include "audio/tuner.h"
#include "audio/usb_frame_processor.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "controls.h"
#include "ui/tree_internal.h"

// The engine's live device pixel ratio (engine/ui/style.cpp). Declared rather
// than included: this file already reaches into the engine for the tree, and
// the one number it needs from the style system has a C entry point.
extern "C" double gea_style_get_device_pixel_ratio();

// The card behind the "SD card" row, which is a real slot on the board and a
// folder the player picks in a browser. Implemented by whichever of
// src/native/storage/sd_models.cpp and web/sd_models_web.cpp is linked.
extern "C" bool pedalboard_sd_open(void);
extern "C" const char* pedalboard_sd_hint(void);
extern "C" const char* pedalboard_sd_action(void);

// Defined in task.cpp: asks the Gea runtime for a repaint. The runtime owns the
// render task, the panel and touch; nothing here draws or polls hardware.
void s3_v1_ui_wake();

namespace {
int screen{}, selected{}, parameter{};
// The last pointer position, in the layout's POINTS. A pointer event carries
// physical pixels, so the ratio between them is the device pixel ratio -- which
// is 2 on the panel (gea.cssDevicePixelRatio in package.json) and is NOT 2
// everywhere this UI runs: the browser build supersamples, drawing the same
// 251x205-point layout into twice the pixels at a ratio of 4. Hardcoding 2 left
// every drag moving at twice the pointer's speed there, while taps -- which
// only ever ask which node is under a point -- stayed correct, so the error
// showed up as "the sliders are too fast" and nowhere else. Ask the style
// system, which is told the ratio at App::init and is the one thing that knows.
double pixel_ratio() {
    const double ratio = gea_style_get_device_pixel_ratio();
    return ratio > 0.0 ? ratio : 1.0;
}
double pointer_x{}, pointer_y{};
bool ready{};
std::atomic<uint32_t> network_revision{};
portMUX_TYPE network_mux = portMUX_INITIALIZER_UNLOCKED;
char network_name[33]{"Connecting"}, network_detail[64]{"Starting Wi-Fi"};
// Preset rename and confirmation fields (host keys 54 through 58).
double preset_fields[5]{};
std::string preset_draft;
std::string preset_status;
int64_t frame_time{};
char error[64]{};

// The library browser: a folder walk, a page at a time.
//
// The factory captures sit at the top, imports under "Imported", and the SD
// card's own directory tree under "SD card", so the card is organised however
// its owner laid it out. A flat list would need a UI node per capture, far past
// GEA_EMBEDDED_MAX_NODES, and Tree::createNode() fails silently past the cap, so
// the panel is only ever handed one page.
constexpr unsigned kBrowseRowsPerPage = 4;
constexpr char kSdFolder[] = "SD card/";
constexpr char kImportedFolder[] = "Imported/";

// The folder the browser is in, with a trailing slash, or empty at the top.
std::string browse_prefix;
unsigned browse_page{};

// Every capture's folder, in the same form as browse_prefix. Built once per
// catalogue, because the page walk below compares them for every capture.
std::vector<std::string> browse_folders;
unsigned browse_folders_for{COYOPEDAL_MODEL_NONE};

const std::vector<std::string>& browse_folder_table() {
    if (browse_folders_for == coyopedal_model_count)
        return browse_folders;
    browse_folders.assign(coyopedal_model_count, std::string{});
    for (unsigned i = 0; i < coyopedal_model_count; ++i) {
        const coyopedal_model_t& model = coyopedal_models[i];
        if (model.user_model) {
            browse_folders[i] = kImportedFolder;
        } else if (model.sd_model) {
            browse_folders[i] = kSdFolder;
            if (const char* const slash = std::strrchr(model.sd_filename, '/'))
                browse_folders[i].append(model.sd_filename, slash - model.sd_filename + 1);
        }
    }
    browse_folders_for = coyopedal_model_count;
    return browse_folders;
}

// The name of the subfolder of browse_prefix that `folder` lies in.
std::string browse_child(const std::string& folder) {
    const std::size_t start = browse_prefix.size();
    return folder.substr(start, folder.find('/', start) - start);
}

// One row of the current folder: a subfolder, standing for the first capture
// in it, or a capture.
struct BrowseRow {
    unsigned model;
    unsigned under; // captures inside a folder row, zero for a capture
    bool folder;
    // The card's own row, which exists whether or not there is a card in the
    // slot. A pedal that reads captures off a card should say so on the screen
    // where you choose one, rather than only once a card happens to be in it --
    // and on a page, where the "card" is a folder nobody has picked yet, that row
    // IS how you pick it. Tapping it with nothing behind it asks the platform for
    // a card instead of descending into an empty folder.
    bool card;
};

// The current folder's rows, subfolders first and then captures, each in
// catalogue order. Rebuilt once per move, keyed on the revision every move
// bumps, because the panel asks for every row's title on every frame.
struct BrowseListing {
    unsigned revision;
    unsigned models;
    std::vector<BrowseRow> rows;
};
BrowseListing browse_shown{};
bool browse_shown_valid{};

// Bumped by every move through the browser. Host key 15 is what the panel polls
// to decide whether anything is worth repainting, and coyopedal_ui_revision()
// hashes the pedal's state, not the browser's place in the library, so walking
// into a folder has to bump this for the panel to redraw.
unsigned browse_revision{};

void browse_touch() {
    ++browse_revision;
}

void browse_rebuild() {
    const auto& folders = browse_folder_table();
    auto& rows = browse_shown.rows;
    rows.clear();
    std::size_t folder_rows = 0;
    for (unsigned i = 0; i < coyopedal_model_count; ++i) {
        const std::string& folder = folders[i];
        if (folder.compare(0, browse_prefix.size(), browse_prefix) != 0)
            continue;
        if (folder.size() == browse_prefix.size())
            continue;
        const std::string name = browse_child(folder);
        bool seen = false;
        for (std::size_t row = 0; row < folder_rows && !seen; ++row) {
            if (browse_child(folders[rows[row].model]) == name) {
                ++rows[row].under;
                seen = true;
            }
        }
        if (!seen) {
            rows.push_back({i, 1U, true, false});
            ++folder_rows;
        }
    }
    for (unsigned i = 0; i < coyopedal_model_count; ++i)
        if (folders[i] == browse_prefix)
            rows.push_back({i, 0U, false, false});
    // The card, at the top of the top level, always. When it holds captures the
    // loop above has already made its folder row and this only moves it up;
    // when it does not, this is the row that goes looking for one.
    if (browse_prefix.empty()) {
        const auto card = std::find_if(rows.begin(), rows.end(), [&](const BrowseRow& row) {
            return row.folder &&
                   folders[row.model].compare(0, sizeof kSdFolder - 1U, kSdFolder) == 0;
        });
        if (card == rows.end())
            rows.insert(rows.begin(), {0U, 0U, true, true});
        else
            std::rotate(rows.begin(), card, card + 1);
    } else if (browse_prefix == kSdFolder && pedalboard_sd_action()[0] != '\0') {
        // Inside the card, last: the way to swap one. On the board that is a
        // rescan for a card that was changed while the pedal was on, in the
        // browser it is the folder picker again -- the only way to change the
        // folder now that the page around the panel does not offer one.
        rows.push_back({0U, 0U, false, true});
    }
    // Clamped here rather than on read: the catalogue can shrink underneath a
    // parked page, and a page past the end would render an empty screen with
    // Prev as the only way out. Clamping is not a move, so it does not bump the
    // revision.
    const unsigned pages =
        rows.empty()
            ? 1U
            : (static_cast<unsigned>(rows.size()) + kBrowseRowsPerPage - 1U) / kBrowseRowsPerPage;
    if (browse_page >= pages)
        browse_page = pages - 1U;
    browse_shown.revision = browse_revision;
    browse_shown.models = coyopedal_model_count;
    browse_shown_valid = true;
}

const std::vector<BrowseRow>& browse_rows() {
    if (!browse_shown_valid || browse_shown.revision != browse_revision ||
        browse_shown.models != coyopedal_model_count)
        browse_rebuild();
    return browse_shown.rows;
}

unsigned browse_total() {
    return static_cast<unsigned>(browse_rows().size());
}

unsigned browse_page_count() {
    const unsigned total = browse_total();
    return total == 0U ? 1U : (total + kBrowseRowsPerPage - 1U) / kBrowseRowsPerPage;
}

unsigned browse_current_page() {
    browse_rows();
    return browse_page;
}

unsigned browse_page_rows() {
    const unsigned total = browse_total();
    const unsigned start = browse_current_page() * kBrowseRowsPerPage;
    return total > start ? std::min(kBrowseRowsPerPage, total - start) : 0U;
}

// The row at `row` on the current page, or nullptr past the end.
const BrowseRow* browse_row(const unsigned row) {
    if (row >= browse_page_rows())
        return nullptr;
    return &browse_rows()[browse_page * kBrowseRowsPerPage + row];
}

// How deep the browser is: zero at the top.
unsigned browse_depth() {
    return static_cast<unsigned>(std::count(browse_prefix.begin(), browse_prefix.end(), '/'));
}

// A folder whose only content is one subfolder asks nothing and is not worth a
// tap, so the walk passes through it in both directions.
bool browse_single_folder() {
    const auto& rows = browse_rows();
    return rows.size() == 1U && rows[0].folder && !rows[0].card;
}

void browse_enter_folder(const unsigned model) {
    browse_prefix += browse_child(browse_folder_table()[model]);
    browse_prefix += '/';
    browse_page = 0;
    browse_touch();
}

bool browse_descend(const unsigned row) {
    const BrowseRow* const entry = browse_row(row);
    if (entry == nullptr || !entry->folder)
        return false;
    if (entry->card)
        // Nothing to walk into: ask for a card. On the board that is a rescan of
        // the slot, on a page it is the folder picker, and either way the
        // catalogue comes back through coyopedal_ui_catalog_changed().
        return pedalboard_sd_open();
    browse_enter_folder(entry->model);
    while (browse_single_folder())
        browse_enter_folder(browse_rows()[0].model);
    return true;
}

void browse_ascend() {
    do {
        browse_prefix.pop_back();
        browse_prefix.erase(browse_prefix.rfind('/') + 1);
        browse_page = 0;
        browse_touch();
    } while (!browse_prefix.empty() && browse_single_folder());
}

// The name of the folder the browser is in.
std::string browse_folder_name() {
    if (browse_prefix.empty())
        return "Amplifiers";
    const std::size_t end = browse_prefix.size() - 1U;
    const std::size_t slash = browse_prefix.rfind('/', end - 1U);
    const std::size_t start = slash == std::string::npos ? 0U : slash + 1U;
    return browse_prefix.substr(start, end - start);
}

// The panel's effect slots are the DSP block numbers.
coyopedal_fx_block_t block(int slot) {
    return static_cast<coyopedal_fx_block_t>(std::clamp(slot, 0, COYOPEDAL_FX_BLOCK_COUNT - 1));
}
void frame() {
    if (!ready)
        return;
    frame_time = esp_timer_get_time();
#if !defined(GEA_EMBEDDED_NO_DISPLAY) || !GEA_EMBEDDED_NO_DISPLAY
    // Without a display there is no render task to wake, and the wake would
    // notify a task handle that was never created.
    s3_v1_ui_wake();
#endif
}
// Read after a repaint by the on-device UI check. Best effort: the render task
// owns the tree, so a concurrent mount can make this miss a node.
bool text_mounted(const char* text) {
    auto& tree = gea::embedded::ui::Tree::instance();
    for (int i = 0; i < tree.nodeCount(); ++i) {
        if (tree.node(i).text != text)
            continue;
        int node = i;
        for (int depth = 0; node >= 0 && depth < tree.nodeCount(); ++depth) {
            if (gea::embedded::ui::isDisplayNone(tree.node(node).style))
                break;
            if (node == tree.mountedRoot())
                return true;
            node = tree.node(node).parent;
        }
    }
    return false;
}
} // namespace
double pbGet(double key) {
    const int k = static_cast<int>(key);
    if (k >= 54 && k <= 58)
        return preset_fields[k - 54];
    if (k >= 100 && k < 196) {
        const int slot = (k - 100) / 16, param = (k - 100) % 16;
        const auto b = block(slot);
        const auto* info = coyopedal_fx_param_info(b, param);
        return info && info->maximum > info->minimum
                   ? std::clamp((coyopedal_fx_param(b, param) - info->minimum) /
                                    double(info->maximum - info->minimum),
                                0.0, 1.0)
                   : 0;
    }
    switch (k) {
    case 1:
        return pointer_x;
    case 2:
        return pointer_y;
    case 4:
        return screen;
    case 5:
        return selected;
    case 7:
        return parameter;
    case 8:
        return coyopedal_model_count;
    case 9:
        return coyopedal_fx_param_count(block(selected));
    case 10:
        return !coyopedal_pedal_dsp_pedal_bypassed();
    case 11:
        return coyopedal_pedal_dsp_tuner_active();
    case 12:
        return coyopedal_active_model();
    case 13:
        return frame_time / 1000.0;
    case 15:
        return coyopedal_ui_revision() + network_revision.load(std::memory_order_relaxed) +
               browse_revision;
    case 16:
        return !coyopedal_pedal_dsp_bypassed();
    case 17:
        return panel_presets::active();
    case 18:
        return panel_presets::edited();
    case 26:
        return panel_presets::count();
    case 27:
        return browse_page_rows();
    case 28:
        return browse_total();
    case 29:
        return browse_current_page();
    case 30:
        return browse_page_count();
    case 31:
        return browse_depth();
    case 60:
    case 61: {
        coyopedal_tuner_reading_t r{};
        coyopedal_tuner_read(&r);
        return k == 60 ? r.voiced : r.cents;
    }
    default:
        if (k >= 32 && k < 32 + static_cast<int>(kBrowseRowsPerPage)) {
            const BrowseRow* const row = browse_row(static_cast<unsigned>(k - 32));
            return row != nullptr && !row->folder && !row->card &&
                   row->model == coyopedal_active_model();
        }
        // Whether a row is a folder: a tap on it descends rather than loading.
        if (k >= 44 && k < 44 + static_cast<int>(kBrowseRowsPerPage)) {
            const BrowseRow* const row = browse_row(static_cast<unsigned>(k - 44));
            return row != nullptr && row->folder;
        }
        // Whether a row asks the platform for a card rather than loading a
        // capture: the swap row inside the card. It looks like a capture to the
        // panel -- no chevron, nothing to descend into -- but nothing is
        // prepared when it is tapped, and the tap has to reach the platform
        // inside the gesture that made it, which is what a browser's folder
        // picker will not open without.
        if (k >= 48 && k < 48 + static_cast<int>(kBrowseRowsPerPage)) {
            const BrowseRow* const row = browse_row(static_cast<unsigned>(k - 48));
            return row != nullptr && row->card && !row->folder;
        }
        return k >= 20 && k < 26 ? coyopedal_fx_enabled(block(k - 20)) : 0;
    }
}
void pbSet(double key, double value) {
    if (coyopedal_remote_mode_busy())
        return;
    const int k = static_cast<int>(key);
    if (k >= 54 && k <= 58) {
        preset_fields[k - 54] = value;
        return;
    }
    switch (k) {
    case 1:
        pointer_x = value / pixel_ratio();
        break;
    case 2:
        pointer_y = value / pixel_ratio();
        break;
    case 4:
        screen = std::clamp(static_cast<int>(value), 0, 10);
        break;
    case 5:
        selected = std::clamp(static_cast<int>(value), 0, 5);
        break;
    case 7:
        parameter = std::max(0, static_cast<int>(value));
        break;
    }
}
std::string pbLabel(double kind, double index) {
    const unsigned i = static_cast<unsigned>(index);
    switch (static_cast<int>(kind)) {
    case 0: {
        const auto active = coyopedal_active_model();
        return active < coyopedal_model_count ? coyopedal_models[active].name : "SELECT AN AMP";
    }
    case 1:
        return coyopedal_fx_name(block(i));
    case 2:
        return i < coyopedal_model_count ? coyopedal_models[i].name : "";
    case 3: {
        const auto b = block(i);
        const auto* info = coyopedal_fx_param_info(b, parameter);
        if (!info)
            return "";
        char label[64];
        std::snprintf(label, sizeof label, "%s  %d", info->label, coyopedal_fx_param(b, parameter));
        return label;
    }
    case 4: {
        coyopedal_tuner_reading_t reading{};
        coyopedal_tuner_read(&reading);
        if (!reading.voiced)
            return "LISTENING / AUDIO MUTED";
        char label[64];
        std::snprintf(label, sizeof label, "%s%d  %+.0f cents",
                      coyopedal_tuner_note_name(reading.note), reading.octave,
                      static_cast<double>(reading.cents));
        return label;
    }
    case 5:
        return error;
    case 6:
    case 7: {
        const auto b = block(i / 16);
        const auto* info = coyopedal_fx_param_info(b, i % 16);
        if (!info)
            return "";
        if (kind == 6)
            return info->label;
        const int value = coyopedal_fx_param(b, i % 16);
        char label[32];
        switch (info->unit) {
        case COYOPEDAL_FX_UNIT_DECIBEL_TENTHS:
            std::snprintf(label, sizeof label, "%.1f dB", value / 10.0);
            break;
        case COYOPEDAL_FX_UNIT_MILLISECONDS:
            std::snprintf(label, sizeof label, "%d ms", value);
            break;
        case COYOPEDAL_FX_UNIT_PERCENT:
            std::snprintf(label, sizeof label, "%d%%", value);
            break;
        case COYOPEDAL_FX_UNIT_RATIO_TENTHS:
            std::snprintf(label, sizeof label, "%.1f:1", value / 10.0);
            break;
        case COYOPEDAL_FX_UNIT_HERTZ:
            std::snprintf(label, sizeof label, "%d Hz", value);
            break;
        case COYOPEDAL_FX_UNIT_HERTZ_TENTHS:
            std::snprintf(label, sizeof label, "%.1f Hz", value / 10.0);
            break;
        case COYOPEDAL_FX_UNIT_OUTPUT_MODE:
            std::snprintf(label, sizeof label, "%s", value ? "Mono" : "Stereo");
            break;
        }
        return label;
    }
    case 9:
        return panel_presets::name(panel_presets::active());
    case 10:
        return panel_presets::name(i);
    case 11:
        return preset_draft;
    case 12:
        return preset_status;
    case 15:
    case 16: {
        char value[64];
        portENTER_CRITICAL(&network_mux);
        std::snprintf(value, sizeof value, "%s", kind == 15 ? network_name : network_detail);
        portEXIT_CRITICAL(&network_mux);
        return value;
    }
    case 13:
    case 14: {
        coyopedal_tuner_reading_t r{};
        coyopedal_tuner_read(&r);
        char value[32];
        if (kind == 13)
            std::snprintf(value, sizeof value, "%s%d", coyopedal_tuner_note_name(r.note), r.octave);
        else
            std::snprintf(value, sizeof value, "%.1f Hz", double(r.frequency));
        return value;
    }
    case 8:
        return i < coyopedal_model_count && coyopedal_models[i].sd_model ? "SD card"
                                                                         : "Factory capture";
    case 17: {
        const BrowseRow* const row = browse_row(i);
        if (row == nullptr)
            return "";
        if (row->card)
            return row->folder ? std::string(kSdFolder, sizeof kSdFolder - 2U)
                               : std::string(pedalboard_sd_action());
        return row->folder ? browse_child(browse_folder_table()[row->model])
                           : coyopedal_models[row->model].name;
    }
    case 18: {
        const BrowseRow* const row = browse_row(i);
        if (row == nullptr)
            return "";
        char text[48]{};
        // An empty card says what would fill it, which is not the same sentence
        // on a board with a slot and a page with a folder picker. The swap row
        // inside a card that already has captures says nothing; its own title
        // is the whole of it.
        if (row->card)
            return row->folder ? std::string(pedalboard_sd_hint()) : std::string();
        if (row->folder) {
            std::snprintf(text, sizeof text, "%u capture%s", row->under,
                          row->under == 1U ? "" : "s");
            return text;
        }
        // A capture carries no subtitle; the playing one is marked by a dot.
        return "";
    }
    case 19:
        return browse_folder_name();
    case 20: {
        // Everything under the current folder, however deep.
        unsigned count = 0;
        const auto& folders = browse_folder_table();
        for (const auto& folder : folders)
            if (folder.compare(0, browse_prefix.size(), browse_prefix) == 0)
                ++count;
        char text[48]{};
        std::snprintf(text, sizeof text, "%u capture%s", count, count == 1U ? "" : "s");
        return text;
    }
    default:
        return "";
    }
}
void pbAction(double action, double index, double value) {
    if (coyopedal_remote_mode_busy())
        return;
    if (!coyopedal_remote_audio_mode() && action != 13)
        return;
    const auto b = block(index);
    bool ok = true;
    error[0] = '\0';
    preset_status.clear();
    switch (static_cast<int>(action)) {
    case 0:
        ok = coyopedal_ui_control_set_model(index);
        if (ok)
            screen = 0;
        break;
    case 1:
        ok = coyopedal_ui_control_set_enabled(b, !coyopedal_fx_enabled(b));
        break;
    case 2: {
        const auto* info = coyopedal_fx_param_info(b, parameter);
        if (!info) {
            ok = false;
            break;
        }
        const int next = coyopedal_fx_param(b, parameter) + static_cast<int>(value) * info->step;
        ok = coyopedal_ui_control_set_param(
            b, parameter,
            std::clamp(next, static_cast<int>(info->minimum), static_cast<int>(info->maximum)));
        break;
    }
    case 3:
        ok = coyopedal_ui_control_set_engaged(coyopedal_pedal_dsp_pedal_bypassed());
        break;
    case 4:
        ok = coyopedal_ui_control_set_tuner(!coyopedal_pedal_dsp_tuner_active());
        break;
    case 5:
    case 8:
        ok = panel_presets::load(action == 5 ? 0 : static_cast<unsigned>(index));
        if (ok) {
            screen = 0;
        }
        break;
    case 9:
        ok = panel_presets::save();
        if (ok)
            preset_status = "Saved";
        break;
    case 11:
        ok = panel_presets::save();
        if (ok)
            ok = panel_presets::load(index);
        if (ok) {
            screen = 0;
        }
        break;
    case 13:
        if (screen != 9 && !panel_presets::remember_active()) {
            std::snprintf(error, sizeof error, "%s", panel_presets::error());
            return;
        }
        coyopedal_remote_toggle_mode();
        break;
    case 12:
        ok = panel_presets::erase(index);
        if (ok) {
            screen = 3;
            preset_status = "Deleted";
        }
        break;
    case 7:
        ok = coyopedal_ui_control_set_enabled(COYOPEDAL_CONTROL_AMP_BLOCK,
                                              coyopedal_pedal_dsp_bypassed());
        break;
    case 6: {
        const auto* info = coyopedal_fx_param_info(b, parameter);
        if (!info || info->step <= 0) {
            ok = false;
            break;
        }
        const int steps =
            std::lround(std::clamp(value, 0.0, 1.0) * (info->maximum - info->minimum) / info->step);
        const int next =
            std::clamp(info->minimum + steps * info->step, static_cast<int>(info->minimum),
                       static_cast<int>(info->maximum));
        if (next != coyopedal_fx_param(b, parameter))
            ok = coyopedal_ui_control_set_param(b, parameter, next);
        break;
    }
    case 14: {
        const BrowseRow* const row = browse_row(static_cast<unsigned>(index));
        if (row == nullptr) {
            ok = false;
            break;
        }
        if (row->card && !row->folder) {
            ok = pedalboard_sd_open();
        } else if (row->folder) {
            ok = browse_descend(static_cast<unsigned>(index));
        } else {
            ok = coyopedal_ui_control_set_model(row->model);
            if (ok)
                screen = 0;
        }
        break;
    }
    case 15:
        // Up one level, and out to the home screen from the top -- the same
        // gesture the header's back button has on every other screen.
        if (browse_prefix.empty())
            screen = 0;
        else
            browse_ascend();
        break;
    case 16: {
        const unsigned pages = browse_page_count();
        const int next = static_cast<int>(browse_current_page()) + static_cast<int>(value);
        const unsigned wanted =
            static_cast<unsigned>(std::clamp(next, 0, static_cast<int>(pages) - 1));
        if (wanted != browse_page) {
            browse_page = wanted;
            ++browse_revision;
        }
        break;
    }
    }
    if (!ok && (action == 5 || (action >= 8 && action <= 12))) {
        std::snprintf(error, sizeof error, "%s", panel_presets::error());
        return;
    }
    if (!ok)
        std::snprintf(error, sizeof error, "%s",
                      static_cast<int>(action) == 0 && coyopedal_last_model_error()[0]
                          ? coyopedal_last_model_error()
                          : "CONTROL FAILED");
}
// The library changed underneath the panel: a card went in or came out, or a
// folder was picked in the browser build. Everything the browser screen caches
// -- the folder table, the current listing, the page -- is keyed on the
// catalogue it was built from, so it is dropped rather than patched, and the
// revision is bumped because host key 15 is the only thing the panel watches to
// decide whether a repaint is worth doing.
//
// `reveal` opens the browser on the card, and belongs to the moment a card is
// put in or a folder is picked -- the one time the player is waiting to see what
// arrived. It is off when a page reloads and finds the folder it was given last
// time: that is not news, and a pedal that came up on its library screen every
// morning instead of its own would be wrong. Even then it only moves from the
// home screen or the browser itself; interrupting a rename or a confirmation to
// announce a card would be worse than saying nothing.
extern "C" void coyopedal_ui_catalog_changed(const bool reveal) {
    coyopedal_models_rescan();
    browse_prefix.clear();
    browse_page = 0;
    browse_folders_for = COYOPEDAL_MODEL_NONE;
    browse_shown_valid = false;
    for (unsigned i = 0; reveal && i < coyopedal_model_count; ++i) {
        if (coyopedal_models[i].sd_model) {
            if (screen == 0 || screen == 1) {
                screen = 1;
                browse_prefix = kSdFolder;
            }
            break;
        }
    }
    browse_touch();
    frame();
}

// A repaint, for state the panel reads through the bridge but nothing here
// wrote -- presets restored from outside, for one.
extern "C" void coyopedal_ui_touch() {
    browse_touch();
    frame();
}

extern "C" bool coyopedal_ui_init() {
    coyopedal_controls_init();
    {
        panel_presets::init();
        if (panel_presets::count())
            panel_presets::load(panel_presets::active());
        if (panel_presets::error()[0])
            std::snprintf(error, sizeof error, "%s", panel_presets::error());
    }
    ready = true;
    frame();
    return ready;
}
extern "C" const char* coyopedal_ui_screen_name() {
    return coyopedal_pedal_dsp_tuner_active() ? "tuner"
           : screen == 1                      ? "models"
           : screen == 2                      ? "editor"
           : screen == 3                      ? "presets"
           : screen == 6                      ? "keyboard"
           : screen == 7                      ? "confirm"
           : screen == 9                      ? "maintenance"
           : screen == 8                      ? "setup"
           : screen == 10                     ? "confirm"
                                              : "home";
}
void pbPresetName(double action, const std::string& text) {
    if (coyopedal_remote_mode_busy() || !coyopedal_remote_audio_mode())
        return;
    error[0] = 0;
    preset_status.clear();
    if (action == 0) {
        preset_draft = text.substr(0, 23);
        pbSet(55, 1);
    }
    if (action == 1 || action == 2) {
        const auto before = preset_draft;
        if (action == 1) {
            for (unsigned char c : text)
                if (c >= 32 && c <= 126 && preset_draft.size() < 23)
                    preset_draft += c;
        } else if (!preset_draft.empty())
            preset_draft.pop_back();
        if (before != preset_draft) {
            if (preset_draft.empty() || preset_draft.back() == ' ')
                pbSet(55, 1);
            else if (pbGet(55) != 2)
                pbSet(55, 0);
        }
    }
    if (action == 3) {
        auto name = preset_draft;
        auto first = name.find_first_not_of(' '), last = name.find_last_not_of(' ');
        name = first == std::string::npos ? "" : name.substr(first, last - first + 1);
        const bool create = pbGet(56) != 0;
        const bool ok = create ? panel_presets::add(name.c_str())
                               : panel_presets::rename(pbGet(54), name.c_str());
        if (ok) {
            screen = create ? 0 : 3;
            preset_status = create ? "" : "Renamed";
            pbSet(56, 0);
        } else
            std::snprintf(error, sizeof error, "%s", panel_presets::error());
    }
}

bool pedalboard_panel_maintenance_init() {
    screen = 9;
    ready = true;
    frame();
    return ready;
}
void pedalboard_panel_network(const char* ssid, const char* detail) {
    portENTER_CRITICAL(&network_mux);
    std::snprintf(network_name, sizeof network_name, "%s", ssid ? ssid : "Connecting");
    std::snprintf(network_detail, sizeof network_detail, "%s", detail ? detail : "Starting Wi-Fi");
    portEXIT_CRITICAL(&network_mux);
    network_revision.fetch_add(1, std::memory_order_relaxed);
}

bool pedalboard_panel_mode(unsigned mode) {
    // An audio window that asks for the pedal engaged switches it on through
    // the same control the footswitch uses, and logs what the panel shows.
    if (mode == 7) {
        coyopedal_ui_control_set_engaged(true);
        frame();
        vTaskDelay(pdMS_TO_TICKS(32));
        frame();
        ESP_LOGI("panel", "UI CHECK tuner=%u title=%u waiting=%u engaged=%u",
                 coyopedal_pedal_dsp_tuner_active(), text_mounted("Tuner"),
                 text_mounted("Play a string"), !coyopedal_pedal_dsp_pedal_bypassed());
        return true;
    }

    if (mode == 1) {
        if (coyopedal_remote_audio_mode() && !panel_presets::remember_active()) {
            std::snprintf(error, sizeof error, "%s", panel_presets::error());
            frame();
            return false;
        }
        screen = 9;
        coyopedal_pedal_dsp_set_tuner(false);
        pedalboard_panel_network("Please wait", coyopedal_remote_audio_mode() ? "Stopping audio"
                                                                              : "Restoring audio");
    } else {
        screen = 8;
        std::snprintf(error, sizeof error, "Could not stop audio; retry");
    }
    vTaskDelay(pdMS_TO_TICKS(16));
    frame();
    return true;
}

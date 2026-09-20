// The pedal's audio, in the browser.
//
// This is the board's own DSP: src/audio/{processor,effects,tuner}.cpp and the
// A2-Full engine, compiled for wasm and run on an AudioWorklet thread against
// the same memory the panel draws from. That is the board's arrangement too --
// audio on one core, the panel on the other -- and it is what lets the panel's
// controls reach the engine with a plain function call, exactly as
// src/native/ui/board_bridge.cpp does on the device.
//
// What the ESP32 supplies and a page does not is replaced here and nowhere
// else: the USB transport becomes a MediaStream, flash-backed model storage
// becomes bytes the page fetches, and the two DSP stages the board splits
// across its cores run one after the other on the single audio thread. The
// signal chain itself -- what runs, in what order, at what block size -- is
// copied from usb_audio.cpp's stage A and stage B, because a demo that sounded
// different from the pedal would not be a demo of the pedal.

#include <emscripten/emscripten.h>
#include <emscripten/webaudio.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

#include "audio/effects.h"
#include "audio/processor.hpp"
#include "audio/tuner.h"
#include "audio/usb_frame_processor.h"
#include "nam/runtime_allocator.h"

#include "pedal_audio.hpp"

namespace {

constexpr std::size_t kBlockFrames = COYOPEDAL_PEDAL_BLOCK_FRAMES;
constexpr double kSampleRate = 48000.0;
// The board's line: two seconds at 48 kHz, plus the four samples the block
// reader runs past the tap.
constexpr std::size_t kDelaySamples = 48000U * 2U + 4U;
// A render quantum is 128 frames and the block is 64, so this never holds more
// than one quantum. It exists so a browser that ever hands over a size that is
// not a multiple of the block still gets whole blocks.
constexpr std::size_t kFifoFrames = 1024;

} // namespace

// The engine, with the name the device's control surface uses for it
// (s3_dsp_control.cpp declares `extern coyopedal::pedal::Processor g_engine`).
coyopedal::pedal::Processor g_engine;

namespace {

coyopedal::pedal::Processor::BlockScratch g_scratch;

float g_block[kBlockFrames];
float g_block_right[kBlockFrames];
float* g_delay_line = nullptr;

float g_input_fifo[kFifoFrames];
float g_output_left[kFifoFrames];
float g_output_right[kFifoFrames];
std::size_t g_input_fill = 0;
std::size_t g_output_fill = 0;

// The control state the board keeps in usb_audio.cpp. Atomics because the panel
// writes them on the main thread and the block loop reads them on the worklet.
std::atomic<bool> g_pedal_bypassed{false};
std::atomic<bool> g_tuner_active{false};
std::atomic<int> g_update_depth{0};
std::atomic<bool> g_update_open{false};
std::atomic<bool> g_started{false};

std::atomic<int> g_load_permille{0};
std::atomic<int> g_input_peak_permille{0};
std::atomic<int> g_output_peak_permille{0};
// One peak per input channel, so the page can show WHICH input the guitar is
// in rather than only how loud the chosen one is. 32 is past any interface a
// browser will hand a page in one stream, and the array costs 128 bytes.
constexpr int kMaxInputChannels = 32;
std::atomic<int> g_channel_peak_permille[kMaxInputChannels]{};
std::atomic<int> g_input_channels{0};
// Which channel the pedal takes. The board has no such setting -- it reads
// channel 0 because that is the channel its USB interface puts a guitar on --
// but an 8-in interface on a desk does not, and a page that could only listen
// to input 1 would be unusable with one.
std::atomic<int> g_input_channel{0};
std::atomic<int> g_underruns{0};
std::atomic<int> g_ready{0};

// The worklet thread's stack. No allocation happens on it: every buffer above
// is static and the engine's scratch is preallocated.
alignas(16) std::uint8_t g_audio_stack[16384];

// A context the emscripten helper cannot build: it passes latencyHint as a
// string, so it can only ask for one of the named categories, the smallest of
// which still reserves two render quanta. The numeric hint asks for one -- 128
// frames, 2.7 ms -- which is the floor the Web Audio spec allows, and the only
// latency in this page a guitarist can feel.
//
// 48 kHz is not a preference: every profile is captured at it, and a context at
// another rate would put a resampler in a path whose whole point is that there
// is nothing in it.
// emscripten_get_now() is unusable HERE. In an AudioWorkletGlobalScope it
// resolves to a Date.now()-style epoch clock: measured over 1126 consecutive
// quanta with the engine running, `after != before` was true exactly ZERO
// times, so every callback appeared to take no time at all and the load read a
// flat 0% while the pedal was plainly processing audio. A callback that runs in
// tens of microseconds never crosses a millisecond boundary.
//
// `performance.now()` is exposed in the worklet scope and, on a
// cross-origin-isolated page -- which this one must be anyway, for the shared
// memory the worklet runs on -- it resolves to 5 us. That is two orders of
// magnitude finer than the thing being measured, which is what a load figure
// needs. Date.now() remains the fallback, and on a host that only has it the
// figure degrades to the old useless zero rather than to a wrong number.
// clang-format off
// The body is JavaScript, and clang-format reads it as C++: left to itself it
// splits `!==` into `!= =`, which is a syntax error emcc only reports once the
// glue is loaded in a browser.
EM_JS(double, pedal_now_ms, (), {
    return typeof performance !== 'undefined' && performance.now ? performance.now() : Date.now();
})
// clang-format on

EM_JS(int, pedal_web_create_context, (), {
    const context =
        new AudioContext({latencyHint : 0, sampleRate : 48000, renderSizeHint : 'hardware'});
    return emscriptenRegisterAudioObject(context);
})

// The page owns everything around the pedal: where the signal comes from and
// where it goes. The module only ever hands over handles.
//
// The pedal itself is wired unconditionally -- node to destination, both
// published -- and a source is attached only if the page has opened one. A
// pedal with nothing plugged into it is a pedal, not an error: the page can
// attach a source afterwards, which is exactly what changing the input device
// does, and the panel stays usable while the browser is still asking about the
// microphone.
EM_JS(void, pedal_web_wire_graph, (int context_handle, int node_handle), {
    const context = emscriptenGetAudioObject(context_handle);
    const node = emscriptenGetAudioObject(node_handle);
    node.connect(context.destination);
    globalThis.pedalAudioContext = context;
    globalThis.pedalNode = node;
    const stream = globalThis.pedalInputStream;
    if (!stream)
        return;
    const source = context.createMediaStreamSource(stream);
    source.connect(node);
    globalThis.pedalInputSource = source;
})

EM_JS_DEPS(pedal_web_audio_deps, "$emscriptenRegisterAudioObject,$emscriptenGetAudioObject")

void record_peak(std::atomic<int>& target, const float peak) {
    target.store(static_cast<int>(1000.0F * std::min(peak, 1.0F)), std::memory_order_relaxed);
}

// One DSP block, in the order usb_audio.cpp runs it. The board splits this
// across two cores at layer 8; here the layers run in one pass, which is the
// only difference and changes no arithmetic.
void process_block(float* const samples, float* const right, bool& stereo) noexcept {
    stereo = false;
    if (g_tuner_active.load(std::memory_order_relaxed)) {
        coyopedal_tuner_feed(samples, kBlockFrames);
        std::fill_n(samples, kBlockFrames, 0.0F);
        return;
    }
    if (g_pedal_bypassed.load(std::memory_order_relaxed)) {
        return;
    }
    const bool chain = coyopedal_fx_any_enabled();
    const bool delay = coyopedal_fx_enabled(COYOPEDAL_FX_DELAY);
    const bool reverb = coyopedal_fx_enabled(COYOPEDAL_FX_REVERB);
    if (chain) {
        coyopedal_fx_process_pre(samples, kBlockFrames);
    }
    if (!g_engine.bypassed()) {
        if (g_engine.begin_block(samples, kBlockFrames, g_scratch)) {
            g_engine.process_layers(g_scratch, 0, coyopedal::pedal::Processor::kStagedLayerCount);
            g_engine.finish_block(g_scratch, samples);
        }
    }
    if (delay) {
        coyopedal_fx_process_delay(samples, kBlockFrames);
    }
    if (reverb) {
        coyopedal_fx_process_reverb_stereo(samples, right, kBlockFrames);
        stereo = true;
    }
}

bool process(const int numInputs, const AudioSampleFrame* const inputs, const int numOutputs,
             AudioSampleFrame* const outputs, int, const AudioParamFrame*, void*) {
    if (numOutputs < 1) {
        return true;
    }
    const double started = pedal_now_ms();
    const int frames = outputs[0].samplesPerChannel;
    const int out_channels = outputs[0].numberOfChannels;
    const int in_channels = numInputs < 1 ? 0 : inputs[0].numberOfChannels;
    g_input_channels.store(in_channels, std::memory_order_relaxed);

    // Nothing plugged in: silence out, and NOT an underrun. The underrun count
    // answers one question -- can this machine run the pedal in real time -- and
    // a quantum with no input to run is not evidence either way. The zeroed
    // peaks are what says the input is absent.
    if (in_channels < 1) {
        std::memset(outputs[0].data, 0,
                    static_cast<std::size_t>(frames) * out_channels * sizeof(float));
        g_input_peak_permille.store(0, std::memory_order_relaxed);
        g_output_peak_permille.store(0, std::memory_order_relaxed);
        return true;
    }

    // The channel the pedal takes, clamped rather than trusted: the page picks a
    // channel and the device can then be swapped for one with fewer.
    const int selected =
        std::clamp(g_input_channel.load(std::memory_order_relaxed), 0, in_channels - 1);

    // Every channel is metered, not just the one being played: that is how the
    // page can say "the signal is on input 3" rather than "there is no signal".
    // The stream is planar -- channel n starts at data[n * frames].
    const int metered = std::min(in_channels, kMaxInputChannels);
    float selected_peak = 0.0F;
    for (int channel = 0; channel < metered; ++channel) {
        const float* const samples = &inputs[0].data[channel * frames];
        float peak = 0.0F;
        for (int frame = 0; frame < frames; ++frame) {
            peak = std::max(peak, std::abs(samples[frame]));
        }
        record_peak(g_channel_peak_permille[channel], peak);
        if (channel == selected) {
            selected_peak = peak;
        }
    }
    for (int channel = metered; channel < kMaxInputChannels; ++channel) {
        g_channel_peak_permille[channel].store(0, std::memory_order_relaxed);
    }
    record_peak(g_input_peak_permille, selected_peak);

    const float* const source = &inputs[0].data[selected * frames];

    const std::size_t room = kFifoFrames - g_input_fill;
    const std::size_t taken = std::min(room, static_cast<std::size_t>(frames));
    std::memcpy(&g_input_fifo[g_input_fill], source, taken * sizeof(float));
    g_input_fill += taken;

    // While a control update is open the engine's weights may be half replaced,
    // so the block passes dry rather than running it -- the same guard
    // coyopedal_pedal_dsp_begin_update() opens on the board.
    const bool updating = g_update_open.load(std::memory_order_acquire);
    std::size_t consumed = 0;
    while (g_input_fill - consumed >= kBlockFrames && g_output_fill + kBlockFrames <= kFifoFrames) {
        std::memcpy(g_block, &g_input_fifo[consumed], kBlockFrames * sizeof(float));
        consumed += kBlockFrames;
        bool stereo = false;
        if (!updating) {
            process_block(g_block, g_block_right, stereo);
        }
        std::memcpy(&g_output_left[g_output_fill], g_block, kBlockFrames * sizeof(float));
        std::memcpy(&g_output_right[g_output_fill], stereo ? g_block_right : g_block,
                    kBlockFrames * sizeof(float));
        g_output_fill += kBlockFrames;
    }
    if (consumed != 0) {
        g_input_fill -= consumed;
        std::memmove(g_input_fifo, &g_input_fifo[consumed], g_input_fill * sizeof(float));
    }

    const std::size_t emitted = std::min(g_output_fill, static_cast<std::size_t>(frames));
    if (emitted < static_cast<std::size_t>(frames)) {
        g_underruns.fetch_add(1, std::memory_order_relaxed);
    }
    float output_peak = 0.0F;
    for (int channel = 0; channel < out_channels; ++channel) {
        float* const out = &outputs[0].data[channel * frames];
        const float* const source = channel == 0 ? g_output_left : g_output_right;
        for (std::size_t frame = 0; frame < emitted; ++frame) {
            out[frame] = source[frame];
            output_peak = std::max(output_peak, std::abs(source[frame]));
        }
        std::fill(&out[emitted], &out[frames], 0.0F);
    }
    record_peak(g_output_peak_permille, output_peak);
    if (emitted != 0) {
        g_output_fill -= emitted;
        std::memmove(g_output_left, &g_output_left[emitted], g_output_fill * sizeof(float));
        std::memmove(g_output_right, &g_output_right[emitted], g_output_fill * sizeof(float));
    }

    // What fraction of the quantum's own wall time the callback took: the only
    // number that says whether this machine can run the pedal. Accumulated over
    // a window because a single 2.7 ms quantum is dominated by whatever else the
    // audio thread was just doing; 64 of them is about a sixth of a second.
    static double window_spent_ms = 0.0;
    static double window_budget_ms = 0.0;
    static int window_quanta = 0;
    window_spent_ms += pedal_now_ms() - started;
    window_budget_ms += 1000.0 * frames / kSampleRate;
    if (++window_quanta >= 64) {
        g_load_permille.store(static_cast<int>(1000.0 * window_spent_ms / window_budget_ms),
                              std::memory_order_relaxed);
        window_spent_ms = 0.0;
        window_budget_ms = 0.0;
        window_quanta = 0;
    }
    return true;
}

void node_created(EMSCRIPTEN_WEBAUDIO_T context, const bool success, void*) {
    if (!success) {
        return;
    }
    // Stereo out, because the reverb returns a pair and the interfaces this runs
    // into are two-channel; handing the device the channel count it already has
    // keeps an up-mix out of the output path.
    int channels = 2;
    EmscriptenAudioWorkletNodeCreateOptions options{};
    options.numberOfInputs = 1;
    options.numberOfOutputs = 1;
    options.outputChannelCounts = &channels;
    const EMSCRIPTEN_AUDIO_WORKLET_NODE_T node = emscripten_create_wasm_audio_worklet_node(
        context, "nam-pedalboard", &options, &process, nullptr);
    pedal_web_wire_graph(context, node);
    g_ready.store(1, std::memory_order_release);
}

void thread_started(const EMSCRIPTEN_WEBAUDIO_T context, const bool success, void*) {
    if (!success) {
        return;
    }
    WebAudioWorkletProcessorCreateOptions options{};
    options.name = "nam-pedalboard";
    emscripten_create_wasm_audio_worklet_processor_async(context, &options, &node_created, nullptr);
}

} // namespace

// --- the platform hooks the engine and the effects expect --------------------

// NAM asks its platform for a batch of aligned, zeroed blocks. On the board that
// is a best-fit walk of internal SRAM; here every allocation is the same kind of
// memory, so the batch is a loop.
extern "C" bool coyopedal_nam_internal_batch(const std::size_t count,
                                             const std::size_t* const sizes, void** const blocks) {
    for (std::size_t index = 0; index < count; ++index) {
        blocks[index] = std::calloc(1, sizes[index]);
        if (blocks[index] == nullptr) {
            for (std::size_t undo = 0; undo < index; ++undo) {
                std::free(blocks[undo]);
                blocks[undo] = nullptr;
            }
            return false;
        }
    }
    return true;
}

// --- the control surface, which is s3_dsp_control.cpp's job on the board -----

extern "C" bool coyopedal_pedal_dsp_begin_update(void) {
    if (g_update_depth++ == 0) {
        g_update_open.store(true, std::memory_order_release);
    }
    return true;
}

extern "C" void coyopedal_pedal_dsp_end_update(void) {
    if (g_update_depth > 0 && --g_update_depth == 0) {
        g_update_open.store(false, std::memory_order_release);
    }
}

extern "C" void coyopedal_pedal_dsp_set_input_gain(const float gain) {
    g_engine.set_input_gain(gain);
}

extern "C" void coyopedal_pedal_dsp_set_output_gain(const float gain) {
    g_engine.set_output_gain(gain);
}

extern "C" void coyopedal_pedal_dsp_set_tone(const float bass_db, const float mid_db,
                                             const float treble_db) {
    g_engine.set_tone(bass_db, mid_db, treble_db);
}

extern "C" bool coyopedal_pedal_dsp_load_namb(const std::uint8_t* const data,
                                              const std::size_t size, char* const error,
                                              const std::size_t error_capacity) {
    coyopedal_pedal_dsp_begin_update();
    const bool loaded = g_engine.load_namb(data, size, error, error_capacity);
    coyopedal_pedal_dsp_end_update();
    return loaded;
}

extern "C" bool coyopedal_pedal_dsp_model_loaded(void) {
    return g_engine.model_loaded();
}

extern "C" void coyopedal_pedal_dsp_set_bypass(const bool bypass) {
    g_engine.set_bypass(bypass);
}

extern "C" bool coyopedal_pedal_dsp_bypassed(void) {
    return g_engine.bypassed();
}

extern "C" void coyopedal_pedal_dsp_set_pedal_bypass(const bool bypass) {
    g_pedal_bypassed.store(bypass, std::memory_order_relaxed);
}

extern "C" bool coyopedal_pedal_dsp_pedal_bypassed(void) {
    return g_pedal_bypassed.load(std::memory_order_relaxed);
}

extern "C" void coyopedal_pedal_dsp_set_tuner(const bool active) {
    if (active) {
        coyopedal_tuner_reset();
    }
    g_tuner_active.store(active, std::memory_order_relaxed);
}

extern "C" bool coyopedal_pedal_dsp_tuner_active(void) {
    return g_tuner_active.load(std::memory_order_relaxed);
}

// --- what the page and the board bridge call --------------------------------

namespace pedal_audio {

// Allocates and wires the effects chain, in main.cpp's order: the reverb's core
// state, its tank lines, its predelay, then coyopedal_fx_init(), then the delay's
// buffer. The board places each of those in a particular memory class; a page
// has one, so the placement is the only thing that drops out.
bool init() {
    static bool done = false;
    if (done) {
        return true;
    }
    const std::size_t state_bytes = coyopedal_fx_reverb_state_size();
    if (state_bytes != 0U &&
        !coyopedal_fx_reverb_attach(std::calloc(1, state_bytes), state_bytes)) {
        return false;
    }
    for (std::size_t line = 0; line < coyopedal_fx_reverb_line_count(); ++line) {
        const std::size_t bytes = coyopedal_fx_reverb_line_state_size(line);
        if (bytes != 0U && !coyopedal_fx_reverb_line_attach(line, std::calloc(1, bytes), bytes)) {
            return false;
        }
    }
    const std::size_t predelay_bytes = coyopedal_fx_reverb_predelay_state_size();
    if (predelay_bytes != 0U &&
        !coyopedal_fx_reverb_predelay_attach(std::calloc(1, predelay_bytes), predelay_bytes)) {
        return false;
    }
    coyopedal_fx_init();
    g_delay_line = static_cast<float*>(std::calloc(kDelaySamples, sizeof(float)));
    if (g_delay_line == nullptr || !coyopedal_fx_delay_attach(g_delay_line, kDelaySamples)) {
        // Six blocks instead of seven, exactly as the board reports it.
        g_delay_line = nullptr;
    }
    // Every block off until the panel recalls a preset, which is the board's
    // boot order: the UI applies a preset atomically before audio resumes.
    for (int block = 0; block < COYOPEDAL_FX_BLOCK_COUNT; ++block) {
        coyopedal_fx_set_enabled(static_cast<coyopedal_fx_block_t>(block), false);
    }
    // Unity, and a flat tone stack. On the board these come from the recalled
    // preset (controls.c feeds them from ui.amp_value); there is no preset store
    // behind this page, and the gains default to a linear zero, which is
    // silence rather than a neutral setting.
    g_engine.set_input_gain(1.0F);
    g_engine.set_output_gain(1.0F);
    g_engine.set_tone(0.0F, 0.0F, 0.0F);
    done = true;
    return true;
}

// Searches for a pitch if a full window has arrived. Not real-time safe, which
// is why it is here and not in the block loop -- the same split the board makes
// between coyopedal_tuner_feed() on the audio task and this on the UI task.
void pump() {
    if (g_tuner_active.load(std::memory_order_relaxed)) {
        (void)coyopedal_tuner_update();
    }
}

bool running() {
    return g_ready.load(std::memory_order_acquire) != 0;
}

int load_permille() {
    return g_load_permille.load(std::memory_order_relaxed);
}

int input_peak_permille() {
    return g_input_peak_permille.load(std::memory_order_relaxed);
}

int output_peak_permille() {
    return g_output_peak_permille.load(std::memory_order_relaxed);
}

int channel_peak_permille(const int channel) {
    if (channel < 0 || channel >= kMaxInputChannels) {
        return 0;
    }
    return g_channel_peak_permille[channel].load(std::memory_order_relaxed);
}

int input_channels() {
    return g_input_channels.load(std::memory_order_relaxed);
}

int input_channel() {
    return g_input_channel.load(std::memory_order_relaxed);
}

void set_input_channel(const int channel) {
    g_input_channel.store(channel < 0 ? 0 : channel, std::memory_order_relaxed);
}

int underruns() {
    return g_underruns.load(std::memory_order_relaxed);
}

} // namespace pedal_audio

extern "C" {

// Loads an A2-Full profile the page has fetched. Same entry point the board's
// model catalogue uses, minus the flash read in front of it.
EMSCRIPTEN_KEEPALIVE int pedal_audio_load_model(const std::uint8_t* const data, const int size) {
    char error[96]{};
    if (!pedal_audio::init()) {
        return 0;
    }
    return coyopedal_pedal_dsp_load_namb(data, static_cast<std::size_t>(size), error, sizeof error)
               ? 1
               : 0;
}

// Starts the audio thread. The page must have put a MediaStream on
// globalThis.pedalInputStream first, and must call this from a user gesture.
EMSCRIPTEN_KEEPALIVE int pedal_audio_start(void) {
    if (g_started.exchange(true)) {
        return 1;
    }
    if (!pedal_audio::init()) {
        g_started.store(false);
        return 0;
    }
    const EMSCRIPTEN_WEBAUDIO_T context = pedal_web_create_context();
    if (context <= 0) {
        g_started.store(false);
        return 0;
    }
    // Asking for 48 kHz does not guarantee it. Refuse rather than run at the
    // wrong rate, where every profile would be transposed and nothing would say
    // so.
    if (emscripten_audio_context_sample_rate(context) != kSampleRate) {
        emscripten_destroy_audio_context(context);
        g_started.store(false);
        return -1;
    }
    emscripten_start_wasm_audio_worklet_thread_async(context, g_audio_stack, sizeof g_audio_stack,
                                                     &thread_started, nullptr);
    emscripten_resume_audio_context_sync(context);
    return 1;
}

EMSCRIPTEN_KEEPALIVE int pedal_audio_running(void) {
    return pedal_audio::running() ? 1 : 0;
}

// The page's status line, packed into one call so it is one crossing per frame
// rather than seven.
EMSCRIPTEN_KEEPALIVE int pedal_audio_telemetry(const int field) {
    switch (field) {
    case 0:
        return pedal_audio::load_permille();
    case 1:
        return pedal_audio::input_peak_permille();
    case 2:
        return pedal_audio::output_peak_permille();
    case 3:
        return pedal_audio::input_channel();
    case 5:
        return pedal_audio::input_channels();
    case 6:
        return pedal_audio::underruns();
    case 7:
        return static_cast<int>(kBlockFrames);
    default:
        // 8 and up are the per-channel peaks, one field each, so a page with an
        // eight-in interface reads them in a loop rather than through eight
        // named entry points.
        return field >= 8 ? pedal_audio::channel_peak_permille(field - 8) : 0;
    }
}

// Which input channel the pedal takes. The board reads channel 0 and has no
// reason to do otherwise; an interface with eight inputs on a desk does.
EMSCRIPTEN_KEEPALIVE void pedal_audio_set_input_channel(const int channel) {
    pedal_audio::set_input_channel(channel);
}

} // extern "C"

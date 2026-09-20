#include "usb_host_hooks.hpp"

#include <atomic>

#include "esp_attr.h"

namespace {
std::atomic<unsigned> first_input_error{}, first_output_error{};
std::atomic<unsigned> fifo_input{}, fifo_output{};
} // namespace

// Called from the USB interrupt.
extern "C" void IRAM_ATTR pedalboard_usb_isoc_status(bool input, unsigned status) {
    auto& error = input ? first_input_error : first_output_error;
    if (!error.load(std::memory_order_relaxed))
        error.store(status, std::memory_order_relaxed);
}

unsigned pedalboard_usb_first_isoc_error(bool input) {
    return (input ? first_input_error : first_output_error).load(std::memory_order_relaxed);
}

extern "C" void pedalboard_usb_fifo_configured(unsigned input, unsigned output) {
    fifo_input.store(input, std::memory_order_relaxed);
    fifo_output.store(output, std::memory_order_release);
}

PedalboardUsbFifoCapacity pedalboard_usb_fifo_capacity() {
    const unsigned output = fifo_output.load(std::memory_order_acquire);
    return {fifo_input.load(std::memory_order_relaxed), output};
}

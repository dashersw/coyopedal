#pragma once

// Values the vendored USB host controller driver (third_party/usb) reports
// through its weak hooks.

// The first nonzero isochronous descriptor status seen in each direction.
unsigned pedalboard_usb_first_isoc_error(bool input);

// The controller's periodic FIFO sizes in bytes, known once the port is
// configured. A full-speed 48 kHz stream must fit them.
struct PedalboardUsbFifoCapacity {
    unsigned input, output;
};
PedalboardUsbFifoCapacity pedalboard_usb_fifo_capacity();

# Security

## Reporting a vulnerability

Report it privately through GitHub's
[security advisories](https://github.com/dashersw/coyopedal/security/advisories/new)
rather than in a public issue. You will get an acknowledgement, and credit in
the fix unless you would rather not have it.

## What is worth reporting

The pedal is an audio device that spends almost all of its life with the radios
switched off, so most of its attack surface only exists in maintenance mode:

- **The maintenance HTTP service** (`src/native/services/`) — it authenticates
  with a token compiled into the firmware and accepts OTA images. Anything that
  gets past the token, or gets an unsigned image booted, matters.
- **BLE discovery**, which is advertised in the same mode.
- **The parsers**, which is where untrusted bytes actually reach the device: the
  `.nam` and `.s3cache` readers (`src/native/storage/`), the preset document,
  and the USB descriptor parser (`src/native/drivers/`, `tests/usb/`), which
  reads whatever the interface plugged into it claims to be. The descriptor
  tests run under AddressSanitizer and UndefinedBehaviorSanitizer for this
  reason.

Wi-Fi credentials and the maintenance token live in
`src/native/services/remote_config.h`, which is generated locally and is not in
this repository. If you find a build of it committed anywhere, that is a report
worth making.

## What is out of scope

Physical access to the board. It flashes over USB with public tools, which is
deliberate — the firmware is GPL-3.0 and you are meant to be able to install
your own build on your own pedal.

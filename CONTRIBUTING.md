# Contributing

Thanks for helping. Bug reports, captures that fail to load, and support for
more USB interfaces are all welcome. Taking part means keeping to the
[Code of Conduct](CODE_OF_CONDUCT.md); security problems go through
[SECURITY.md](SECURITY.md) rather than a public issue.

## Setup

Follow the requirements in the [README](README.md#requirements), then install the
formatters. They go into `build/format-tools`, not your system Python:

```bash
npm ci
```

```bash
npm run format:setup
```

C and C++ formatting also needs **clang-format 22.1.8** on your `PATH`.

## Before you open a pull request

```bash
npm run check
```

```bash
npm test
```

```bash
npm run build:firmware
```

- `npm run check` runs ESLint, both TypeScript projects and the formatting check.
- `npm test` builds and runs the host tests: effects, presets, the `.nam` parser,
  the USB descriptor parser, the bank allocator, the UI stores, and a bit-exact
  check of the NAM engine against its reference kernels.
- `npm run build:firmware` must build without warnings in `src/`.

`npm run format` formats everything the check covers.

Host tests and a clean build do not prove that audio is glitch-free. If a change
touches the audio path, memory placement or the USB driver, say in the pull
request how you tested it on hardware, and read [docs/MEMORY.md](docs/MEMORY.md)
first.

## Style

- **TypeScript, JavaScript, CSS, JSON, Markdown:** Prettier, with two spaces,
  single quotes, no semicolons, trailing commas and 100 columns. ESLint uses the
  recommended JavaScript and TypeScript rules.
- **C and C++:** clang-format, with four spaces and 100 columns.
- **Python:** Ruff. **CMake:** cmake-format.
- **Xtensa assembly:** keep the instruction alignment; only whitespace is
  cleaned up.
- `third_party/` keeps its upstream formatting.

Write comments that explain why the code is the way it is, especially when the
reason is a measurement on the device.

## Things to keep in mind

- The firmware is a Gea app, and the `gea` CLI owns the ESP-IDF project.
  Everything this repository adds to the native build is declared in
  `gea.targets.esp32` in `package.json`. Don't add a board `CMakeLists.txt`, a
  partition CSV or a display driver.
- Don't edit generated C++ under `.gea/build/`, and don't patch the Gea
  packages in place: they track `latest` and carry no local patches, so a fix
  for one belongs upstream and a board-specific need is asked for through
  `gea.defines`. See [src/ui/README.md](src/ui/README.md).
- Users copy original `.nam` files to the SD card, and the pedal prepares them.
  Don't add a step that needs a desktop converter.
- Never commit `src/native/services/remote_config.h`. It holds Wi-Fi
  credentials and the maintenance token.

## Licensing your contribution

CoyoPedal asks contributors to sign a [Contributor License Agreement](CLA.md).
You keep the copyright in what you write — the agreement grants a licence, it
does not take ownership — and because that licence is non-exclusive you stay
free to use, publish and relicense your own work anywhere else. What it lets the
maintainer do is license CoyoPedal as a whole, including under different terms
later, without having to find everyone who ever sent a patch.

Signing takes one comment. Open a pull request, and a bot will ask you to reply
with:

```text
I have read the CLA Document and I hereby sign the CLA
```

You sign once; it covers everything you contribute afterwards. If you are
contributing as part of a job, read [clause 8](CLA.md#8-signing-on-behalf-of-an-employer)
first — your employer may be the one who owns the copyright.

"""The internal-SRAM settings docs/MEMORY.md explains.

These are one-line Kconfig values whose absence is silent: the build succeeds,
the image boots, and the cost surfaces as a symptom nowhere near the cause. The
DMA reserve pool is the example: a 32 KiB carve lands in the middle of the span
a NAM history bank needs, and the graph then fails to load with plenty of
internal SRAM still free.

So each setting the memory doc reasons about is pinned here against the file the
build reads, and the doc is held to naming it.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULTS = ROOT / "src" / "native" / "sdkconfig.defaults"
BUDGET = ROOT / "docs" / "MEMORY.md"

# Setting -> why it is load-bearing, quoted in the failure.
PINNED = {
    "CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL": (
        "0",
        "a carve splits the span NAM needs contiguously for a Core 0 history "
        "bank -- at 32768 the graph fails to build with 158 KiB still free, "
        "because the largest block falls to 31,744",
    ),
    "CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL": (
        "0",
        "default mallocs go to PSRAM so control-plane state cannot fragment the DMA-capable SRAM",
    ),
    "CONFIG_ESP32S3_INSTRUCTION_CACHE_16KB": (
        "y",
        "a 32 KiB cache claims the IRAM-only region and costs 16,640 bytes of "
        "internal heap, which is more than the A2-Full arenas leave",
    ),
    "CONFIG_ESP32S3_DATA_CACHE_32KB": (
        "y",
        "the gea CLI defaults the data cache to 64 KiB for PSRAM framebuffers "
        "unless the app pins a size, and the extra 32 KiB is the DRAM-only heap "
        "region the graph's histories are planned into",
    ),
}


def settings():
    text = DEFAULTS.read_text()
    found = {}
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        found[key.strip()] = value.strip().strip('"')
    return found


class SramBudget(unittest.TestCase):
    def test_every_pinned_setting_is_present_with_its_value(self):
        found = settings()
        for key, (value, why) in PINNED.items():
            self.assertIn(key, found, f"{key} is missing from {DEFAULTS.name}: {why}")
            self.assertEqual(found[key], value, f"{key} changed: {why}")

    def test_the_reserve_is_accounted_for_in_the_budget(self):
        # The reserve is the one whose absence is invisible in the config and
        # expensive on the device, so the document has to keep explaining it.
        text = BUDGET.read_text()
        self.assertIn("CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL", text)

    def test_the_reserve_is_explained_where_it_is_set(self):
        # The comment surviving without the setting is exactly what happened, so
        # the pairing is what gets checked rather than either half alone.
        text = DEFAULTS.read_text()
        reserve = text.index("CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL")
        preceding = text[:reserve].rsplit("\n\n", 1)[-1]
        self.assertTrue(
            re.search(r"^#", preceding, re.MULTILINE),
            "the reserve is set with no comment saying what it is for",
        )


if __name__ == "__main__":
    unittest.main()


class ReverbReservationOrder(unittest.TestCase):
    """The reverb's internal blocks must be claimed before the NAM graph loads.

    The compact reverb needs one contiguous internal block the size of
    CompactReverb (11,772 bytes) plus a small one for the two modulated allpass
    lines. The NAM graph takes ~213 KB of internal SRAM in large aligned banks,
    so if the reverb allocates afterwards the largest free internal block is
    routinely smaller than its state and the whole object falls back to PSRAM
    without failing anything. What that sounds like is crackle: the tank state is
    touched at 24 kHz, so every tick becomes a cache-dependent PSRAM access and
    the DSP stage overruns its 320,000-cycle deadline.

    Nothing in the build or the boot log fails when the order is wrong, so the
    order is pinned here instead.
    """

    def setUp(self):
        self.source = (ROOT / "src" / "native" / "main" / "main.cpp").read_text()

    def test_both_load_paths_reserve_before_loading_a_model(self):
        entries = (
            "bool pedalboard_audio_reload() {",
            'extern "C" void gea_app_native_boot(void) {',
        )
        for path in entries:
            start = self.source.find(path)
            self.assertNotEqual(start, -1, f"{path} is gone from main.cpp")
            body = self.source[start:]
            reserve = body.find("reserve_reverb_internal()")
            load = min(
                position
                for position in (body.find("load_namb"), body.find("coyopedal_load_model"))
                if position != -1
            )
            self.assertNotEqual(reserve, -1, f"{path} no longer reserves the reverb's internal RAM")
            self.assertLess(
                reserve,
                load,
                f"{path} loads the NAM model before reserving the reverb's internal blocks, "
                "so the reverb gets whatever contiguity the graph's banks leave -- "
                "usually none, which is the crackle",
            )

    def test_start_audio_effects_reuses_the_reservation(self):
        start = self.source.find("bool start_audio_effects()")
        self.assertNotEqual(start, -1, "start_audio_effects is gone from main.cpp")
        body = self.source[start : self.source.find("\n}\n", start)]
        for name in ("audio_reverb_state", "audio_tank_internal"):
            allocation = body.find(f"{name} = heap_caps_aligned_calloc")
            self.assertNotEqual(allocation, -1, f"{name} is no longer allocated here")
            guard = body.rfind(f"if ({name} == nullptr) {{", 0, allocation)
            self.assertNotEqual(
                guard,
                -1,
                f"start_audio_effects overwrites the reserved {name} with a fresh allocation "
                "taken after the graph's banks, which leaks the reservation and puts the "
                "reverb back in PSRAM",
            )

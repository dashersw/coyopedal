"""The panic record must be in the shipping image, not only in diagnostic ones.

The board's only console is its USB port, and in audio mode that port is a host
for the guitar interface, so a panic leaves no serial trace at all. A reset in
maintenance was therefore invisible -- which is how it came to be blamed on
CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH, an option that was then reverted while
the resets carried on.

Three things have to hold together or the record silently does nothing: the
capture must be compiled into the shipping image, the linker must actually
wrap esp_panic_handler, and the symbol must be kept so the object linking it in
is not dropped. Each one fails quietly on its own, so all three are pinned here.
"""

import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAIN = ROOT / "src" / "native" / "main" / "main.cpp"
MANIFEST = ROOT / "package.json"


class PanicRecord(unittest.TestCase):
    def setUp(self):
        self.shipping = MAIN.read_text()

    def test_the_capture_survives_in_the_shipping_image(self):
        for symbol in (
            "__wrap_esp_panic_handler",
            "g_s3_panic_magic",
            "report_previous_panic",
        ):
            self.assertIn(
                symbol,
                self.shipping,
                f"{symbol} is gone from main.cpp, so a panic leaves no evidence",
            )

    def test_the_record_is_gated_on_a_magic_word(self):
        # RTC memory holds garbage after power-on, so an ungated report would
        # invent a panic on every cold boot and nobody would trust the next one.
        self.assertIn("kPanicRecordMagic", self.shipping)
        self.assertIn("g_s3_panic_magic != kPanicRecordMagic", self.shipping)

    def test_the_linker_wraps_and_keeps_the_handler(self):
        manifest = json.loads(MANIFEST.read_text())
        options = manifest["gea"]["targets"]["esp32"]["linkOptions"]
        for option in ("-Wl,--wrap=esp_panic_handler", "-Wl,-u,__wrap_esp_panic_handler"):
            self.assertIn(
                option,
                options,
                f"{option} is missing, so the panic path never reaches the capture "
                "and /v1/logs stays silent about why the board came back",
            )

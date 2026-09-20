"""The UI pump's stack must not come from internal SRAM.

With the A2-Full graph resident, internal SRAM has no block large enough for a
4 KiB stack by the time the UI starts, so a task created with an internal stack
is never created and the tuner never updates. The pump runs at priority 3 and
touches neither flash nor an ISR path, so its stack belongs in PSRAM.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TASK = ROOT / "src" / "native" / "ui" / "task.cpp"


class PumpStack(unittest.TestCase):
    def setUp(self):
        self.source = TASK.read_text()

    def test_the_pump_is_not_created_with_an_internal_stack(self):
        self.assertNotIn(
            "xTaskCreatePinnedToCore(pump_task",
            self.source,
            "xTaskCreatePinnedToCore takes the stack from internal SRAM, which the "
            "A2-Full graph has already spent, so the task is never created",
        )

    def test_the_pump_stack_is_allocated_in_psram(self):
        # xTaskCreatePinnedToCoreWithCaps takes the stack from the given caps and
        # keeps the task control block in internal memory, which is the only
        # part FreeRTOS needs there.
        self.assertRegex(
            self.source,
            r"xTaskCreatePinnedToCoreWithCaps\(pump_task,[^;]*MALLOC_CAP_SPIRAM",
            "the pump's stack must be claimed from PSRAM explicitly",
        )

    def test_a_failed_pump_is_reported_as_an_error_with_its_consequence(self):
        self.assertIn(
            "the tuner will not update",
            self.source,
            "a failed pump must be logged as an error that names what stops working",
        )

import copy
import json
from pathlib import Path
import struct
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / "build" / "test-nam"


class NamJsonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        WORK.mkdir(parents=True, exist_ok=True)
        cls.raw = (ROOT / "assets/models/volum-ampete-4-v30.namb").read_bytes()
        cls.model = {
            "architecture": "WaveNet",
            "sample_rate": 48000,
            "config": {
                "head": None,
                "head_scale": 1.0,
                "layers": [
                    {
                        "input_size": 1,
                        "condition_size": 1,
                        "channels": 8,
                        "bottleneck": 8,
                        "groups_input": 1,
                        "groups_input_mixin": 1,
                        "kernel_sizes": [6] * 14 + [15, 15] + [6] * 7,
                        "dilations": [
                            1,
                            3,
                            7,
                            17,
                            41,
                            101,
                            239,
                            1,
                            3,
                            7,
                            17,
                            41,
                            101,
                            239,
                            1,
                            13,
                            1,
                            3,
                            7,
                            17,
                            41,
                            101,
                            239,
                        ],
                        "activation": [{"type": "LeakyReLU", "negative_slope": 0.01}] * 23,
                        "gating_mode": ["none"] * 23,
                        "secondary_activation": [None] * 23,
                        "layer1x1": {"active": True, "groups": 1},
                        "head": {"out_channels": 1, "kernel_size": 16, "bias": True},
                    }
                ],
            },
            "weights": list(struct.unpack("<12146f", cls.raw[32:])),
        }

    def parse(self, model, suffix=""):
        source = WORK / "input.nam"
        source.write_text(json.dumps(model) + suffix)
        result = subprocess.run(
            [str(ROOT / "build/nam_parser_test"), str(source), str(WORK / "output.namb")],
            capture_output=True,
            text=True,
        )
        return result

    def test_factory_weights_round_trip_exactly(self):
        result = self.parse(self.model)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((WORK / "output.namb").read_bytes(), self.raw)

    def test_slimmable_container_selects_full_member(self):
        wrong = copy.deepcopy(self.model)
        wrong["config"]["layers"][0]["channels"] = 3
        result = self.parse(
            {
                "architecture": "SlimmableContainer",
                "config": {"submodels": [{"model": wrong}, {"model": self.model}]},
            }
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((WORK / "output.namb").read_bytes(), self.raw)

    def test_rejects_unsupported_topology(self):
        for field, value in [
            ("channels", 16),
            ("gated", True),
            ("kernel_sizes", [3] * 23),
            ("head", {"out_channels": 2}),
            ("conv_pre_film", {"active": True}),
        ]:
            with self.subTest(field=field):
                model = copy.deepcopy(self.model)
                model["config"]["layers"][0][field] = value
                self.assertNotEqual(self.parse(model).returncode, 0)

    def test_rejects_bad_rate_weight_count_and_values(self):
        for change in ("rate", "count", "infinite", "bool"):
            model = copy.deepcopy(self.model)
            if change == "rate":
                model["sample_rate"] = 44100
            elif change == "count":
                model["weights"].pop()
            elif change == "infinite":
                model["weights"][42] = 1e100
            else:
                model["weights"][0] = True
            self.assertNotEqual(self.parse(model).returncode, 0, change)

    def test_rejects_trailing_data(self):
        self.assertNotEqual(self.parse(self.model, "garbage").returncode, 0)
        self.assertEqual(self.parse(self.model, " \n\t").returncode, 0)

    def test_rejects_truncated_json(self):
        source = WORK / "broken.nam"
        source.write_text('{"weights": [')
        result = subprocess.run(
            [str(ROOT / "build/nam_parser_test"), str(source), str(WORK / "broken.namb")],
            capture_output=True,
        )
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()

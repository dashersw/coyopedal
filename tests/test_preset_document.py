"""A preset is one document, and four things have to agree about it.

tools/presets_to_json.py checks assets/presets.json and copies it into the
partition image. src/native/storage/preset_json.c parses it on the board and
writes it back out. src/native/storage/factory_presets.h fixes the width of the
fields a record holds in RAM. src/native/drivers/flash_storage.cpp decides
whether what is embedded in the app image is a preset file at all.

The container these replaced was binary, and the test that used to live here
pinned its geometry -- a 16-byte header, 126-byte records, a 16-bit count whose
high byte was zero for every list anyone would ship. None of that exists now:
what is flashed is the file in this repo, byte for byte. So what is pinned here
is the agreement that remains -- the field widths, the block names and their
order, and that the file this repo ships is one the board can read.
"""

import json
import pathlib
import re
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
CHECKER = ROOT / "tools" / "presets_to_json.py"
PARSER = ROOT / "src" / "native" / "storage" / "preset_json.c"
PARSER_HEADER = ROOT / "src" / "native" / "storage" / "preset_json.h"
RECORD = ROOT / "src" / "native" / "storage" / "factory_presets.h"
EFFECTS = ROOT / "src" / "audio" / "effects.h"
VALIDATOR = ROOT / "src" / "native" / "drivers" / "flash_storage.cpp"
SOURCE = ROOT / "assets" / "presets.json"
MANIFEST = ROOT / "assets" / "models" / "factory.json"
IMAGE = ROOT / "build" / "factory" / "presets.json"


def checker_constant(name):
    match = re.search(rf"^{name} = (\d+)$", CHECKER.read_text(), re.MULTILINE)
    assert match, f"{name} not found in {CHECKER}"
    return int(match.group(1))


def define(path, name):
    match = re.search(rf"^#define {name} (\d+)U?$", path.read_text(), re.MULTILINE)
    assert match, f"{name} not found in {path}"
    return int(match.group(1))


def checker_blocks():
    match = re.search(r"BLOCK_ORDER = \[([^\]]*)\]", CHECKER.read_text())
    assert match, f"BLOCK_ORDER not found in {CHECKER}"
    return re.findall(r'"([a-z]+)"', match.group(1))


def parser_blocks():
    match = re.search(r"block_names\[COYOPEDAL_FX_BLOCK_COUNT\] = \{([^}]*)\}", PARSER.read_text())
    assert match, f"block_names not found in {PARSER}"
    return re.findall(r'"([a-z]+)"', match.group(1))


def enum_blocks():
    body = re.search(
        r"COYOPEDAL_FX_GATE = 0,(.*?)COYOPEDAL_FX_BLOCK_COUNT", EFFECTS.read_text(), re.S
    )
    assert body, f"coyopedal_fx_block_t not found in {EFFECTS}"
    return ["gate"] + [
        name.lower() for name in re.findall(r"COYOPEDAL_FX_([A-Z]+),", body.group(1))
    ]


class FieldWidths(unittest.TestCase):
    """What the checker refuses to ship is what the board cannot hold."""

    def test_name_width_agrees(self):
        self.assertEqual(checker_constant("NAME_SIZE"), define(RECORD, "COYOPEDAL_PRESET_NAME_MAX"))

    def test_profile_width_agrees(self):
        model_catalog = ROOT / "src" / "native" / "storage" / "model_catalog.h"
        self.assertEqual(
            checker_constant("PROFILE_SIZE"), define(model_catalog, "COYOPEDAL_MODEL_ID_MAX")
        )

    def test_parameter_count_agrees(self):
        self.assertEqual(checker_constant("PARAMS"), define(RECORD, "COYOPEDAL_PRESET_PARAMS"))

    def test_amp_value_count_agrees(self):
        self.assertEqual(checker_constant("AMP"), define(RECORD, "COYOPEDAL_PRESET_AMP"))

    def test_buffer_size_agrees(self):
        # The checker refuses a document the board could not parse, and the board
        # parses it in a buffer of exactly this size.
        self.assertEqual(
            checker_constant("JSON_MAX"), define(PARSER_HEADER, "COYOPEDAL_PRESET_JSON_MAX")
        )

    def test_version_agrees(self):
        self.assertEqual(
            checker_constant("JSON_VERSION"), define(PARSER_HEADER, "COYOPEDAL_PRESET_JSON_VERSION")
        )


class BlockNames(unittest.TestCase):
    """The names are the wire format; their ORDER is coyopedal_fx_block_t's, which
    is what a record's blocks[] is indexed by. Renaming one or moving one without
    the others would read every preset's settings into the wrong effects."""

    def test_parser_matches_the_enum(self):
        self.assertEqual(parser_blocks(), enum_blocks())

    def test_checker_matches_the_parser(self):
        self.assertEqual(checker_blocks(), parser_blocks())


class Validator(unittest.TestCase):
    def test_the_app_image_is_checked_as_text(self):
        # Not as a container: the binary header is gone, and a check that still
        # looked for magic would refuse every file this repo ships.
        source = VALIDATOR.read_text()
        self.assertNotIn('"TPRS"', source)
        self.assertIn("image[at] == '{'", source)


class ShippedDocument(unittest.TestCase):
    def setUp(self):
        self.text = SOURCE.read_text()
        self.document = json.loads(self.text)

    def test_version_is_the_one_the_firmware_reads(self):
        self.assertEqual(self.document["version"], checker_constant("JSON_VERSION"))

    def test_every_preset_fits_the_fields_it_lands_in(self):
        for preset in self.document["presets"]:
            self.assertTrue(preset["name"])
            self.assertLess(len(preset["name"].encode()), checker_constant("NAME_SIZE"))
            self.assertLess(len(preset["profile"].encode()), checker_constant("PROFILE_SIZE"))
            self.assertEqual(len(preset["amp"]), checker_constant("AMP"))
            for block in preset["blocks"]:
                self.assertIn(block["block"], checker_blocks())
                self.assertLessEqual(len(block["params"]), checker_constant("PARAMS"))

    def test_every_preset_names_a_profile_in_the_library(self):
        library = {entry["id"] for entry in json.loads(MANIFEST.read_text())}
        for preset in self.document["presets"]:
            self.assertIn(preset["profile"], library)

    def test_third_preset_pairs_an_amp_only_capture_with_an_embedded_cabinet(self):
        library = {entry["id"]: entry for entry in json.loads(MANIFEST.read_text())}
        third = self.document["presets"][2]
        self.assertEqual(library[third["profile"]]["gearType"], "amp")
        self.assertEqual(
            third["cabinet"], {"path": "Factory/V30 SM57.wav", "enabled": True, "level": 0}
        )
        for preset in self.document["presets"][:2]:
            self.assertEqual(library[preset["profile"]]["gearType"], "amp_cab")
            self.assertFalse(preset.get("cabinet", {}).get("enabled", False))
        embedded = json.loads((ROOT / "package.json").read_text())["gea"]["targets"]["esp32"][
            "embedFiles"
        ]
        for filename in (
            "jester-v30-sm57.wav",
            "jester-dv77-sm57.wav",
            "jester-rockdriver-e606.wav",
        ):
            self.assertIn(f"assets/cabinets/{filename}", embedded.values())

    def test_factory_container_preserves_capture_metadata_and_model_bytes(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            output = pathlib.Path(temporary) / "models.bin"
            subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools/models_to_bin.py"),
                    str(MANIFEST),
                    str(ROOT),
                    str(output),
                ],
                check=True,
                capture_output=True,
            )
            image = output.read_bytes()
        magic, version, count, size, reserved = struct.unpack_from("<4sHHII", image)
        entries = json.loads(MANIFEST.read_text())
        self.assertEqual(
            (magic, version, count, size, reserved), (b"TMDL", 2, len(entries), len(image), 0)
        )
        for index, entry in enumerate(entries):
            offset, length, identifier, name, flags = struct.unpack_from(
                "<II24s32sI", image, 16 + index * 68
            )
            self.assertEqual(identifier.rstrip(b"\0").decode(), entry["id"])
            self.assertEqual(name.rstrip(b"\0").decode(), entry["name"])
            self.assertEqual(flags, int(entry["gearType"] in ("amp_cab", "amp_pedal_cab")))
            self.assertEqual(offset % 4, 0)
            self.assertEqual(image[offset : offset + length], (ROOT / entry["file"]).read_bytes())

    def test_it_fits_the_buffer_the_board_parses_it_in(self):
        self.assertLessEqual(len(self.text.encode()), checker_constant("JSON_MAX"))

    @unittest.skipUnless(IMAGE.exists(), "build/factory/presets.json not written yet")
    def test_what_is_flashed_is_this_file(self):
        # The whole point of dropping the container: no packing step stands
        # between the repo and the partition, so these are the same bytes.
        self.assertEqual(IMAGE.read_bytes(), self.text.encode())


if __name__ == "__main__":
    unittest.main()

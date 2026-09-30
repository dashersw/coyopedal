#!/usr/bin/env python3
"""Reject linked firmware containing incompatible Gea UI record layouts.

Run with ESP-IDF's Python (which supplies pyelftools) against the unstripped ELF.
This is a qualification check, not a replacement for device rendering tests.
"""

import argparse
import json
from collections import defaultdict
from pathlib import Path

from elftools.elf.elffile import ELFFile


RECORDS = {
    "Node",
    "NodeClassList",
    "ComputedStyle",
    "LayoutBox",
    "RenderState",
    "TreeState",
    "RareStyle",
    "NodeRareData",
    "NodeCustomProperty",
    "NodeStyleOverrideStore",
}

# Optional in the generic UI check: programs with no circle rendering can omit
# this class entirely. The Bouncing Balls qualification applies its own ceiling.
OPTIONAL_RECORDS = {
    "CanvasMath": "gea::framework::graphics",
    "SharedStyleRecord": "gea::embedded::ui",
    "PersistentLayoutState": "gea::embedded::ui::",
    "LayoutPassMemo": "gea::embedded::ui::",
}


def text_attribute(die, key):
    attribute = die.attributes.get(key)
    return attribute.value.decode("utf-8", errors="replace") if attribute else ""


def namespace(die):
    names = []
    parent = die.get_parent()
    while parent is not None:
        if parent.tag == "DW_TAG_namespace":
            names.append(text_attribute(parent, "DW_AT_name"))
        parent = parent.get_parent()
    return "::".join(reversed(names))


def record_fields(die, base=0, prefix="", nested=False):
    """Include anonymous-union alternatives and their nested scratch records."""
    fields = []
    for member in die.iter_children():
        if member.tag != "DW_TAG_member":
            continue
        location = member.attributes.get("DW_AT_data_member_location")
        if location is None:
            bit_location = member.attributes.get("DW_AT_data_bit_offset")
            if bit_location is not None:
                offset = bit_location.value // 8
            elif die.tag != "DW_TAG_union_type":
                continue  # Static constants have no instance storage.
            else:
                offset = 0
        elif isinstance(location.value, int):
            offset = location.value
        else:
            raise ValueError(f"Unsupported member offset in {text_attribute(die, 'DW_AT_name')}")
        name = text_attribute(member, "DW_AT_name")
        member_type = member.get_DIE_from_attribute("DW_AT_type")
        while member_type and member_type.tag in {
            "DW_TAG_typedef",
            "DW_TAG_const_type",
            "DW_TAG_volatile_type",
        }:
            member_type = member_type.get_DIE_from_attribute("DW_AT_type")
        path = prefix + name
        if (
            member_type
            and member_type.tag in {"DW_TAG_union_type", "DW_TAG_structure_type"}
            and (not name or nested)
        ):
            fields.extend(
                record_fields(member_type, base + offset, path + "." if name else prefix, True)
            )
        else:
            fields.append((path, base + offset))
    return fields


def record_bit_fields(die):
    """Compare bit placement too: equal byte offsets do not prove a packed ABI."""
    fields = []
    for member in die.iter_children():
        if member.tag != "DW_TAG_member" or "DW_AT_bit_size" not in member.attributes:
            continue
        width = member.attributes["DW_AT_bit_size"].value
        bit_location = member.attributes.get("DW_AT_data_bit_offset")
        if bit_location is not None:
            bit_offset = bit_location.value
        else:
            location = member.attributes["DW_AT_data_member_location"].value
            storage_bytes = member.attributes.get("DW_AT_byte_size")
            member_type = member.get_DIE_from_attribute("DW_AT_type")
            while member_type and member_type.tag in {
                "DW_TAG_typedef",
                "DW_TAG_const_type",
                "DW_TAG_volatile_type",
            }:
                member_type = member_type.get_DIE_from_attribute("DW_AT_type")
            storage_bits = 8 * (
                storage_bytes.value
                if storage_bytes is not None
                else member_type.attributes["DW_AT_byte_size"].value
            )
            # Xtensa S3 is little endian. DWARF4's bit_offset counts from the
            # most significant end of the containing storage unit.
            bit_offset = (
                8 * location + storage_bits - member.attributes["DW_AT_bit_offset"].value - width
            )
        fields.append((text_attribute(member, "DW_AT_name"), bit_offset, width))
    return fields


def inspect(path):
    variants = defaultdict(lambda: defaultdict(list))
    static_storage = None
    with path.open("rb") as source:
        elf = ELFFile(source)
        if not elf.has_dwarf_info():
            raise ValueError("The ELF has no debug information; use the unstripped build ELF")
        symbols = elf.get_section_by_name(".symtab")
        if symbols is not None:
            tracked_symbols = {
                "triangleOcclusionBytes": ("s_occlBits",),
                "cssDenseMarkBytes": ("g_pendingRecomputeMark", "g_forceFullSubtreeMark"),
                "cssDynamicLengthCacheBytes": ("dynamicLengthExpressionResolutionCacheEvE5cache",),
                "textLineBreakCacheBytes": ("gLineBreakCache",),
                "cssRuleIndexBytes": ("g_ruleIndex",),
                "cssActiveRulePlanCacheBytes": ("activeRulePlanCacheEvE5cache",),
                "layoutGenerationBytes": ("gLayoutPassSerial",),
            }
            static_storage = dict.fromkeys(tracked_symbols, 0)
            for symbol in symbols.iter_symbols():
                if (
                    symbol["st_info"]["type"] != "STT_OBJECT"
                    or symbol["st_shndx"] == "SHN_UNDEF"
                    or symbol.name.startswith("_ZGV")  # Initialization guards are separate objects.
                ):
                    continue
                for key, names in tracked_symbols.items():
                    if any(name in symbol.name for name in names):
                        static_storage[key] += symbol["st_size"]
        for unit in elf.get_dwarf_info().iter_CUs():
            source_name = text_attribute(unit.get_top_DIE(), "DW_AT_name")
            for die in unit.iter_DIEs():
                if die.tag not in {"DW_TAG_structure_type", "DW_TAG_class_type"}:
                    continue
                name = text_attribute(die, "DW_AT_name")
                size = die.attributes.get("DW_AT_byte_size")
                expected_namespace = (
                    "gea::embedded::ui" if name in RECORDS else OPTIONAL_RECORDS.get(name)
                )
                if (
                    expected_namespace is None
                    or size is None
                    or namespace(die) != expected_namespace
                ):
                    continue
                fields = record_fields(die)
                signature = (size.value, tuple(fields), tuple(record_bit_fields(die)))
                variants[name][signature].append(source_name)
    missing = sorted(RECORDS - variants.keys())
    report = {
        "elf": str(path),
        "consistent": not missing,
        "missingRecords": missing,
        "records": {},
        "staticStorage": static_storage,
    }
    for name, layouts in sorted(variants.items()):
        report["records"][name] = [
            {
                "bytes": signature[0],
                "members": dict(signature[1]),
                "bitFields": {
                    name: {"bitOffset": offset, "bitSize": width}
                    for name, offset, width in signature[2]
                },
                "translationUnits": sources,
            }
            for signature, sources in sorted(layouts.items())
        ]
        if len(layouts) != 1:
            report["consistent"] = False
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    arguments = parser.parse_args()
    report = inspect(arguments.elf)
    print(json.dumps(report, indent=2))
    return 0 if report["consistent"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

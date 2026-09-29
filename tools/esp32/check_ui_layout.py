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
            if die.tag != "DW_TAG_union_type":
                continue  # Static constants have no instance storage.
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


def inspect(path):
    variants = defaultdict(lambda: defaultdict(list))
    with path.open("rb") as source:
        elf = ELFFile(source)
        if not elf.has_dwarf_info():
            raise ValueError("The ELF has no debug information; use the unstripped build ELF")
        for unit in elf.get_dwarf_info().iter_CUs():
            source_name = text_attribute(unit.get_top_DIE(), "DW_AT_name")
            for die in unit.iter_DIEs():
                if die.tag not in {"DW_TAG_structure_type", "DW_TAG_class_type"}:
                    continue
                name = text_attribute(die, "DW_AT_name")
                size = die.attributes.get("DW_AT_byte_size")
                if name not in RECORDS or size is None or namespace(die) != "gea::embedded::ui":
                    continue
                fields = record_fields(die)
                signature = (size.value, tuple(fields))
                variants[name][signature].append(source_name)
    missing = sorted(RECORDS - variants.keys())
    report = {"elf": str(path), "consistent": not missing, "missingRecords": missing, "records": {}}
    for name, layouts in sorted(variants.items()):
        report["records"][name] = [
            {"bytes": signature[0], "members": dict(signature[1]), "translationUnits": sources}
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

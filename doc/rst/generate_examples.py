#!/usr/bin/env python3
#
# Copyright (c) 2024-2026 by the CRESCENT cyberinfrastructure team (https://cascadiaquakes.org/)
# See LICENSE for copying and redistribution conditions.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation; version 3 or any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Lesser General Public License for more details.
#
# Contact info: abioyeajala@gmail.com (Rasheed Ajala)
#-------------------------------------------------------------------------------
#

"""Generate Sphinx example pages from the maintained example directories."""

from __future__ import annotations

import argparse
import re
import shutil
from pathlib import Path


MODULES = ("merge1d", "merge2d", "merge3d", "topobath", "elygtl", "ssh1d", "ssh2d", "ssh3d")
ASSET_SUFFIXES = {".sh", ".png", ".pdf"}


def inline_markup(text: str) -> str:
    text = re.sub(r"(?<!`)`([^`]+)`(?!`)", r"``\1``", text)
    return re.sub(r"\[([^]]+)\]\(([^)]+)\)", r"`\1 <\2>`__", text)


def is_table_separator(line: str) -> bool:
    cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
    return bool(cells) and all(re.fullmatch(r":?-{3,}:?", cell) for cell in cells)


def table_to_rst(lines: list[str]) -> list[str]:
    rows = [[inline_markup(cell.strip()) for cell in line.strip().strip("|").split("|")] for line in lines]
    if len(rows) > 1 and is_table_separator(lines[1]):
        rows.pop(1)
    result = [".. list-table::", "   :header-rows: 1", ""]
    for row in rows:
        result.append(f"   * - {row[0]}")
        result.extend(f"     - {cell}" for cell in row[1:])
    result.append("")
    return result


def markdown_to_rst(text: str) -> tuple[str, str]:
    source = text.splitlines()
    title = "Example"
    output: list[str] = []
    i = 0
    in_fence = False
    while i < len(source):
        line = source[i]
        if line.startswith("```"):
            if in_fence:
                output.append("")
                in_fence = False
            else:
                language = line[3:].strip() or "text"
                output.extend([f".. code-block:: {language}", ""])
                in_fence = True
            i += 1
            continue
        if in_fence:
            output.append(f"   {line}" if line else "")
            i += 1
            continue
        if line.startswith("# "):
            title = line[2:].strip()
            i += 1
            continue
        if line.startswith("|") and i + 1 < len(source) and is_table_separator(source[i + 1]):
            table_lines = [line, source[i + 1]]
            i += 2
            while i < len(source) and source[i].startswith("|"):
                table_lines.append(source[i])
                i += 1
            output.extend(table_to_rst(table_lines))
            continue
        heading = re.match(r"^(#{2,6})\s+(.+)$", line)
        if heading:
            heading_text = inline_markup(heading.group(2).strip())
            adornments = ("-", "^", '"', "~", "+")
            adornment = adornments[min(len(heading.group(1)) - 2, len(adornments) - 1)]
            output.extend([heading_text, adornment * len(heading_text), ""])
        else:
            output.append(inline_markup(line))
        i += 1
    return title, "\n".join(output).strip() + "\n"


def write_page(example: Path, destination: Path) -> tuple[str, str]:
    title, body = markdown_to_rst((example / "README.md").read_text(encoding="utf-8"))
    destination.mkdir(parents=True, exist_ok=True)
    assets = destination / "assets"
    assets.mkdir(exist_ok=True)

    selected = sorted(path for path in example.iterdir() if path.is_file() and path.suffix.lower() in ASSET_SUFFIXES)
    for source in selected:
        shutil.copy2(source, assets / source.name)

    page = [title, "=" * len(title), "", body.rstrip(), ""]
    scripts = [path for path in selected if path.suffix.lower() == ".sh"]
    figures = [path for path in selected if path.suffix.lower() == ".png"]
    if figures:
        page.extend(["Figures", "-------", ""])
        for figure in figures:
            page.extend([f".. image:: assets/{figure.name}", f"   :alt: {title}", "   :width: 95%", ""])

    pdfs = [path for path in selected if path.suffix.lower() == ".pdf"]
    downloads = scripts + pdfs
    if downloads:
        page.extend(["Downloads", "---------", ""])
        page.extend(f"* :download:`{asset.name} <assets/{asset.name}>`" for asset in downloads)
        page.append("")

    (destination / "index.rst").write_text("\n".join(page), encoding="utf-8")
    return example.name, title


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--examples", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    if args.output.exists():
        shutil.rmtree(args.output)
    args.output.mkdir(parents=True)

    module_entries: list[str] = []
    for module in MODULES:
        source_module = args.examples / module
        if not source_module.is_dir():
            continue
        destination_module = args.output / module
        destination_module.mkdir()
        examples = sorted(path for path in source_module.iterdir() if path.is_dir() and (path / "README.md").is_file())
        entries = [write_page(example, destination_module / example.name) for example in examples]
        module_title = f"{module} examples"
        module_index = [module_title, "=" * len(module_title), "", ".. toctree::", "   :maxdepth: 1", ""]
        module_index.extend(f"   {name}/index" for name, _ in entries)
        module_index.append("")
        (destination_module / "index.rst").write_text("\n".join(module_index), encoding="utf-8")
        module_entries.append(f"   {module}/index")

    index = ["Examples", "========", "", "The gallery is generated from the maintained example scripts and figures.", "", ".. toctree::", "   :maxdepth: 2", ""]
    index.extend(module_entries)
    index.append("")
    (args.output / "index.rst").write_text("\n".join(index), encoding="utf-8")


if __name__ == "__main__":
    main()

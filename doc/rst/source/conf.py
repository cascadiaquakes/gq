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

import json
import os
from pathlib import Path

from docutils import nodes
from docutils.parsers.rst import Directive, directives


project = "GQ"
author = "Rasheed Ajala"
copyright = "2026, The CRESCENT cyberinfrastructure team"
version = os.environ.get("GQ_DOC_VERSION", "1.0.0")
release = version

extensions = []
templates_path = ["_templates"]
exclude_patterns = []
source_suffix = ".rst"
master_doc = "index"

html_theme = "sphinx_rtd_theme"
html_title = f"GQ {release} documentation"
html_static_path = ["_static"]
html_css_files = ["gq.css"]
html_js_files = ["gq.js"]
html_show_sourcelink = False
html_show_sphinx = True

github_repository = "https://github.com/cascadiaquakes/gq"
github_version = os.environ.get("GQ_DOC_GITHUB_VERSION", "devel")


def _gq_versions():
    default_versions = [(release, "")]
    raw_versions = os.environ.get("GQ_DOC_VERSIONS")
    if not raw_versions:
        return default_versions

    try:
        versions = json.loads(raw_versions)
    except json.JSONDecodeError as exc:
        raise RuntimeError("GQ_DOC_VERSIONS must be JSON") from exc

    parsed_versions = []
    for item in versions:
        if not isinstance(item, list) or len(item) != 2:
            raise RuntimeError("GQ_DOC_VERSIONS entries must be [label, url] pairs")
        parsed_versions.append((str(item[0]), str(item[1])))
    return parsed_versions


html_context = {
    "gq_current_version": os.environ.get("GQ_DOC_CURRENT_VERSION", release),
    "gq_versions": _gq_versions(),
}


def _github_source_path(pagename):
    parts = pagename.split("/")
    if parts[0] != "examples":
        return f"doc/rst/source/{pagename}.rst"
    if len(parts) == 4 and parts[-1] == "index":
        return f"doc/examples/{parts[1]}/{parts[2]}/README.md"
    return "doc/rst/generate_examples.py"


def _add_github_edit_link(app, pagename, templatename, context, doctree):
    if pagename in {"genindex", "search"}:
        return
    source_path = _github_source_path(pagename)
    metadata = dict(context.get("meta") or {})
    metadata["github_url"] = (
        f"{github_repository}/blob/{github_version}/{source_path}"
    )
    context["meta"] = metadata

latex_documents = [
    (master_doc, "gq.tex", "GQ Documentation", author, "manual"),
]
latex_elements = {
    "papersize": "letterpaper",
    "pointsize": "10pt",
    "preamble": r"\setcounter{tocdepth}{2}",
    "maketitle": r"""
\sphinxmaketitle
\thispagestyle{empty}
\vspace*{\fill}
\begin{center}
Copyright (c) 2024-2026 by the CRESCENT cyberinfrastructure team
(\url{https://cascadiaquakes.org/cvm/})
\end{center}
\vspace*{\fill}
\clearpage
""",
}

man_pages = [
    (f"modules/{name}", name, f"GQ {name} module", [author], 1)
    for name in ("merge1d", "merge2d", "merge3d", "topobath", "elygtl", "ssh1d", "ssh2d", "ssh3d")
]


class GQUsageDirective(Directive):
    required_arguments = 1
    optional_arguments = 0
    final_argument_whitespace = False
    has_content = False
    option_spec = {"caption": directives.unchanged}

    def run(self):
        module = self.arguments[0]
        usage_dir = Path(os.environ.get("GQ_DOC_USAGE_DIR", ""))
        usage_file = usage_dir / f"{module}.txt"
        if not usage_file.is_file():
            raise self.error(f"Generated usage text is missing for {module}: {usage_file}")
        self.state.document.settings.env.note_dependency(str(usage_file))
        usage = usage_file.read_text(encoding="utf-8")
        block = nodes.literal_block(usage, usage)
        block["language"] = "text"
        if "caption" in self.options:
            block["caption"] = self.options["caption"]
        container = nodes.container(classes=["gq-usage"])
        container += block
        return [container]


def setup(app):
    app.add_directive("gq-usage", GQUsageDirective)
    app.connect("html-page-context", _add_github_edit_link)
    return {"version": release, "parallel_read_safe": True}

#!/usr/bin/env python3
# PlatformIO pre-build script: embeds dashboard.json into src/dashboard_generated.h
# as a minified C string, validating it first. Runs automatically on every build
# (local and CI) via `extra_scripts = pre:tools/gen_dashboard.py` in platformio.ini.
import os, json, sys

VALID_TYPES = {"gauge", "counter", "text", "update", "graph", "theme"}
MAX_PER_PAGE = 6
MAX_PAGES = 6


def validate_component(where, c):
    t = c.get("type")
    if t not in VALID_TYPES:
        raise ValueError("dashboard.json: %s has invalid type %r (expected one of %s)"
                         % (where, t, sorted(VALID_TYPES)))
    r = c.get("row")
    if r is not None and r not in (0, 1, 2):
        raise ValueError("dashboard.json: %s has row %r (must be 0, 1 or 2)" % (where, r))
    col = c.get("col")
    if col is not None and col not in (0, 1, 2):
        raise ValueError("dashboard.json: %s has col %r (must be 0, 1 or 2)" % (where, col))
    rs = c.get("rowspan")
    if rs is not None and rs not in (1, 2, 3):
        raise ValueError("dashboard.json: %s has rowspan %r (must be 1, 2 or 3)" % (where, rs))
    cs = c.get("colspan")
    if cs is not None and cs not in (1, 2, 3):
        raise ValueError("dashboard.json: %s has colspan %r (must be 1, 2 or 3)" % (where, cs))


def generate(project_dir):
    src = os.path.join(project_dir, "dashboard.json")
    out = os.path.join(project_dir, "src", "dashboard_generated.h")

    with open(src, "r", encoding="utf-8") as f:
        raw = f.read()

    data = json.loads(raw)  # raises on invalid JSON -> build fails with a clear error

    if "pages" in data:
        pages = data["pages"]
        if not isinstance(pages, list):
            raise ValueError("dashboard.json: 'pages' must be a list")
        if len(pages) > MAX_PAGES:
            raise ValueError("dashboard.json: at most %d pages, got %d" % (MAX_PAGES, len(pages)))
        total = 0
        for pi, pg in enumerate(pages):
            comps = pg.get("components", [])
            if not isinstance(comps, list):
                raise ValueError("dashboard.json: page %d: 'components' must be a list" % pi)
            if len(comps) > MAX_PER_PAGE:
                raise ValueError("dashboard.json: page %d: at most %d components, got %d"
                                 % (pi, MAX_PER_PAGE, len(comps)))
            for ci, c in enumerate(comps):
                validate_component("page %d component %d" % (pi, ci), c)
            total += len(comps)
        out_obj = {"pages": pages}
        summary = "%d page(s), %d component(s)" % (len(pages), total)
    else:
        comps = data.get("components", [])
        if not isinstance(comps, list):
            raise ValueError("dashboard.json: 'components' must be a list")
        if len(comps) > MAX_PER_PAGE:
            raise ValueError("dashboard.json: at most %d components, got %d" % (MAX_PER_PAGE, len(comps)))
        for ci, c in enumerate(comps):
            validate_component("component %d" % ci, c)
        out_obj = {"components": comps}
        summary = "1 page, %d component(s)" % len(comps)

    mini = json.dumps(out_obj, separators=(",", ":"), ensure_ascii=False)

    with open(out, "w", encoding="utf-8") as f:
        f.write("// Auto-generated from dashboard.json by tools/gen_dashboard.py - do not edit.\n")
        f.write("#pragma once\n")
        f.write('static const char DASHBOARD_JSON[] = R"DASHJSON(')
        f.write(mini)
        f.write(')DASHJSON";\n')

    print("gen_dashboard: wrote src/dashboard_generated.h (%s)" % summary)


# --- PlatformIO entry point ---
try:
    Import("env")  # type: ignore  # noqa: F821  (provided by SCons)
    generate(env["PROJECT_DIR"])  # type: ignore  # noqa: F821
except NameError:
    # Allow running standalone for testing: python tools/gen_dashboard.py [project_dir]
    generate(sys.argv[1] if len(sys.argv) > 1 else os.getcwd())

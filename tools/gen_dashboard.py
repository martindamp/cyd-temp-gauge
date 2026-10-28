#!/usr/bin/env python3
# PlatformIO pre-build script: embeds dashboard.json into src/dashboard_generated.h
# as a minified C string, validating it first. Runs automatically on every build
# (local and CI) via `extra_scripts = pre:tools/gen_dashboard.py` in platformio.ini.
import os, json, sys

VALID_TYPES = {"gauge", "counter", "text"}
MAX_COMPONENTS = 6


def generate(project_dir):
    src = os.path.join(project_dir, "dashboard.json")
    out = os.path.join(project_dir, "src", "dashboard_generated.h")

    with open(src, "r", encoding="utf-8") as f:
        raw = f.read()

    data = json.loads(raw)  # raises on invalid JSON -> build fails with a clear error
    comps = data.get("components", [])
    if not isinstance(comps, list):
        raise ValueError("dashboard.json: 'components' must be a list")
    if len(comps) > MAX_COMPONENTS:
        raise ValueError("dashboard.json: at most %d components, got %d"
                         % (MAX_COMPONENTS, len(comps)))
    for i, c in enumerate(comps):
        t = c.get("type")
        if t not in VALID_TYPES:
            raise ValueError("dashboard.json: component %d has invalid type %r "
                             "(expected one of %s)" % (i, t, sorted(VALID_TYPES)))

    # Keep only the components array, minified, to save flash.
    mini = json.dumps({"components": comps}, separators=(",", ":"), ensure_ascii=False)

    with open(out, "w", encoding="utf-8") as f:
        f.write("// Auto-generated from dashboard.json by tools/gen_dashboard.py - do not edit.\n")
        f.write("#pragma once\n")
        f.write('static const char DASHBOARD_JSON[] = R"DASHJSON(')
        f.write(mini)
        f.write(')DASHJSON";\n')

    print("gen_dashboard: wrote src/dashboard_generated.h (%d component(s))" % len(comps))


# --- PlatformIO entry point ---
try:
    Import("env")  # type: ignore  # noqa: F821  (provided by SCons)
    generate(env["PROJECT_DIR"])  # type: ignore  # noqa: F821
except NameError:
    # Allow running standalone for testing: python tools/gen_dashboard.py [project_dir]
    generate(sys.argv[1] if len(sys.argv) > 1 else os.getcwd())

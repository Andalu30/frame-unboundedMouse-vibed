#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Checks that the files describing the same thing agree with each other.

Run by ctest (see CMakeLists.txt), or directly: python3 tests/check_resources.py [repo root]

- every JSON file SteamVR loads from driver/mouselaser parses;
- the settings keys are the same in the code, default.vrsettings and the README table, and
  default.vrsettings and the README agree on the default values;
- every binding for the left hand is repeated for the right hand and the stylus;
- the input profile, the components the code creates and the localization labels match;
- names and file references between manifest, profile and bindings resolve.
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent)
DRIVER = ROOT / "driver" / "mouselaser"
RES = DRIVER / "resources"
SOURCE = (ROOT / "src" / "driver.cpp").read_text()

failures = []


def check(ok, message):
    print(("ok    " if ok else "FAIL  ") + message)
    if not ok:
        failures.append(message)


def load(path):
    try:
        return json.loads(path.read_text())
    except ValueError as e:
        check(False, f"{path.relative_to(ROOT)} is not valid JSON: {e}")
        return None


# --- JSON parses -----------------------------------------------------------------------------
files = [DRIVER / "driver.vrdrivermanifest"] + sorted(
    p for p in RES.rglob("*") if p.suffix in (".json", ".vrsettings"))
data = {p.relative_to(DRIVER).as_posix(): load(p) for p in files}
check(all(v is not None for v in data.values()), f"{len(files)} resource files parse as JSON")
manifest = data["driver.vrdrivermanifest"]
defaults = data["resources/settings/default.vrsettings"]["driver_mouselaser"]
profile = data["resources/input/mouselaser_profile.json"]
localization = data["resources/localization/localization.json"]

# --- settings --------------------------------------------------------------------------------
code_keys = set(re.findall(r'\bReadSetting\("(\w+)"', SOURCE))
readme = (ROOT / "README.md").read_text()
settings_section = readme.split("\n## Settings", 1)[1].split("\n## ", 1)[0]
readme_rows = dict(re.findall(r"^\| `(\w+)` \| `([^`]*)` \|", settings_section, re.M))


def show(keys):
    return ", ".join(sorted(keys)) or "none"


check(code_keys == set(defaults),
      "settings read by the code == keys in default.vrsettings "
      f"(only in code: {show(code_keys - set(defaults))}; only in file: {show(set(defaults) - code_keys)})")
check(code_keys == set(readme_rows),
      "settings read by the code == README settings table "
      f"(only in code: {show(code_keys - set(readme_rows))}; only in README: {show(set(readme_rows) - code_keys)})")
mismatched = [k for k in readme_rows if k in defaults and json.loads(readme_rows[k]) != defaults[k]]
check(not mismatched, f"README defaults == default.vrsettings values (differ: {show(mismatched)})")

# --- bindings: left hand, right hand and stylus stay in step -----------------------------------
HANDS = ("/user/hand/left/", "/user/hand/right/", "/user/stylus/")
for name in sorted(n for n in data if n.startswith("resources/input/") and "bindings" in n):
    bindings = data[name]
    check(bindings.get("controller_type") == profile["controller_type"],
          f"{name}: controller_type is {profile['controller_type']!r}")
    for action_set, groups in bindings["bindings"].items():
        for kind, entries in groups.items():
            shapes = {}
            for e in entries:
                for prefix in HANDS:
                    if e.get("path", "").startswith(prefix):
                        rest = e["path"][len(prefix):]
                        shape = (e.get("mode"), tuple(sorted(e.get("inputs", {}))))
                        shapes.setdefault((rest, shape), set()).add(prefix)
            missing = [f"{rest} {shape} for {show(set(HANDS) - prefixes)}"
                       for (rest, shape), prefixes in shapes.items() if prefixes != set(HANDS)]
            check(not missing, f"{name} {action_set} {kind}: every entry exists for left, right and stylus"
                  + (f" (missing: {'; '.join(missing)})" if missing else ""))

# --- profile, code components and labels ---------------------------------------------------------
profile_inputs = {k for k in profile["input_source"] if k.startswith("/input/")}
code_inputs = {m for m in re.findall(r'Create(?:Boolean|Scalar)Component\(c, "(/input/[^/"]+)', SOURCE)}
check(profile_inputs == code_inputs,
      "profile inputs == components created in driver.cpp "
      f"(only in profile: {show(profile_inputs - code_inputs)}; only in code: {show(code_inputs - profile_inputs)})")
for lang in localization:
    missing = (profile_inputs | {profile["controller_type"]}) - set(lang)
    check(not missing, f"localization {lang['language_tag']} labels every input (missing: {show(missing)})")

# --- names and file references ---------------------------------------------------------------------
check(manifest["name"] == profile["driver_name"] == profile["resource_root"],
      "manifest name == profile driver_name == profile resource_root")
for b in profile.get("default_bindings", []):
    check((RES / "input" / b["binding_url"]).is_file(), f"default binding {b['binding_url']} exists")
model = re.search(r'Prop_RenderModelName_String, "\{mouselaser\}(\w+)"', SOURCE)
check(model is not None and (RES / "rendermodels" / model.group(1)).is_dir(),
      "render model named in driver.cpp exists under resources/rendermodels")

print(f"\n{len(failures)} failure(s)")
sys.exit(1 if failures else 0)

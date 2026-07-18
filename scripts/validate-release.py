#!/usr/bin/env python3
"""Fail a tagged build when its version metadata disagrees."""

import json
import os


with open("modules/overtake/mark/module.json", encoding="utf-8") as handle:
    module_version = json.load(handle)["version"]
with open("release.json", encoding="utf-8") as handle:
    release_version = json.load(handle)["version"]

if os.environ.get("GITHUB_REF_TYPE") == "tag":
    expected_version = os.environ["GITHUB_REF_NAME"].removeprefix("v")
else:
    expected_version = release_version

if len({expected_version, module_version, release_version}) != 1:
    raise SystemExit(
        f"version mismatch: expected={expected_version}, "
        f"module.json={module_version}, release.json={release_version}"
    )

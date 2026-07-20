#!/usr/bin/env python3
"""Fail a tagged build when its version metadata disagrees."""

import json
import os


with open("modules/overtake/mark/module.json", encoding="utf-8") as handle:
    module_version = json.load(handle)["version"]
with open("release.json", encoding="utf-8") as handle:
    release = json.load(handle)

release_version = release["version"]
repository_url = "https://github.com/timncox/schwung-mark"
expected_download_url = (
    f"{repository_url}/releases/download/v{release_version}/mark-module.tar.gz"
)

if os.environ.get("GITHUB_REF_TYPE") == "tag":
    expected_version = os.environ["GITHUB_REF_NAME"].removeprefix("v")
else:
    expected_version = release_version

if len({expected_version, module_version, release_version}) != 1:
    raise SystemExit(
        f"version mismatch: expected={expected_version}, "
        f"module.json={module_version}, release.json={release_version}"
    )

if release.get("download_url") != expected_download_url:
    raise SystemExit(
        "download URL mismatch: "
        f"expected={expected_download_url}, actual={release.get('download_url')}"
    )

if release.get("repo_url") != repository_url:
    raise SystemExit(
        f"repository URL mismatch: expected={repository_url}, "
        f"actual={release.get('repo_url')}"
    )

#!/usr/bin/env python3
"""Opt-in MiSTer FPGA evidence runner for a prebuilt Saturn CUE/BIN image.

No Saturn guest assertions are implemented: a successful run is NOT a test pass.
Requires SSH/SCP and Zaparoo Core on a trusted private network.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time
from urllib import error, parse, request

REMOTE_ROOT = "/media/fat/games/Saturn/PsyZHIL"
CUE_FILE = re.compile(r'^\s*FILE\s+(?:"([^"]+)"|(\S+))\s+\S+\s*$', re.I)
VALID_NAME = re.compile(r"^[A-Za-z0-9_-]{1,48}$")
VALID_HOST = re.compile(r"^[A-Za-z0-9][A-Za-z0-9.-]*$")
VALID_BUTTONS = re.compile(
    r"(?:[ABXYLRabxylr^V<>]|\{(?:start|select|up|down|left|right|l1|r1|l2|r2|menu)\})+"
)
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


class HILError(Exception):
    """An infrastructure error; not a guest test failure."""


def cue_files(cue: Path) -> list[Path]:
    cue = cue.resolve(strict=True)
    if not cue.is_file() or cue.suffix.lower() != ".cue":
        raise HILError("Expected a .cue file")
    try:
        lines = cue.read_text(encoding="utf-8-sig").splitlines()
    except UnicodeError as exc:
        raise HILError("CUE must be UTF-8 text") from exc
    files = [cue]
    for line in lines:
        match = CUE_FILE.match(line)
        if not match:
            continue
        name = match.group(1) or match.group(2)
        if name in {"", ".", ".."} or "/" in name or "\\" in name:
            raise HILError("Only same-directory CUE FILE references are supported")
        file = cue.parent / name
        if not file.is_file() or file.is_symlink():
            raise HILError(f"Missing or unsafe CUE FILE reference: {name}")
        if file not in files:
            files.append(file)
    if len(files) < 2:
        raise HILError("CUE contains no FILE references")
    return files


def rpc(url: str, method: str, params: dict | None = None) -> object:
    payload = {"jsonrpc": "2.0", "id": "psyz-hil", "method": method}
    if params is not None:
        payload["params"] = params
    headers = {"Content-Type": "application/json"}
    api_key = os.environ.get("ZAPAROO_API_KEY")
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"
    req = request.Request(
        url.rstrip("/") + "/api/v0.1",
        data=json.dumps(payload).encode("utf-8"),
        headers=headers,
        method="POST",
    )
    try:
        with request.urlopen(req, timeout=35) as response:
            result = json.load(response)
    except (error.URLError, TimeoutError, ValueError) as exc:
        raise HILError(f"Zaparoo API failure calling {method}: {type(exc).__name__}") from exc
    if not isinstance(result, dict) or result.get("id") != payload["id"]:
        raise HILError(f"Invalid JSON-RPC response for {method}")
    if "error" in result:
        detail = result["error"]
        message = detail.get("message", "unknown") if isinstance(detail, dict) else "unknown"
        raise HILError(f"Zaparoo {method} error: {message}")
    if "result" not in result:
        raise HILError(f"Missing JSON-RPC result for {method}")
    return result["result"]


def deploy(host: str, run_id: str, files: list[Path]) -> str:
    if not VALID_HOST.fullmatch(host):
        raise HILError("Invalid --host; use a DNS name or IPv4 address")
    if not VALID_NAME.fullmatch(run_id):
        raise HILError("Invalid --run-id (ASCII letters, digits, underscore, hyphen)")
    target = f"{REMOTE_ROOT}/{run_id}"
    destination = f"root@{host}:{target}/"
    ssh_options = ["-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes", "-o", "ConnectTimeout=10"]
    subprocess.run(
        ["ssh", *ssh_options, f"root@{host}", f"mkdir -p -- {shlex.quote(target)}"],
        check=True, timeout=30,
    )
    subprocess.run(
        ["scp", *ssh_options, *[str(file) for file in files], destination],
        check=True, timeout=300,
    )
    return f"{target}/{files[0].name}"


def screenshot(url: str, output: Path) -> str:
    result = rpc(url, "screenshot")
    if not isinstance(result, dict) or not isinstance(result.get("data"), str):
        raise HILError("Zaparoo did not return screenshot image data")
    try:
        png = base64.b64decode(result["data"], validate=True)
    except (ValueError, base64.binascii.Error) as exc:
        raise HILError("Invalid screenshot base64") from exc
    if not png.startswith(PNG_MAGIC):
        raise HILError("Screenshot did not contain a PNG image")
    output.write_bytes(png)
    return hashlib.sha256(png).hexdigest()


def run(args: argparse.Namespace) -> dict:
    url = args.zaparoo_url or f"http://{args.host}:7497"
    parsed = parse.urlparse(url)
    if parsed.scheme not in {"http", "https"} or not parsed.hostname or parsed.path not in {"", "/"}:
        raise HILError("--zaparoo-url must be a simple http(s) origin")
    if args.buttons and not VALID_BUTTONS.fullmatch(args.buttons):
        raise HILError("Unsupported --buttons macro")
    if args.settle < 0 or args.settle > 60:
        raise HILError("--settle must be between 0 and 60 seconds")

    files = cue_files(args.cue)
    version = rpc(url, "version")
    remote_cue = deploy(args.host, args.run_id, files)
    rpc(url, "run", {"text": f"**launch:{remote_cue}"})
    time.sleep(args.settle)
    active = rpc(url, "media.active")
    if not isinstance(active, dict) or "saturn" not in str(active.get("systemId", "")).lower():
        raise HILError("Saturn media is not confirmed active; inspect MiSTer/core setup")
    if args.buttons:
        rpc(url, "input.gamepad", {"buttons": args.buttons})
        time.sleep(1)
    sha = screenshot(url, args.output / "screenshot.png")
    return {
        "status": "evidence_only",
        "guest_result": "not_asserted",
        "remote_cue": remote_cue,
        "zaparoo_version": version,
        "active_media": active,
        "screenshot_sha256": sha,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cue", type=Path, required=True)
    parser.add_argument("--host", required=True, help="Private MiSTer hostname or IPv4")
    parser.add_argument("--run-id", default="smoke", help="Dedicated remote test directory")
    parser.add_argument("--zaparoo-url", help="Default: http://HOST:7497")
    parser.add_argument("--buttons", default="", help='Optional pad sequence, e.g. "{start}"')
    parser.add_argument("--settle", type=float, default=4, help="Seconds after launch")
    parser.add_argument("--output", type=Path, default=Path("build/mister-hil"))
    args = parser.parse_args(argv)
    args.output.mkdir(parents=True, exist_ok=True)
    try:
        report = run(args)
        code = 0
    except (HILError, OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
        report = {"status": "infrastructure_failed", "guest_result": "not_run", "error": str(exc)}
        code = 2
    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return code


if __name__ == "__main__":
    sys.exit(main())

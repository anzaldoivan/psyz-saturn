#!/usr/bin/env python3
"""Verify fork copies of PsyZ workflows against upstream Git blob hashes."""
import hashlib
import json
import pathlib
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[2]
DIR = ROOT / ".github/workflows"
UP = "https://api.github.com/repos/Xeeynamo/psyz"
FORK_ONLY = {"upstream-parity.yaml"}


def blob_sha(data):
    return hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()


def get_json(url):
    request = urllib.request.Request(url, headers={
        "Accept": "application/vnd.github+json",
        "User-Agent": "psyz-saturn-workflow-parity",
    })
    with urllib.request.urlopen(request, timeout=25) as response:
        return json.load(response)


def main(argv):
    if argv == ["--self-test"]:
        assert blob_sha(b"test\n") == "9daeafb9864cf43055ae93beb0afd6c7d144bfa4"
        assert blob_sha(b"") == "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391"
        print("WORKFLOW-PARITY self-test ok")
        return 0
    if argv:
        print("usage: check_upstream_workflows.py [--self-test]", file=sys.stderr)
        return 2
    try:
        head = get_json(UP + "/commits/main")
        tree = get_json(UP + "/git/trees/" + head["commit"]["tree"]["sha"] + "?recursive=1")
        if tree.get("truncated"):
            raise ValueError("upstream tree truncated")
    except (OSError, KeyError, ValueError) as error:
        print(f"WORKFLOW-PARITY BLOCKED: upstream not available ({error})")
        return 2
    prefix = ".github/workflows/"
    upstream = {item["path"][len(prefix):]: item["sha"]
                for item in tree["tree"]
                if item["type"] == "blob" and item["path"].startswith(prefix)
                and "/" not in item["path"][len(prefix):]
                and item["path"].endswith((".yaml", ".yml"))}
    local = {path.name: blob_sha(path.read_bytes())
             for path in DIR.iterdir() if path.is_file() and path.suffix in (".yaml", ".yml")
             and path.name not in FORK_ONLY}
    missing = sorted(upstream.keys() - local.keys())
    extra = sorted(local.keys() - upstream.keys())
    changed = sorted(n for n in upstream.keys() & local.keys() if upstream[n] != local[n])
    for kind, names in (("missing", missing), ("fork-only-unapproved", extra), ("changed", changed)):
        for name in names:
            print(f"WORKFLOW-PARITY {kind}: {name}")
    ok = not (missing or extra or changed) and len(upstream) > 0
    print(f"WORKFLOW-PARITY {'ok' if ok else 'FAIL'} {len(upstream) - len(missing) - len(changed)} of {len(upstream)} "
          f"upstream workflows at {head['sha']}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

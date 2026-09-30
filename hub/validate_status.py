"""Command-line validator for generic hub_status.json files."""
from __future__ import annotations

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import status_contract as contract


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if len(argv) != 1:
        print("usage: python validate_status.py <hub_status.json>", file=sys.stderr)
        return 2
    path = argv[0]
    try:
        with open(path, encoding="utf-8-sig") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        print("ERROR: cannot read JSON: %s" % e)
        return 1

    out = contract.validate_status(data)
    for item in out["errors"]:
        print("ERROR:", item)
    for item in out["warnings"]:
        print("WARNING:", item)
    if not out["errors"]:
        print("OK: hub_status.json contract is valid")
    return 1 if out["errors"] else 0


if __name__ == "__main__":
    raise SystemExit(main())

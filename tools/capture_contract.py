#!/usr/bin/env python3
"""Capture contract fixtures from a live McpLink editor.

Usage: capture_contract.py <spec.jsonl> [out_dir]

Each spec line is {"name", "description", "route", "tool", "request"} and, once
POSTed to the plugin, becomes tests/fixtures/contract/<name>.json with the real
HTTP status and the response envelope exactly as the plugin serialised it.

Fixtures are the shared contract between the Rust server and the plugin, so they
must be captured, never hand-written.
"""
import json
import os
import sys
import urllib.error
import urllib.request

PORT = int(os.environ.get("PLUGIN_PORT", "8091"))
OUT = sys.argv[2] if len(sys.argv) > 2 else "tests/fixtures/contract"


def post(route: str, body: dict) -> tuple[int, str]:
    req = urllib.request.Request(
        f"http://127.0.0.1:{PORT}{route}",
        data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            return resp.status, resp.read().decode("utf-8")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8")


def main() -> int:
    specs = [json.loads(l) for l in open(sys.argv[1], encoding="utf-8") if l.strip()]
    os.makedirs(OUT, exist_ok=True)
    failed = 0
    for spec in specs:
        if "sleep" in spec:
            import time

            time.sleep(spec["sleep"])
            continue
        status, text = post(spec["route"], spec["request"])
        try:
            envelope = json.loads(text)
        except json.JSONDecodeError:
            print(f"!! {spec['name']}: non-JSON response: {text[:200]}")
            failed += 1
            continue
        if spec.get("skip_capture"):
            print(f"-- {spec['name']}: setup call, status {status}")
            continue
        expect_ok = spec.get("expect_ok", True)
        if envelope.get("ok") is not expect_ok:
            print(f"!! {spec['name']}: expected ok={expect_ok}, got {text[:300]}")
            failed += 1
            continue
        fixture = {
            "name": spec["name"],
            "description": spec["description"],
            "route": spec["route"],
            "tool": spec["tool"],
            "request": spec["request"],
            "status": status,
            "response": envelope,
        }
        path = os.path.join(OUT, f"{spec['name']}.json")
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(fixture, f, indent=2, ensure_ascii=False, sort_keys=False)
            f.write("\n")
        print(f"ok {spec['name']} -> {path} (HTTP {status})")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())

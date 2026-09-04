#!/usr/bin/env python3
"""Sequential MCP stdio driver for manual/E2E testing.

Usage: mcp_call.py <calls.jsonl>
Each line: {"name": "<tool>", "arguments": {...}} — sent one at a time,
waiting for each response so ordering is deterministic (the server handles
requests concurrently, so pipelining them races).
A line of {"sleep": <seconds>} pauses instead, for letting the game tick.
"""
import json
import os
import subprocess
import sys
import time

BIN = os.environ.get("MCP_UNREAL_BIN", "./target/debug/mcp-unreal.exe")
# Result bodies are truncated for readability; raise for full payloads.
MAXLEN = int(os.environ.get("MCP_CALL_MAXLEN", "700"))


def main() -> int:
    calls = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
    proc = subprocess.Popen(
        [BIN],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        encoding="utf-8",
    )

    def send(msg):
        proc.stdin.write(json.dumps(msg) + "\n")
        proc.stdin.flush()

    def read_id(want):
        while True:
            line = proc.stdout.readline()
            if not line:
                raise SystemExit("server closed the connection")
            msg = json.loads(line)
            if msg.get("id") == want:
                return msg

    send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
          "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                     "clientInfo": {"name": "e2e", "version": "0"}}})
    read_id(1)
    send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    failures = 0
    for n, call in enumerate(calls, start=2):
        if "sleep" in call:
            time.sleep(call["sleep"])
            print("=== sleep %.2fs" % call["sleep"])
            continue
        send({"jsonrpc": "2.0", "id": n, "method": "tools/call",
              "params": {"name": call["name"], "arguments": call.get("arguments", {})}})
        msg = read_id(n)
        result, error = msg.get("result"), msg.get("error")
        print("=== " + call["name"])
        if error:
            failures += 1
            print("  PROTOCOL ERROR:", json.dumps(error)[:MAXLEN])
        elif result.get("isError"):
            failures += 1
            print("  TOOL ERROR:", json.dumps(result.get("content"))[:MAXLEN])
        else:
            body = result.get("structuredContent", result.get("content"))
            print(" ", json.dumps(body)[:MAXLEN])
    proc.stdin.close()
    proc.wait(timeout=10)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

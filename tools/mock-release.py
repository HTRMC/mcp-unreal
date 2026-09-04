#!/usr/bin/env python3
"""Serve a fake "newer release" so the self-updater can be exercised for real.

The updater's interesting paths only run when a *newer* version exists, which
is never true of the release you are developing on. This takes the assets of a
real published release, renames them to a higher version, and serves them
through the two endpoints the updater uses — so the download, the digest check,
the plugin swap and the binary swap all run against genuine multi-megabyte
archives rather than fixtures.

    gh release download v0.1.0 -D assets
    py -3 tools/mock-release.py assets --as 0.2.0

Then point a server at it, with its state directory redirected somewhere
disposable so nothing real is touched:

    MCP_UNREAL_UPDATE_API=http://127.0.0.1:8799 \
    MCP_UNREAL_UPDATE_DIR=/tmp/update-state \
    MCP_UNREAL_AUTO_UPDATE=apply \
    MCP_UNREAL_PROJECT=<a throwaway project> \
    MCP_UNREAL_BIN=<a copy of the binary> \
    py -3 tools/mcp_call.py calls.jsonl

Use a *copy* of the binary and a throwaway project: a successful run replaces
both, which is the point.
"""
import argparse
import hashlib
import http.server
import json
import pathlib
import shutil
import sys

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument("assets", type=pathlib.Path, help="folder of real release assets")
parser.add_argument("--from", dest="old", default="0.1.0", help="version in those filenames")
parser.add_argument("--as", dest="new", default="0.2.0", help="version to present them as")
parser.add_argument("--port", type=int, default=8799)
parser.add_argument("--out", type=pathlib.Path, help="where the renamed copies go")
args = parser.parse_args()

if not args.assets.is_dir():
    sys.exit(f"{args.assets} is not a directory — download a release into it first")

out = args.out or args.assets.parent / f"release-{args.new}"
out.mkdir(parents=True, exist_ok=True)

assets = []
for src in sorted(args.assets.iterdir()):
    if not src.is_file() or args.old not in src.name:
        continue
    dst = out / src.name.replace(args.old, args.new)
    if not dst.exists() or dst.stat().st_size != src.stat().st_size:
        shutil.copyfile(src, dst)
    body = dst.read_bytes()
    assets.append(
        {
            "name": dst.name,
            "browser_download_url": f"http://127.0.0.1:{args.port}/assets/{dst.name}",
            "size": len(body),
            "digest": "sha256:" + hashlib.sha256(body).hexdigest(),
        }
    )

if not assets:
    sys.exit(f"no files in {args.assets} carry the version {args.old}")

RELEASE = {"tag_name": f"v{args.new}", "assets": assets}


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *fargs):
        print("  " + fmt % fargs, file=sys.stderr, flush=True)

    def _send(self, body: bytes, content_type: str) -> None:
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        # Any owner/repo is accepted: the updater derives the path from the
        # crate's own repository field, and this stands in for whatever it asks.
        if self.path.endswith("/releases/latest"):
            self._send(json.dumps(RELEASE).encode(), "application/json")
            return
        if self.path.startswith("/assets/"):
            target = out / pathlib.Path(self.path).name
            if target.is_file():
                self._send(target.read_bytes(), "application/octet-stream")
                return
        self.send_response(404)
        self.end_headers()


server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
print(f"serving v{args.new} on http://127.0.0.1:{args.port}", flush=True)
for asset in assets:
    print(f"  {asset['name']}  {asset['size']} bytes", flush=True)
server.serve_forever()

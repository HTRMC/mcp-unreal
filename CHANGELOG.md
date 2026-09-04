# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[semantic versioning](https://semver.org/spec/v2.0.0.html).

The server (`mcp-unreal`) and the editor plugin (`McpLink`) are versioned and
released together: a release's server archive and plugin zip are meant to be
installed as a pair.

## [Unreleased]

## [0.1.0] - 2026-09-04

First public release. Requires **Unreal Engine 5.8**; support for other UE 5
versions is planned.

### Added

- **MCP server** (`mcp-unreal`) — a single Rust binary speaking MCP over stdio,
  exposing 50 tools across builds, tests, editor control, content authoring and
  engine API lookup.
- **McpLink editor plugin** — HTTP control surface on `127.0.0.1:8091` with
  game-thread-safe handlers, undo-able mutations and real HTTP status codes.
  Ships as precompiled binaries, so Blueprint-only projects can use it without
  Visual Studio.
- **PIE input injection** — drive the running game with injected keys, mouse
  look, gamepad axes and Enhanced Input actions, with the PIE window unfocused.
- **Headless build and test** — `build_project`, `run_tests`, `run_visual_tests`
  and friends drive `Build.bat` / `UnrealEditor-Cmd` with parsed, structured
  output.
- **Authoring tools** — Blueprints, Animation Blueprints, Widget Blueprints
  (UMG), materials and material graphs, DataTables, Enhanced Input assets,
  instanced meshes, Level Sequences and widget animations, landscape and
  foliage.
- **Interop plugins** — `McpLinkNiagara`, `McpLinkGAS`, `McpLinkPCG` and
  `McpLinkPython` add Niagara, Gameplay Ability System, PCG and Python route
  groups when the matching engine plugin is enabled.
- **Engine API lookup** — `lookup_class` and `search_api` read the installed
  engine's own C++ headers, so signatures always match the exact build.
- **Contract fixtures** — captured request/response envelopes checked by both
  the Rust tests and the plugin's automation tests, so a rename on either side
  fails a test rather than an agent.
- Engine auto-discovery from the Epic Launcher manifest and the conventional
  install locations on Windows, macOS and Linux; `UE_ENGINE_ROOT` overrides it.
- `status` reports live capabilities, the detected engine version and a
  remediation hint for anything offline, including an engine-version mismatch.

### Known limitations

- The plugin binaries are built for **Windows / UE 5.8**. On macOS and Linux,
  build the plugin from source.
- A headless editor (`-nullrhi`) cannot capture the viewport, spawn Niagara
  systems or play widget animations; `status` and the affected tools say so.
- One editor at a time — a second instance cannot bind port 8091.

[Unreleased]: https://github.com/HTRMC/mcp-unreal/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/HTRMC/mcp-unreal/releases/tag/v0.1.0

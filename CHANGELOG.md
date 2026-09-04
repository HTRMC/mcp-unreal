# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[semantic versioning](https://semver.org/spec/v2.0.0.html).

The server (`mcp-unreal`) and the editor plugin (`McpLink`) are versioned and
released together: a release's server archive and plugin zip are meant to be
installed as a pair.

## [Unreleased]

### Added

- **`niagara_author`** — Niagara authoring, not just driving: create system and
  emitter assets, add/remove/rename/enable emitters, read the whole stack
  (every emitter's emitter-spawn/update and particle-spawn/update module lists
  with typed inputs, plus renderers), add and remove modules anywhere in a
  stack, set literal module input values, add and remove renderers, then
  compile — with each script's status and the real compile errors reported,
  since `PollForCompilationComplete`'s bool means "still compiling" and
  "nothing to compile" alike.
- **`anim_asset_ops` / `anim_notify_ops` / `skeleton_ops` /
  `skeletal_mesh_ops` / `physics_asset_ops`** (new `McpLinkAnimation` module) —
  the animation assets that could previously only be *referenced*. Montages
  (slots, segments, sections), Anim Composites, Blend Spaces and Aim Offsets;
  notifies, notify states, notify tracks, sync markers and curves; skeleton
  sockets, virtual bones and slot groups; skeletal-mesh LODs, material slots
  and sockets; and ragdoll generation with per-body and per-constraint editing.
- **`localization_ops`** — the Localization Dashboard's pipeline: targets and
  their cultures, the native (source) culture, which sources are gathered from,
  regenerating the commandlet config scripts, and running `gather` and `compile`
  to produce a target's manifest, per-culture archives and `.locres` files. Target edits go through
  the editor; the two runs shell out to the `GatherText` commandlet the way the
  dashboard itself does.
- **`movie_render`** (new `McpLinkMovieRender` interop plugin) — Movie Render
  Queue: build a render config, tune its settings, and render a Level Sequence
  to an image sequence or video. `sequence_ops` could author a sequence but not
  render one, which left the cinematic pipeline a step short of any output.
- **`render_ops` / `material_function`** — render targets (create, clear,
  draw a material into, read pixels, export to PNG/EXR/HDR, bake to a
  Texture2D), asset thumbnail rendering, and Material Function authoring.
- **`state_tree_ops` / `nav_ops`** — State Tree authoring (states,
  transitions, tasks, conditions, evaluators, and a compile report) and
  navigation queries against the built nav mesh (point projection, pathing with
  waypoints, reachability, raycasts, random reachable points).
- **`metasound_ops`** (new `McpLinkMetaSound` interop plugin) — MetaSound
  authoring: create a Source or Patch, discover the registered node classes,
  add and remove nodes, connect them to each other and to the graph interface,
  add graph inputs and outputs, and set literal input defaults. Built on the
  engine's own builder API, the only path that keeps the document, the frontend
  registry and the editor graph in step.
- **`source_control_ops` / `validate_ops` / `gameplay_tag_ops` / `curve_ops` /
  `reference_ops`** (new `McpLinkWorkflow` module) — the editor's own
  housekeeping. Source control through whichever provider the project uses;
  Data Validation, Map Check and a compile-every-Blueprint sweep; gameplay tags
  in the project ini; keyframes on Curve assets; and the reference graph behind
  the Reference Viewer and Size Map.
- **`sublevel_ops` / `world_partition_ops` / `level_instance_ops`** — the world
  beyond the one level `level_ops` opens. Streaming sublevels (create, add,
  remove, visibility, locking, current level, level transform, moving actors
  between levels); World Partition data layers and editor region loading; and
  Level Instances, Packed Level Actors and actor merging.
- **`package_project`** — packaging past the end of `cook_project`: RunUAT
  BuildCookRun through compile, cook, stage, pak and archive, with
  dedicated-server targets, distribution builds, IoStore, and deploy-and-run on
  a connected device (Launch On).
- **`code_ops`** — C++ scaffolding with no editor running. `create_class`
  substitutes the engine's own class templates, and resolves any reflected
  engine class as a base (header and ancestry from the class index behind
  `lookup_class`), so the output matches the New C++ Class wizard's.
  `create_module` writes a module and registers it in the `.uproject` and the
  build targets, splicing the descriptor rather than reserialising it.
- **`trace_ops`** — reading Unreal Insights traces back, which `perf_ops`
  could only record. Lists the .utrace files on disk, then analyses one
  headlessly through UnrealInsights and exports timers, aggregated timer
  statistics, timing events, threads or counters to CSV with the first rows
  returned inline.
- **`asset_ops`** — the asset lifecycle that was missing entirely: create any
  factory-backed asset class (Data Assets, Curves, Curve Tables, String Tables,
  User-Defined Structs and Enums) with optional factory configuration; import
  and reimport source files from disk through the automated importer, so no
  dialog appears; rename, move, duplicate and delete with full reference
  fixup; save individual assets or every dirty package; create, rename, list
  and delete content folders; fix up redirectors; export; read and write
  metadata tags.
- **`user_type_ops`** — members of User-Defined Structs and entries of
  User-Defined Enums: add, rename, retype, default and remove.
- **`blackboard_ops` / `behavior_tree_ops`** (new `McpLinkAI` module) — author
  Blackboards (keys of every type, parent inheritance) and Behavior Trees. The
  tree is built through the editor graph and compiled into the runtime tree the
  way the Behavior Tree editor does, so the result survives being reopened;
  `list_node_classes` discovers every task, composite, decorator and service the
  project has.
- **`sound_cue_ops`** — Sound Cue node trees: discover SoundNode classes, add
  and wire nodes, choose the output node.
- **`static_mesh_ops`** — LOD generation and LOD groups, simple and
  convex-decomposition collision, Nanite settings, lightmap UV generation,
  sockets and material slots, through `UStaticMeshEditorSubsystem`.
- **`editor_ops`** — undo and redo (with the name of the transaction each would
  affect), selection, actor attach/detach, per-instance components, duplication
  with offsets, outliner folders, labels, and snap-to-floor.
- **`build_level`** — the editor's Build menu through `FEditorBuildUtils`:
  lighting, navigation, geometry, HLOD, reflection captures, texture streaming,
  virtual textures and landscapes.
- **`perf_ops`** — the structured performance readback `stat fps` cannot give:
  real frame-time distribution (average, median, min, max, p99), process
  memory, and Unreal Insights trace start/stop.
- **`blueprint_modify`** node vocabulary grown from 5 node types to more than
  30 — casts, macros from any library, spawn actor, struct make/break,
  containers, select, all four switches, timelines, event overrides,
  component-bound events, the delegate family, reroutes and comments — plus
  Blueprint structure: components on the construction-script tree, function
  parameters and return values, local variables, interfaces, event dispatchers
  with signatures, and reparenting.
- Blueprint variable types now cover containers (`array<int>`, `set<name>`,
  `map<name,float>`), structs, enums, soft object/class and interface
  references.
- `pie_control start` takes multiplayer options — client count, net mode,
  dedicated server, one-process — and a spawn transform.
- `tools/scan-unity-collisions.py`, run in CI: catches two files in one plugin
  module declaring the same anonymous-namespace helper, which adaptive unity
  builds turn into an error that appears only on some machines.

### Fixed

- `spawn_actor` on a volume class (Nav Mesh Bounds, Trigger, Blocking,
  Post Process, ...) produced a brush-less actor that enclosed nothing: a Nav
  Mesh Bounds Volume placed that way built an empty nav mesh, and Map Check
  reported "collision component with 0 radius". Volumes now get the same cube
  brush the editor's own placement builds.

- Duplicate anonymous-namespace helpers (`McpTestFlags`, `IntOr`) broke the
  plugin build depending on which files had been edited most recently. They now
  live in `McpLinkCore` (`McpTestFlags.h`, `McpJson.h`), along with a shared
  `ResolveWorldOrError`.

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

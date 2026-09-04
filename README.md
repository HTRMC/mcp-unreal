# mcp-unreal

MCP server giving AI coding agents autonomous control of **Unreal Engine 5.8** — headless builds and tests, live editor manipulation, viewport capture, and the headline feature: **playing the game in PIE via injected input** (keys, mouse look, gamepad axes, Enhanced Input actions).

Two halves:

- **`mcp-unreal`** — a single Rust binary speaking MCP over stdio (official [rmcp](https://github.com/modelcontextprotocol/rust-sdk) SDK). Headless operations spawn `Build.bat` / `UnrealEditor-Cmd` directly; editor operations go over loopback HTTP to the plugin.
- **`McpLink`** — a UE 5.8 editor plugin (C++23) hosting the HTTP control surface on `127.0.0.1:8091` (CVar `McpLink.Port`). Real HTTP status codes, undo-able mutations (`FScopedTransaction`), game-thread-safe handlers, and deferred responses so `pie_control` can block until PIE actually starts.

No Remote Control API dependency — property reads/writes and function calls go through engine reflection in the plugin.

## Input injection

The reference implementations this was modeled on can inspect and teleport, but cannot *play*. `input_inject` holds a desired-input state that a per-frame game-thread applier feeds into `APlayerController::InputKey` using UE 5.8's `FInputKeyEventArgs::CreateSimulated`:

- Drives **both legacy and Enhanced Input** — injected keys land in the same `KeyStateMap` that mapping contexts evaluate.
- Works with the **PIE window unfocused** (no Slate focus or mouse capture involved).
- Held keys and axes are **re-injected every frame**, so the character keeps moving; a held key is re-asserted if the engine drops it (viewport focus loss flushes pressed keys).
- Safety: per-command `timeout_ms`, auto-release on `EndPIE`, and a watchdog that releases everything after 30s of inactivity (`McpLink.InputIdleTimeout`).

```jsonc
input_inject {"operation": "press_key", "key": "W", "timeout_ms": 3000}
input_inject {"operation": "tap_key", "key": "SpaceBar"}
input_inject {"operation": "set_axis", "axis": "Gamepad_LeftX", "value": 0.8}
input_inject {"operation": "mouse_move", "dx": 300, "dy": 0, "mode": "once", "frames": 10}
input_inject {"operation": "inject_action", "action": "/Game/Input/IA_Move", "value": [0, 1, 0]}
input_inject {"operation": "get_state"}   // includes what the engine reports as actually down
```

## Tools (50)

| Area | Tools |
|---|---|
| Health | `status` — live-probed features and remediation hints |
| Build (headless) | `build_project`, `generate_project_files` |
| Tests (headless) | `run_tests`, `run_visual_tests`, `list_tests`, `get_test_log` |
| Reflection | `get_property`, `set_property`, `call_function` |
| Actors | `get_level_actors`, `spawn_actor`, `delete_actors`, `move_actor`, `get_actor_components` |
| Levels & assets | `level_ops`, `search_assets`, `get_asset_info` |
| Editor | `run_console_command`, `get_output_log`, `capture_viewport` |
| Play | `pie_control`, `player_control`, **`input_inject`** |
| Blueprints | `blueprint_query`, `blueprint_modify`, `anim_blueprint_query`, `anim_blueprint_modify`, `widget_blueprint_query`, `widget_blueprint_modify` |
| Content | `material_ops`, `material_graph`, `texture_info`, `data_table_ops`, `input_asset_ops`, `ism_ops`, `sequence_ops` |
| World building | `landscape_ops`, `foliage_ops` |
| Introspection | `subsystem_query`, `ui_query` |
| Engine API | `lookup_class`, `search_api` |
| Project config | `project_ops`, `config_ops`, `cook_project` |
| Interop plugins | `niagara_ops`, `gas_ops`, `pcg_ops`, `python_exec` (need McpLinkNiagara / McpLinkGAS / McpLinkPCG / McpLinkPython enabled) |

Headless tools work with no editor open. Editor tools need the Unreal Editor running with McpLink enabled — call `status` to see what is currently available.

### Engine API lookup

`lookup_class` and `search_api` read the **installed engine's own C++ headers**, so signatures always match your exact build — no curated docs to drift. The first call indexes ~49k headers (about 25s) and caches the result per engine version; later calls are instant.

```jsonc
lookup_class {"class_name": "ACharacter"}   // parent, module, doc, 42 properties, 49 functions
search_api   {"pattern": "InjectInputForAction", "module": "EnhancedInput"}
```

### Blueprint editing

`blueprint_modify` spawns nodes through `FGraphNodeCreator` and configures them before finalizing, so a `call_function` node comes back with the target function's real pins. Connections go through the graph schema, so type-incompatible links are refused with a reason rather than silently corrupting the graph. Every edit is undo-able in the editor.

### Animation Blueprints

`anim_blueprint_modify` creates an Animation Blueprint for a skeleton and authors its state machines the way the editor does: `add_state_machine` (wired into the output pose), `add_state` with an animation and entry flag, `add_transition` with crossfade, priority, automatic end-of-animation rules, or a bool variable as the rule. Nested machines work by targeting a state's graph. Every state and transition rule graph is addressable by path (`Locomotion/Idle`, `Locomotion/Walk to Idle`), so `blueprint_modify add_node` / `connect_pins` build arbitrary rules and anim node networks inside them. `anim_blueprint_query inspect` reports the whole structure — states, animations, entry state, transitions and whether each rule is bound.

### Widget Blueprints (UMG)

`widget_blueprint_modify` is the UMG designer as an API, built on the engine's own `FWidgetBlueprintOperationUtils` so the designer's rules apply: `create` (parent class, root panel), `add_widget` under any panel by class short name or Widget Blueprint path, `move_widget`, `remove_widget`, `rename_widget`, `wrap_widget`, `replace_widget`. Appearance goes through `set_widget` and layout through `set_slot`, both taking plain JSON (`{"Text": "Play"}`, `{"LayoutData": {"Offsets": {...}, "Anchors": {...}}}`, `{"Padding": {...}, "HorizontalAlignment": "HAlign_Center"}`) with partial nested-struct updates and unknown keys rejected with the editable list. `bind_event` creates the OnClicked / OnValueChanged / OnTextCommitted event node in the event graph (exposing the widget as a variable first) so `blueprint_modify` can wire the logic. `widget_blueprint_query` lists assets, dumps the tree with slots, reads one widget's current property values in the same JSON shapes, and discovers placeable classes and each class's events and properties.

### Sequencer: Level Sequences and widget animations

`sequence_ops` authors cinematics: `create` a sequence at a chosen display rate and length, `add_binding` to possess a level actor or one of its components (a component binding creates the actor's parent binding automatically), or bind it as a spawnable. Tracks come from `add_track` by short class name ("Transform", "SkeletalAnimation", "CameraCut", "Audio", "Fade") or from `add_property_track`, which picks the track class, channel count and enum or object class from the property itself. `add_key` keys any channel by the name `inspect` reports ("Location.X", or "Value" on a single-channel section), one key or a batch, with linear, constant or auto interpolation; `remove_keys`, `set_channel_default`, `set_section_range`, `add_camera_cut` and `add_marked_frame` fill in the rest. `add_to_level` spawns a LevelSequenceActor so PIE plays the result.

UMG animations are the same machinery, so the same tool authors them: `create_widget_animation` adds one to a Widget Blueprint, `add_widget_binding` binds a widget of that blueprint (or `"Self"`), and every track, section and key operation then works on it — address it as `/Game/UI/WBP_Menu:Anim_FadeIn` wherever `sequence` is taken. `save` writes the owning Widget Blueprint. A widget animation only *advances* where Slate ticks, so a headless editor authors and inspects one but never plays it; play it in a windowed editor with `call_function` `PlayAnimation` on a live widget.

Times cross the wire as display-rate frames or as seconds, interchangeably, and come back as both plus the raw tick. Everything a section exposes as a UPROPERTY — a skeletal animation section's `Params.Animation`, an audio section's `Sound`, easing — is a normal `set_property` on the `path` each operation reports.

### Landscape and foliage

`landscape_ops` creates a landscape at a chosen resolution (quads per section x sections per component x components) and scale, reads and writes heights over any vertex rect, and adds or paints the weightmap layers the landscape material blends. Heights are centimetres of Z relative to the landscape actor in both directions — the uint16 heightmap encoding never crosses the wire — and a sculpt takes either one flat height or one value per vertex, row-major from `min_y`. Writing heights also rebuilds the collision heightfield, so a line trace (or a foliage scatter) sees the new terrain immediately.

`foliage_ops` makes foliage types from Static Meshes, then places instances either at explicit transforms or by scattering a count over an area — each point is dropped onto whatever has collision beneath it, with optional alignment to the surface normal, random yaw, and a scale range with a fixed seed for a repeatable result. Instances can be removed inside a sphere or wholesale. Foliage type settings are ordinary properties on the reported `type` path, so `set_property` edits them.

### Interop plugins: Niagara, Gameplay Abilities, PCG

Three optional sibling plugins add route groups for engine systems a project may or may not use; `status` lists `niagara`, `gameplay_abilities` and `pcg` in `features` when they are loaded.

- `niagara_ops` lists systems (engine templates under `/Niagara`), reads emitters and user parameters, spawns systems at a location or attached to an actor, sets typed user parameters on live components, and pauses/activates/destroys them. Spawning needs a rendering editor (Niagara refuses under `-nullrhi`); the tool says so. A freshly loaded system compiles for a few seconds first — `system_ready` tells you when it is actually running.
- `gas_ops` reads an actor's attribute sets (current and base), granted abilities, active effects and owned tags, grants and activates abilities, applies and removes gameplay effects, sets attribute base values and adds loose tags.
- `pcg_ops` authors PCG graphs (create, add nodes from any of the ~200 settings classes, connect pins, save), attaches components to actors, sets graph, seed and parameters, generates asynchronously and reports the result. Per-node options are ordinary properties on the reported `settings_path`, so `set_property` edits them.

### Contract fixtures

`tests/fixtures/contract/` holds real request/response exchanges captured from a live editor by `tools/capture_contract.py`. Both halves check the same files: the Rust tests assert that each fixture's request deserializes into the owning tool's input type, that the body the tool builds matches it byte for byte, and that the client unwraps every recorded envelope to exactly its `data` or to an error carrying the plugin's own code and status. The plugin's `McpLink.Core.Contract.Fixtures` test asserts each route is still registered and that its responder still produces the recorded envelope. A field renamed on either side fails a test instead of an agent.

### Python escape hatch

`python_exec` (McpLinkPython plugin, which enables the Python Editor Script Plugin) runs code in the editor's interpreter and returns print output, warnings and, in `eval` mode, the expression's value. A Python exception fails the call with the traceback. `persist: true` keeps names between calls. Anything the `unreal` module exposes that has no dedicated tool yet is reachable this way.

### Content authoring

`material_graph` edits a material's node graph: add expressions (constants, parameters, texture samples, math), connect them to each other and to the material inputs (BaseColor, Roughness, Normal, ...), inspect, auto-layout and recompile with error reporting; unnamed channel-mask outputs are picked by index. `material_ops` creates materials and instances and reads/writes instance parameters (validated against the parameter list — UE 5.8's `UMaterialEditingLibrary::SetMaterialInstance*ParameterValue` helpers always return `false`, so their return value is deliberately ignored). `data_table_ops` creates tables for any `FTableRowBase` struct and edits rows with partial updates or CSV import. `input_asset_ops` authors Input Actions and Mapping Contexts. `ism_ops` scatters instanced meshes in bulk.

Character movement tuning needs no dedicated tool: `get_property`/`set_property` on the pawn's `CharacterMovement` component already covers `MaxWalkSpeed`, `JumpZVelocity`, etc.

### Introspection

`subsystem_query` lists the live engine, editor, world, game-instance and local-player subsystems (with their owning world), so an agent can see what systems a project actually runs. `ui_query` reads the UI two ways: the raw Slate tree of any open window (`windows`, `tree`, `find` by widget type, each with the C++ location that created the widget) and the UMG view of every live `UUserWidget` in a world (`umg`), including the current text of each `UTextBlock`. Both work headless under `-nullrhi`, where Slate windows exist without being drawn.

Not included: a RealtimeMesh interop — the plugin is third-party and not part of the engine, so there is nothing to build against by default.

## Requirements

- **Unreal Engine 5.8.** The plugin uses 5.8-only APIs and C++23; it will not compile against 5.7 or earlier. Support for more UE 5 versions is planned — `status` tells you when your engine is a different version.
- Windows for the prebuilt plugin. The server itself runs on Windows, macOS and Linux; on macOS and Linux build the plugin from source.
- No Visual Studio needed to *use* a release — the plugin ships compiled, so **Blueprint-only projects work**. You only need a compiler to build from source.

## Quick start

**1. Install the plugin.** Download `McpLink-<version>-UE5.8-Win64.zip` from [Releases](https://github.com/HTRMC/mcp-unreal/releases) and unzip it into `<YourProject>/Plugins/`, so you get `<YourProject>/Plugins/McpLink/`. Drop in the `McpLink{Niagara,GAS,PCG,Python}` folders too if you want those interop tools. Open the project and enable them under Edit → Plugins if they are not on already.

**2. Install the server.** Download the `mcp-unreal` archive for your platform from the same release and unzip it anywhere, then register it with your MCP client:

```powershell
claude mcp add mcp-unreal -- C:\Tools\mcp-unreal\mcp-unreal.exe
```

Or copy `.mcp.json.example` to `.mcp.json` and edit the paths.

**3. Check it.** Open your project in the Unreal Editor and call `status` — it reports the detected engine, the project, whether the plugin is reachable and what is currently available.

| Env var | Default | Meaning |
|---|---|---|
| `UE_ENGINE_ROOT` | auto-detected | Engine install root; found via the Epic Launcher manifest and the usual install locations |
| `UE_EDITOR_PATH` | derived | Override the UnrealEditor-Cmd path |
| `MCP_UNREAL_PROJECT` | walk up from cwd | `.uproject` file or its folder |
| `PLUGIN_PORT` | `8091` | McpLink HTTP port (CVar `McpLink.Port` in the editor) |
| `MCP_UNREAL_LOG_LEVEL` | `info` | tracing filter (stderr only) |

## Security

McpLink is a **development tool that gives an agent full control of your editor**, so treat it accordingly:

- The plugin listens on loopback only (UE's HTTP server defaults to `BindAddress=localhost`) and has **no authentication**. Any process on your machine can drive the editor while it is running — and with `McpLinkPython` enabled, that means running arbitrary Python inside the editor. Don't enable it on a shared machine, and don't set `[HTTPServer.Listeners] DefaultBindAddress=any` in a project that has McpLink.
- An agent can delete actors, overwrite assets and save packages. **Use source control** on any project you point it at.
- Ship your game without these plugins: everything here is editor-only and never gets cooked into a build.

## Development

Building from source needs a Rust toolchain (1.85+) and the usual UE C++ prerequisites.

```powershell
cargo build --release
cargo test                       # parser fixtures + unit tests, no UE needed

pwsh -File tools/setup-dev.ps1   # link plugin/McpLink* into test-project/Plugins
& "$env:UE_ENGINE_ROOT\Engine\Build\BatchFiles\Build.bat" McpTestEditor Win64 Development -Project="<repo>\test-project\McpTest.uproject" -WaitMutex

py -3 tools\mcp_call.py calls.jsonl   # drive tools sequentially against a running editor
```

`test-project/` is a minimal UE 5.8 project wired up for development, with an Enhanced Input character used to verify injection end to end. Its `Plugins/` entries are NTFS junctions into `plugin/`, which is why they are not committed and `tools/setup-dev.ps1` recreates them after a clone.

`tools/mcp_call.py` takes one JSON object per line (`{"name": ..., "arguments": {...}}`, or `{"sleep": 1.5}` to let the game tick) and is the harness used for the end-to-end checks above.

The plugin ships its own automation tests (`McpLink.*`, in each module's `Private/Tests/`). They run headless through the server's own tool — `run_tests {"filter": "McpLink"}` — which doubles as the end-to-end check that the test runner parses real engine output.

## Releasing

CI builds and tests the server on Windows, macOS and Linux. It cannot build the plugin — that needs an Unreal install, which GitHub-hosted runners do not have — so plugin binaries are packaged locally and attached to the release.

```powershell
# write the release notes under "## [Unreleased]" in CHANGELOG.md first
pwsh -File tools/bump-version.ps1 0.2.0      # Cargo.toml, every .uplugin, CHANGELOG
git commit -am "Release v0.2.0"
git tag v0.2.0 && git push origin main v0.2.0   # Release workflow drafts the release
pwsh -File tools/publish-release.ps1 -Publish   # packages the plugin, attaches it, publishes
```

`tools/package-plugin.ps1` runs `RunUAT BuildPlugin` on each plugin (passing McpLink as a dependency for the interop ones), drops the debug symbols and produces the zip users install.

## License

MIT — see [LICENSE](LICENSE). Unreal Engine is a trademark of Epic Games, Inc.; this project is unaffiliated.

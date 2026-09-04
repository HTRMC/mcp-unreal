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

`pie_control start` also takes the Play settings a networked test needs — `players` (client windows), `net_mode` (`standalone`, `listen_server`, `client`), `dedicated_server` and `one_process` — plus a spawn `location` / `rotation`, so multi-client sessions are reachable by the same tools.

## Tools (87)

| Area | Tools |
|---|---|
| Health | `status` — live-probed features and remediation hints |
| Build (headless) | `build_project`, `generate_project_files` |
| Tests (headless) | `run_tests`, `run_visual_tests`, `list_tests`, `get_test_log` |
| Reflection | `get_property`, `set_property`, `call_function` |
| Actors | `get_level_actors`, `spawn_actor`, `delete_actors`, `move_actor`, `get_actor_components`, `editor_ops` |
| Levels & assets | `level_ops`, `search_assets`, `get_asset_info`, `asset_ops` |
| Editor | `run_console_command`, `get_output_log`, `capture_viewport`, `build_level`, `perf_ops`, `trace_ops` |
| Play | `pie_control`, `player_control`, **`input_inject`** |
| Blueprints | `blueprint_query`, `blueprint_modify`, `blueprint_debug`, `anim_blueprint_query`, `anim_blueprint_modify`, `widget_blueprint_query`, `widget_blueprint_modify` |
| Animation | `anim_asset_ops`, `anim_notify_ops`, `skeleton_ops`, `skeletal_mesh_ops`, `physics_asset_ops` |
| Content | `material_ops`, `material_graph`, `material_function`, `material_layers`, `render_ops`, `texture_info`, `texture_ops`, `data_table_ops`, `input_asset_ops`, `ism_ops`, `sequence_ops`, `static_mesh_ops`, `sound_cue_ops`, `user_type_ops` |
| World building | `landscape_ops`, `foliage_ops`, `sublevel_ops`, `world_partition_ops`, `level_instance_ops` |
| AI | `blackboard_ops`, `behavior_tree_ops`, `state_tree_ops`, `nav_ops` |
| Editor workflow | `source_control_ops`, `validate_ops`, `gameplay_tag_ops`, `curve_ops`, `reference_ops`, `localization_ops` |
| Introspection | `subsystem_query`, `ui_query` |
| Engine API | `lookup_class`, `search_api` |
| Project config | `project_ops`, `config_ops`, `cook_project`, `package_project`, `code_ops` |
| Interop plugins | `niagara_ops`, `niagara_author`, `metasound_ops`, `movie_render`, `chaos_ops`, `gas_ops`, `pcg_ops`, `python_exec` (need McpLinkNiagara / McpLinkMetaSound / McpLinkMovieRender / McpLinkChaos / McpLinkGAS / McpLinkPCG / McpLinkPython enabled) |

Headless tools work with no editor open. Editor tools need the Unreal Editor running with McpLink enabled — call `status` to see what is currently available.

### Engine API lookup

`lookup_class` and `search_api` read the **installed engine's own C++ headers**, so signatures always match your exact build — no curated docs to drift. The first call indexes ~49k headers (about 25s) and caches the result per engine version; later calls are instant.

```jsonc
lookup_class {"class_name": "ACharacter"}   // parent, module, doc, 42 properties, 49 functions
search_api   {"pattern": "InjectInputForAction", "module": "EnhancedInput"}
```

### Blueprint editing

`blueprint_modify` spawns nodes through `FGraphNodeCreator` and configures them *before* finalizing, so a node comes back with real pins: a `call_function` with the target function's signature, a `make_struct` with one pin per member, a `switch_enum` with one case per enumerator, a `macro` with the macro's tunnel pins. The vocabulary covers calls and parent calls, variable get/set, event overrides, custom events and component-bound events, branch, sequence, select and the four switches, macros from any library (`ForEachLoop`, `DoOnce`, `IsValid`, ... — an unknown name comes back with the list), casts, spawn actor, struct make/break, array/set/map literals, timelines, the delegate family, reroutes and comments.

Structure is editable too: components on the construction-script tree, function parameters and return values (the return node is created on demand), local variables, interfaces, event dispatchers with their signatures, the parent class, and variable types including containers (`array<int>`, `set<name>`, `map<name,float>`), structs, enums and soft references.

Connections go through the graph schema, so type-incompatible links are refused with a reason rather than silently corrupting the graph. Every edit is undo-able in the editor.

`blueprint_debug` covers breakpoints, watched pins and the instance their values are read from. Watches are the part that works unattended: during PIE a watched pin backed by a class property reports its live value, with nothing stopped — `list_watches` after each step of a scripted play session is a running trace of the Blueprint's state. Breakpoints can be set, listed, enabled and cleared, but *halting* needs a windowed editor with a human at it. A breakpoint that hits enters Slate's own debugging loop, which ticks Slate and nothing else until the debugger's Resume; `FTSTicker` stops, and the HTTP server ticks on `FTSTicker`, so McpLink cannot answer — including the request that would step or resume. Headlessly there is no debugger UI either, so a hit would hang the editor for good, and arming one there is refused with that explanation.

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

### Sublevels, World Partition and level instances

`sublevel_ops` is the Levels panel: list the streaming levels under the persistent level with their visibility, lock state, actor count and transform; create a new sublevel or add an existing level asset as always-loaded or dynamic; remove one; choose which level new actors spawn into; move a sublevel and its loaded contents in world space; and move existing actors into it.

`world_partition_ops` handles the other kind of world. It reports whether a map is partitioned at all (safe to ask on any map), creates and deletes data layers — making the Data Layer asset when the path is empty — moves actors in and out of them, sets a layer's editor visibility and loading and its initial runtime state, and loads a world-space region so a partitioned map's actors are actually in memory, which they mostly are not when it first opens.

`level_instance_ops` covers the three ways to treat a group of actors as one: a Level Instance, a Packed Level Actor (contents baked into instanced-mesh components), or `merge_actors`, which collapses their meshes into one Static Mesh and leaves the sources alone. `create` needs a windowed editor — `ULevelInstanceSubsystem` hardcodes a Save As dialog for the new level, and a `-nullrhi` editor is refused rather than left wedged on a dialog it cannot draw.

### Asset lifecycle

`asset_ops` is the content browser as an API. `create` makes an empty asset of any class that has a "create new" factory — Data Assets, Curve assets, Curve Tables, String Tables, User-Defined Structs and Enums — and `factory_properties` configures the factory first (a Blueprint's `ParentClass`, a Data Asset's `DataAssetClass`). `import` runs the automated importer on files from disk (FBX, glTF, OBJ, images, audio, CSV) with no dialog, and `reimport` re-runs one from its recorded source or a new file. `rename`, `move`, `duplicate` and `delete` go through `IAssetTools` so every reference is fixed up, and `fixup_redirectors` clears the redirectors a rename leaves behind. Folders, metadata tags, `export` and `save` / `save_all` round it out.

`texture_ops` authors a texture's bytes with no source file involved: create a Texture2D from base64 BGRA8 pixels or a solid fill, read a rectangle back, write one, or flood-fill one. Masks, gradients, palettes and lookup tables have no file to import, and `asset_ops import` covers the ones that do. Everything works on the editor *source* data, so an edit survives recompression; regions are capped at 256x256 per call, so a large texture is painted in tiles — or drawn in one shot with a material through `render_ops`.

### Editor workflow

`editor_ops` covers what a person does with the mouse: `undo` and `redo` (every McpLink edit is a transaction, and `get_history` names what each would do), reading and setting the **selection**, `attach` / `detach` between actors with a socket, `add_component` / `remove_component` on one actor *instance*, `duplicate` with a repeated offset, outliner `set_folder`, `set_label`, and `snap_to_floor`, which drops actors onto whatever has collision beneath them and reports the ones that had nothing below.

### AI: Behavior Trees and Blackboards

`blackboard_ops` creates a Blackboard and adds, retypes and removes keys — `bool`, `int`, `float`, `string`, `name`, `vector`, `rotator`, `object:<Class>`, `class:<Class>`, `enum:<Enum>` — and can point one at a parent blackboard to inherit keys.

`behavior_tree_ops` authors the tree the way the Behavior Tree editor does: it builds the asset's `BTGraph` (with its Root node) and lets `UBehaviorTreeGraph::UpdateAsset` compile that into the runtime tree, because a runtime tree written directly is discarded the next time the asset is opened. `list_node_classes` enumerates every task, composite, decorator and service the project has, native or Blueprint — start there. Composites and tasks are wired as children; decorators and services attach to a node as sub-nodes. `compile` reports whether the graph actually produced a root, which is the difference between a tree that runs and one that silently does nothing. Node settings are properties on the `instance` path `get_tree` reports.

### Animation assets, skeletons and physics assets

`anim_asset_ops` creates and edits the animation assets that were previously reference-only: Montages (slots, animation segments with trim/rate/loop, named sections and their next-section links), Anim Composites, Blend Spaces (1D and 2D) and Aim Offsets (parameter axes, samples placed in the space). `info` reads the whole structure back.

`anim_notify_ops` covers notify tracks, notifies and notify states, sync markers and float curves, through the engine's own `UAnimationBlueprintLibrary` — so curve names land on the skeleton and tracks stay valid. A notify's own settings are properties on the reported path, so `set_property` tunes them.

`skeleton_ops` reads the bone hierarchy and edits sockets, virtual bones and montage slot groups; `skeletal_mesh_ops` handles the LOD chain (through the real reduction module), material slots, mesh-only sockets and morph target listing. Either accepts a Skeletal Mesh path where a skeleton is wanted.

`physics_asset_ops` generates a ragdoll from a skeletal mesh — the same body-and-constraint pass the Physics Asset editor runs on a new asset — then adds and removes individual bodies and constraints. Body and constraint tuning is ordinary `set_property` work on the reported paths.

### State Trees and navigation

`state_tree_ops` authors State Trees, the hierarchical state machine replacing Behavior Trees for a lot of AI and gameplay logic. There is no EdGraph — the editor draws `UStateTreeState` objects directly — so authoring is building that hierarchy: states, transitions, and tasks, enter conditions and evaluators from the structs the project has registered. Then `compile`: an uncompiled State Tree has empty runtime data and runs nothing, and the compile report names whatever the schema rejected.

`nav_ops` asks the built nav mesh the questions that decide whether a level's navigation actually works — project a point onto the mesh, path between two points with waypoints and length, test reachability, raycast for a clear straight walk, pick a random reachable point. `build_level navigation` builds the mesh; a mesh with a hole in it looks exactly like a good one until something tries to walk it.

### Static meshes

`static_mesh_ops` drives `UStaticMeshEditorSubsystem`, so LOD generation runs the real reduction module and `add_convex_collision` runs the real convex decomposition. It reports LODs with vertex counts and screen sizes, material slots, sockets, Nanite settings and collision counts; generates or removes a LOD chain or applies a project LOD group; adds simple (box/sphere/capsule/K-DOP) or convex collision; toggles Nanite and lightmap UV generation; and adds, moves and removes sockets.

### Audio

`sound_cue_ops` builds the node tree a Sound Cue plays: `list_node_classes` for what is available with each node's child capacity, then `add_node` (wave players, random, mixer, modulator, attenuation, concatenator, ...) wired under a parent or as the cue's output, plus `connect`, `set_root` and `remove_node`. Node settings are properties on the reported node path. Sound Classes, Submixes, Attenuation and Concurrency assets need no tool of their own — `asset_ops create` plus `set_property` covers them.

### Builds and performance

`build_level` runs the editor's Build menu through its own entry point (`FEditorBuildUtils`): static lighting, navigation mesh, BSP geometry, Hierarchical LODs, reflection captures, texture streaming, virtual textures and landscapes. Everything except lighting finishes before the call returns; lighting hands off to a Lightmass process, so poll `get_output_log` for it.

`perf_ops` is the structured readback `stat fps` cannot give, because that draws to the viewport: it samples the real frame time over a window of frames and returns average, median, min, max and p99 in milliseconds, reads process memory, and starts or stops an Unreal Insights trace.

`trace_ops` reads those traces back. `list` finds the .utrace files in the shared UnrealTrace store and the project's `Saved/Profiling`; `export` runs UnrealInsights headlessly (`-OpenTraceFile -NoUI -AutoQuit`) against one and exports timers, aggregated timer statistics (instance count with inclusive and exclusive time — where to start when something is slow), individual timing events, threads or counters to CSV, returning the first rows inline. `run` passes arbitrary `TimingInsights.*` commands through for anything `export` does not model.

`package_project` goes past `cook_project` to a runnable build: RunUAT BuildCookRun with compile, cook, stage, pak and archive, plus dedicated-server targets, distribution builds and deploy-and-run on a connected device (Launch On). `cook_project` remains the tool for cooked content alone.

`code_ops` scaffolds C++ without the editor. `create_class` writes the header and source from the engine's *own* class templates (`Engine/Content/Editor/Templates`), so the output matches what the editor's New C++ Class wizard would have produced; the base class can be any reflected class in the installed engine, since its header and ancestry come from the same class index `lookup_class` uses. `create_module` writes a module's `Build.cs`, implementation and folders, then registers it in the `.uproject` and the build targets — the `.uproject` is spliced, not reserialised, so the diff is the new entry and nothing else. Both leave compiling to `generate_project_files` and `build_project`.

### Interop plugins: Niagara, MetaSounds, Movie Render Queue, Chaos, Gameplay Abilities, PCG

Optional sibling plugins add route groups for engine systems a project may or may not use; `status` lists `niagara`, `metasounds`, `movie_render_queue`, `chaos_destruction`, `gameplay_abilities` and `pcg` in `features` when they are loaded.

- `niagara_ops` lists systems (engine templates under `/Niagara`), reads emitters and user parameters, spawns systems at a location or attached to an actor, sets typed user parameters on live components, and pauses/activates/destroys them. Spawning needs a rendering editor (Niagara refuses under `-nullrhi`); the tool says so. A freshly loaded system compiles for a few seconds first — `system_ready` tells you when it is actually running.
- `niagara_author` builds systems rather than just driving them: create system and emitter assets, add/remove/rename/enable emitters, read the full stack (every emitter's four script stacks with their ordered modules and typed inputs, plus renderers), add and remove modules anywhere in a stack, set literal module input values, and add or remove renderers. `compile` reports each script's status and the actual compile errors — run it after authoring, since that is what surfaces a broken stack. (`ready_to_run` is always false under `-nullrhi`; `compiled` and `errors` are the signal.)
- `metasound_ops` authors MetaSounds. `create` makes a Source or Patch with its interface already wired; `list_node_classes` reports what the project has registered; then add nodes, connect them to each other and to the graph's own inputs and outputs, add the graph inputs the game will drive, and set literal defaults on unconnected pins. Everything goes through the engine's builder API, which is the only path that keeps the document, the frontend registry and the editor graph in step. `sound_cue_ops` remains the tool for the older Sound Cue graph.
- `movie_render` finishes the cinematic pipeline `sequence_ops` starts: create a render config (a render pass plus an output type — a config missing either produces no files), tune it with `set_property` on the reported setting paths (output directory, resolution, frame range, codec), then render a sequence on a map and poll for progress. The render runs through PIE, so it needs a windowed editor.
- `chaos_ops` is destruction: build a Geometry Collection from Static Meshes, then fracture it. Each fracture splits the selected bones into children, so fracturing twice gives the two-level cluster the solver breaks apart at runtime, and `info` reports the bones per level. `fracture_uniform` scatters Voronoi sites for you, `fracture_voronoi` takes sites you place, and `fracture_planar` slices with random planes; all three take grout, surface noise and an island split. Place the result with `spawn_actor` on `GeometryCollectionActor` and point the component's `RestCollection` at the asset.
- `gas_ops` reads an actor's attribute sets (current and base), granted abilities, active effects and owned tags, grants and activates abilities, applies and removes gameplay effects, sets attribute base values and adds loose tags.
- `pcg_ops` authors PCG graphs (create, add nodes from any of the ~200 settings classes, connect pins, save), attaches components to actors, sets graph, seed and parameters, generates asynchronously and reports the result. Per-node options are ordinary properties on the reported `settings_path`, so `set_property` edits them.

### Editor workflow

`localization_ops` covers the Localization Dashboard: list and create targets, manage their supported cultures and which one is native, point `configure_gather` at the sources to collect from, then `gather` source strings into the target's manifest and archives, translate them, and `compile` the result into the `.locres` files the game loads. Translation works either way round: `list_translations` and `set_translation` edit the archive in place (`untranslated_only` is the work queue), while `export_po` and `import_po` hand the same strings to translators as PO files and read them back. Every project already has a `Game` target — the engine ships one in `BaseEditor.ini` — but with no native culture and every gather source disabled, so it collects nothing until `set_native_culture` and `configure_gather` have run; `list_targets` reports both, which is how you can see it. It is one tool over two transports — target and culture edits go to the running editor, while `gather` and `compile` run the `GatherText` commandlet as a subprocess, exactly as the dashboard does, because the engine's in-editor wrappers need a progress dialog and cannot run headless.

`source_control_ops` drives whatever provider the project configured — status per file, check out, mark for add or delete, revert, sync, submit. McpLink writes and saves assets directly, so this is what stops those edits being invisible to a team. `status` is safe to call with source control off; it reports that rather than failing, and every other operation says the same.

`validate_ops` is the pre-ship sweep an agent otherwise has no way to run: Data Validation over named assets or a whole folder, Map Check on the current level, and compiling every Blueprint under a path with the failures named and counted.

`gameplay_tag_ops` lists, adds, renames and removes gameplay tags. They live in the project's tag ini, and only the editor's tag module keeps that ini, the tag manager and the tag sources in step — which is why `config_ops` is the wrong tool for them.

`curve_ops` fills in the Curve assets `asset_ops create` can already make: keys with times, values and per-key interpolation on every channel of a CurveFloat, CurveVector or CurveLinearColor.

`reference_ops` is the Reference Viewer and Size Map as data — what references an asset (what breaks if it goes), what it depends on, and the deduplicated on-disk size of its whole dependency tree with the largest contributors named.

### Contract fixtures

`tests/fixtures/contract/` holds real request/response exchanges captured from a live editor by `tools/capture_contract.py`. Both halves check the same files: the Rust tests assert that each fixture's request deserializes into the owning tool's input type, that the body the tool builds matches it byte for byte, and that the client unwraps every recorded envelope to exactly its `data` or to an error carrying the plugin's own code and status. The plugin's `McpLink.Core.Contract.Fixtures` test asserts each route is still registered and that its responder still produces the recorded envelope. A field renamed on either side fails a test instead of an agent.

### Python escape hatch

`python_exec` (McpLinkPython plugin, which enables the Python Editor Script Plugin) runs code in the editor's interpreter and returns print output, warnings and, in `eval` mode, the expression's value. A Python exception fails the call with the traceback. `persist: true` keeps names between calls. Anything the `unreal` module exposes that has no dedicated tool yet is reachable this way.

### Content authoring

`material_graph` edits a material's node graph: add expressions (constants, parameters, texture samples, math), connect them to each other and to the material inputs (BaseColor, Roughness, Normal, ...), inspect, auto-layout and recompile with error reporting; unnamed channel-mask outputs are picked by index. `material_ops` creates materials and instances and reads/writes instance parameters (validated against the parameter list — UE 5.8's `UMaterialEditingLibrary::SetMaterialInstance*ParameterValue` helpers always return `false`, so their return value is deliberately ignored). `data_table_ops` creates tables for any `FTableRowBase` struct and edits rows with partial updates or CSV import. `input_asset_ops` authors Input Actions and Mapping Contexts. `ism_ops` scatters instanced meshes in bulk.

Character movement tuning needs no dedicated tool: `get_property`/`set_property` on the pawn's `CharacterMovement` component already covers `MaxWalkSpeed`, `JumpZVelocity`, etc.

### Render targets, textures and Material Functions

`render_ops` is the other direction from `capture_viewport`: create a Render Target 2D, clear it, draw a material across it (procedural masks, gradients, noise), read a pixel back to check the result, export it to PNG/EXR/HDR, or bake it into a Texture2D asset — overwriting an existing one in place, which keeps everything referencing it. It also renders an asset's thumbnail, to a PNG file or into the asset's package where the content browser shows it. All of it needs a windowed editor; under `-nullrhi` there is no RHI to draw with, and the tool says so rather than returning black.

`material_layers` builds the layer stack. Material Layers and Material Layer Blends are Material Functions with a usage flag, so `create_layer` and `create_blend` make them — with the MaterialAttributes inputs and layer output the Material Editor would add on first open, since a headless pipeline never opens the asset — and `material_function` authors their bodies. The stack itself lives on a Material, in the Material Attribute Layers node `material_graph add_expression` places, or as a per-instance override on a Material Instance; the same operations edit both. Layer 0 is the background layer and has no blend under it, so every layer above is combined with what is below by its own blend function.

`material_function` authors the reusable sub-graphs a material calls into: create the asset, add expressions (FunctionInput and FunctionOutput are its parameters and results), wire them, lay the graph out, then update to recompile the function and every material using it.

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

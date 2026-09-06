# mcp-unreal
project is vibecoded

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

`pie_control start` also takes the Play settings a networked test needs — `players` (client windows), `net_mode` (`standalone`, `listen_server`, `client`), `dedicated_server` and `one_process` — plus a spawn `location` / `rotation`, so multi-client sessions are reachable by the same tools. `net_emulation` adds latency and packet loss to every connection the session makes (both directions, or `incoming` / `outgoing` separately, aimed at the server, the clients or everyone), which is how to see what replication does on a bad link; `preview` runs the session as the Mobile Preview (`mobile`, the mobile feature level in the PIE viewport) or the VR Preview (`vr`, into a connected headset).

`replay_ops` records and plays back replays inside the PIE session through the Replay Subsystem — the DemoNetDriver behind `demorec` and `demoplay`. `start_recording` writes `Saved/Demos/<name>.replay` until `stop`; `play` travels the PIE world to the recorded map and plays the file, with `goto`, `pause` / `resume` and `set_speed` for scrubbing and `status` reporting the current and total time; `list` and `delete` manage the files. A replay short enough to fit in one stream chunk would otherwise sit on its first frame forever (the driver keeps waiting for data it has already read), so `play` seeks a tenth of a second in when nothing has advanced, after which playback runs normally.

## Tools (116)

| Area | Tools |
|---|---|
| Health | `status` — live-probed features and remediation hints |
| Build (headless) | `build_project`, `generate_project_files`, `world_partition_build` |
| Tests (headless) | `run_tests`, `run_visual_tests`, `list_tests`, `get_test_log` |
| Reflection | `get_property`, `set_property`, `call_function` |
| Actors | `get_level_actors`, `spawn_actor`, `delete_actors`, `move_actor`, `get_actor_components`, `editor_ops` |
| Levels & assets | `level_ops`, `search_assets`, `get_asset_info`, `asset_ops` |
| Editor | `run_console_command`, `get_output_log`, `capture_viewport`, `build_level`, `perf_ops`, `trace_ops`, `visual_log_ops` |
| Play | `pie_control`, `player_control`, `replay_ops`, **`input_inject`** |
| Blueprints | `blueprint_query`, `blueprint_modify`, `blueprint_debug`, `anim_blueprint_query`, `anim_blueprint_modify`, `widget_blueprint_query`, `widget_blueprint_modify` |
| Animation | `anim_asset_ops`, `anim_notify_ops`, `skeleton_ops`, `skeletal_mesh_ops`, `physics_asset_ops`, `pose_asset_ops`, `mirror_table_ops` |
| Content | `material_ops`, `material_graph`, `material_function`, `material_layers`, `material_parameter_collection_ops`, `render_ops`, `texture_info`, `texture_ops`, `media_ops`, `data_table_ops`, `input_asset_ops`, `ism_ops`, `sequence_ops`, `static_mesh_ops`, `sound_cue_ops`, `user_type_ops` |
| World building | `landscape_ops`, `foliage_ops`, `sublevel_ops`, `world_partition_ops`, `level_instance_ops`, `rvt_ops` |
| AI | `blackboard_ops`, `behavior_tree_ops`, `state_tree_ops`, `eqs_ops`, `nav_ops` |
| Editor workflow | `source_control_ops`, `validate_ops`, `gameplay_tag_ops`, `curve_ops`, `reference_ops`, `localization_ops`, `collection_ops`, `editor_utility_ops`, `asset_manager_ops`, `live_coding_ops`, `ddc_ops` |
| Data tables | `data_table_ops`, `string_table_ops`, `curve_table_ops` |
| Introspection | `subsystem_query`, `ui_query`, `ui_ops` |
| Engine API | `lookup_class`, `search_api` |
| Project config | `project_ops`, `config_ops`, `cook_project`, `package_project`, `code_ops` |
| Interop plugins | `niagara_ops`, `niagara_author`, `metasound_ops`, `movie_render`, `chaos_ops`, `gas_ops`, `pcg_ops`, `python_exec`, `toolset_ops`, `geometry_script_ops`, `control_rig_ops`, `level_snapshot_ops`, `ik_rig_ops`, `remote_control_ops`, `live_link_ops`, `pose_search_ops`, `mass_ops`, `chooser_ops`, `gameplay_camera_ops`, `rigvm_graph_ops` (need McpLinkNiagara / McpLinkMetaSound / McpLinkMovieRender / McpLinkChaos / McpLinkGAS / McpLinkPCG / McpLinkPython / McpLinkToolsets / McpLinkGeometryScript / McpLinkControlRig / McpLinkLevelSnapshots / McpLinkIKRig / McpLinkRemoteControl / McpLinkLiveLink / McpLinkPoseSearch / McpLinkMass / McpLinkChooser / McpLinkCameras enabled) |

Headless tools work with no editor open. Editor tools need the Unreal Editor running with McpLink enabled — call `status` to see what is currently available.

`ui_ops` drives the editor's own UI. `find_widgets` locates Slate widgets by driver id or type path — `<SWindow>//<SDockTab>` is every dock tab under any window — and reports each one's text, visibility, whether it can be interacted with and where it is on screen; `click`, `double_click`, `hover`, `focus`, `type`, `press_key` and `scroll` then act on one of them by `index` into that same list. It goes through the engine's AutomationDriver, which has to run off the game thread (its synchronous API blocks on work it posts *to* the game thread), so the response is deferred while a worker drives. It needs a windowed editor: widgets are found through arranged geometry, which `-nullrhi` never produces.

`ui_ops` also opens, closes and lists asset editors, which is worth having for more than the window: some assets are only finished off when their editor constructs them — a freshly created Material Layer gets its input and output nodes that way — so opening one can be the step that completes an asset.

`eqs_ops` authors Environment Queries — how an AI picks where to stand or what to shoot. A query is a list of *options*, each one a **generator** that produces candidate items (a grid of points around the querier, every actor of a class) plus ordered **tests** that score and filter them (distance, line of sight, dot product). The graph is the source of truth: the runtime `Options` array is rebuilt from it, and opening an asset whose graph is missing creates an empty one rather than reconstructing it, so options written directly would be thrown away. The graph classes ship in an editor plugin that exports no symbols, so McpLink reaches them through reflection and virtual dispatch on their exported AIGraph bases instead of linking. Generator and test settings are ordinary properties — `info` reports each one's `object_path` and `set_property` edits it, exactly as the details panel would. `run` executes the query as a chosen actor and reports the scored items best first, which is the only way to see whether a query picks the spots you meant; it needs no PIE, because the editor world carries an AI system of its own.

### Engine API lookup

`lookup_class` and `search_api` read the **installed engine's own C++ headers**, so signatures always match your exact build — no curated docs to drift. The first call indexes ~49k headers (about 25s) and caches the result per engine version; later calls are instant.

```jsonc
lookup_class {"class_name": "ACharacter"}   // parent, module, doc, 42 properties, 49 functions
search_api   {"pattern": "InjectInputForAction", "module": "EnhancedInput"}
```

### Blueprint editing

`blueprint_modify` spawns nodes through `FGraphNodeCreator` and configures them *before* finalizing, so a node comes back with real pins: a `call_function` with the target function's signature, a `make_struct` with one pin per member, a `switch_enum` with one case per enumerator, a `macro` with the macro's tunnel pins. The vocabulary covers calls and parent calls, variable get/set, event overrides, custom events and component-bound events, branch, sequence, select and the four switches, macros from any library (`ForEachLoop`, `DoOnce`, `IsValid`, ... — an unknown name comes back with the list), casts, spawn actor, struct make/break, array/set/map literals, timelines, the delegate family, reroutes and comments. The gameplay-authoring kinds are there too: Enhanced Input action events (one per action, as the editor enforces), legacy key, action and axis events, Construct Object, Add Component by Class and Create Widget with the class pin set so their exposed pins appear, async and latent actions from any factory function (`AsyncLoadPrimaryAsset`, `WaitGameplayEvent` and the other ability tasks, Play Montage) with their delegate outputs, interface messages, Get Data Table Row typed by the table with the row checked, Get Subsystem for world, player, engine and editor subsystems, promotable operators (`Add`, `Multiply`, `Greater`, ... starting wildcard) and Set Fields in Struct. `create` makes a normal Blueprint, a Blueprint Interface, a Function Library or a Macro Library.

Structure is editable too: components on the construction-script tree, function parameters and return values (the return node is created on demand), local variables, interfaces, event dispatchers with their signatures, the parent class, and variable types including containers (`array<int>`, `set<name>`, `map<name,float>`), structs, enums and soft references.

Connections go through the graph schema, so type-incompatible links are refused with a reason rather than silently corrupting the graph. Every edit is undo-able in the editor.

`blueprint_debug` covers breakpoints, watched pins and the instance their values are read from. Watches are the part that works unattended: during PIE a watched pin backed by a class property reports its live value, with nothing stopped — `list_watches` after each step of a scripted play session is a running trace of the Blueprint's state. Breakpoints halt execution, and `halt_status`, `step_into`, `step_over`, `step_out`, `abort` and `resume` drive it from there — the same calls the debugger toolbar makes. A halt parks the game thread in Slate's own debugging loop, which stops `FTSTicker` and would normally take the HTTP server down with it; McpLink keeps answering by ticking the server from Slate's pre-tick instead, so a halted editor is still reachable, windowed or headless. While halted only `blueprint_debug`, `status` and `output_log` are served: every other handler would be re-entering the engine from inside a paused Blueprint's call stack, and is refused with that explanation.

### Animation Blueprints

`anim_blueprint_modify` creates an Animation Blueprint for a skeleton and authors its state machines the way the editor does: `add_state_machine` (wired into the output pose), `add_state` with an animation and entry flag, `add_transition` with crossfade, priority, automatic end-of-animation rules, or a bool variable as the rule. Nested machines work by targeting a state's graph. Every state and transition rule graph is addressable by path (`Locomotion/Idle`, `Locomotion/Walk to Idle`), so `blueprint_modify add_node` / `connect_pins` build arbitrary rules and anim node networks inside them. `anim_blueprint_query inspect` reports the whole structure — states, animations, entry state, transitions and whether each rule is bound.

### Widget Blueprints (UMG)

`widget_blueprint_modify` is the UMG designer as an API, built on the engine's own `FWidgetBlueprintOperationUtils` so the designer's rules apply: `create` (parent class, root panel), `add_widget` under any panel by class short name or Widget Blueprint path, `move_widget`, `remove_widget`, `rename_widget`, `wrap_widget`, `replace_widget`. Appearance goes through `set_widget` and layout through `set_slot`, both taking plain JSON (`{"Text": "Play"}`, `{"LayoutData": {"Offsets": {...}, "Anchors": {...}}}`, `{"Padding": {...}, "HorizontalAlignment": "HAlign_Center"}`) with partial nested-struct updates and unknown keys rejected with the editable list. `bind_event` creates the OnClicked / OnValueChanged / OnTextCommitted event node in the event graph (exposing the widget as a variable first) so `blueprint_modify` can wire the logic. `widget_blueprint_query` lists assets, dumps the tree with slots, reads one widget's current property values in the same JSON shapes, and discovers placeable classes and each class's events and properties.

### Sequencer: Level Sequences and widget animations

`sequence_ops` authors cinematics: `create` a sequence at a chosen display rate and length, `add_binding` to possess a level actor or one of its components (a component binding creates the actor's parent binding automatically), or bind it as a spawnable. Tracks come from `add_track` by short class name ("Transform", "SkeletalAnimation", "CameraCut", "Audio", "Fade") or from `add_property_track`, which picks the track class, channel count and enum or object class from the property itself. `add_key` keys any channel by the name `inspect` reports ("Location.X", or "Value" on a single-channel section), one key or a batch, with linear, constant or auto interpolation; `remove_keys`, `set_channel_default`, `set_section_range`, `add_camera_cut` and `add_marked_frame` fill in the rest. `add_subsequence` nests another Level Sequence on the Subsequences track for its own length or a given range; `add_camera_shake` plays a CameraShakeBase class on a camera binding with a play scale and space; `add_event` fires into the sequence's director Blueprint — a custom event is created there on first use (the bound object becomes a pin on a binding's track), `parameters` add typed pins and `payload` the values the sequence passes in, and the reported `director_blueprint` path is what `blueprint_modify` takes to wire the logic under it. `add_to_level` spawns a LevelSequenceActor so PIE plays the result.

UMG animations are the same machinery, so the same tool authors them: `create_widget_animation` adds one to a Widget Blueprint, `add_widget_binding` binds a widget of that blueprint (or `"Self"`), and every track, section and key operation then works on it — address it as `/Game/UI/WBP_Menu:Anim_FadeIn` wherever `sequence` is taken. `save` writes the owning Widget Blueprint. A widget animation only *advances* where Slate ticks, so a headless editor authors and inspects one but never plays it; play it in a windowed editor with `call_function` `PlayAnimation` on a live widget.

Times cross the wire as display-rate frames or as seconds, interchangeably, and come back as both plus the raw tick. Everything a section exposes as a UPROPERTY — a skeletal animation section's `Params.Animation`, an audio section's `Sound`, easing — is a normal `set_property` on the `path` each operation reports.

### Landscape and foliage

`landscape_ops` creates a landscape at a chosen resolution (quads per section x sections per component x components) and scale, reads and writes heights over any vertex rect, and adds or paints the weightmap layers the landscape material blends. Heights are centimetres of Z relative to the landscape actor in both directions — the uint16 heightmap encoding never crosses the wire — and a sculpt takes either one flat height or one value per vertex, row-major from `min_y`. Every 5.8 landscape is built from edit layers, and `landscape_ops` addresses them: `list_edit_layers`, `add_edit_layer`, `set_edit_layer` (name, visibility, lock, heightmap and weightmap alpha), `reorder_edit_layer`, `clear_edit_layer`, `remove_edit_layer` and `set_editing_layer`, while `set_heights`, `import_heightmap`, `paint_layer` and `apply_splines` take an `edit_layer` to write into (the first layer by default). The layers are merged into the final heightmap on the GPU, so a `-nullrhi` editor keeps a layer write but does not show it in `get_heights` until the level is opened with rendering; headless, an unnamed write goes straight to the final data. Landscape splines are authored point by point — `add_spline_point` with `connect_to` makes the segment, `set_spline_segment` puts a repeated mesh, a paint layer and raise/lower flags on it, `apply_splines` deforms the terrain to them — and `create_grass_type` makes the Landscape Grass Type a material's Grass Output node scatters. Writing heights also rebuilds the collision heightfield, so a line trace (or a foliage scatter) sees the new terrain immediately. Terrain can also come from a file: `create` with `heightmap_file` reads a 16-bit greyscale PNG or a raw/r16 heightmap through the engine's own landscape file formats and picks the component layout that fits it, as the New Landscape panel's import does, and `import_heightmap` writes a file over an existing landscape or a region of one — resampled to fit by default, or corner-aligned or centred with padding.

`foliage_ops` makes foliage types from Static Meshes, then places instances either at explicit transforms or by scattering a count over an area — each point is dropped onto whatever has collision beneath it, with optional alignment to the surface normal, random yaw, and a scale range with a fixed seed for a repeatable result. Instances can be removed inside a sphere or wholesale. Foliage type settings are ordinary properties on the reported `type` path, so `set_property` edits them.

### Sublevels, World Partition and level instances

`sublevel_ops` is the Levels panel: list the streaming levels under the persistent level with their visibility, lock state, actor count and transform; create a new sublevel or add an existing level asset as always-loaded or dynamic; remove one; choose which level new actors spawn into; move a sublevel and its loaded contents in world space; and move existing actors into it.

`world_partition_ops` handles the other kind of world. It reports whether a map is partitioned at all (safe to ask on any map), creates and deletes data layers — making the Data Layer asset when the path is empty — moves actors in and out of them, sets a layer's editor visibility and loading and its initial runtime state, and loads a world-space region so a partitioned map's actors are actually in memory, which they mostly are not when it first opens.

`level_instance_ops` covers the three ways to treat a group of actors as one: a Level Instance, a Packed Level Actor (contents baked into instanced-mesh components), or `merge_actors`, which collapses their meshes into one Static Mesh and leaves the sources alone. `create` needs a windowed editor — `ULevelInstanceSubsystem` hardcodes a Save As dialog for the new level, and a `-nullrhi` editor is refused rather than left wedged on a dialog it cannot draw.

### Asset lifecycle

`asset_ops` is the content browser as an API. `create` makes an empty asset of any class that has a "create new" factory — Data Assets, Curve assets, Curve Tables, String Tables, User-Defined Structs and Enums — and `factory_properties` configures the factory first (a Blueprint's `ParentClass`, a Data Asset's `DataAssetClass`). `import` runs the automated importer on files from disk (FBX, glTF, OBJ, images, audio, CSV) with no dialog, and `reimport` re-runs one from its recorded source or a new file; `interchange: true` sends the files through the Interchange framework instead, with the project's pipeline stack or the `pipelines` given, which is what 5.8's own import dialog does for FBX, glTF, OBJ and USD. `rename`, `move`, `duplicate` and `delete` go through `IAssetTools` so every reference is fixed up, and `fixup_redirectors` clears the redirectors a rename leaves behind. Folders, metadata tags, `export` and `save` / `save_all` round it out. `export` takes a `format` — `gltf` or `glb` (meshes, materials, levels, through the glTF Exporter plugin), `fbx`, `obj`, `t3d`, `png`, `wav` — and picks the exporter registered for that extension with every prompt off, reporting per file what was written and why not; `list_exporters` shows which exporters and extensions an asset's class has.

`texture_ops` authors a texture's bytes with no source file involved: create a Texture2D from base64 BGRA8 pixels or a solid fill, read a rectangle back, write one, or flood-fill one. `create_cube` builds a cube map from six faces and `create_volume` a volume texture from a stack of slices, both from raw BGRA8 or a fill, and `texture_info` reports all three kinds. Masks, gradients, palettes and lookup tables have no file to import, and `asset_ops import` covers the ones that do. Everything works on the editor *source* data, so an edit survives recompression; regions are capped at 256x256 per call, so a large texture is painted in tiles — or drawn in one shot with a material through `render_ops`.

### Editor workflow

`editor_ops` covers what a person does with the mouse: `undo` and `redo` (every McpLink edit is a transaction, and `get_history` names what each would do), reading and setting the **selection**, `attach` / `detach` between actors with a socket, `add_component` / `remove_component` on one actor *instance*, `duplicate` with a repeated offset, outliner `set_folder`, `set_label`, and `snap_to_floor`, which drops actors onto whatever has collision beneath them and reports the ones that had nothing below.

### AI: Behavior Trees and Blackboards

`blackboard_ops` creates a Blackboard and adds, retypes and removes keys — `bool`, `int`, `float`, `string`, `name`, `vector`, `rotator`, `object:<Class>`, `class:<Class>`, `enum:<Enum>` — and can point one at a parent blackboard to inherit keys.

`behavior_tree_ops` authors the tree the way the Behavior Tree editor does: it builds the asset's `BTGraph` (with its Root node) and lets `UBehaviorTreeGraph::UpdateAsset` compile that into the runtime tree, because a runtime tree written directly is discarded the next time the asset is opened. `list_node_classes` enumerates every task, composite, decorator and service the project has, native or Blueprint — start there. Composites and tasks are wired as children; decorators and services attach to a node as sub-nodes. `compile` reports whether the graph actually produced a root, which is the difference between a tree that runs and one that silently does nothing. Node settings are properties on the `instance` path `get_tree` reports.

### Animation assets, skeletons and physics assets

`anim_asset_ops` creates and edits the animation assets that were previously reference-only: Montages (slots, animation segments with trim/rate/loop, named sections and their next-section links), Anim Composites, Blend Spaces (1D and 2D) and Aim Offsets (parameter axes, samples placed in the space). `info` reads the whole structure back. It also runs Animation Modifiers: `list_modifiers` shows the engine's library (motion extraction, root re-orientation, footstep events, curves from sync markers, bone copies, …) and any loaded Blueprint modifier with their defaults, `apply_modifier` adds an instance to a sequence with its properties and applies it, and `revert_modifier` / `remove_modifier` undo it.

`anim_notify_ops` covers notify tracks, notifies and notify states, sync markers and float curves, through the engine's own `UAnimationBlueprintLibrary` — so curve names land on the skeleton and tracks stay valid. A notify's own settings are properties on the reported path, so `set_property` tunes them.

`skeleton_ops` reads the bone hierarchy and edits sockets, virtual bones and montage slot groups; `skeletal_mesh_ops` handles the LOD chain (through the real reduction module), material slots, mesh-only sockets, morph targets and clothing. Either accepts a Skeletal Mesh path where a skeleton is wanted. A morph target is added as deltas over the LOD's source vertices — `morph_target_info` reports the vertex count and reads deltas back — and goes into the mesh description, so the build produces it the way an imported one is produced and it survives every rebuild; `remove_morph_target` is the engine's own removal. `get_skin_weights` and `set_skin_weights` read and write per-vertex bone weights (bones by name, normalized by the engine's own `FBoneWeights`) on the source mesh, so a weight fix survives every rebuild the way an imported weight does. `create_clothing` builds a clothing asset from a section's triangles (the Skeletal Mesh Editor's "Create Clothing Data from Section"), `bind_clothing` and `unbind_clothing` apply it to a section and `list_clothing` shows what is bound where.

`pose_asset_ops` creates Pose Assets — from an animation, one pose per frame under the names given, or empty on a skeleton with its reference pose — and edits them: re-extract from an animation, add the reference pose, rename or delete poses and curves, and switch between full and additive space around a base pose. `mirror_table_ops` creates Mirror Data Tables for a skeleton by running find/replace expressions (Left/Right, `_l`/`_r`, or your own, as prefix, suffix or regular expression) over every bone, notify, curve and sync marker, re-syncs after the skeleton or the expressions change, and sets the mirror axis; the rows are DataTable rows, so `data_table_ops` edits individual entries.

`physics_asset_ops` generates a ragdoll from a skeletal mesh — the same body-and-constraint pass the Physics Asset editor runs on a new asset — then adds and removes individual bodies and constraints. Body and constraint tuning is ordinary `set_property` work on the reported paths.

### State Trees and navigation

`state_tree_ops` authors State Trees, the hierarchical state machine replacing Behavior Trees for a lot of AI and gameplay logic. There is no EdGraph — the editor draws `UStateTreeState` objects directly — so authoring is building that hierarchy: states, transitions, and tasks, enter conditions and evaluators from the structs the project has registered. Then `compile`: an uncompiled State Tree has empty runtime data and runs nothing, and the compile report names whatever the schema rejected.

`nav_ops` asks the built nav mesh the questions that decide whether a level's navigation actually works — project a point onto the mesh, path between two points with waypoints and length, test reachability, raycast for a clear straight walk, pick a random reachable point. `build_level navigation` builds the mesh; a mesh with a hole in it looks exactly like a good one until something tries to walk it.

### Static meshes

`static_mesh_ops` drives `UStaticMeshEditorSubsystem`, so LOD generation runs the real reduction module and `add_convex_collision` runs the real convex decomposition. It reports LODs with vertex counts and screen sizes, material slots, sockets, Nanite settings and collision counts; generates or removes a LOD chain or applies a project LOD group; adds simple (box/sphere/capsule/K-DOP) or convex collision; toggles Nanite and lightmap UV generation; and adds, moves and removes sockets.

### Audio

`sound_cue_ops` builds the node tree a Sound Cue plays: `list_node_classes` for what is available with each node's child capacity, then `add_node` (wave players, random, mixer, modulator, attenuation, concatenator, ...) wired under a parent or as the cue's output, plus `connect`, `set_root` and `remove_node`. Node settings are properties on the reported node path. Sound Classes, Submixes, Attenuation and Concurrency assets need no tool of their own — `asset_ops create` plus `set_property` covers them.

`visual_log_ops` reads the Visual Logger as data. `UE_VLOG` is how engine AI, navigation and movement explain themselves — per-actor, per-frame entries carrying log lines, named status blocks and the shapes the Visual Logger window draws — and this exposes that stream instead of the window: start recording, run PIE, then query by actor, category or game time. Only code that calls `UE_VLOG` shows up, and only during a play session; a long line (Mass logs its whole processor graph in one) is cut with the dropped length reported.

### Builds and performance

`world_partition_build` runs a World Partition builder without the editor open — HLOD generation (`setup`, `build`, `delete`, `finalize`, one layer or all), the minimap, navigation data or an actor resave — through `WorldPartitionBuilderCommandlet`, the way the Build menu and Epic's own build scripts do; it reports the commandlet's errors, its own World Partition log lines and whether the map turned out not to be partitioned. `build_level` runs the editor's Build menu through its own entry point (`FEditorBuildUtils`): static lighting, navigation mesh, BSP geometry, Hierarchical LODs, reflection captures, texture streaming, virtual textures and landscapes. Everything except lighting finishes before the call returns; lighting hands off to a Lightmass process, so poll `get_output_log` for it.

`perf_ops` is the structured readback `stat fps` cannot give, because that draws to the viewport: it samples the real frame time over a window of frames and returns average, median, min, max and p99 in milliseconds, reads process memory, and starts or stops an Unreal Insights trace. `stat_group` reads any `stat <group>` — `GPU`, `SceneRendering`, `Game`, `Memory`, `Physics`, `Niagara` and the rest, an unknown name lists them — as the numbers the overlay would draw: each stat's inclusive and exclusive time in milliseconds, averaged and at its worst over the window, with call counts, counters and memory, and switches the group back off afterwards. `memreport` runs the engine's `MemReport -full` and returns the report it writes (objects by class with counts and sizes, texture and render-target memory, pools, the allocator summary) with its path. `csv_start` / `csv_stop` capture a CSV profile — per-frame timings and counters in the format Unreal's PerfReportTool reads — to `Saved/Profiling/CSV`.

`capture_viewport` has three sources. The default reads the viewport's current frame. `mode: "high_res"` redraws it at `width` x `height` (or `multiplier` times its size) through the editor's own High Resolution Screenshot path, for detail a 1280px capture loses. `mode: "camera"` renders the scene from a camera placed at `location` / `rotation` with a `fov`, through a transient scene capture, so an agent can look at something from where no viewport is pointing. All three need a rendering editor.

`trace_ops` reads those traces back. `list` finds the .utrace files in the shared UnrealTrace store and the project's `Saved/Profiling`; `export` runs UnrealInsights headlessly (`-OpenTraceFile -NoUI -AutoQuit`) against one and exports timers, aggregated timer statistics (instance count with inclusive and exclusive time — where to start when something is slow), individual timing events, threads or counters to CSV, returning the first rows inline. `run` passes arbitrary `TimingInsights.*` commands through for anything `export` does not model.

`package_project` goes past `cook_project` to a runnable build: RunUAT BuildCookRun with compile, cook, stage, pak and archive, plus dedicated-server targets, distribution builds and deploy-and-run on a connected device (Launch On). `cook_project` remains the tool for cooked content alone.

`code_ops` scaffolds C++ without the editor. `create_class` writes the header and source from the engine's *own* class templates (`Engine/Content/Editor/Templates`), so the output matches what the editor's New C++ Class wizard would have produced; the base class can be any reflected class in the installed engine, since its header and ancestry come from the same class index `lookup_class` uses. `create_module` writes a module's `Build.cs`, implementation and folders, then registers it in the `.uproject` and the build targets — the `.uproject` is spliced, not reserialised, so the diff is the new entry and nothing else. Both leave compiling to `generate_project_files` and `build_project`.

### Pipeline and data

`collection_ops` manages content browser collections — list, create (local, private or shared; static or dynamic), destroy, add and remove assets, read a collection's assets — the way a team tags asset sets outside the folder tree. `editor_utility_ops` lists Editor Utility Blueprints and Widgets, runs a utility Blueprint's Run entry point, and opens or closes an Editor Utility Widget in a tab, after which the live widget is reachable with `get_property` and `call_function`. `asset_manager_ops` reads the Asset Manager: primary asset types with their scan paths, the primary assets of a type, an asset's cook and chunk rules, and it creates Primary Asset Labels that tag assets or a whole folder with a cook rule and a pak chunk. `live_coding_ops` reports Live Coding and hot reload state, enables Live Coding for the session, triggers the compile that patches the running editor and waits for its verdict, or runs the older hot reload. `ddc_ops` reports the Derived Data Cache graph in use and each node's hits, misses, bytes and time this session — the numbers behind a slow first load.

`string_table_ops` and `curve_table_ops` cover the two table assets `data_table_ops` does not: a String Table's keys, texts and developer notes (with namespace, CSV import and export), and a Curve Table's rows as keys with interpolation, evaluated at any time, imported from CSV or exported to it.

### Interop plugins: Niagara, MetaSounds, Movie Render Queue, Chaos, Gameplay Abilities, PCG, the Toolset Registry, Geometry Script, Control Rig, Level Snapshots, IK Rig, Remote Control, Live Link, Motion Matching, Mass, Choosers, Gameplay Cameras

Optional sibling plugins add route groups for engine systems a project may or may not use; `status` lists `niagara`, `metasounds`, `movie_render_queue`, `chaos_destruction`, `gameplay_abilities`, `pcg`, `python` and `toolset_registry`, `geometry_script`, `control_rig`, `level_snapshots`, `ik_rig`, `remote_control`, `live_link`, `pose_search`, `mass`, `chooser`, `gameplay_cameras` in `features` when they are loaded.

- `niagara_ops` lists systems (engine templates under `/Niagara`), reads emitters and user parameters, spawns systems at a location or attached to an actor, sets typed user parameters on live components, and pauses/activates/destroys them. Spawning needs a rendering editor (Niagara refuses under `-nullrhi`); the tool says so. A freshly loaded system compiles for a few seconds first — `system_ready` tells you when it is actually running.
- `niagara_author` builds systems rather than just driving them: create system and emitter assets, add/remove/rename/enable emitters, read the full stack (every emitter's four script stacks with their ordered modules and inputs, plus renderers), add and remove modules anywhere in a stack, set module inputs, and add or remove renderers. Inputs are read and written through the Niagara editor's own stack view model, so each one reports what the stack panel would show — a literal value, a linked parameter, a dynamic input with that script's own inputs nested beneath it, a data interface, an expression — and `set_module_input` can give any input, at any depth (`Drag/Minimum`), a literal, a dynamic input such as Random Range Float with its sub-inputs filled in the same call, a link to a parameter, an HLSL expression, a data interface with properties, or a reset to default. `list_input_options` lists what fits a given input. `compile` reports each script's status and the actual compile errors — run it after authoring, since that is what surfaces a broken stack. (`ready_to_run` is always false under `-nullrhi`; `compiled` and `errors` are the signal.)
- `metasound_ops` authors MetaSounds. `create` makes a Source or Patch with its interface already wired; `list_node_classes` reports what the project has registered; then add nodes, connect them to each other and to the graph's own inputs and outputs, add the graph inputs the game will drive, and set literal defaults on unconnected pins. Everything goes through the engine's builder API, which is the only path that keeps the document, the frontend registry and the editor graph in step. `sound_cue_ops` remains the tool for the older Sound Cue graph.
- `movie_render` finishes the cinematic pipeline `sequence_ops` starts, through either of the engine's two pipelines. The legacy one: create a render config (a render pass plus an output type — a config missing either produces no files) and tune it with `set_property` on the reported setting paths. The node-based Movie Render Graph: `create_graph` seeds an asset from the engine's default graph (warm-up, game overrides, global output settings, a deferred pass into a JPG sequence on the default render layer) so it renders as-is, `graph_info` shows every node with its pins, connections and overridable properties, `add_graph_node` / `connect_graph_nodes` / `remove_graph_node` reshape it from the classes `list_graph_node_classes` reports, and `set_graph_node_properties` sets values while switching on their override checkboxes — the thing a bare `set_property` would miss, since an un-overridden value changes nothing at render time. Then render a sequence on a map with either a `config` or a `graph` and poll for progress. The render runs through PIE, so it needs a windowed editor.
- `chaos_ops` is destruction: build a Geometry Collection from Static Meshes, then fracture it. Each fracture splits the selected bones into children, so fracturing twice gives the two-level cluster the solver breaks apart at runtime, and `info` reports the bones per level. `fracture_uniform` scatters Voronoi sites for you, `fracture_voronoi` takes sites you place, and `fracture_planar` slices with random planes; all three take grout, surface noise and an island split. Clustering decides what breaks apart together and in what order: `auto_cluster` groups a bone's children by count, fraction, size or a grid, and `cluster`, `merge_clusters` and `cluster_magnet` shape it by hand from the indices `bones` reports. A fracture leaves its new interior faces wearing the original material and no useful UVs, so `set_interior_material` points them at another slot and `box_project_uvs` or `layout_uvs` gives them coordinates — projection for a tiling material, an atlas for a baked one. `generate_convex` rebuilds the hulls the solver collides with — a fracture already leaves usable ones, so this is for tuning them — and `simplify_convex` trades accuracy for cheaper collision. Place the result with `spawn_actor` on `GeometryCollectionActor` and point the component's `RestCollection` at the asset. Breaking it needs no extra tool: during PIE, `call_function` on the component reaches `ApplyExternalStrain`, `ApplyInternalStrain` and `CrumbleCluster`, and a `FieldSystemActor` spawned the same way takes `ApplyStrainField` and `ApplyRadialForce`.
- `gas_ops` reads an actor's attribute sets (current and base), granted abilities, active effects and owned tags, grants and activates abilities, applies and removes gameplay effects, sets attribute base values and adds loose tags.
- `pcg_ops` authors PCG graphs (create, add nodes from any of the ~200 settings classes, connect pins, save), attaches components to actors, sets graph, seed and parameters, generates asynchronously and reports the result. Per-node options are ordinary properties on the reported `settings_path`, so `set_property` edits them.
- `toolset_ops` bridges the engine's own agent surface. UE 5.8 ships an experimental **Toolset Registry**: an editor subsystem holding every AI-callable toolset the editor has registered — Epic's own (Dataflow graphs, Chaos Cloth assets, Game Feature and ordinary plugins, MVVM, Data Registries, World Conditions, Conversations, Live Coding, semantic asset search, gameplay cues, config settings with schemas, a Slate driver, Sequencer and Control Rig, the editor camera and content browser; 52 toolsets and some 830 C++ and Python tools with `AllToolsets` enabled), any toolset a project defines as a `UToolsetDefinition` subclass or a Python toolset, and Agent Skill assets — Epic ships twenty of its own (Blueprint, material and Niagara guidance) and a project can author more. `list_toolsets` and `list_tools` show what is registered, `get_tool` reports one tool's argument schema exactly as the registry generated it from the UFunction, and `execute` runs it and returns its converted return value; `list_skills` and `get_skill` read the skill assets. Tools are named `<Toolset>.<Tool>` with a dotted toolset name (`EditorToolset.EditorAppToolset.GetCameraTransform`, `animation_toolset.toolsets.sequencer.SequencerTools.create_camera`), and a bare tool name resolves when only one toolset defines it. The registry's own block and allow lists apply, and each tool manages its own transaction. The bridge needs the McpLinkToolsets plugin, which enables the engine's `ToolsetRegistry`; Epic's toolsets come from its `AllToolsets` plugin or the individual `*Toolset` plugins, all off by default.
- `geometry_script_ops` turns Geometry Script into an API. `new_mesh` makes a dynamic mesh; `call` runs any function of the `UGeometryScriptLibrary_*` classes on it by name — primitives, booleans, extrusions, remeshing, simplification, UVs, normals, the lot — with JSON arguments (structs as objects, enums by name, other meshes by path), so a whole modeling recipe is a sequence of calls; `list_functions` and `signature` find the function and its parameters; `mesh_info` reports counts and bounds; `to_static_mesh` writes a new Static Mesh asset or an existing one's LOD, `from_static_mesh` reads one in, and `release` drops the mesh.
- `control_rig_ops` authors Control Rigs and drives them. `create` makes the rig Blueprint with a skeleton's bones imported; `add_bone`, `add_null` and `add_control` (every control type, with parent, offset, shape and initial value) build the hierarchy, `set_control_value` and `remove_element` edit it, and `compile` runs the Blueprint compiler — the rig's graph itself (RigVM nodes) is not authored here. `add_track` puts a Control Rig track for the rig on a sequence binding or an actor, with its first section, and `set_control_key` keys a control at a time or frame; `list_tracks` reads the tracks and their controls back. Also the fix for the earlier hole where `sequence_ops` could add a Control Rig track but never set its rig.
- `level_snapshot_ops` wraps Level Snapshots: `take` captures every actor's properties into a snapshot asset, `diff` lists what has changed, been removed or been added since, `apply` restores the level to it (undo-able), and `info` and `save` do what they say. Headless-safe, and the natural bookend around a batch of world edits an agent may want to roll back.
- `ik_rig_ops` is IK Rig and the IK Retargeter. `create_rig` makes an IK Rig for a skeletal mesh and `auto_retarget` characterizes a humanoid skeleton (the retarget root and every chain, as the editor's button does); `add_solver` (Full Body IK, Limb, Pole, Body Mover, Stretch Limb, Set Transform), `add_goal`, `set_retarget_root`, `add_chain` and `remove_chain` do it by hand; `create_retargeter` pairs a source rig with a target rig, seeds the 5.8 op stack and maps chains by closest name, `map_chain`, `auto_map` and `auto_align` refine the mapping and the retarget poses, and `retarget_animations` runs the Retarget Animations batch — every animation duplicated (into a folder, renamed by prefix, suffix or search-replace) and baked onto the target skeleton.
- `remote_control_ops` is Remote Control presets — the asset the Remote Control web app, REST API and protocol bindings serve. `create` makes an empty preset, `expose_property` (nested paths like `RelativeLocation.X` work), `expose_function` and `expose_actor` put an object's fields, callables or a whole actor under a label, `rename` and `unexpose` manage the labels, `info` lists every exposed entity with its id and bound objects, and `save` writes the asset.
- `live_link_ops` is the Live Link client. `status` lists sources and subjects, `list_source_types` the factories, roles and virtual subject classes; `add_source` builds a source from a factory's connection string (what a preset stores — a Message Bus source still needs a provider address only the picker discovers), `add_virtual_source` and `add_virtual_subject` make virtual subjects, `set_subject_enabled` toggles one, `save_preset` captures the client into a Live Link Preset asset and `apply_preset` restores it (additive, or `replace`). Presets holding a virtual source crash the 5.8 client on apply, so the route applies a copy without them and recreates the virtual sources and subjects itself.
- `pose_search_ops` is Motion Matching. `create_schema` makes a Pose Search Schema for a skeleton (with a mirror table and role) seeded with the editor's default pose and trajectory channels, `add_channel` / `set_channel` / `remove_channel` edit feature channels by class (`Position`, `Velocity`, `Heading`, `Phase`, `Curve`, …) with their editable properties, `create_database` makes a database on a schema, `add_animation` / `set_animation` / `remove_animation` manage entries (mirroring, sampling range, reselection), `build_index` runs the derived-data index build and reports the pose count, and `save_schema` / `save_database` write them.
- `mass_ops` is Mass Entity. `create_config` makes a Mass Entity Config (optionally inheriting a parent), `add_trait` / `set_trait` / `remove_trait` edit its traits by class with JSON properties (`MassAssortedFragmentsTrait` takes `{"_structType": "/Script/MassCore.TransformFragment"}` entries), `validate_config` builds the template; `create_spawner` / `configure_spawner` place a Mass Spawner with entity types and spawn data generators — the EQS and Zone Graph ones, or McpLink's `McpMassRadiusSpawnGenerator`, which scatters within a radius or on explicit points and needs no query — and `do_spawning` / `do_despawning` run it in PIE, with `entity_stats` counting what lives in the world.
- `chooser_ops` is Chooser tables. `create` makes a table with its result type (object, class or outputs only), result class and context parameters (classes or structs); `add_column` adds a filter, scoring, output or randomize column by struct name (`FloatRangeColumn`, `BoolColumn`, `EnumColumn`, `GameplayTagColumn`, `OutputFloatColumn`, `RandomizeColumn`, …) bound to a context property by name chain; `add_row` / `set_row` / `remove_row` manage rows with an asset, class, nested-chooser or custom result and one cell per column; `set_fallback` sets the no-match result; `evaluate` runs the table against live actors, objects or structs (output columns write back into the structs); `info` dumps everything and `save` writes it.
- `gameplay_camera_ops` is Gameplay Cameras. `create_rig` makes a Camera Rig and `add_node` grows its node tree (`Array` as a sequence root, then `AttachToPlayerPawn`, `BoomArm`, `Offset`, `FieldOfView`, input bindings, blends, …) with camera parameters given as literals or variable bindings; `set_node` / `remove_node` edit it; `create_camera` pairs a Camera asset with a director (Single by default) and a rig; `build_rig` / `build_camera` run the headless build with its log; `save` writes an asset (which builds it); and `activate` spawns a Gameplay Camera actor running the camera as the player's view target in PIE, `deactivate` hands the view back.
- `rigvm_graph_ops` is Control Rig graph authoring. `list_node_types` lists unit structs and template notations (filtered), `list_graphs` / `list_nodes` / `node_info` read a rig's graphs with pins and links, `add_node` places a unit node by struct, a template node by notation or name (wildcard pins resolve when linked), a variable getter or setter, a comment, or a branch / if / select, with pin defaults; `link_pins` / `unlink_pins` / `break_all_links` wire it (execute pins included — a new rig gets its Forwards Solve event on the first node), `set_pin_default` writes RigVM text, `add_variable` adds a rig variable and `compile` recompiles the VM with its log.
- `anim_blueprint_modify` also does linked anim layers: `create_layer_interface` makes an Animation Layer Interface declaring layers (with a group), `add_layer` adds one, `implement_layer_interface` gives an Animation Blueprint a graph per layer (addressed by the layer name in every `graph` field, so state machines and nodes go straight in), and `add_linked_layer_node` / `add_linked_graph_node` run a layer or another Animation Blueprint's class inside a graph; at runtime `call_function` `LinkAnimClassLayers` on the anim instance swaps the implementation.

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

`rvt_ops` covers Runtime Virtual Textures end to end: create the asset (its tile count, tile size, border and material type as properties), spawn the volume that renders it and size it to a landscape or to everything assigned, `assign` primitives or a landscape so they render into it (the same list the details panel's Runtime Virtual Textures array holds), list the volumes in a world, and bake a volume's streaming mips into its Streaming Texture in a windowed editor.

`media_ops` is the Media Framework: File Media Sources (a file under Content is stored relative, so the asset survives a project move), Media Players, the Media Texture a material samples, and Playlists; `open` starts a source, file or URL asynchronously and `status` reports ready, playing, time, duration and tracks, with `play`, `pause`, `seek`, `rewind`, `set_rate`, `set_looping` and `close` for control. Playback control needs a media player that can render — headless with `-nosound` the Windows Media Foundation player opens the file but refuses to play.

`material_parameter_collection_ops` covers Material Parameter Collections, the global scalars and vectors a material reads without an instance: create the asset, add or edit parameters and their defaults (adding or removing one changes the collection's layout, so every material reading it is recompiled and gets a new state id), rename or remove them, and read or set the runtime values on a world's instance — the editor world or the PIE world — which is what a Blueprint's SetScalarParameterValue writes and is never saved.

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
claude mcp add mcp-unreal -- "C:\Tools\mcp-unreal\mcp-unreal.exe"
```

**Quote the path.** In a POSIX shell — Git Bash, WSL, the Bash tool — an unquoted `\` is an escape character, so the backslashes are eaten and the entry is silently recorded as `C:Toolsmcp-unrealmcp-unreal.exe`. Forward slashes work everywhere and dodge the problem entirely. Confirm what was actually recorded with `claude mcp get mcp-unreal`.

Or copy `.mcp.json.example` to `.mcp.json` and edit the paths.

**3. Check it.** Open your project in the Unreal Editor and call `status` — it reports the detected engine, the project, whether the plugin is reachable and what is currently available.

If `status` says the engine root is a *fallback guess*, discovery found no install: it looks in the Epic Launcher's manifest and the conventional locations (including drive roots, so `D:\UE_5.8` is found), but a source build or an install somewhere else needs `UE_ENGINE_ROOT` set to the folder containing `Engine/`. `engine_installs_found` lists everything discovery did see.

| Env var | Default | Meaning |
|---|---|---|
| `UE_ENGINE_ROOT` | auto-detected | Engine install root; found via the Epic Launcher manifest and the usual install locations |
| `UE_EDITOR_PATH` | derived | Override the UnrealEditor-Cmd path |
| `MCP_UNREAL_PROJECT` | walk up from cwd | `.uproject` file or its folder |
| `PLUGIN_PORT` | `8091` | McpLink HTTP port (CVar `McpLink.Port` in the editor) |
| `MCP_UNREAL_LOG_LEVEL` | `info` | tracing filter (stderr only) |
| `MCP_UNREAL_AUTO_UPDATE` | `apply` | `apply`, `check` (report only) or `off` (no network) |
| `MCP_UNREAL_UPDATE_TOKEN` | unset | GitHub token, only needed for a private fork |

## Staying up to date

The server keeps itself and the plugin current. It checks the releases page once a day in the background, and installs the new version the next time it starts **with the editor closed** — Windows keeps a loaded DLL locked, so that is the only moment the plugin can be replaced. The update takes effect on the following start; nothing changes underneath a running session.

Both halves move together or neither does. They are one contract — a server that runs ahead of its plugin fails as a 404 on a route it is sure exists — so if the plugin cannot be replaced, the server is left alone too and `status` says why. It refuses when:

- the plugin folder is a **junction or symlink** (how `tools/setup-dev.ps1` wires a source checkout — an update must never write through one),
- the plugin has **no `Binaries/`**, meaning it was compiled rather than installed from a release,
- the engine is not the line the release was built for, or the host is not Windows (the prebuilt plugin is Windows-only).

Downloads are checked against the sha256 GitHub records for each release asset, and only the plugin folders you already have are replaced — a release carrying more interop plugins does not install the rest.

Set `MCP_UNREAL_AUTO_UPDATE=check` to be told about new versions without anything being written, or `off` to make no network requests at all. Either way `status` reports what it knows under `update`, and it flags a server/plugin version mismatch whether or not the updater is on.

## Security

McpLink is a **development tool that gives an agent full control of your editor**, so treat it accordingly:

- The plugin listens on loopback only (UE's HTTP server defaults to `BindAddress=localhost`) and has **no authentication**. Any process on your machine can drive the editor while it is running — and with `McpLinkPython` enabled, that means running arbitrary Python inside the editor. Don't enable it on a shared machine, and don't set `[HTTPServer.Listeners] DefaultBindAddress=any` in a project that has McpLink.
- An agent can delete actors, overwrite assets and save packages. **Use source control** on any project you point it at.
- Auto-update **downloads and installs code from the releases page without asking**, into your project's `Plugins/` and over the server binary. Each asset is checked against the sha256 GitHub recorded for it, which catches a corrupted download but is not a signature — it is only as trustworthy as the release itself. `MCP_UNREAL_AUTO_UPDATE=off` turns it off entirely.
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

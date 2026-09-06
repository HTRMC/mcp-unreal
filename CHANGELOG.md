# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[semantic versioning](https://semver.org/spec/v2.0.0.html).

The server (`mcp-unreal`) and the editor plugin (`McpLink`) are versioned and
released together: a release's server archive and plugin zip are meant to be
installed as a pair.

## [Unreleased]

## [0.3.1] - 2026-09-06

### Fixed

- **`status` no longer reports a finished update as pending.** After the
  updater had swapped the binary and the plugin, the next start kept
  reporting "installed, takes effect on the next start" — and a release
  reported as available stayed reported after being installed by hand —
  until the next daily check. A recorded state naming a version the running
  server already is now settles to `up_to_date` at startup.
- **The updater leaves no empty `staged/` folder behind** once an install is
  done.

## [0.3.0] - 2026-09-06

### Added

- **`chooser_ops` (McpLinkChooser).** Chooser tables: signature, columns by
  struct bound to context properties, rows with results and cells, the
  fallback, evaluation against live objects or structs, and saving.
- **`gameplay_camera_ops` (McpLinkCameras).** Camera Rigs as node trees,
  Camera assets with a director, the headless build and its log, and
  activation on the player in PIE.
- **`rigvm_graph_ops` (McpLinkControlRig).** Control Rig graph authoring:
  unit, template, variable, comment, branch/if/select nodes, links, pin
  defaults, rig variables and the VM compile log.
- **`anim_blueprint_modify` linked anim layers.** Animation Layer
  Interfaces with layers and groups, implementing them on an Animation
  Blueprint, and Linked Anim Layer / Linked Anim Graph nodes; layer graphs
  are addressable in every `graph` field.
- **`remote_control_ops` (McpLinkRemoteControl).** Remote Control presets:
  create, expose properties, functions and actors under labels, rename,
  unexpose, list and save.
- **`live_link_ops` (McpLinkLiveLink).** Live Link sources and subjects,
  sources from a factory's connection string, virtual sources and virtual
  subjects, and Live Link Presets saved from or applied to the client —
  virtual sources are recreated by the route, since the 5.8 client
  check()-fails recreating one from a preset.
- **`pose_search_ops` (McpLinkPoseSearch).** Motion Matching: Pose Search
  Schemas with feature channels edited by class and properties, Pose
  Search Databases with animation entries, the derived-data index build
  and saving.
- **`mass_ops` (McpLinkMass).** Mass Entity Configs with traits (JSON
  properties, `_structType` for instanced-struct fragments), Mass Spawners
  with entity types and spawn data generators including a built-in
  radius/points generator, spawning and despawning in PIE, entity counts.
- **`anim_asset_ops` runs Animation Modifiers.** `list_modifiers`,
  `apply_modifier` (instance on the sequence's asset user data, properties
  set, applied), `list_applied_modifiers`, `revert_modifier`,
  `remove_modifier`.
- **`skeletal_mesh_ops` edits skin weights.** `get_skin_weights` and
  `set_skin_weights` on the source mesh description (default weights or a
  named profile), rebuilt into render data.
- **`ik_rig_ops` (McpLinkIKRig).** IK Rigs for a skeletal mesh with humanoid
  auto-characterization or hand-built solvers, goals, retarget root and
  chains; IK Retargeters with the 5.8 op stack, chain mapping and pose
  alignment; and the Retarget Animations batch, which duplicates and bakes
  animations onto the target skeleton.
- **`world_partition_build` (headless).** HLOD setup/build/delete/finalize,
  minimap, navigation data and actor resaves through
  `WorldPartitionBuilderCommandlet`.
- **`geometry_script_ops` (McpLinkGeometryScript).** Geometry Script as an
  API: dynamic meshes in the transient package, every
  `UGeometryScriptLibrary_*` function callable by name through reflection
  (`list_functions` and `signature` to find them), counts and bounds, and
  `to_static_mesh` / `from_static_mesh` for the way in and out of assets.
- **`control_rig_ops` (McpLinkControlRig).** Control Rig Blueprints from a
  skeleton, hierarchy authoring (bones, nulls, controls of every type with
  shapes, offsets and initial values, removal, value setting), compile, and
  Sequencer: a Control Rig track on a binding or actor, and keys on its
  controls by time or frame.
- **`level_snapshot_ops` (McpLinkLevelSnapshots).** Take a Level Snapshot
  asset of the open level, diff the level against it (changed, removed,
  added actors) and restore it.
- **`asset_ops import` through Interchange.** `interchange: true` runs the
  Interchange framework with the project's pipeline stack (or `pipelines`),
  asynchronously with the reply waiting, and reports every asset it made.
- **`call_function` argument conversion moved to McpLinkCore.**
  `CallFunctionFromJson` and `FunctionSignatureJson` (McpReflection.h) are
  shared with the Geometry Script bridge; a failed argument now cancels the
  transaction.
- **`pose_asset_ops` and `mirror_table_ops`.** Pose Assets from an animation
  (a pose per frame) or a skeleton, with renaming, deletion, re-extraction
  and full/additive conversion; Mirror Data Tables synced from find/replace
  expressions over a skeleton, with axis and expression edits.
- **`skeletal_mesh_ops` edits morph targets and clothing.** `add_morph_target`
  writes deltas over the LOD's source vertices into the mesh description so
  the build produces the morph target (a hand-registered `UMorphTarget` is
  discarded by the next build); `morph_target_info` reads them back;
  `remove_morph_target`. `create_clothing` builds a clothing asset from a
  section, `bind_clothing` / `unbind_clothing` apply it (keeping the section
  user data the build reads, as Persona does), `list_clothing`,
  `remove_clothing`.
- **`landscape_ops` edit layers, splines and grass types.** List, add, rename,
  show/hide, lock, reorder, clear and remove edit layers; `edit_layer` on the
  height and weight writes; spline control points and segments with meshes,
  paint layers and raise/lower flags, `apply_splines`; `create_grass_type`.
- **`rvt_ops`.** Runtime Virtual Texture assets, volumes sized to a landscape
  or to what is assigned, `assign` / `unassign` for primitives and landscapes,
  `list`, and streaming-mip baking in a windowed editor.
- **`media_ops`.** File Media Sources, Media Players, Media Textures and
  Playlists; open, play, pause, seek, rewind, rate, looping, close and status.
- **`material_parameter_collection_ops`.** Material Parameter Collections:
  create, add or edit scalar and vector parameters and their defaults
  (through the same edit bracket the details panel uses, so materials that
  read the collection are recompiled and its state id changes), rename and
  remove, and read or set the runtime values on the editor or PIE world's
  instance.
- **`replay_ops`.** Replay recording and playback in PIE through the Replay
  Subsystem: `start_recording` / `stop`, `play`, `goto`, `pause`, `resume`,
  `set_speed`, `status`, and `list` / `delete` for the `.replay` files under
  `Saved/Demos`. A replay that fits in one stream chunk stalls on its first
  frame in the engine; `play` nudges it with a short seek.
- **`texture_ops create_cube` and `create_volume`.** Cube maps from six BGRA8
  faces and volume textures from a stack of slices, or a solid fill;
  `texture_info` and `texture_ops save` now take any texture class.
- **`asset_ops export` takes a `format`.** The extension picks the exporter —
  `gltf` / `glb` through the glTF Exporter plugin, `fbx`, `obj`, `t3d`,
  `png`, `wav`, … — and the result lists each file written or the exporter's
  errors; `list_exporters` shows what an asset's class can be exported as.
- **Seven pipeline and data tools.** `collection_ops` (content browser
  collections), `editor_utility_ops` (run Editor Utility Blueprints, open
  and close Editor Utility Widgets), `asset_manager_ops` (primary asset
  types, ids and rules; Primary Asset Label creation), `live_coding_ops`
  (Live Coding status, enable, compile-and-patch; hot reload), `ddc_ops`
  (Derived Data Cache graph and per-node usage), `string_table_ops` (String
  Table keys, texts, notes, namespace, CSV) and `curve_table_ops` (Curve
  Table rows as keys with interpolation, evaluation, CSV).
- **`movie_render` authors Movie Render Graphs.** `create_graph` makes the
  node-based pipeline's asset, seeded from the engine's default graph so it
  renders as-is (or empty); `list_graph_node_classes`, `graph_info` (nodes,
  pins and connections as "Node.Pin", every overridable property with its
  value and override state), `add_graph_node`, `remove_graph_node`,
  `connect_graph_nodes` / `disconnect_graph_nodes` build it, and
  `set_graph_node_properties` sets values and switches on their override
  checkboxes in one step, dynamic properties included. `render` and `save`
  take a `graph` as well as the legacy `config`.
- **`sequence_ops` nests, shakes and fires events.** `add_subsequence` puts
  another Level Sequence on the Subsequences track (root or on a binding)
  for its own length or a given range; `add_camera_shake` plays a
  CameraShakeBase class on a camera binding with play scale and space;
  `add_event` creates a custom event in the sequence's director Blueprint
  (made on first use, the way Sequencer does it) and keys a trigger at a
  frame — or a repeater over a range — that calls it, with `parameters`
  becoming the event's pins and `payload` the values passed in; a binding's
  event carries the bound object as a pin. The reported `director_blueprint`
  is addressable by `blueprint_modify`, which is how the event's logic gets
  wired.
- **`perf_ops` reads the engine's own profilers.** `stat_group` returns any
  `stat <group>` — GPU, SceneRendering, Game, Memory, Physics, Niagara and
  the rest — as data: each stat's inclusive and exclusive time in
  milliseconds, averaged and at its worst over a frame window, with call
  counts, counters and memory, switching the group back off afterwards; an
  unknown group lists them. `memreport` runs `MemReport -full` and returns
  the report it writes with its path. `csv_start` / `csv_stop` / `csv_status`
  drive the CSV profiler.
- **`capture_viewport` renders more than the viewport.** `mode: "high_res"`
  redraws the view at any size through the editor's High Resolution
  Screenshot path; `mode: "camera"` renders the scene from a camera placed
  at a `location` / `rotation` with a `fov` through a transient scene
  capture, no viewport needed.
- **`pie_control start` emulates a bad network and runs previews.**
  `net_emulation` adds latency and packet loss to every connection the
  session makes (both directions, or `incoming` / `outgoing` separately;
  aimed at the server, the clients or everyone); `preview` runs the session
  as the Mobile Preview or the VR Preview.
- **`landscape_ops` imports heightmap files.** `create` takes a
  `heightmap_file` (16-bit PNG, raw or r16) and picks the component layout
  that fits it, as the New Landscape panel does; `import_heightmap` writes a
  file over an existing landscape or a region, resampled, corner-aligned or
  centred.
- **`blueprint_modify` authors the rest of a gameplay graph.** `create`
  takes a `blueprint_type`: a Blueprint Interface (functions added with
  `add_function`, implemented elsewhere with `add_interface`), a Function
  Library or a Macro Library, each with the parent its editor factory fixes.
  `add_node` gains the kinds real gameplay needs: `enhanced_input_action`
  (one node per action, as the editor enforces), the legacy `input_key`
  (with modifiers), `input_action` and `input_axis` events,
  `construct_object`, `add_component_by_class` and `create_widget` with the
  class pin set so their exposed pins appear, `async_action` from any
  factory function — Blueprint async actions, gameplay and ability tasks
  such as Wait Gameplay Event, and Play Montage (also `play_montage`) — with
  their delegate outputs, `interface_message`, `get_data_table_row` typed by
  the table with the row name checked, `get_subsystem` picking the world,
  player-controller, engine or editor node for the class, `operator` for the
  promotable operators (starting wildcard, listing the operations on an
  unknown name) and `set_fields_in_struct`.
- **`toolset_ops` bridges the engine's own agent surface.** UE 5.8 ships an
  experimental Toolset Registry — an editor subsystem holding every
  AI-callable toolset the editor has registered: Epic's own (Dataflow graphs,
  Chaos Cloth assets, Game Feature and ordinary plugins, MVVM, Data
  Registries, World Conditions, Conversations, Live Coding, semantic asset
  search, gameplay cues, config settings with schemas, a Slate driver,
  Sequencer and Control Rig, the editor camera and content browser — 52
  toolsets and some 830 C++ and Python tools with `AllToolsets` enabled),
  any toolset a project defines as a `UToolsetDefinition` subclass or a
  Python toolset, and Agent Skill assets, Epic's twenty and a project's own.
  The new `McpLinkToolsets` sibling plugin exposes it as one route:
  `list_toolsets` and `list_tools` show what is registered (and what the
  project's block and allow lists hide), `get_tool` reports a tool's
  argument schema exactly as the registry generated it from the UFunction,
  `execute` runs a tool — by full `<Toolset>.<Tool>` name, or by bare name
  when only one toolset defines it — and returns its converted return value,
  waiting on asynchronous tools for up to `timeout_secs`, and `list_skills`
  / `get_skill` read the skill assets. `status` lists `toolset_registry` in
  `features` when the plugin is loaded.
- **`niagara_author` reads and writes module inputs through the Niagara
  editor's own stack view model.** Inputs a template had handed to a dynamic
  input — an emitter's drag, a spawn-speed range — were locked: the binder the
  tool used refuses any input whose override is linked, so those stayed at
  their template defaults. `stack` and the new `get_module` now report each
  input the way the stack panel shows it: its `mode` (a literal `value`, a
  `linked_parameter`, a `dynamic_input` with that script's own inputs nested
  beneath it, a data interface, an object asset, an expression, or the
  module's default function), plus `can_reset`, enum options, static
  switches and edit conditions. `set_module_input` addresses inputs at any
  depth by path (`Drag/Minimum`) and can give one a literal, a dynamic input
  such as Random Range Float with its sub-inputs set in the same call
  (`inputs`), a `link` to a parameter (a new `User.` name becomes an exposed
  user parameter), an HLSL `expression`, a `data_interface` class with
  `properties`, an `object_asset`, a `reset` to the default, or an
  `edit_condition_enabled` toggle — replacing whatever drove the input
  before, as typing into the panel would. `list_input_options` lists, for one
  input, the dynamic inputs and parameters that fit its type, the namespaces
  a new parameter may go in and the data-interface classes it accepts;
  `list_dynamic_inputs` lists every dynamic input script with the type it
  produces.

## [0.2.0] - 2026-09-05

### Added

- **Auto-update.** The server checks the releases page once a day in the
  background and installs a newer release the next time it starts with the
  editor closed, which is the only window in which the plugin's DLLs are not
  locked. The update takes effect on the start after that: an MCP client owns
  the server's stdio, so exiting to re-exec would read as a crash.

  The server and the plugin move together or not at all. They are one contract,
  so a server that ran ahead of its plugin would fail as a 404 on a route it is
  sure exists — the breakage that updating by hand avoids, since whoever
  downloads by hand takes both from the same release. When the plugin cannot be
  replaced the server is left alone too, and `status` says why: the plugin
  folder is a junction into a source checkout, it has no `Binaries/` and was
  therefore compiled rather than installed, the engine is not the line the
  release was built for, or the host is not Windows.

  Assets are verified against the sha256 GitHub records for them. Only plugin
  folders that are already installed are replaced, so a release carrying more
  interop plugins does not install the rest. `MCP_UNREAL_AUTO_UPDATE=check`
  reports without writing anything, `off` makes no network requests at all, and
  `status` reports the outcome under `update`.

- `status` now reports a **server/plugin version mismatch** as a hint. The pair
  ships from one release, and a mismatched pair fails as a missing route rather
  than as anything that names a version.

- **`eqs_ops`** — Environment Query System authoring: create a query, add
  options with a generator, add, reorder-by-adding, disable and remove their
  tests, compile and save. This was previously written off as impossible
  because the EQS graph is the source of truth (a query whose options are
  written directly is discarded the next time the asset is opened) and the
  graph classes live in an editor plugin that exports no symbols to link
  against. They do not need to be linked against: their UClasses are in the
  reflection system, and every entry point that matters is a virtual override
  of an exported AIGraph base, so `NewObject` plus virtual dispatch builds the
  real graph and `UpdateAsset` compiles it. Generator and test settings are
  ordinary properties, edited through `set_property` on the paths `info`
  reports. `run` then executes the query as a chosen actor and reports the
  scored items best first — the check that a query actually picks the spots
  it was meant to. It needs no PIE: the editor world carries an AI system,
  which is where the query manager lives.
- **Editor UI automation.** `ui_ops` gained `find_widgets`, `click`,
  `double_click`, `hover`, `focus`, `type`, `press_key` and `scroll`, so the
  editor's own interface is drivable and not only readable. This was previously
  written off because the AutomationDriver asserted on the first call and took
  the editor with it — that turned out to be the engine's path parser choking on
  a malformed selector of ours (a leading `/` indexes an empty array), not a
  headless limitation, so malformed paths are now refused with an explanation
  instead. The driver runs on a worker thread, since its synchronous API blocks
  on work it posts to the game thread. It needs a windowed editor: widgets are
  located through arranged geometry, which `-nullrhi` never produces.
- **Blueprint stepping.** `blueprint_debug` gained `halt_status`, `resume`,
  `step_into`, `step_over`, `step_out` and `abort`, so a breakpoint is now
  something an agent can actually drive rather than only set. A halt parks the
  game thread in Slate's intra-frame debugging loop, which never returns to
  `FEngineLoop` — `FTSTicker` stops, and the HTTP server ticks on it, so the
  request that would resume execution could not be delivered. That loop does
  keep ticking Slate, so the server is now pumped from
  `FSlateApplication::OnPreTick` and a halted editor stays answerable. It works
  headless as well, so arming a breakpoint is no longer refused there and the
  `force` flag is gone. While halted only `blueprint_debug`, `status` and
  `output_log` are served; anything else is refused, because it would be
  re-entering the engine from inside a paused Blueprint's call stack.

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
- **`ui_ops`** — open, close and list asset editors. Opening one is sometimes
  the step that finishes an asset, because part of the work lives in the
  editor's own construction. Widget driving is covered under Editor UI
  automation above.
- **`visual_log_ops`** — the Visual Logger, read back as data rather than drawn
  on a timeline: record a play session and query the engine's own `UE_VLOG`
  entries by actor, category or game time, with their log lines, status blocks
  and shapes.
- **`texture_ops`** — import-free texture authoring: create a Texture2D from
  base64 BGRA8 pixels or a solid fill, and read, write or flood-fill a
  rectangle of an existing one. Masks, gradients, palettes and lookup tables
  have no file to import, and until now nothing could make them.
- **`chaos_ops`** (new `McpLinkChaos` interop plugin) — Chaos destruction:
  Geometry Collections built from Static Meshes, and uniform, Voronoi and
  planar fracturing with grout, surface noise and island splitting. Fracture
  again to add a cluster level; the response reports the bones per level.
  Clustering shapes what breaks apart together — automatic clustering by
  count, fraction, size or grid, plus clustering, merging, magnet-growing and
  deleting bones by index — convex hulls can be rebuilt or simplified for the
  collision the solver actually uses, and the interior faces a cut exposes can
  be given their own material slot and UVs, by box projection or by packing
  them into an atlas.
- **`blueprint_debug`** — breakpoints, watched pins, and the debug object their
  values are read from. Watch values are live during PIE without halting
  anything; halting and stepping are covered under Blueprint stepping above.
- **`material_layers`** — Material Layers and layer blends: create the two
  function assets (seeded with the inputs and output the Material Editor would
  add on first open), then build the stack on a Material or override it per
  instance — add, replace, reorder, rename, hide and remove layers and the
  blends between them. `material_graph`'s graph report now also covers the
  Material Attributes input, which is what a layer stack connects to.
- **`localization_ops`** — the Localization Dashboard's whole pipeline: targets
  and their cultures, the native (source) culture, which sources are gathered
  from, regenerating the commandlet config scripts, running `gather` and
  `compile` to produce a target's manifest, per-culture archives and `.locres`
  files, and translating in between — either in place through
  `list_translations` / `set_translation`, or by `export_po` / `import_po` for
  translators working outside the editor. Target edits go through
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

- **The plugin reported a hardcoded version that could never be right.**
  `MCPLINK_VERSION` was a `#define` in `McpLinkCoreModule.cpp`, and
  `tools/bump-version.ps1` — which rewrites `Cargo.toml`, every `.uplugin` and
  the CHANGELOG — had no idea it existed. The first release cut after 0.1.0
  would have shipped a plugin still calling itself 0.1.0 in `/api/status` and
  in its startup log, and any version comparison against it would have been
  comparing against a constant. The version is now read from the plugin
  descriptor at runtime, so it cannot drift from the file the release process
  already maintains.

- **Use-after-free when a one-shot delegate unbound itself.** A multicast
  delegate's `Remove()` destroys the bound lambda *immediately*, even when
  called from inside that delegate's own broadcast — `FDelegateBase::Unbind`
  runs the instance's destructor there and then, and only the invocation
  list's compaction is deferred. Both `pie_control`'s wait-for-PIE handler and
  `capture_viewport`'s screenshot handler unbound themselves and then went on
  using their own captures, which by that point lived in freed storage. The
  crash surfaced as an access violation at `0xffffffffffffffff` inside
  `FMcpResponder::Send`, which made it look like the responder had been freed;
  the responder was fine, the `TSharedRef` pointing at it was not. Whether it
  faulted depended on whether the allocator had reused the memory yet, so a
  PIE restart — which churns allocations — is where it showed up. The captures
  are now taken by value or copied to locals before the unbind.
- Engine discovery also looks in `<drive>:\Epic Games` and at drive roots, so
  an install moved out of `Program Files` (`D:\UE_5.8`) is found. When nothing
  is found at all, `status` now says the engine root is a *fallback guess*
  rather than reporting it like a detection, lists everywhere it searched, and
  reports `engine_root_source` plus `engine_installs_found` so a wrong root is
  visible immediately instead of surfacing as a file-not-found from every
  headless tool in turn.
- Every route handler now runs with `GIsRunningUnattendedScript` set. Editor
  code that prompts — asset rename when class defaults or soft references point
  at the asset, save prompts, "are you sure" — took the modal path, which
  blocks the game thread forever: the responder never fires, and the request
  that would cancel it cannot be served either, because the HTTP server ticks
  on that same thread. A prompt now takes its default answer, so the operation
  is declined and reported instead of wedging the editor. `asset_ops rename`
  says so explicitly when that is why it failed.

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

[Unreleased]: https://github.com/HTRMC/mcp-unreal/compare/v0.3.1...HEAD
[0.1.0]: https://github.com/HTRMC/mcp-unreal/releases/tag/v0.1.0
[0.2.0]: https://github.com/HTRMC/mcp-unreal/releases/tag/v0.2.0
[0.3.0]: https://github.com/HTRMC/mcp-unreal/releases/tag/v0.3.0
[0.3.1]: https://github.com/HTRMC/mcp-unreal/releases/tag/v0.3.1

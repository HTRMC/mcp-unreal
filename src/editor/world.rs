//! World building: landscape terrain and foliage scatter.
//!
//! Variants serialise straight into the plugin request body, so field names
//! here are the contract with `McpLandscapeRoutes.cpp` / `McpFoliageRoutes.cpp`.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
pub struct GrassVariety {
    /// Static mesh path.
    pub mesh: String,
    /// Instances per 10m x 10m.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub density: Option<f64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub min_scale: Option<f64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub max_scale: Option<f64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub random_rotation: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub align_to_surface: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub start_cull_distance: Option<i32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub end_cull_distance: Option<i32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub use_grid: Option<bool>,
    /// Any other FGrassVariety property by name.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub properties: Option<serde_json::Value>,
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum RvtOp {
    /// New Runtime Virtual Texture asset; `properties` are its UPROPERTY
    /// names (TileCount, TileSize, TileBorderSize as log2, MaterialType,
    /// bCompressTextures, RemoveLowMips, LODGroup).
    Create {
        /// e.g. /Game/VT/RVT_Terrain.
        path: String,
        properties: Option<serde_json::Value>,
    },
    Info {
        virtual_texture: String,
    },
    Save {
        virtual_texture: String,
    },
    /// Every Runtime Virtual Texture Volume in the world.
    List {
        world: Option<String>,
    },
    /// Place a volume that renders the texture, sized to `align_actor` (a
    /// landscape, usually) or to everything already assigned to it.
    SpawnVolume {
        virtual_texture: String,
        name: Option<String>,
        location: Option<[f64; 3]>,
        align_actor: Option<String>,
        snap_to_landscape: Option<bool>,
        /// Default true.
        fit_bounds: Option<bool>,
        world: Option<String>,
    },
    /// Refit a volume's bounds (the details panel's Set Bounds button).
    SetBounds {
        volume: String,
        align_actor: Option<String>,
        world: Option<String>,
    },
    /// Make an actor's primitives — or a landscape — render into the texture.
    Assign {
        virtual_texture: String,
        actor: String,
        world: Option<String>,
    },
    Unassign {
        virtual_texture: String,
        actor: String,
        world: Option<String>,
    },
    /// Bake the volume's streaming mips into its Streaming Texture (needs a
    /// windowed editor).
    BuildStreamingMips {
        volume: String,
        world: Option<String>,
    },
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum LandscapeOp {
    /// Every landscape in the world, with extent, resolution and layers.
    List { world: Option<String> },
    /// New landscape. Resolution is quads_per_section x sections_per_component
    /// x components, so the defaults give a 64x64-vertex landscape.
    Create {
        world: Option<String>,
        /// Editor label for the actor.
        name: Option<String>,
        /// World location of the landscape's corner [X, Y, Z] in cm.
        location: Option<[f64; 3]>,
        /// Scale [X, Y, Z]; the default [100, 100, 100] gives 1 m per quad.
        scale: Option<[f64; 3]>,
        /// 7, 15, 31, 63 (default), 127 or 255.
        quads_per_section: Option<i32>,
        /// 1 (default) or 4.
        sections_per_component: Option<i32>,
        components_x: Option<i32>,
        components_y: Option<i32>,
        /// Starting height in cm relative to the actor (default 0 — flat).
        height: Option<f64>,
        /// Landscape material asset path.
        material: Option<String>,
        /// A heightmap file to build the terrain from: a 16-bit greyscale
        /// PNG, or raw/r16 uint16 samples. With no component counts given,
        /// the layout is chosen to fit the image as the New Landscape panel
        /// does; otherwise the image is fitted to the layout (`transform`).
        heightmap_file: Option<String>,
        /// How a heightmap of a different size is fitted: "resample"
        /// (default, stretch), "original" (corner-aligned), or "expand"
        /// (centred).
        transform: Option<String>,
    },
    /// Replace a landscape's heights (or a region's) from a heightmap file.
    ImportHeightmap {
        /// Edit layer to write into (name or index) when the landscape has
        /// edit layers. Default: the first layer.
        edit_layer: Option<String>,
        world: Option<String>,
        landscape: Option<String>,
        /// A 16-bit greyscale PNG, or raw/r16 uint16 samples.
        file: String,
        /// Vertex region to write; the whole landscape when omitted.
        min_x: Option<i32>,
        min_y: Option<i32>,
        max_x: Option<i32>,
        max_y: Option<i32>,
        /// "resample" (default), "original" or "expand" when the image and
        /// the region differ in size.
        transform: Option<String>,
    },
    /// Extent, resolution and paintable layers of one landscape.
    Info {
        world: Option<String>,
        /// Actor label or path; optional when the level has exactly one.
        landscape: Option<String>,
    },
    /// Heights in cm relative to the landscape actor, row-major from min_y.
    /// At most 65536 vertices per call.
    GetHeights {
        world: Option<String>,
        landscape: Option<String>,
        /// Vertex coordinates; the whole landscape when omitted.
        min_x: Option<i32>,
        min_y: Option<i32>,
        max_x: Option<i32>,
        max_y: Option<i32>,
    },
    /// Sculpt: flatten a region to one height, or write a height per vertex.
    SetHeights {
        /// Edit layer to write into (name or index) when the landscape has
        /// edit layers. Default: the first layer.
        edit_layer: Option<String>,
        world: Option<String>,
        landscape: Option<String>,
        min_x: Option<i32>,
        min_y: Option<i32>,
        max_x: Option<i32>,
        max_y: Option<i32>,
        /// Flatten the whole region to this height in cm.
        height: Option<f64>,
        /// One height in cm per vertex, row-major from min_y; overrides height.
        heights: Option<Vec<f64>>,
    },
    /// Add a paintable weightmap layer, creating its LayerInfo asset if needed.
    AddLayer {
        world: Option<String>,
        landscape: Option<String>,
        /// Layer name, matching a layer the landscape material samples.
        layer: String,
        /// Where to put the LayerInfo asset (default
        /// /Game/Landscape/LayerInfo/<layer>_LayerInfo).
        layer_info: Option<String>,
    },
    /// Paint a layer's weight over a region.
    PaintLayer {
        /// Edit layer to write into (name or index) when the landscape has
        /// edit layers. Default: the first layer.
        edit_layer: Option<String>,
        world: Option<String>,
        landscape: Option<String>,
        layer: String,
        min_x: Option<i32>,
        min_y: Option<i32>,
        max_x: Option<i32>,
        max_y: Option<i32>,
        /// 0 to 1 (default 1).
        weight: Option<f64>,
    },
    /// Edit layers: name, guid, visibility, lock, alphas, which is being edited.
    ListEditLayers { landscape: Option<String> },
    /// Turn edit layers on, moving the existing data into a first layer.
    EnableEditLayers { landscape: Option<String> },
    AddEditLayer {
        landscape: Option<String>,
        name: String,
    },
    /// Rename, show/hide, lock or set the alphas of an edit layer.
    SetEditLayer {
        landscape: Option<String>,
        /// Layer name or index.
        layer: String,
        name: Option<String>,
        visible: Option<bool>,
        locked: Option<bool>,
        heightmap_alpha: Option<f64>,
        weightmap_alpha: Option<f64>,
    },
    RemoveEditLayer {
        landscape: Option<String>,
        layer: String,
    },
    ClearEditLayer {
        landscape: Option<String>,
        layer: String,
    },
    ReorderEditLayer {
        landscape: Option<String>,
        layer: String,
        index: i32,
    },
    /// The layer sculpt and paint writes go to when `layer` is omitted.
    SetEditingLayer {
        landscape: Option<String>,
        layer: String,
    },
    /// Spline control points and segments with their indices.
    ListSplines { landscape: Option<String> },
    /// Add a spline control point (world location); `connect_to` an existing
    /// point's index makes the segment between them, optionally with a mesh
    /// repeated along it.
    AddSplinePoint {
        landscape: Option<String>,
        location: [f64; 3],
        rotation: Option<[f64; 3]>,
        connect_to: Option<i32>,
        width: Option<f64>,
        side_falloff: Option<f64>,
        end_falloff: Option<f64>,
        /// Weightmap layer to paint under the spline on apply_splines.
        layer_name: Option<String>,
        /// Static mesh placed at the control point.
        mesh: Option<String>,
        /// Static mesh repeated along the new segment.
        segment_mesh: Option<String>,
        raise_terrain: Option<bool>,
        lower_terrain: Option<bool>,
    },
    SetSplinePoint {
        landscape: Option<String>,
        index: i32,
        location: Option<[f64; 3]>,
        rotation: Option<[f64; 3]>,
        width: Option<f64>,
        side_falloff: Option<f64>,
        end_falloff: Option<f64>,
        layer_name: Option<String>,
        mesh: Option<String>,
        raise_terrain: Option<bool>,
        lower_terrain: Option<bool>,
    },
    /// Segment meshes (repeated along it), paint layer, terrain flags and
    /// tangent lengths.
    SetSplineSegment {
        landscape: Option<String>,
        index: i32,
        /// One mesh; an empty string clears them.
        mesh: Option<String>,
        /// Several meshes, in order along the segment.
        meshes: Option<Vec<String>>,
        layer_name: Option<String>,
        raise_terrain: Option<bool>,
        lower_terrain: Option<bool>,
        start_tangent: Option<f64>,
        end_tangent: Option<f64>,
    },
    /// Remove a control point and every segment touching it.
    RemoveSplinePoint {
        landscape: Option<String>,
        index: i32,
    },
    RemoveSplineSegment {
        landscape: Option<String>,
        index: i32,
    },
    /// Deform the terrain to the splines and paint their layer (the spline
    /// tool's Apply Splines).
    ApplySplines {
        landscape: Option<String>,
        /// Edit layer to deform (default the first) on a layered landscape.
        edit_layer: Option<String>,
    },
    /// New Landscape Grass Type asset — the meshes a landscape material's
    /// Grass Output node scatters — from varieties.
    CreateGrassType {
        /// e.g. /Game/Landscape/LG_Meadow.
        path: String,
        varieties: Vec<GrassVariety>,
    },
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum FoliageOp {
    /// Foliage types in the level with their instance counts.
    ListTypes { world: Option<String> },
    /// Make a foliage type from a Static Mesh. Density, scale range, alignment
    /// and collision are properties on the returned `type` path — set_property
    /// edits them.
    AddType {
        world: Option<String>,
        /// Static Mesh asset path, e.g. /Engine/BasicShapes/Cone.
        static_mesh: String,
    },
    /// Remove a foliage type and every instance of it.
    RemoveType {
        world: Option<String>,
        /// Type name from list_types, or its object path.
        r#type: String,
    },
    /// Place instances: either explicit transforms, or scatter `count` of them
    /// by dropping points onto whatever has collision under the area.
    AddInstances {
        world: Option<String>,
        r#type: String,
        /// Explicit placements, each an object with a "location" [X, Y, Z] and
        /// optionally "rotation" [Pitch, Yaw, Roll] and "scale" [X, Y, Z].
        instances: Option<Vec<Value>>,
        /// How many to scatter when `instances` is omitted (max 10000).
        count: Option<i32>,
        /// Middle of the scatter area [X, Y, Z] in cm.
        center: Option<[f64; 3]>,
        /// Half-size of the scatter area [X, Y, Z]; Z is unused.
        extent: Option<[f64; 3]>,
        /// How far down to look for ground, in cm (default 100000).
        trace_height: Option<f64>,
        /// Tilt each instance to the surface it landed on.
        align_to_normal: Option<bool>,
        /// Random yaw per instance (default true).
        random_yaw: Option<bool>,
        min_scale: Option<f64>,
        max_scale: Option<f64>,
        /// Fixed seed, for a repeatable scatter.
        seed: Option<i32>,
    },
    /// Instance count, and the first `max_results` transforms.
    ListInstances {
        world: Option<String>,
        r#type: String,
        max_results: Option<i32>,
    },
    /// Remove instances of a type: those inside a sphere, or all of them.
    RemoveInstances {
        world: Option<String>,
        r#type: String,
        center: Option<[f64; 3]>,
        radius: Option<f64>,
    },
}

/// Sublevels — the streaming levels layered under the persistent level.
/// `level_ops` handles the one level open in the editor; this handles the rest.
#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum SublevelOp {
    /// Every streaming level of the world, with its visibility, lock state,
    /// actor count and level transform.
    List { world: Option<String> },
    /// Create a new sublevel and add it to the world.
    Create {
        /// Package path for the new level, e.g. /Game/Maps/Sub_Props.
        path: String,
        /// "always_loaded" (default) or "dynamic".
        streaming: Option<String>,
        location: Option<[f64; 3]>,
        /// [Pitch, Yaw, Roll].
        rotation: Option<[f64; 3]>,
        world: Option<String>,
    },
    /// Add an existing level asset to the world as a sublevel.
    Add {
        path: String,
        streaming: Option<String>,
        location: Option<[f64; 3]>,
        rotation: Option<[f64; 3]>,
        world: Option<String>,
    },
    Remove {
        /// Package path or short name from `list`.
        level: String,
        world: Option<String>,
    },
    SetVisible {
        level: String,
        visible: bool,
        world: Option<String>,
    },
    SetLocked {
        level: String,
        locked: bool,
        world: Option<String>,
    },
    /// Make this the level new actors are spawned into.
    SetCurrent {
        level: String,
        world: Option<String>,
    },
    /// Move the sublevel — and everything loaded in it — in world space.
    SetTransform {
        level: String,
        location: Option<[f64; 3]>,
        rotation: Option<[f64; 3]>,
        world: Option<String>,
    },
    /// Move existing actors out of their level and into this one.
    MoveActors {
        level: String,
        /// Actor paths or editor labels.
        actors: Vec<String>,
        world: Option<String>,
    },
}

/// World Partition: data layers, and loading regions of a partitioned world.
#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum WorldPartitionOp {
    /// Whether the world is partitioned, its streaming state, and its data
    /// layers. Safe to call on a non-partitioned world — it says so.
    Info { world: Option<String> },
    /// Add a data layer to the world, creating the Data Layer asset if the
    /// path does not already hold one.
    CreateDataLayer {
        /// Data Layer asset path, e.g. /Game/DataLayers/DL_Props.
        asset: String,
        world: Option<String>,
    },
    DeleteDataLayer {
        /// Data layer name or asset path, from `info`.
        data_layer: String,
        world: Option<String>,
    },
    SetDataLayerState {
        data_layer: String,
        visible: Option<bool>,
        /// Whether the layer's actors load in the editor.
        loaded_in_editor: Option<bool>,
        /// "Unloaded", "Loaded" or "Activated" at game start.
        initial_runtime_state: Option<String>,
        world: Option<String>,
    },
    AddActors {
        data_layer: String,
        actors: Vec<String>,
        world: Option<String>,
    },
    RemoveActors {
        data_layer: String,
        actors: Vec<String>,
        world: Option<String>,
    },
    /// Load the partition cells inside a world-space box so their actors
    /// become reachable — a partitioned map starts with almost nothing loaded.
    LoadRegion {
        min: [f64; 3],
        max: [f64; 3],
        world: Option<String>,
    },
    /// Release every region this session loaded.
    UnloadRegion { world: Option<String> },
}

/// Level Instances, Packed Level Actors and actor merging.
#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum LevelInstanceOp {
    /// Level instance actors in the world.
    List { world: Option<String> },
    /// Move actors into their own level and leave an instance behind.
    /// Needs a windowed editor: the engine hardcodes a Save As dialog for the
    /// new level, so a `-nullrhi` editor refuses rather than exiting on it.
    Create {
        /// Package path for the new level, e.g. /Game/Maps/LI_Shed.
        path: String,
        /// Actor paths or editor labels.
        actors: Vec<String>,
        /// "level_instance" (default) or "packed" for a Packed Level Actor,
        /// which bakes the contents into instanced-mesh components.
        kind: Option<String>,
        /// Store the level's actors as one file each (World Partition style).
        external_actors: Option<bool>,
        world: Option<String>,
    },
    /// Dissolve an instance, moving its actors back into the current level.
    Break {
        actor: String,
        /// How many nested levels of instance to break (default 1).
        levels: Option<i32>,
        world: Option<String>,
    },
    /// Collapse actors' meshes into one Static Mesh asset. The source actors
    /// are left alone; save the result with asset_ops save.
    MergeActors {
        /// Package path for the merged mesh, e.g. /Game/Meshes/SM_Merged.
        path: String,
        actors: Vec<String>,
        /// Carry collision across (default true).
        merge_physics: Option<bool>,
        /// Bake the materials into one (default false).
        merge_materials: Option<bool>,
        merge_sockets: Option<bool>,
        generate_lightmap_uvs: Option<bool>,
        world: Option<String>,
    },
}

#[tool_router(router = world_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Build and sculpt Landscape terrain: create a landscape at a chosen resolution and scale (flat, or from a 16-bit PNG/raw heightmap file), import a heightmap file over an existing landscape, read and write heights over any region (heights are centimetres of Z relative to the landscape actor, not raw samples), and add or paint the weightmap layers the landscape material blends. Sculpting a region takes either one flat height or one height per vertex, row-major from min_y."
    )]
    async fn landscape_ops(
        &self,
        Parameters(op): Parameters<LandscapeOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/world/landscape", body)
            .await
            .map(Json)
    }

    #[tool(
        description = "Runtime Virtual Textures: create the asset, spawn and size the volume that renders it, assign primitives or a landscape to it, list the volumes in a world, and bake streaming mips."
    )]
    async fn rvt_ops(&self, Parameters(op): Parameters<RvtOp>) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/world/virtual_texture", body)
            .await
            .map(Json)
    }

    #[tool(
        description = "Scatter foliage: make foliage types from Static Meshes, then place instances either at explicit transforms or by scattering a count over an area, dropping each onto whatever has collision beneath it (a landscape or static meshes) with optional surface alignment and random yaw and scale. Foliage type settings are properties on the reported type path, so set_property edits them."
    )]
    async fn foliage_ops(
        &self,
        Parameters(op): Parameters<FoliageOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/world/foliage", body).await.map(Json)
    }

    #[tool(
        description = "Sublevels: list the streaming levels under the persistent level with their visibility, lock state and transform; create a new one or add an existing level asset (always-loaded or dynamic streaming); remove one; toggle visibility and locking; choose which level new actors spawn into; move a sublevel and its contents in world space; and move existing actors into it. A World Partition map has no sublevels — use world_partition_ops there."
    )]
    async fn sublevel_ops(
        &self,
        Parameters(op): Parameters<SublevelOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/world/streaming", body)
            .await
            .map(Json)
    }

    #[tool(
        description = "World Partition: report whether a world is partitioned and how it streams, create and delete data layers (making the Data Layer asset when needed), move actors in and out of them, set a layer's editor visibility/loading and its initial runtime state, and load or unload a world-space region so a partitioned map's actors are actually in memory — which they largely are not when it first opens."
    )]
    async fn world_partition_ops(
        &self,
        Parameters(op): Parameters<WorldPartitionOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/world/partition", body)
            .await
            .map(Json)
    }

    #[tool(
        description = "Level Instances, Packed Level Actors and actor merging — the three ways to treat a group of actors as one thing. create moves actors into their own level and leaves an instance behind (or a Packed Level Actor, which bakes them into instanced-mesh components) — it needs a windowed editor, since the engine always opens a Save As dialog for the new level; break dissolves one again; merge_actors collapses their meshes into a single Static Mesh asset, leaving the sources untouched."
    )]
    async fn level_instance_ops(
        &self,
        Parameters(op): Parameters<LevelInstanceOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/world/instances", body)
            .await
            .map(Json)
    }
}

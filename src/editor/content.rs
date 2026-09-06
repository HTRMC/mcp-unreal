//! Asset authoring: materials, textures, data tables, Enhanced Input assets,
//! and instanced meshes.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::{Value, json};

use crate::UnrealMcp;

#[derive(serde::Deserialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum MaterialOp {
    /// Create an empty Material asset. Editing the material graph itself is not
    /// supported yet — instance an existing material instead where possible.
    Create {
        /// e.g. "/Game/Materials/M_Thing".
        path: String,
    },
    /// Create a Material Instance of an existing material, which is what you
    /// parameterise.
    CreateInstance {
        /// e.g. "/Game/Materials/MI_Thing".
        path: String,
        /// Material to instance, e.g. "/Engine/BasicShapes/BasicShapeMaterial".
        parent: String,
    },
    /// Scalar, vector, texture and static-switch parameter names of a material.
    ListParameters {
        /// Material or Material Instance path.
        material: String,
    },
    /// Read one parameter's current value and type.
    GetParameter { material: String, parameter: String },
    /// Set a parameter on a Material Instance. Pass `value` as a number for a
    /// scalar, `value` as [R,G,B,A] for a vector, or `texture` as an asset path.
    SetParameter {
        /// Material Instance path.
        material: String,
        parameter: String,
        value: Option<Value>,
        texture: Option<String>,
    },
    /// Write the material asset to disk.
    Save { material: String },
}

#[derive(serde::Deserialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum DataTableOp {
    /// Create a DataTable asset for a row struct.
    Create {
        /// e.g. "/Game/Data/DT_Items".
        path: String,
        /// A struct deriving from FTableRowBase: "/Script/MyGame.ItemRow" or the bare name "ItemRow".
        row_struct: String,
    },
    /// Row struct, row count and column names/types.
    GetInfo {
        table: String,
    },
    /// Read rows (all, or one by name).
    GetRows {
        table: String,
        /// Read just this row.
        row: Option<String>,
        /// Max rows (default 50, cap 500).
        max_rows: Option<u32>,
    },
    /// Create or update a row. Unspecified columns keep their current values.
    SetRow {
        table: String,
        row: String,
        /// Column values, keyed by column name (see get_info).
        values: Value,
    },
    DeleteRow {
        table: String,
        row: String,
    },
    /// Replace the table's contents from CSV text (first column is the row name).
    ImportCsv {
        table: String,
        csv: String,
    },
    Save {
        table: String,
    },
}

#[derive(serde::Deserialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum InputAssetOp {
    /// Create an Input Action asset.
    CreateAction {
        /// e.g. "/Game/Input/IA_Move".
        path: String,
        /// bool (default), float, vector2d, or vector.
        value_type: Option<String>,
    },
    /// Create an Input Mapping Context asset.
    CreateContext {
        /// e.g. "/Game/Input/IMC_Default".
        path: String,
    },
    /// Bind a key to an action inside a mapping context.
    MapKey {
        context: String,
        action: String,
        /// UE key name, e.g. "W", "SpaceBar", "Gamepad_LeftX".
        key: String,
    },
    UnmapKey {
        context: String,
        action: String,
        key: String,
    },
    /// List a mapping context's bindings.
    GetContext { context: String },
    /// Write an Input Action or Mapping Context asset to disk.
    Save { path: String },
}

#[derive(serde::Deserialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum IsmOp {
    /// Attach a new instanced-static-mesh component to an actor.
    Create {
        /// Actor path or editor label.
        actor: String,
        /// StaticMesh asset path, e.g. "/Engine/BasicShapes/Cube.Cube".
        mesh: String,
        /// Use a hierarchical (HISM) component, which culls and LODs per instance.
        hierarchical: Option<bool>,
        world: Option<String>,
    },
    /// Mesh, class and instance count of the component.
    GetInfo {
        actor: String,
        /// Component name; defaults to the actor's first ISM component.
        component: Option<String>,
        world: Option<String>,
    },
    /// Add instances in bulk — the efficient way to scatter meshes.
    AddInstances {
        actor: String,
        component: Option<String>,
        /// Each entry: {"location":[x,y,z], "rotation":[p,y,r], "scale":[x,y,z]}.
        instances: Vec<Value>,
        world: Option<String>,
    },
    /// Move/rotate/scale one instance by index.
    UpdateInstance {
        actor: String,
        component: Option<String>,
        index: u32,
        location: Option<[f64; 3]>,
        rotation: Option<[f64; 3]>,
        scale: Option<[f64; 3]>,
        world: Option<String>,
    },
    /// Remove every instance from the component.
    Clear {
        actor: String,
        component: Option<String>,
        world: Option<String>,
    },
}

#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct TextureInfoInput {
    /// Texture asset path, e.g. "/Engine/EngineResources/DefaultTexture".
    pub texture: String,
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum TextureOp {
    /// New Texture2D from raw pixels — no source file involved. Masks,
    /// gradients, palettes and lookup tables have no file to import.
    Create {
        /// e.g. /Game/Textures/T_Mask.
        path: String,
        width: i32,
        height: i32,
        /// base64 BGRA8: four bytes per pixel, rows top to bottom, exactly
        /// width * height * 4 bytes. Omit for a solid `fill`.
        pixels: Option<String>,
        /// [r, g, b, a] 0-255 to fill with when `pixels` is omitted. Default
        /// opaque black.
        fill: Option<Vec<i32>>,
        /// Default true. Turn it off for data — masks, gradients, lookup
        /// tables — where sRGB silently skews the values.
        srgb: Option<bool>,
        /// e.g. "TC_Default", "TC_Masks", "TC_Grayscale", "TC_HDR".
        compression: Option<String>,
    },
    /// New cube map from six faces of raw BGRA8 pixels, or a solid fill.
    CreateCube {
        /// e.g. /Game/Textures/TC_Sky.
        path: String,
        /// Edge length; every face is size x size.
        size: i32,
        /// base64 BGRA8 of all six faces back to back in +X, -X, +Y, -Y, +Z,
        /// -Z order: exactly size * size * 4 * 6 bytes. Omit for a solid
        /// `fill`.
        faces: Option<String>,
        /// [r, g, b, a] 0-255 to fill with when `faces` is omitted.
        fill: Option<Vec<i32>>,
        srgb: Option<bool>,
        /// e.g. "TC_Default", "TC_HDR".
        compression: Option<String>,
    },
    /// New volume texture from raw BGRA8 slices, or a solid fill.
    CreateVolume {
        /// e.g. /Game/Textures/TV_Noise.
        path: String,
        width: i32,
        height: i32,
        depth: i32,
        /// base64 BGRA8: `depth` slices of width * height pixels, front to
        /// back, rows top to bottom — exactly width * height * depth * 4
        /// bytes. Omit for a solid `fill`.
        pixels: Option<String>,
        fill: Option<Vec<i32>>,
        srgb: Option<bool>,
        compression: Option<String>,
    },
    /// Read a rectangle back as base64 BGRA8. At most 256x256 per call.
    ReadPixels {
        texture: String,
        x: Option<i32>,
        y: Option<i32>,
        /// Defaults to the rest of the row / column.
        width: Option<i32>,
        height: Option<i32>,
    },
    /// Write a rectangle of base64 BGRA8 into the texture's source data.
    WritePixels {
        texture: String,
        /// base64 BGRA8, exactly width * height * 4 bytes.
        pixels: String,
        x: Option<i32>,
        y: Option<i32>,
        width: Option<i32>,
        height: Option<i32>,
    },
    /// Fill a rectangle with one colour.
    Fill {
        texture: String,
        /// [r, g, b, a] 0-255. Default opaque black.
        color: Option<Vec<i32>>,
        x: Option<i32>,
        y: Option<i32>,
        width: Option<i32>,
        height: Option<i32>,
    },
    /// Save any texture asset — 2D, cube or volume.
    Save { texture: String },
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum MediaOp {
    /// New File Media Source pointing at a video or audio file (absolute, or
    /// relative to the project's Content folder).
    CreateFileSource {
        /// e.g. /Game/Media/MS_Intro.
        path: String,
        file: String,
        precache: Option<bool>,
    },
    /// New Media Player asset.
    CreatePlayer {
        /// e.g. /Game/Media/MP_Intro.
        path: String,
        /// Default true.
        play_on_open: Option<bool>,
        r#loop: Option<bool>,
    },
    /// New Media Texture bound to a player, for a material to sample.
    CreateTexture {
        /// e.g. /Game/Media/MT_Intro.
        path: String,
        player: String,
    },
    /// New Media Playlist from source assets, files or URLs.
    CreatePlaylist {
        path: String,
        sources: Option<Vec<String>>,
    },
    /// Open a source asset, file or URL in a player (asynchronous — poll
    /// status until `ready`).
    Open {
        player: String,
        source: Option<String>,
        file: Option<String>,
        url: Option<String>,
    },
    Play {
        player: String,
    },
    Pause {
        player: String,
    },
    Rewind {
        player: String,
    },
    Seek {
        player: String,
        time: f64,
    },
    SetRate {
        player: String,
        rate: f64,
    },
    SetLooping {
        player: String,
        r#loop: Option<bool>,
    },
    Close {
        player: String,
    },
    /// Open state, playback state, time, duration and tracks.
    Status {
        player: String,
    },
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum MaterialParameterCollectionOp {
    /// New, empty collection.
    Create {
        /// e.g. /Game/Materials/MPC_Global.
        path: String,
    },
    /// Parameters with their defaults, ids and — for the editor world, or
    /// the PIE world with world="pie" — the current runtime values.
    Info {
        collection: String,
        world: Option<String>,
    },
    /// Add a scalar parameter, or change an existing one's default.
    SetScalar {
        collection: String,
        name: String,
        value: f64,
    },
    /// Add a vector parameter, or change an existing one's default.
    SetVector {
        collection: String,
        name: String,
        /// [r, g, b] or [r, g, b, a] as linear floats.
        value: Vec<f64>,
    },
    Rename {
        collection: String,
        name: String,
        new_name: String,
    },
    Remove {
        collection: String,
        name: String,
    },
    /// Set a parameter's runtime value on a world's instance — what
    /// Blueprint's SetScalarParameterValue writes. Not saved; resets with
    /// the world.
    SetValue {
        collection: String,
        name: String,
        /// A number for a scalar, [r, g, b, a] for a vector.
        value: serde_json::Value,
        /// "editor" (default) or "pie".
        world: Option<String>,
    },
    /// Every parameter's runtime value on a world's instance.
    GetValues {
        collection: String,
        world: Option<String>,
    },
    Save {
        collection: String,
    },
}

#[tool_router(router = content_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Materials: create materials and material instances, list/read/write instance parameters, and save. Parameterising a Material Instance is the supported way to vary a material; editing material graphs is not yet available."
    )]
    async fn material_ops(
        &self,
        Parameters(op): Parameters<MaterialOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body = match op {
            MaterialOp::Create { path } => json!({"operation": "create", "path": path}),
            MaterialOp::CreateInstance { path, parent } => {
                json!({"operation": "create_instance", "path": path, "parent": parent})
            }
            MaterialOp::ListParameters { material } => {
                json!({"operation": "list_parameters", "material": material})
            }
            MaterialOp::GetParameter {
                material,
                parameter,
            } => {
                json!({"operation": "get_parameter", "material": material, "parameter": parameter})
            }
            MaterialOp::SetParameter {
                material,
                parameter,
                value,
                texture,
            } => json!({
                "operation": "set_parameter", "material": material,
                "parameter": parameter, "value": value, "texture": texture,
            }),
            MaterialOp::Save { material } => json!({"operation": "save", "material": material}),
        };
        self.call_plugin("/api/materials/ops", body).await.map(Json)
    }

    #[tool(
        description = "DataTables: inspect the row struct and columns, read rows, create/update/delete rows, import CSV, and save."
    )]
    async fn data_table_ops(
        &self,
        Parameters(op): Parameters<DataTableOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body = match op {
            DataTableOp::Create { path, row_struct } => {
                json!({"operation": "create", "path": path, "row_struct": row_struct})
            }
            DataTableOp::GetInfo { table } => json!({"operation": "get_info", "table": table}),
            DataTableOp::GetRows {
                table,
                row,
                max_rows,
            } => json!({
                "operation": "get_rows", "table": table,
                "row": row, "max_rows": max_rows,
            }),
            DataTableOp::SetRow { table, row, values } => {
                json!({"operation": "set_row", "table": table, "row": row, "values": values})
            }
            DataTableOp::DeleteRow { table, row } => {
                json!({"operation": "delete_row", "table": table, "row": row})
            }
            DataTableOp::ImportCsv { table, csv } => {
                json!({"operation": "import_csv", "table": table, "csv": csv})
            }
            DataTableOp::Save { table } => json!({"operation": "save", "table": table}),
        };
        self.call_plugin("/api/data/ops", body).await.map(Json)
    }

    #[tool(
        description = "Author Enhanced Input assets: create Input Actions and Mapping Contexts, bind or unbind keys, and inspect a context. This is the design-time counterpart to input_inject, which drives input at runtime."
    )]
    async fn input_asset_ops(
        &self,
        Parameters(op): Parameters<InputAssetOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body = match op {
            InputAssetOp::CreateAction { path, value_type } => {
                json!({"operation": "create_action", "path": path, "value_type": value_type})
            }
            InputAssetOp::CreateContext { path } => {
                json!({"operation": "create_context", "path": path})
            }
            InputAssetOp::MapKey {
                context,
                action,
                key,
            } => json!({"operation": "map_key", "context": context, "action": action, "key": key}),
            InputAssetOp::UnmapKey {
                context,
                action,
                key,
            } => {
                json!({"operation": "unmap_key", "context": context, "action": action, "key": key})
            }
            InputAssetOp::GetContext { context } => {
                json!({"operation": "get_context", "context": context})
            }
            InputAssetOp::Save { path } => json!({"operation": "save", "path": path}),
        };
        self.call_plugin("/api/input_assets/ops", body)
            .await
            .map(Json)
    }

    #[tool(
        description = "Instanced static meshes: attach an ISM/HISM component to an actor and add, update or clear instances in bulk. Use this to place many copies of a mesh cheaply instead of spawning many actors."
    )]
    async fn ism_ops(&self, Parameters(op): Parameters<IsmOp>) -> Result<Json<Value>, ErrorData> {
        let body = match op {
            IsmOp::Create {
                actor,
                mesh,
                hierarchical,
                world,
            } => json!({
                "operation": "create", "actor": actor, "mesh": mesh,
                "hierarchical": hierarchical, "world": world,
            }),
            IsmOp::GetInfo {
                actor,
                component,
                world,
            } => json!({
                "operation": "get_info", "actor": actor,
                "component": component, "world": world,
            }),
            IsmOp::AddInstances {
                actor,
                component,
                instances,
                world,
            } => json!({
                "operation": "add_instances", "actor": actor, "component": component,
                "instances": instances, "world": world,
            }),
            IsmOp::UpdateInstance {
                actor,
                component,
                index,
                location,
                rotation,
                scale,
                world,
            } => json!({
                "operation": "update_instance", "actor": actor, "component": component,
                "index": index, "location": location, "rotation": rotation,
                "scale": scale, "world": world,
            }),
            IsmOp::Clear {
                actor,
                component,
                world,
            } => json!({
                "operation": "clear", "actor": actor,
                "component": component, "world": world,
            }),
        };
        self.call_plugin("/api/ism/ops", body).await.map(Json)
    }

    #[tool(
        description = "Author a Texture2D's pixels directly: create one from base64 BGRA8 bytes or a solid fill, read a rectangle back, write a rectangle, or flood-fill one, then save. This is the import-free half — asset_ops import handles files on disk, and render_ops draws a whole texture with a material. Everything works on the editor source data, so the result survives recompression; regions are capped at 256x256 per call, so paint large textures in tiles."
    )]
    async fn texture_ops(
        &self,
        Parameters(op): Parameters<TextureOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/textures/ops", body).await.map(Json)
    }

    #[tool(
        description = "Media Framework: create File Media Sources, Media Players, Media Textures and Playlists, open a source, file or URL in a player, and play, pause, seek, rewind, set rate or looping, close, and read status (ready, playing, time, duration, tracks)."
    )]
    async fn media_ops(
        &self,
        Parameters(op): Parameters<MediaOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/content/media", body).await.map(Json)
    }

    #[tool(
        description = "Material Parameter Collections: create one, add or edit its scalar and vector parameters and their defaults (materials reading it are recompiled), rename or remove parameters, and read or set the runtime values on a world's instance without saving."
    )]
    async fn material_parameter_collection_ops(
        &self,
        Parameters(op): Parameters<MaterialParameterCollectionOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/materials/parameter_collection", body)
            .await
            .map(Json)
    }

    #[tool(description = "Get a texture asset's dimensions, pixel format and sRGB/LOD settings.")]
    async fn texture_info(
        &self,
        Parameters(input): Parameters<TextureInfoInput>,
    ) -> Result<Json<Value>, ErrorData> {
        self.call_plugin("/api/textures/info", json!({"texture": input.texture}))
            .await
            .map(Json)
    }

    #[tool(
        description = "String Table assets — the key-to-text tables localised FText reads from: create one (with a namespace), list its strings, set one string or a batch (with developer notes), remove strings, import or export CSV, and save. Text in Blueprints and widgets references a table as (namespace, key)."
    )]
    async fn string_table_ops(
        &self,
        Parameters(op): Parameters<StringTableOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/data/string_table", body)
            .await
            .map(Json)
    }

    #[tool(
        description = "Curve Table assets — one named float curve per row, as gameplay effects and scalable stats read them: create one, list rows, read a row's keys, set a row from keys (with linear, constant or cubic interpolation), remove rows, evaluate a row at a time, import CSV (first row the times, one row per curve) or export it, and save."
    )]
    async fn curve_table_ops(
        &self,
        Parameters(op): Parameters<CurveTableOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/data/curve_table", body)
            .await
            .map(Json)
    }
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum StringTableOp {
    /// New String Table asset (in memory — follow with `save`).
    Create {
        /// e.g. "/Game/Text/ST_Dialogue".
        path: String,
        /// The namespace its keys live in (default: none).
        namespace: Option<String>,
    },
    /// Namespace and string count.
    Info {
        table: String,
    },
    /// The strings: key, text and developer notes.
    GetStrings {
        table: String,
        /// Cap (default 500).
        max_results: Option<u32>,
    },
    /// Set (or add) one string.
    SetString {
        table: String,
        key: String,
        text: String,
        /// Developer notes shown to translators.
        notes: Option<String>,
    },
    /// Set many strings at once: {"Key": "Text", ...}.
    SetStrings {
        table: String,
        strings: Value,
    },
    RemoveString {
        table: String,
        key: String,
    },
    SetNamespace {
        table: String,
        namespace: String,
    },
    /// Import a CSV (Key,SourceString[,Comment]) into the table.
    ImportCsv {
        table: String,
        file: String,
    },
    /// Export the table as CSV.
    ExportCsv {
        table: String,
        file: String,
    },
    Save {
        table: String,
    },
}

/// One key of a curve table row.
#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
pub struct CurveKey {
    pub time: f64,
    pub value: f64,
    /// "linear" (default), "constant" or "cubic".
    #[serde(skip_serializing_if = "Option::is_none")]
    pub interp: Option<String>,
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum CurveTableOp {
    /// New, empty Curve Table (in memory — follow with `save`). The first
    /// row decides whether it holds rich (interpolated) or simple curves.
    Create {
        /// e.g. "/Game/Data/CT_Damage".
        path: String,
    },
    /// Mode and rows, with key counts and time ranges.
    Info {
        table: String,
        /// Include every row's keys (default false).
        include_keys: Option<bool>,
    },
    /// One row's keys.
    GetRow {
        table: String,
        row: String,
    },
    /// Replace (or create) a row from keys.
    SetRow {
        table: String,
        row: String,
        keys: Vec<CurveKey>,
        /// On an empty table, make it a simple-curve table (no per-key
        /// interpolation) instead of rich curves.
        simple: Option<bool>,
    },
    RemoveRow {
        table: String,
        row: String,
    },
    /// The row's value at a time.
    Evaluate {
        table: String,
        row: String,
        time: f64,
    },
    /// Rebuild the table from CSV: first row the times, then one row per
    /// curve. Pass `csv` text or a `file` path.
    ImportCsv {
        table: String,
        csv: Option<String>,
        file: Option<String>,
        /// Interpolation for every key: "linear" (default), "constant" or "cubic".
        interp: Option<String>,
    },
    /// The table as CSV — returned inline, or written to `file`.
    ExportCsv {
        table: String,
        file: Option<String>,
    },
    Save {
        table: String,
    },
}

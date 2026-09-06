//! Editor housekeeping: source control, validation, gameplay tags, curve
//! assets and the reference graph.
//!
//! Variants serialise straight into the plugin request body, so the field
//! names here are the contract with the `McpLinkWorkflow` route files.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum SourceControlOp {
    /// Which provider is configured, whether it is connected, and — with
    /// `files` — each file's real state. Safe to call with source control off;
    /// it says so rather than failing.
    Status {
        /// Package paths, asset paths or filenames.
        files: Option<Vec<String>>,
    },
    /// Re-run the provider's login/connect step.
    Connect {},
    CheckOut {
        files: Vec<String>,
    },
    MarkForAdd {
        files: Vec<String>,
    },
    MarkForDelete {
        files: Vec<String>,
    },
    Revert {
        files: Vec<String>,
    },
    /// Get the latest revision of these files.
    Sync {
        files: Vec<String>,
    },
    /// Check in. A submit with no description is rejected by every provider.
    Submit {
        files: Vec<String>,
        description: String,
    },
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum ValidateOp {
    /// Run the project's Data Validation validators, over named assets or
    /// everything under a path.
    Assets {
        /// Specific assets. Omit to validate `path_prefix` instead.
        assets: Option<Vec<String>>,
        /// Package path to validate under (default "/Game").
        path_prefix: Option<String>,
        /// Cap on assets to load and check (default 500).
        max_assets: Option<i32>,
        /// Cap on problems reported inline (default 50).
        max_results: Option<i32>,
    },
    /// The editor's Map Check on the current level.
    MapCheck { world: Option<String> },
    /// Compile every Blueprint under a path and report the ones with errors or
    /// warnings — the sweep the editor has no button for.
    CompileBlueprints {
        path_prefix: Option<String>,
        max_blueprints: Option<i32>,
        max_results: Option<i32>,
    },
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum GameplayTagOp {
    /// The project's gameplay tags, optionally filtered or rooted at a parent.
    List {
        name_contains: Option<String>,
        /// Only this tag and its children, e.g. "Ability".
        under: Option<String>,
        max_results: Option<i32>,
    },
    /// Add a tag to the project's tag ini and register it with the manager.
    Add {
        /// Dot-separated, e.g. "Ability.Melee.Heavy".
        tag: String,
        comment: Option<String>,
        /// Tag source ini to write to; the project default when omitted.
        source: Option<String>,
    },
    Rename {
        tag: String,
        new_tag: String,
    },
    Remove {
        tag: String,
    },
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum CurveOp {
    /// Every channel of the curve with its keys.
    Info {
        curve: String,
    },
    /// Replace a channel's keys.
    SetKeys {
        /// Curve asset path; asset_ops create makes CurveFloat, CurveVector
        /// and CurveLinearColor assets.
        curve: String,
        /// 0 for a float curve; X/Y/Z or R/G/B/A for vector and colour curves.
        channel: Option<i32>,
        /// [{"time": 0.0, "value": 1.0, "interp": "cubic"}, ...].
        keys: Vec<Value>,
        /// Interpolation for keys that do not name one: "cubic" (default),
        /// "linear" or "constant".
        interp: Option<String>,
    },
    /// Add keys without clearing the ones already there.
    AddKeys {
        curve: String,
        channel: Option<i32>,
        keys: Vec<Value>,
        interp: Option<String>,
    },
    Clear {
        curve: String,
        channel: Option<i32>,
    },
    Save {
        curve: String,
    },
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum ReferenceOp {
    /// What would break if this asset were deleted or renamed.
    Referencers {
        asset: String,
        /// Hard references only (default true); false also counts soft ones.
        hard_only: Option<bool>,
        max_results: Option<i32>,
    },
    /// What this asset pulls in.
    Dependencies {
        asset: String,
        hard_only: Option<bool>,
        max_results: Option<i32>,
    },
    /// On-disk size of the whole dependency tree, deduplicated, with the
    /// largest contributors — the Size Map, as numbers.
    SizeMap {
        asset: String,
        hard_only: Option<bool>,
        /// How deep to walk the tree (default 8).
        max_depth: Option<i32>,
        max_results: Option<i32>,
    },
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum CollectionOp {
    /// Every collection in the project's collection container, with its
    /// share type and asset count.
    List {},
    /// New collection, optionally seeded with assets.
    Create {
        name: String,
        /// "local" (default, this machine), "private" (this user, in source
        /// control) or "shared" (everyone).
        share_type: Option<String>,
        /// "static" (default: an explicit asset list) or "dynamic" (a query).
        storage: Option<String>,
        /// Asset paths to add straight away.
        assets: Option<Vec<String>>,
    },
    Destroy {
        name: String,
        share_type: Option<String>,
    },
    /// Add assets (paths such as "/Game/Meshes/SM_Rock") to a collection.
    Add {
        name: String,
        share_type: Option<String>,
        assets: Vec<String>,
    },
    Remove {
        name: String,
        share_type: Option<String>,
        assets: Vec<String>,
    },
    /// The assets in a collection (and its children with recursive=true).
    Assets {
        name: String,
        share_type: Option<String>,
        recursive: Option<bool>,
    },
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum EditorUtilityOp {
    /// Editor Utility Blueprints and Editor Utility Widget Blueprints under a path.
    List {
        /// Default "/Game".
        path_prefix: Option<String>,
    },
    /// Run an Editor Utility Blueprint's Run entry point (the same as its
    /// context-menu Run), for one-shot editor scripts authored in Blueprint.
    Run {
        /// The Editor Utility Blueprint asset path.
        asset: String,
    },
    /// Open an Editor Utility Widget in a tab. Reports the live widget's
    /// object path so get_property / call_function can drive it, and the
    /// tab id for close_widget. Needs a windowed editor to dock into.
    OpenWidget {
        asset: String,
        /// A tab id to reuse; a new one is generated otherwise.
        tab_id: Option<String>,
    },
    CloseWidget {
        tab_id: String,
    },
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum AssetManagerOp {
    /// The primary asset types the Asset Manager scans, with their base
    /// class, scan paths and how many assets each found.
    Types {},
    /// The primary assets of one type, as "Type:Name" ids with their paths.
    List {
        /// e.g. "Map", "PrimaryAssetLabel", or a project type.
        #[serde(rename = "type")]
        asset_type: String,
    },
    /// One primary asset's path and its cook/chunk rules.
    Info {
        /// "Type:Name", or the asset's path.
        id: String,
    },
    /// New Primary Asset Label: a data asset that tags assets (explicitly,
    /// or everything in its folder) with cook and chunk rules for
    /// packaging. Save it with asset_ops save.
    CreateLabel {
        /// e.g. "/Game/Labels/PAL_Level1".
        path: String,
        /// Assets the label applies to.
        assets: Option<Vec<String>>,
        /// Also label everything in the label's own folder (default false).
        label_directory: Option<bool>,
        /// Rule priority (higher wins on conflict).
        priority: Option<i32>,
        /// Pak chunk to put the assets in (-1 for none).
        chunk_id: Option<i32>,
        /// Apply the rules to the assets' dependencies too.
        apply_recursively: Option<bool>,
        /// "always_cook", "never_cook", "production_never_cook",
        /// "development_always_cook" or
        /// "development_always_production_never_cook".
        cook_rule: Option<String>,
    },
    /// Rescan the primary asset directories (after creating labels or assets).
    Refresh {},
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum LiveCodingOp {
    /// Whether Live Coding is available, enabled, started and compiling,
    /// and whether the older hot reload is compiling.
    Status {},
    /// Enable (or disable) Live Coding for this editor session.
    Enable { enabled: Option<bool> },
    /// Compile changed C++ and patch it into the running editor (Ctrl+Alt+F11).
    /// Reports success, no_changes, failure, or in_progress when not waiting.
    Compile {
        /// Block until the patch is applied (default true).
        wait: Option<bool>,
    },
    /// The older hot reload: recompile the game modules and reload them.
    HotReload { wait: Option<bool> },
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum DdcOp {
    /// The Derived Data Cache graph in use (local, shared, Zen, ...) and, per
    /// node, hits, misses, bytes and time for gets, puts, exists and
    /// prefetches in this session.
    Stats {},
}

#[tool_router(router = workflow_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Content browser collections: list them, create (local, private or shared; static or dynamic), destroy, add and remove assets, and read a collection's assets. Collections are how a team tags sets of assets outside the folder tree."
    )]
    async fn collection_ops(
        &self,
        Parameters(op): Parameters<CollectionOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/workflow/collections", body)
            .await
            .map(Json)
    }

    #[tool(
        description = "Editor Utility Blueprints and Widgets: list them, run an Editor Utility Blueprint's Run entry point, and open or close an Editor Utility Widget in a tab (the live widget is then reachable with get_property / call_function). Opening a widget needs a windowed editor."
    )]
    async fn editor_utility_ops(
        &self,
        Parameters(op): Parameters<EditorUtilityOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/workflow/editor_utilities", body)
            .await
            .map(Json)
    }

    #[tool(
        description = "The Asset Manager: primary asset types with their scan paths, the primary assets of a type, one asset's cook and chunk rules, Primary Asset Label creation (tag assets or a whole folder with cook rules and a chunk for packaging), and a rescan."
    )]
    async fn asset_manager_ops(
        &self,
        Parameters(op): Parameters<AssetManagerOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/workflow/asset_manager", body)
            .await
            .map(Json)
    }

    #[tool(
        description = "Live Coding and hot reload from inside the running editor: report status, enable Live Coding for the session, trigger a compile that patches the editor (the Ctrl+Alt+F11 build) and wait for its result, or run the older hot reload. For a full build from outside the editor use build_project."
    )]
    async fn live_coding_ops(
        &self,
        Parameters(op): Parameters<LiveCodingOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let timeout = std::time::Duration::from_secs(600);
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin_with_timeout("/api/workflow/live_coding", body, timeout)
            .await
            .map(Json)
    }

    #[tool(
        description = "Derived Data Cache statistics: which cache graph the editor uses (local, shared, Zen) and, per cache node, this session's hits, misses, bytes and time for gets, puts, exists and prefetches — the numbers behind slow first loads and shader or texture builds."
    )]
    async fn ddc_ops(&self, Parameters(op): Parameters<DdcOp>) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/workflow/ddc", body).await.map(Json)
    }

    #[tool(
        description = "Source control through whatever provider the project configured (Perforce, Git, Plastic, ...): report the connection and each file's real state, check out, mark for add or delete, revert, sync, and submit with a description. McpLink writes and saves assets directly, so this is what keeps those edits from being invisible to the rest of a team. status is safe to call when source control is off — it says so."
    )]
    async fn source_control_ops(
        &self,
        Parameters(op): Parameters<SourceControlOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/workflow/source_control", body)
            .await
            .map(Json)
    }

    #[tool(
        description = "The checks the editor runs before you ship: Data Validation over named assets or a whole folder, Map Check on the current level, and compiling every Blueprint under a path with the failures named. An agent has no equivalent of a human noticing a red message log, so this is how authored content gets verified."
    )]
    async fn validate_ops(
        &self,
        Parameters(op): Parameters<ValidateOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/workflow/validate", body)
            .await
            .map(Json)
    }

    #[tool(
        description = "Gameplay tags: list the project's tags (all, filtered, or under a parent), and add, rename or remove one. Tags live in the project's tag ini rather than in an asset, and only the editor's tag module keeps the ini, the tag manager and the tag sources in step — which is why config_ops is not the right tool for them."
    )]
    async fn gameplay_tag_ops(
        &self,
        Parameters(op): Parameters<GameplayTagOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/workflow/tags", body).await.map(Json)
    }

    #[tool(
        description = "Keyframes on Curve assets (CurveFloat, CurveVector, CurveLinearColor): read every channel's keys, replace or extend them with times, values and per-key interpolation, or clear a channel. asset_ops create makes the asset; this is what fills it in."
    )]
    async fn curve_ops(
        &self,
        Parameters(op): Parameters<CurveOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/workflow/curves", body)
            .await
            .map(Json)
    }

    #[tool(
        description = "The reference graph, as the Reference Viewer and Size Map show it: what references an asset (what breaks if it goes), what it depends on, and the deduplicated on-disk size of its whole dependency tree with the largest contributors named. get_asset_info covers one asset's direct dependencies; this covers the other two directions."
    )]
    async fn reference_ops(
        &self,
        Parameters(op): Parameters<ReferenceOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/workflow/references", body)
            .await
            .map(Json)
    }
}

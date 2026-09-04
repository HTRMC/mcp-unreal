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

#[tool_router(router = workflow_router, vis = "pub(crate)")]
impl UnrealMcp {
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

//! Motion Matching (Pose Search) interop (requires the McpLinkPoseSearch
//! plugin).
//!
//! Variants serialise straight into the plugin request body, so field names
//! here are the contract with `McpPoseSearchRoutes.cpp`.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum PoseSearchOp {
    /// Feature channel classes with their default properties.
    ListChannels,
    /// New Pose Search Schema for a skeleton; seeds the editor's default
    /// channels (pose + trajectory) unless `default_channels` is false.
    CreateSchema {
        /// e.g. /Game/MM/PSS_Hero.
        path: String,
        skeleton: String,
        mirror_table: Option<String>,
        /// Role name for multi-character schemas (default role otherwise).
        role: Option<String>,
        /// Samples per second (default 30).
        sample_rate: Option<i32>,
        default_channels: Option<bool>,
    },
    /// Skeletons, sample rate and channels with their properties.
    SchemaInfo {
        schema: String,
    },
    /// Add a channel by class ("Position", "Velocity", "Heading",
    /// "Trajectory", "Pose", …) with its editable properties, e.g.
    /// {"Bone": {"BoneName": "foot_l"}, "SampleTimeOffset": 0.2}.
    AddChannel {
        schema: String,
        channel: String,
        properties: Option<Value>,
    },
    SetChannel {
        schema: String,
        /// Channel index from schema_info.
        index: i32,
        properties: Value,
    },
    RemoveChannel {
        schema: String,
        index: i32,
    },
    SaveSchema {
        schema: String,
    },
    /// New Pose Search Database on a schema.
    CreateDatabase {
        /// e.g. /Game/MM/PSD_Hero.
        path: String,
        schema: String,
        /// Editable database properties, e.g. {"PoseSearchMode": "PCAKDTree"}.
        properties: Option<Value>,
    },
    /// Schema, animation entries and whether an index is built.
    DatabaseInfo {
        database: String,
    },
    /// Change the schema or database properties.
    SetDatabase {
        database: String,
        schema: Option<String>,
        properties: Option<Value>,
    },
    /// Add an Animation Sequence, Composite, Montage or Blend Space entry;
    /// `properties` are the entry's fields (bEnabled, MirrorOption
    /// "UnmirroredOnly" | "MirroredOnly" | "UnmirroredAndMirrored",
    /// SamplingRange {Min, Max}, bDisableReselection).
    AddAnimation {
        database: String,
        animation: String,
        properties: Option<Value>,
    },
    SetAnimation {
        database: String,
        /// Entry index from database_info.
        index: i32,
        properties: Value,
    },
    RemoveAnimation {
        database: String,
        index: i32,
    },
    /// Build (or rebuild) the search index; waits for completion unless
    /// `wait` is false.
    BuildIndex {
        database: String,
        wait: Option<bool>,
    },
    SaveDatabase {
        database: String,
    },
}

#[tool_router(router = pose_search_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Motion Matching (needs McpLinkPoseSearch): create Pose Search Schemas with feature channels (pose, trajectory, position, velocity, heading, …), create Pose Search Databases on a schema, add animation entries with mirroring and sampling ranges, build the search index and save."
    )]
    async fn pose_search_ops(
        &self,
        Parameters(op): Parameters<PoseSearchOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin_with_timeout(
            "/api/anim/pose_search",
            body,
            std::time::Duration::from_secs(600),
        )
        .await
        .map(Json)
    }
}

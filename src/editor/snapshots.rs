//! Level Snapshots interop (requires the McpLinkLevelSnapshots plugin).
//!
//! Variants serialise straight into the plugin request body, so field names
//! here are the contract with `McpLevelSnapshotRoutes.cpp`.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum LevelSnapshotOp {
    /// Capture every actor of the world into a new Level Snapshot asset.
    Take {
        /// e.g. /Game/Snapshots/LS_Before.
        path: String,
        name: Option<String>,
        description: Option<String>,
        /// "editor" (default) or "pie".
        world: Option<String>,
    },
    Info {
        snapshot: String,
    },
    /// Actors changed, removed or added since the snapshot was taken.
    Diff {
        snapshot: String,
        world: Option<String>,
        include_unchanged: Option<bool>,
        /// Default 200 per list.
        max_results: Option<i32>,
    },
    /// Restore the world to the snapshot: changed properties, deleted actors
    /// respawned, added actors removed. Undo-able.
    Apply {
        snapshot: String,
        world: Option<String>,
    },
    Save {
        snapshot: String,
    },
}

#[tool_router(router = snapshot_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Level Snapshots (needs McpLinkLevelSnapshots): capture a level's actors into a snapshot asset, list what changed since, and restore the level to it."
    )]
    async fn level_snapshot_ops(
        &self,
        Parameters(op): Parameters<LevelSnapshotOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin_with_timeout(
            "/api/world/snapshot",
            body,
            std::time::Duration::from_secs(300),
        )
        .await
        .map(Json)
    }
}

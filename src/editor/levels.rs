//! `level_ops` — the first operation-based mega-tool.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::{Value, json};

use crate::UnrealMcp;

#[derive(serde::Deserialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum LevelOp {
    /// Name, package and dirty state of the level currently open in the editor.
    GetCurrent {},
    /// All level (UWorld) assets under /Game.
    List {},
    /// Open a level in the editor, discarding unsaved changes.
    Load {
        /// Package path, e.g. "/Game/Maps/TestMap".
        path: String,
    },
    /// Save the level currently open in the editor.
    SaveCurrent {},
    /// Save the current level to a specific package path (creates it if new).
    SaveAs {
        /// Destination package path, e.g. "/Game/Maps/TestMap".
        path: String,
    },
    /// Save every dirty map and content package.
    SaveAll {},
    /// Create a new empty level (replaces the current one in the editor).
    NewLevel {},
}

#[tool_router(router = level_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Level management: get_current, list, load, save_current, save_as, save_all, new_level. The editor does not autosave — save after making changes you want to keep."
    )]
    async fn level_ops(
        &self,
        Parameters(op): Parameters<LevelOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body = match op {
            LevelOp::GetCurrent {} => json!({"operation": "get_current"}),
            LevelOp::List {} => json!({"operation": "list"}),
            LevelOp::Load { path } => json!({"operation": "load", "path": path}),
            LevelOp::SaveCurrent {} => json!({"operation": "save_current"}),
            LevelOp::SaveAs { path } => json!({"operation": "save_as", "path": path}),
            LevelOp::SaveAll {} => json!({"operation": "save_all"}),
            LevelOp::NewLevel {} => json!({"operation": "new_level"}),
        };
        self.call_plugin("/api/levels/ops", body).await.map(Json)
    }
}

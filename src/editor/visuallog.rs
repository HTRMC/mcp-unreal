//! `visual_log_ops` — the Visual Logger, read back as data.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum VisualLogOp {
    /// Start listening. Do this before `pie_control start` — the engine only
    /// logs during a play session.
    Start {
        /// Drop what is already buffered (default true).
        clear: Option<bool>,
    },
    /// Stop recording. The entries stay readable.
    Stop {},
    /// Whether it is recording, how many entries are buffered, and the time
    /// range they cover.
    Status {},
    /// Which actors logged anything, with their classes and entry counts.
    Owners {},
    /// The buffered entries, newest first: log lines, named status blocks, the
    /// shapes the Visual Logger would draw, and events.
    Entries {
        /// Substring of the actor's name or label.
        owner: Option<String>,
        /// Substring of the log category, e.g. "LogBehaviorTree", "LogNavigation".
        category: Option<String>,
        /// Only entries at or after this game time.
        since_time: Option<f64>,
        /// Default 50, max 500.
        max_results: Option<i32>,
    },
    Clear {},
}

#[tool_router(router = visual_log_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Read the Visual Logger as data. UE_VLOG is how engine AI, navigation and movement explain themselves — per-actor, per-frame entries with log lines, named status blocks and the shapes the Visual Logger window draws — and this exposes that stream instead of the window. Start recording, run PIE, then query by actor, category or time. Only code that calls UE_VLOG appears, and only during a play session."
    )]
    async fn visual_log_ops(
        &self,
        Parameters(op): Parameters<VisualLogOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/debug/visual_log", body).await.map(Json)
    }
}

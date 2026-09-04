//! Editor utility tools: console commands and the captured output log.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::{Value, json};

use crate::UnrealMcp;

#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct ConsoleCommandInput {
    /// Console command, e.g. "stat fps", "obj list class=StaticMeshActor", "r.ScreenPercentage 50".
    pub command: String,
    /// Which world: "auto" (default), "pie", "editor".
    pub world: Option<String>,
}

#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct OutputLogInput {
    /// Only entries newer than this sequence number (from a previous call's last_seq).
    /// Use 0 (default) for the most recent entries.
    pub since_seq: Option<u64>,
    /// Max entries returned (default 100, cap 500).
    pub max_lines: Option<u32>,
    /// Only this log category, e.g. "LogTemp", "LogBlueprintUserMessages".
    pub category: Option<String>,
    /// Minimum severity: "error", "warning", "display", "log" (default), "verbose".
    pub min_verbosity: Option<String>,
}

#[tool_router(router = editor_utils_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Execute an Unreal console command in the editor and return its captured output."
    )]
    async fn run_console_command(
        &self,
        Parameters(input): Parameters<ConsoleCommandInput>,
    ) -> Result<Json<Value>, ErrorData> {
        self.call_plugin(
            "/api/editor/console_command",
            json!({"command": input.command, "world": input.world}),
        )
        .await
        .map(Json)
    }

    #[tool(
        description = "Read the editor's output log, filterable by category and severity. Poll incrementally by passing the previous last_seq as since_seq."
    )]
    async fn get_output_log(
        &self,
        Parameters(input): Parameters<OutputLogInput>,
    ) -> Result<Json<Value>, ErrorData> {
        self.call_plugin(
            "/api/editor/output_log",
            json!({
                "since_seq": input.since_seq,
                "max_lines": input.max_lines,
                "category": input.category,
                "min_verbosity": input.min_verbosity,
            }),
        )
        .await
        .map(Json)
    }
}

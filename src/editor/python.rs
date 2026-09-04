//! Editor Python escape hatch (requires the McpLinkPython plugin, which pulls
//! in the Python Editor Script Plugin).

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
pub struct PythonExecInput {
    /// Python source to run in the editor's interpreter (multi-line allowed).
    /// `import unreal` is available; print() output comes back in `output`.
    pub code: Option<String>,
    /// Path to a .py file to execute instead of `code`.
    pub file: Option<String>,
    /// "exec" (default) runs statements; "eval" evaluates one expression and
    /// returns its repr in `result`.
    pub mode: Option<String>,
    /// Keep names between calls (runs in __main__ instead of a throwaway scope).
    pub persist: Option<bool>,
}

#[tool_router(router = python_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Run Python inside the Unreal Editor (the `unreal` module) and get its output back. The escape hatch for anything without a dedicated tool: editor utilities, asset batch edits, Sequencer, Landscape, etc. A Python exception fails the call with the traceback. Needs the McpLinkPython plugin (enables the Python Editor Script Plugin)."
    )]
    async fn python_exec(
        &self,
        Parameters(input): Parameters<PythonExecInput>,
    ) -> Result<Json<Value>, ErrorData> {
        let body = serde_json::to_value(input)
            .map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/python/exec", body).await.map(Json)
    }
}

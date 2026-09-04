//! `ui_ops` — asset editors: opening, closing and listing them.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum UiOp {
    /// Open an asset's editor. Some engine code only finishes an asset when
    /// its editor constructs itself — a fresh Material Layer gets its input
    /// and output nodes that way.
    OpenAsset { asset: String },
    CloseAsset { asset: String },
    CloseAll {},
    /// Which assets currently have an editor open. An open editor also lists
    /// its own transient preview objects.
    ListOpen {},
}

#[tool_router(router = ui_ops_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Open, close and list asset editors. Worth doing on its own rather than for the window: some assets are only finished off when their editor constructs them, so opening one can be the step that makes an asset complete. ui_query reads the editor's widget tree; clicking it is not available."
    )]
    async fn ui_ops(&self, Parameters(op): Parameters<UiOp>) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/editor/ui_ops", body).await.map(Json)
    }
}

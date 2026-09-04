pub mod budget;
pub mod config;
pub mod docs;
pub mod editor;
pub mod error;
pub mod headless;
pub mod schema;
pub mod status;

use std::sync::Arc;

use rmcp::ServerHandler;
use rmcp::handler::server::router::tool::ToolRouter;
use serde_json::Value;

use crate::config::Config;
use crate::editor::client::EditorClient;

/// The MCP server: one struct, tool routers combined per domain.
pub struct UnrealMcp {
    pub(crate) cfg: Arc<Config>,
    pub(crate) editor: EditorClient,
    pub(crate) docs: crate::docs::DocsIndex,
    tool_router: ToolRouter<Self>,
}

impl UnrealMcp {
    pub fn new(cfg: Config) -> Self {
        let editor = EditorClient::new(cfg.plugin_port);
        let cfg = Arc::new(cfg);
        Self {
            docs: crate::docs::DocsIndex::new(Arc::clone(&cfg)),
            cfg,
            editor,
            tool_router: Self::status_router()
                + Self::headless_build_router()
                + Self::headless_test_router()
                + Self::headless_log_router()
                + Self::headless_cook_router()
                + Self::headless_package_router()
                + Self::headless_code_router()
                + Self::headless_trace_router()
                + Self::localization_router()
                + Self::project_ops_router()
                + Self::object_router()
                + Self::actor_router()
                + Self::level_router()
                + Self::asset_router()
                + Self::ai_router()
                + Self::navigation_router()
                + Self::audio_router()
                + Self::mesh_router()
                + Self::perf_router()
                + Self::user_type_router()
                + Self::editor_utils_router()
                + Self::play_router()
                + Self::capture_router()
                + Self::docs_router()
                + Self::blueprint_router()
                + Self::anim_blueprint_router()
                + Self::animation_router()
                + Self::content_router()
                + Self::introspect_router()
                + Self::chaos_router()
                + Self::visual_log_router()
                + Self::metasound_router()
                + Self::movie_render_router()
                + Self::niagara_router()
                + Self::gas_router()
                + Self::pcg_router()
                + Self::python_router()
                + Self::material_graph_router()
                + Self::render_router()
                + Self::widget_blueprint_router()
                + Self::sequence_router()
                + Self::world_router()
                + Self::workflow_router(),
        }
    }

    /// POST to a plugin endpoint, mapping transport/API failures into MCP
    /// errors that name the remedy. Null fields are stripped so the plugin
    /// sees "absent" rather than JSON null for optional parameters.
    pub(crate) async fn call_plugin(
        &self,
        path: &str,
        mut body: Value,
    ) -> Result<Value, rmcp::ErrorData> {
        if let Some(obj) = body.as_object_mut() {
            obj.retain(|_, v| !v.is_null());
        }
        self.editor
            .post(path, body)
            .await
            .map_err(|e| e.into_tool_error(self.cfg.plugin_port))
    }
}

const INSTRUCTIONS: &str = "MCP server for Unreal Engine 5.8. Call the `status` tool first to see \
which features are currently available (headless build/test always work when UE is installed; \
editor tools need the Unreal Editor running with the McpLink plugin). Headless builds and test \
runs take minutes — do not assume they hung.";

#[rmcp::tool_handler(router = self.tool_router)]
impl ServerHandler for UnrealMcp {
    fn get_info(&self) -> rmcp::model::ServerInfo {
        let mut info = rmcp::model::ServerInfo::default();
        info.capabilities = rmcp::model::ServerCapabilities::builder()
            .enable_tools()
            .build();
        info.server_info.name = env!("CARGO_PKG_NAME").into();
        info.server_info.version = env!("CARGO_PKG_VERSION").into();
        info.instructions = Some(INSTRUCTIONS.into());
        info
    }
}

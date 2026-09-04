//! Read-only introspection: subsystems and the Slate / UMG widget trees.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::{Value, json};

use crate::UnrealMcp;

#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct SubsystemQueryInput {
    /// "all" (default), "engine", "editor", "world", "game_instance", or "local_player".
    pub kind: Option<String>,
    /// Which world for world/game_instance/local_player kinds: "auto", "pie", "editor".
    pub world: Option<String>,
}

#[derive(serde::Deserialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum UiQuery {
    /// Top-level Slate windows with index, title, size and active state.
    Windows {},
    /// Depth-first dump of a window's Slate widget tree (type, visibility, size,
    /// and where each widget was constructed). Defaults to the PIE preview
    /// window when playing, else the main editor window.
    Tree {
        /// Window index from `windows`, or a title substring.
        window: Option<Value>,
        /// Default 8.
        max_depth: Option<u32>,
        /// Default 300 — the response is truncated past this.
        max_nodes: Option<u32>,
    },
    /// Every Slate widget whose type contains `type`, e.g. "SButton", "STextBlock".
    Find {
        #[serde(rename = "type")]
        widget_type: String,
        window: Option<Value>,
        max_nodes: Option<u32>,
    },
    /// UMG user widgets alive in the world, with their child widgets and any
    /// TextBlock text — the way to read a HUD without a screenshot. Works headless.
    Umg {
        world: Option<String>,
        /// Default 200.
        max_widgets: Option<u32>,
    },
}

#[tool_router(router = introspect_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "List the active UE subsystems (engine, editor, world, game instance, local player) with class and module. Useful for discovering what systems a project has enabled."
    )]
    async fn subsystem_query(
        &self,
        Parameters(input): Parameters<SubsystemQueryInput>,
    ) -> Result<Json<Value>, ErrorData> {
        self.call_plugin(
            "/api/subsystems/query",
            json!({"kind": input.kind, "world": input.world}),
        )
        .await
        .map(Json)
    }

    #[tool(
        description = "Inspect the UI: list Slate windows, dump or search a window's widget tree, or read the UMG user widgets in the world including their text. Slate operations need a windowed editor; 'umg' also works headless."
    )]
    async fn ui_query(
        &self,
        Parameters(op): Parameters<UiQuery>,
    ) -> Result<Json<Value>, ErrorData> {
        let body = match op {
            UiQuery::Windows {} => json!({"operation": "windows"}),
            UiQuery::Tree {
                window,
                max_depth,
                max_nodes,
            } => json!({
                "operation": "tree", "window": window,
                "max_depth": max_depth, "max_nodes": max_nodes,
            }),
            UiQuery::Find {
                widget_type,
                window,
                max_nodes,
            } => json!({
                "operation": "find", "type": widget_type,
                "window": window, "max_nodes": max_nodes,
            }),
            UiQuery::Umg { world, max_widgets } => {
                json!({"operation": "umg", "world": world, "max_widgets": max_widgets})
            }
        };
        self.call_plugin("/api/ui/query", body).await.map(Json)
    }
}

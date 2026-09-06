//! Movie Render Queue (requires the McpLinkMovieRender plugin in the project).
//!
//! Variants serialise straight into the plugin request body, so the field
//! names here are the contract with `McpMovieRenderRoutes.cpp`.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum MovieRenderOp {
    /// Setting classes a config can hold — render passes, output types
    /// (PNG/JPG/EXR/WAV/encoder), anti-aliasing, burn-ins, CVar overrides.
    ListSettingClasses {
        name_contains: Option<String>,
    },
    /// New Movie Pipeline config, seeded with an output setting plus a render
    /// pass and output type (a config missing either produces no files).
    CreateConfig {
        /// e.g. /Game/Cinematics/MRQ_Preview.
        path: String,
        /// Default "MoviePipelineDeferredPassBase".
        render_pass: Option<String>,
        /// Default "MoviePipelineImageSequenceOutput_PNG".
        output_type: Option<String>,
    },
    /// The config's settings with their paths, plus the resolved output
    /// directory, file-name format and resolution.
    ConfigInfo {
        config: String,
    },
    AddSetting {
        config: String,
        class: String,
    },
    RemoveSetting {
        config: String,
        class: String,
    },
    /// Write a config or a graph asset to disk.
    Save {
        config: Option<String>,
        graph: Option<String>,
    },
    /// New Movie Render Graph asset — the node-based pipeline. Seeded from
    /// the engine's default graph (input → global output settings →
    /// deferred pass → PNG output → output), so it renders as-is; pass
    /// from_default=false for a graph with only its Input and Output nodes.
    CreateGraph {
        /// e.g. /Game/Cinematics/MRG_Preview.
        path: String,
        from_default: Option<bool>,
    },
    /// Node classes a graph can hold: render passes, output formats,
    /// global settings, render layers, modifiers, CVars, branches.
    ListGraphNodeClasses {
        name_contains: Option<String>,
    },
    /// Every node with its pins and connections (as "Node.Pin"), each
    /// node's overridable properties with their values and override state,
    /// and the graph's variables.
    GraphInfo {
        graph: String,
        include_properties: Option<bool>,
    },
    /// Add a node. `class` is a node class ("MovieGraphDeferredRenderPassNode",
    /// or just "DeferredRenderPass"); it is unconnected until wired in.
    AddGraphNode {
        graph: String,
        class: String,
        x: Option<i32>,
        y: Option<i32>,
    },
    RemoveGraphNode {
        graph: String,
        /// Node name, GUID, class or title from graph_info.
        node: String,
    },
    /// Wire two nodes. Nodes are named as in graph_info, or "Input" /
    /// "Output" for the graph's own; pins default to the first on each side
    /// ("Globals" on a settings chain).
    ConnectGraphNodes {
        graph: String,
        from: String,
        from_pin: Option<String>,
        to: String,
        to_pin: Option<String>,
    },
    DisconnectGraphNodes {
        graph: String,
        from: String,
        from_pin: Option<String>,
        to: String,
        to_pin: Option<String>,
    },
    /// Set a node's settings and switch on their override checkboxes in the
    /// same step (a value without its override changes nothing at render
    /// time). Values are JSON for the property: numbers, strings, enum names,
    /// objects for structs such as {"ProfileName": "1080p (FHD)"} for
    /// OutputResolution.
    SetGraphNodeProperties {
        graph: String,
        node: String,
        properties: Value,
    },
    /// Queue a render and start it. Asynchronous — poll `render_status`.
    /// Needs a windowed editor, since the render runs through PIE. Pass a
    /// legacy `config` or a Movie Render `graph`.
    Render {
        config: Option<String>,
        graph: Option<String>,
        /// Level Sequence asset path.
        sequence: String,
        /// Map to render on; the open level when omitted.
        map: Option<String>,
        name: Option<String>,
        /// Drop any jobs already queued (default true).
        clear_queue: Option<bool>,
    },
    /// Whether a render is running, and each queued job's progress.
    RenderStatus {},
}

#[tool_router(router = movie_render_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Movie Render Queue — turning an authored Level Sequence into files on disk, which sequence_ops could not do. Two pipelines: the legacy config (create_config: render pass plus output type, tuned with set_property on the reported paths) and the node-based Movie Render Graph (create_graph seeded from the engine's default graph, then list_graph_node_classes, graph_info, add_graph_node, connect_graph_nodes and set_graph_node_properties, which also flips the override checkboxes). Then render a sequence on a map with either and poll for progress. The render runs through PIE, so it needs a windowed editor. Needs the McpLinkMovieRender plugin enabled."
    )]
    async fn movie_render(
        &self,
        Parameters(op): Parameters<MovieRenderOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/render/movie", body).await.map(Json)
    }
}

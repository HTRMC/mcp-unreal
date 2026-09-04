//! Procedural Content Generation interop (requires the McpLinkPCG plugin).
//!
//! Variants serialise straight into the plugin request body, so field names
//! here are the contract with `McpPcgRoutes.cpp`.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum PcgOp {
    /// PCG graph (and graph instance) assets in the project.
    ListGraphs { path_prefix: Option<String> },
    /// Node settings classes available for add_node, with their node titles.
    ListSettingsClasses {
        /// Substring filter on class name or title, e.g. "Sampler".
        filter: Option<String>,
    },
    /// Every PCG component in a world with graph, generation state and seed.
    ListComponents {
        /// "auto" (default: PIE if running, else editor), "pie", or "editor".
        world: Option<String>,
    },
    /// Create an empty PCG graph asset (in memory — `save` to persist).
    CreateGraph {
        /// Destination package path, e.g. "/Game/PCG/PCG_Forest".
        path: String,
    },
    /// Nodes (with settings_path for get/set_property), pins, edges and user parameters.
    GraphInfo { graph: String },
    /// Add a node. `settings_class` is e.g. "PCGSurfaceSamplerSettings" (the
    /// "PCG…Settings" wrapping is optional: "SurfaceSampler" works too).
    AddNode {
        graph: String,
        settings_class: String,
        x: Option<i32>,
        y: Option<i32>,
    },
    RemoveNode {
        graph: String,
        /// Node id from graph_info.
        node: String,
    },
    /// Connect two nodes. Pins default to the first output / first input; the
    /// graph's own nodes are addressed as "Input" and "Output".
    Connect {
        graph: String,
        from: String,
        from_pin: Option<String>,
        to: String,
        to_pin: Option<String>,
    },
    Disconnect {
        graph: String,
        from: String,
        from_pin: Option<String>,
        to: String,
        to_pin: Option<String>,
    },
    /// Write the graph package to disk.
    Save { graph: String },
    /// Add a PCG component (set to generate on demand) to an actor.
    Attach {
        /// Actor path or editor label.
        actor: String,
        world: Option<String>,
        /// Graph asset to assign.
        graph: Option<String>,
        seed: Option<i32>,
    },
    ComponentInfo {
        /// Component object path, or an actor path/label (first PCG component on it).
        component: Option<String>,
        actor: Option<String>,
        world: Option<String>,
    },
    /// Schedule generation (asynchronous: poll component_info until `generating` is false).
    Generate {
        component: Option<String>,
        actor: Option<String>,
        world: Option<String>,
        /// Regenerate even if nothing changed (default true).
        force: Option<bool>,
    },
    /// Remove generated output.
    Cleanup {
        component: Option<String>,
        actor: Option<String>,
        world: Option<String>,
        remove_components: Option<bool>,
    },
    SetGraph {
        component: Option<String>,
        actor: Option<String>,
        world: Option<String>,
        graph: String,
    },
    SetSeed {
        component: Option<String>,
        actor: Option<String>,
        world: Option<String>,
        seed: i32,
    },
    /// Override a graph user parameter on the component's graph instance.
    SetParameter {
        component: Option<String>,
        actor: Option<String>,
        world: Option<String>,
        name: String,
        /// Number, bool, string, or [x,y,z] array (vectors).
        value: Value,
    },
}

#[tool_router(router = pcg_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Procedural Content Generation: author PCG graphs (create, add/remove nodes from ~200 settings classes, connect pins, save), attach PCG components to actors, set graph/seed/parameters, generate or clean up, and inspect components and graph structure. Per-node options are edited with set_property on each node's settings_path. Needs the McpLinkPCG plugin enabled in the project."
    )]
    async fn pcg_ops(&self, Parameters(op): Parameters<PcgOp>) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/pcg/ops", body).await.map(Json)
    }
}

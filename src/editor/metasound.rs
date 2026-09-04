//! MetaSound authoring (requires the McpLinkMetaSound plugin in the project).
//!
//! Variants serialise straight into the plugin request body, so the field
//! names here are the contract with `McpMetaSoundRoutes.cpp`.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum MetaSoundOp {
    /// Node classes the project has registered, as "Namespace.Name" — the form
    /// `add_node` takes.
    ListNodeClasses {
        name_contains: Option<String>,
        max_results: Option<u32>,
    },
    /// New MetaSound asset with its default interface already wired.
    Create {
        /// e.g. /Game/Audio/MS_Beep.
        path: String,
        /// "source" (default, playable) or "patch" (reusable graph).
        kind: Option<String>,
        /// Source only: "Mono" (default), "Stereo", "Quad", "FiveDotOne" or
        /// "SevenDotOne".
        output_format: Option<String>,
        /// Source only: finish after one pass rather than looping (default true).
        one_shot: Option<bool>,
        author: Option<String>,
    },
    /// The graph: every node with its class and vertex names, plus the graph's
    /// own inputs and outputs.
    Info {
        metasound: String,
        max_nodes: Option<u32>,
    },
    /// Add a node. Returns its id and its input/output vertex names.
    AddNode {
        metasound: String,
        /// "Namespace.Name" from list_node_classes, e.g. "UE.Sine".
        class: String,
        major_version: Option<i32>,
    },
    RemoveNode {
        metasound: String,
        /// Node id from add_node or info.
        node: String,
    },
    /// Wire one node's output into another node's input.
    Connect {
        metasound: String,
        from_node: String,
        from_output: String,
        to_node: String,
        to_input: String,
    },
    /// Wire a node output into one of the graph's outputs, e.g. "Out Mono".
    ConnectToGraphOutput {
        metasound: String,
        node: String,
        output: String,
        graph_vertex: String,
    },
    /// Wire one of the graph's inputs into a node input.
    ConnectFromGraphInput {
        metasound: String,
        node: String,
        input: String,
        graph_vertex: String,
    },
    /// Add an input to the graph itself — a parameter the game can set.
    AddGraphInput {
        metasound: String,
        name: String,
        /// MetaSound data type: Float, Int32, Bool, String, Audio, Trigger, ...
        data_type: String,
        default: Option<Value>,
        /// Constructor inputs are fixed when the sound is built, not per-frame.
        constructor: Option<bool>,
    },
    AddGraphOutput {
        metasound: String,
        name: String,
        data_type: String,
        default: Option<Value>,
        constructor: Option<bool>,
    },
    /// Literal value for an unconnected node input.
    SetNodeInputDefault {
        metasound: String,
        node: String,
        input: String,
        /// Number, bool, string, or an asset path.
        value: Value,
    },
    Save {
        metasound: String,
    },
}

#[tool_router(router = metasound_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Author MetaSounds: create a Source or Patch asset with its interface wired, discover the node classes the project has registered, add and remove nodes, connect them to each other and to the graph's own inputs and outputs, add graph inputs and outputs (the parameters the game drives), set literal defaults on unconnected inputs, and save. Sound Cues have sound_cue_ops; this is the modern graph. Needs the McpLinkMetaSound plugin enabled."
    )]
    async fn metasound_ops(
        &self,
        Parameters(op): Parameters<MetaSoundOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/audio/metasound", body)
            .await
            .map(Json)
    }
}

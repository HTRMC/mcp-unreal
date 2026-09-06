//! Control Rig graph (RigVM) authoring (requires the McpLinkControlRig
//! plugin).
//!
//! Variants serialise straight into the plugin request body, so field names
//! here are the contract with `McpRigVmGraphRoutes.cpp`.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
#[allow(clippy::large_enum_variant)]
pub enum RigVmGraphOp {
    /// Registered unit structs (with display name, category and template)
    /// and template notations; `filter` narrows by name, category or keyword.
    ListNodeTypes {
        filter: Option<String>,
        /// Max entries per list (default 200).
        max: Option<i32>,
    },
    /// The rig's graphs (default model, functions, collapsed nodes) and its
    /// variables.
    ListGraphs { rig: String },
    /// Nodes, pins (with links) and links of a graph.
    ListNodes {
        rig: String,
        /// Graph node path from list_graphs (default: the main graph).
        graph: Option<String>,
        /// Include every node's pins (default true).
        pins: Option<bool>,
    },
    NodeInfo {
        rig: String,
        graph: Option<String>,
        node: String,
    },
    /// Add a node: a unit `struct` ("/Script/ControlRig.RigUnit_SetTransform",
    /// "RigUnit_SetTransform" or its display name), a `template` notation
    /// ("Add(in A,in B,out Result)"), a `variable` getter/setter, a
    /// `comment`, or kind = branch | if | select (with cpp_type). `defaults`
    /// sets pin defaults by pin name as RigVM text.
    AddNode {
        rig: String,
        graph: Option<String>,
        #[serde(rename = "struct")]
        struct_: Option<String>,
        template: Option<String>,
        variable: Option<String>,
        /// Variable node reads (default) or writes.
        getter: Option<bool>,
        comment: Option<String>,
        kind: Option<String>,
        cpp_type: Option<String>,
        cpp_type_object: Option<String>,
        /// Unit method (default "Execute").
        method: Option<String>,
        name: Option<String>,
        position: Option<[f64; 2]>,
        default: Option<String>,
        defaults: Option<Value>,
    },
    RemoveNode {
        rig: String,
        graph: Option<String>,
        node: String,
    },
    RenameNode {
        rig: String,
        graph: Option<String>,
        node: String,
        new_name: String,
    },
    SetNodePosition {
        rig: String,
        graph: Option<String>,
        node: String,
        position: [f64; 2],
    },
    /// Set a pin's default as RigVM text: "1.0", "true", "(X=1,Y=0,Z=0)",
    /// "(Type=Bone,Name=spine_01)".
    SetPinDefault {
        rig: String,
        graph: Option<String>,
        /// Pin path such as SetTransform.Value.Translation.X.
        pin: String,
        value: Value,
    },
    /// Link an output pin to an input pin (execute pins included).
    LinkPins {
        rig: String,
        graph: Option<String>,
        from: String,
        to: String,
    },
    UnlinkPins {
        rig: String,
        graph: Option<String>,
        from: String,
        to: String,
    },
    BreakAllLinks {
        rig: String,
        graph: Option<String>,
        pin: String,
        /// Break the links into the pin (default) or out of it.
        as_input: Option<bool>,
    },
    /// Add a rig variable ("float", "bool", "int32", "FVector", "FTransform",
    /// "TArray<FVector>", or a struct/enum path).
    AddVariable {
        rig: String,
        name: String,
        cpp_type: String,
        default: Option<String>,
        public: Option<bool>,
        read_only: Option<bool>,
    },
    /// Recompile the VM and report the status and compile log.
    Compile { rig: String },
}

#[tool_router(router = rigvm_graph_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Control Rig graph authoring (needs McpLinkControlRig): list unit structs and templates, list a rig's graphs and nodes with pins and links, add unit / template / variable / comment / branch / if / select nodes, link pins, set pin defaults, add rig variables and recompile the VM with its log."
    )]
    async fn rigvm_graph_ops(
        &self,
        Parameters(op): Parameters<RigVmGraphOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/anim/rigvm_graph", body)
            .await
            .map(Json)
    }
}

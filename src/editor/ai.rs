//! AI authoring: Blackboard assets and Behavior Trees.
//!
//! Variants serialise straight into the plugin request body, so the field
//! names here are the contract with `McpBlackboardRoutes.cpp` /
//! `McpBehaviorTreeRoutes.cpp`.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum BlackboardOp {
    /// New Blackboard asset.
    Create {
        /// e.g. /Game/AI/BB_Guard.
        path: String,
    },
    /// A blackboard's own keys and the ones it inherits from its parent.
    GetKeys {
        blackboard: String,
    },
    /// Add a key. The reported `key_type_path` is where set_property reaches
    /// the key's own options.
    AddKey {
        blackboard: String,
        name: String,
        /// bool, int, float, string, name, vector, rotator,
        /// "object:<Class>", "class:<Class>" or "enum:<Enum>".
        #[serde(rename = "type")]
        key_type: String,
        /// Share this key's value across every instance of the blackboard.
        instance_synced: Option<bool>,
        description: Option<String>,
    },
    /// Add the key, or replace an existing key's type.
    SetKey {
        blackboard: String,
        name: String,
        #[serde(rename = "type")]
        key_type: String,
        instance_synced: Option<bool>,
        description: Option<String>,
    },
    RemoveKey {
        blackboard: String,
        name: String,
    },
    /// Inherit keys from another Blackboard, or pass no parent to clear it.
    SetParent {
        blackboard: String,
        parent: Option<String>,
    },
    Save {
        blackboard: String,
    },
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum BehaviorTreeOp {
    /// New Behavior Tree asset, with its editor graph and root node.
    Create {
        /// e.g. /Game/AI/BT_Guard.
        path: String,
        /// Blackboard asset to drive it.
        blackboard: Option<String>,
    },
    /// Every task, composite, decorator and service class available in this
    /// project, native and Blueprint. Start here: `class` values from this
    /// list are what add_node takes.
    ListNodeClasses {
        /// "task", "composite", "decorator" or "service"; all kinds when omitted.
        kind: Option<String>,
        name_contains: Option<String>,
    },
    /// The tree's nodes with their ids, children and attached sub-nodes. Node
    /// options live on the reported `instance` path — set_property edits them.
    GetTree {
        tree: String,
    },
    SetBlackboard {
        tree: String,
        blackboard: String,
    },
    /// Add a node. Composites and tasks are wired under `parent` (the root
    /// when omitted); decorators and services attach to `parent` as sub-nodes,
    /// so `parent` is required for those.
    AddNode {
        tree: String,
        /// Node class from list_node_classes, e.g.
        /// "/Script/AIModule.BTComposite_Selector", "BTTask_MoveTo".
        class: String,
        /// Node id from get_tree.
        parent: Option<String>,
        x: Option<i32>,
        y: Option<i32>,
    },
    RemoveNode {
        tree: String,
        /// Node id from get_tree.
        node: String,
    },
    /// Rebuild the runtime tree from the graph and report whether it produced
    /// a root node.
    Compile {
        tree: String,
    },
    Save {
        tree: String,
    },
}

#[tool_router(router = ai_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Author Blackboard assets: create one, add, retype and remove keys (bool, int, float, string, name, vector, rotator, object/class references and enums), and set a parent blackboard to inherit keys from. Key-specific options are properties on the reported key_type_path."
    )]
    async fn blackboard_ops(
        &self,
        Parameters(op): Parameters<BlackboardOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/ai/blackboard", body).await.map(Json)
    }

    #[tool(
        description = "Author Behavior Trees: create the asset with its graph and root, discover the task/composite/decorator/service classes this project has, add and remove nodes (composites and tasks wired as children, decorators and services attached to a node), point the tree at a Blackboard, then compile the graph into the runtime tree and save. Configure a node's own settings with set_property on the instance path get_tree reports."
    )]
    async fn behavior_tree_ops(
        &self,
        Parameters(op): Parameters<BehaviorTreeOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/ai/behavior_tree", body)
            .await
            .map(Json)
    }
}

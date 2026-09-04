//! Navigation queries and State Tree authoring.
//!
//! Variants serialise straight into the plugin request body, so the field
//! names here are the contract with `McpNavigationRoutes.cpp` and
//! `McpStateTreeRoutes.cpp`.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum NavigationOp {
    /// The world's navigation data: agent sizes, bounds, and whether a build
    /// is still running.
    Info { world: Option<String> },
    /// Nearest navigable point to a location — the check for "is this spot on
    /// the nav mesh at all".
    ProjectPoint {
        /// [X, Y, Z]; or name an actor instead.
        location: Option<[f64; 3]>,
        actor: Option<String>,
        /// Search box half-size; the nav data's default when omitted.
        extent: Option<[f64; 3]>,
        world: Option<String>,
    },
    /// Path between two points, with its length and waypoints. `partial` true
    /// means the destination was unreachable and the path stops short.
    FindPath {
        start: Option<[f64; 3]>,
        start_actor: Option<String>,
        end: Option<[f64; 3]>,
        end_actor: Option<String>,
        /// Actor whose navigation agent and filter to path as.
        context_actor: Option<String>,
        max_points: Option<i32>,
        world: Option<String>,
    },
    /// Path length only, plus the query result — cheaper than find_path when
    /// the question is just "can this be reached, and how far".
    PathLength {
        start: Option<[f64; 3]>,
        start_actor: Option<String>,
        end: Option<[f64; 3]>,
        end_actor: Option<String>,
        world: Option<String>,
    },
    /// Whether a straight walk between two points stays on the nav mesh.
    Raycast {
        start: Option<[f64; 3]>,
        start_actor: Option<String>,
        end: Option<[f64; 3]>,
        end_actor: Option<String>,
        world: Option<String>,
    },
    /// A random reachable point within a radius — how patrol and wander
    /// behaviours pick a destination.
    RandomPoint {
        origin: Option<[f64; 3]>,
        actor: Option<String>,
        radius: Option<f64>,
        world: Option<String>,
    },
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum StateTreeOp {
    /// Schema classes `create` accepts — the schema decides which tasks and
    /// conditions are legal in the tree.
    ListSchemas { name_contains: Option<String> },
    /// Task, condition and evaluator structs `add_node` accepts.
    ListNodeStructs {
        /// "task", "condition" or "evaluator"; all three when omitted.
        kind: Option<String>,
        name_contains: Option<String>,
        max_results: Option<i32>,
    },
    /// New State Tree asset with a root state.
    Create {
        /// e.g. /Game/AI/ST_Guard.
        path: String,
        /// Default "StateTreeComponentSchema".
        schema: Option<String>,
    },
    /// The state hierarchy with each state's tasks, enter conditions and
    /// transitions, and whether the tree is compiled.
    Info { state_tree: String },
    AddState {
        state_tree: String,
        name: String,
        /// Parent state name or id; the first root state when omitted.
        parent: Option<String>,
        /// "State" (default), "Group", "Linked", "LinkedAsset" or "Subtree".
        state_type: Option<String>,
    },
    RenameState {
        state_tree: String,
        state: String,
        name: String,
    },
    RemoveState { state_tree: String, state: String },
    /// Where a state goes when it finishes, ticks or receives an event.
    AddTransition {
        state_tree: String,
        state: String,
        /// EStateTreeTransitionTrigger name: OnStateCompleted (default),
        /// OnStateSucceeded, OnStateFailed, OnTick, OnEvent.
        trigger: Option<String>,
        /// EStateTreeTransitionType name: GotoState (default), NextState,
        /// Succeeded, Failed, None.
        transition_type: Option<String>,
        /// Target state, required for GotoState.
        target: Option<String>,
    },
    /// Add a task or enter condition to a state, or an evaluator to the tree.
    AddNode {
        state_tree: String,
        /// The state to attach to; still required for an evaluator, which
        /// attaches to the tree itself.
        state: String,
        /// "task", "condition" or "evaluator".
        kind: String,
        /// Struct name from list_node_structs.
        r#struct: String,
    },
    RemoveNode {
        state_tree: String,
        state: String,
        kind: String,
        index: i32,
    },
    /// Compile the tree and report the compiler's messages. An uncompiled
    /// State Tree has empty runtime data and does nothing, so this is what
    /// tells you the tree actually works.
    Compile { state_tree: String },
    Save { state_tree: String },
}

#[tool_router(router = navigation_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Navigation queries against the built nav mesh: report the world's nav data and agent sizes, project a point onto the mesh, path between two points (with waypoints, length, and whether the path is partial), test reachability, raycast for a clear straight walk, and pick a random reachable point. build_level navigation builds the mesh; this is how you find out whether it actually works — a mesh with a hole in it looks exactly like a good one until something tries to walk it."
    )]
    async fn nav_ops(
        &self,
        Parameters(op): Parameters<NavigationOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/ai/navigation", body).await.map(Json)
    }

    #[tool(
        description = "Author State Trees: create the asset with a schema, add and remove states in a hierarchy, add transitions, and attach tasks, enter conditions and evaluators from the structs the project has registered. Then compile — an uncompiled State Tree has empty runtime data and runs nothing, and the compile report names what the schema rejected. behavior_tree_ops is the equivalent for Behavior Trees."
    )]
    async fn state_tree_ops(
        &self,
        Parameters(op): Parameters<StateTreeOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/ai/statetree", body).await.map(Json)
    }
}

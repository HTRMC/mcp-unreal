//! Animation Blueprint authoring: state machines, states, transitions.
//!
//! The variants serialise straight into the plugin request body (the
//! `operation` tag plus snake_case fields), so field names here are the
//! contract with `McpAnimBlueprintRoutes.cpp`.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum AnimBlueprintQuery {
    /// Find Animation Blueprint assets in the project.
    List {
        /// Package path to search under (default "/Game").
        path_prefix: Option<String>,
        /// Max results (default 100, cap 500).
        max_results: Option<u32>,
    },
    /// Skeleton, variables, animation graphs, and every state machine with its
    /// states (and the animations they play), entry state and transitions.
    /// Transition GUIDs and graph paths from here feed the modify operations.
    Inspect {
        /// Asset path, e.g. "/Game/Anim/ABP_Hero".
        blueprint: String,
    },
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum AnimBlueprintModify {
    /// Create an Animation Blueprint for a skeleton (in memory — finish with
    /// `blueprint_modify compile` and `save`).
    Create {
        /// Destination package path, e.g. "/Game/Anim/ABP_Hero".
        path: String,
        /// Skeleton asset path; a SkeletalMesh is accepted too (its skeleton is
        /// used and it becomes the preview mesh).
        skeleton: String,
        /// AnimInstance subclass to derive from (default "AnimInstance").
        parent_class: Option<String>,
        /// SkeletalMesh shown in the editor preview.
        preview_mesh: Option<String>,
    },
    /// Add a state machine to an animation graph. Its pose is wired into the
    /// graph's output unless `connect_to_output` is false.
    AddStateMachine {
        blueprint: String,
        /// State machine name, e.g. "Locomotion".
        name: String,
        /// Animation graph to add to: the main "AnimGraph" (default), or a
        /// state's graph path such as "Locomotion/InAir" for a nested machine.
        graph: Option<String>,
        connect_to_output: Option<bool>,
        x: Option<i32>,
        y: Option<i32>,
    },
    /// Add a state, optionally playing an animation and/or as the entry state.
    /// The returned name is the final one (clashes are suffixed).
    AddState {
        blueprint: String,
        /// State machine name or path; optional when the Blueprint has one.
        state_machine: Option<String>,
        name: String,
        /// AnimSequence / AnimComposite / AnimMontage / BlendSpace asset path.
        animation: Option<String>,
        /// Make this the machine's entry state.
        entry: Option<bool>,
        /// Sequence player looping (default true in the engine).
        loop_animation: Option<bool>,
        play_rate: Option<f32>,
        x: Option<i32>,
        y: Option<i32>,
    },
    /// Set (or replace) the animation a state plays. Sequences get a Sequence
    /// Player, blend spaces a Blend Space Player.
    SetStateAnimation {
        blueprint: String,
        state_machine: Option<String>,
        /// State name or node GUID.
        state: String,
        animation: String,
        loop_animation: Option<bool>,
        play_rate: Option<f32>,
    },
    SetEntryState {
        blueprint: String,
        state_machine: Option<String>,
        state: String,
    },
    /// Add a transition between two states. `rule_variable` wires a bool member
    /// variable straight into the rule; anything more complex is built in the
    /// returned `rule_graph` with `blueprint_modify add_node` / `connect_pins`,
    /// feeding the result node's `bCanEnterTransition` pin.
    AddTransition {
        blueprint: String,
        state_machine: Option<String>,
        /// Source state name or GUID.
        from: String,
        /// Target state name or GUID.
        to: String,
        /// Blend time in seconds (engine default 0.2).
        crossfade_duration: Option<f32>,
        /// Lower wins when several transitions fire in the same frame.
        priority: Option<i32>,
        /// Fire automatically when the source state's animation is about to end.
        automatic_rule: Option<bool>,
        /// Name of a bool variable that gates the transition.
        rule_variable: Option<String>,
    },
    /// Change an existing transition's settings or rule.
    /// Create an Animation Layer Interface (an Animation Blueprint of the
    /// interface kind) declaring the named layers, each an animation graph
    /// taking an input pose; compiled so other Blueprints can implement it.
    CreateLayerInterface {
        /// Destination package path, e.g. "/Game/Anim/ALI_Hero".
        path: String,
        /// Layer names, e.g. ["UpperBody", "FullBody"].
        layers: Vec<String>,
        /// Layer group (the function category): layers in one group share a
        /// linked instance at runtime.
        group: Option<String>,
    },
    /// Declare another layer on an Animation Layer Interface.
    AddLayer {
        /// The interface asset.
        blueprint: String,
        name: String,
        group: Option<String>,
    },
    /// Implement an Animation Layer Interface on an Animation Blueprint: one
    /// animation graph per layer appears (with its root and input pose),
    /// addressed by the layer name in every 'graph' field.
    ImplementLayerInterface {
        blueprint: String,
        /// The interface asset path.
        interface: String,
    },
    /// Add a Linked Anim Layer node that runs a layer (of an implemented
    /// interface, or a self layer) inside an animation graph; wired into the
    /// graph's output unless `connect_to_output` is false.
    AddLinkedLayerNode {
        blueprint: String,
        layer: String,
        /// Animation graph to add to (default "AnimGraph").
        graph: Option<String>,
        connect_to_output: Option<bool>,
        x: Option<i32>,
        y: Option<i32>,
    },
    /// Add a Linked Anim Graph node running another Animation Blueprint's
    /// class inside this graph.
    AddLinkedGraphNode {
        blueprint: String,
        /// The Animation Blueprint (or its generated class) to run.
        instance_class: String,
        graph: Option<String>,
        connect_to_output: Option<bool>,
        x: Option<i32>,
        y: Option<i32>,
    },
    SetTransition {
        blueprint: String,
        state_machine: Option<String>,
        /// Transition GUID from inspect / add_transition.
        transition: String,
        crossfade_duration: Option<f32>,
        priority: Option<i32>,
        automatic_rule: Option<bool>,
        bidirectional: Option<bool>,
        disabled: Option<bool>,
        rule_variable: Option<String>,
    },
}

#[tool_router(router = anim_blueprint_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Inspect Animation Blueprints: list assets, or dump a Blueprint's skeleton, anim graphs and state machines (states, animations, entry state, transitions with their rule graphs). Graph paths like \"Locomotion/Idle\" work in every blueprint_query/blueprint_modify 'graph' field."
    )]
    async fn anim_blueprint_query(
        &self,
        Parameters(op): Parameters<AnimBlueprintQuery>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/anim_blueprints/query", body)
            .await
            .map(Json)
    }

    #[tool(
        description = "Author Animation Blueprints: create one for a skeleton, add state machines, states with animations, entry state and transitions (crossfade, priority, automatic or bool-variable rules); create Animation Layer Interfaces, implement them and place Linked Anim Layer / Linked Anim Graph nodes. Custom rules and other anim nodes are built with blueprint_modify inside the returned graph paths. Finish with blueprint_modify compile and save."
    )]
    async fn anim_blueprint_modify(
        &self,
        Parameters(op): Parameters<AnimBlueprintModify>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/anim_blueprints/modify", body)
            .await
            .map(Json)
    }
}

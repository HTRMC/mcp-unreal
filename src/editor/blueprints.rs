//! Blueprint inspection and graph editing.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::{Value, json};

use crate::UnrealMcp;

#[derive(serde::Deserialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum BlueprintQuery {
    /// Find Blueprint assets in the project (including Anim and Widget Blueprints).
    List {
        /// Package path to search under (default "/Game").
        path_prefix: Option<String>,
        /// Max results (default 100, cap 500).
        max_results: Option<u32>,
    },
    /// A Blueprint's variables, graphs (including nested sub-graphs with their
    /// paths), components, interfaces and event dispatchers.
    Inspect {
        /// Asset path, e.g. "/Game/Blueprints/BP_Thing".
        blueprint: String,
    },
    /// Every node in a graph with its pins and connections. Node GUIDs from
    /// here are what the modify operations take.
    GetGraph {
        blueprint: String,
        /// Graph name or path from `inspect` (sub-graphs such as anim states
        /// and transition rules are addressed as "Locomotion/Idle"). Defaults
        /// to the first event graph.
        graph: Option<String>,
    },
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
// add_node carries every node kind's options, which makes it much larger than
// the other variants. Boxing it would turn the generated tool schema into a
// $ref indirection, and the schema is the contract with the MCP client, so the
// size stays: one of these exists per tool call.
#[allow(clippy::large_enum_variant)]
pub enum BlueprintModify {
    /// Create a new Blueprint asset (in memory — follow with `save`).
    Create {
        /// Destination package path, e.g. "/Game/Blueprints/BP_Thing".
        path: String,
        /// Parent class (default "Actor"), e.g. "Pawn", "/Script/Engine.Actor".
        parent_class: Option<String>,
    },
    /// Reparent an existing Blueprint and recompile it.
    SetParentClass {
        blueprint: String,
        parent_class: String,
    },
    /// Add a member variable. See the `type` grammar in add_variable's docs.
    AddVariable {
        blueprint: String,
        name: String,
        /// bool, byte, int, int64, float, string, name, text, vector, vector2d,
        /// rotator, transform, linearcolor, color, hitresult,
        /// "object:<Class>", "class:<Class>", "softobject:<Class>",
        /// "softclass:<Class>", "interface:<Class>", "struct:<Struct>",
        /// "enum:<Enum>", or a container: "array<int>", "set<name>",
        /// "map<name,float>".
        #[serde(rename = "type")]
        var_type: String,
        /// Default value as text, e.g. "100.0".
        default: Option<String>,
    },
    RemoveVariable {
        blueprint: String,
        name: String,
    },
    /// Retype an existing member variable, fixing up its nodes.
    SetVariableType {
        blueprint: String,
        name: String,
        #[serde(rename = "type")]
        var_type: String,
    },
    /// Add a new function graph.
    AddFunction {
        blueprint: String,
        name: String,
    },
    RemoveFunction {
        blueprint: String,
        /// Function graph name.
        graph: String,
    },
    /// Add an input parameter or a return value to a function graph. The
    /// return node is created on demand.
    AddFunctionParameter {
        blueprint: String,
        /// Function graph name.
        graph: String,
        name: String,
        #[serde(rename = "type")]
        var_type: String,
        /// "input" (default) or "output".
        direction: Option<String>,
    },
    RemoveFunctionParameter {
        blueprint: String,
        graph: String,
        name: String,
    },
    /// Add a variable local to one function graph.
    AddLocalVariable {
        blueprint: String,
        graph: String,
        name: String,
        #[serde(rename = "type")]
        var_type: String,
        default: Option<String>,
    },
    /// Remove a local variable. The function must have been compiled once.
    RemoveLocalVariable {
        blueprint: String,
        graph: String,
        name: String,
    },
    /// Add a component to an Actor Blueprint's construction-script tree.
    /// Configure it afterwards with set_property on the reported `template`.
    AddComponent {
        blueprint: String,
        /// Variable name for the component.
        name: String,
        /// Component class, e.g. "StaticMeshComponent", "BoxComponent".
        class: String,
        /// Attach under this existing scene component instead of the root.
        parent: Option<String>,
    },
    RemoveComponent {
        blueprint: String,
        name: String,
    },
    /// Implement a Blueprint Interface.
    AddInterface {
        blueprint: String,
        /// Interface class or Blueprint Interface asset path.
        interface: String,
    },
    RemoveInterface {
        blueprint: String,
        interface: String,
        /// Keep the implemented functions as ordinary graphs.
        preserve_functions: Option<bool>,
    },
    /// Add an event dispatcher (a multicast delegate other Blueprints can bind).
    AddEventDispatcher {
        blueprint: String,
        name: String,
    },
    RemoveEventDispatcher {
        blueprint: String,
        name: String,
    },
    /// Add a parameter to an event dispatcher's signature.
    AddDispatcherParameter {
        blueprint: String,
        dispatcher: String,
        name: String,
        #[serde(rename = "type")]
        var_type: String,
    },
    RemoveDispatcherParameter {
        blueprint: String,
        dispatcher: String,
        name: String,
    },
    /// Add a node to a graph. Nodes are created configured, so a call_function
    /// node comes back with the real pins of the target function and a
    /// switch_enum with one pin per enumerator.
    AddNode {
        blueprint: String,
        graph: Option<String>,
        /// One of: call_function, call_parent_function, variable_get,
        /// variable_set, event, custom_event, component_bound_event, branch,
        /// sequence, select, switch_int, switch_string, switch_name,
        /// switch_enum, macro, cast, class_cast, spawn_actor, make_struct,
        /// break_struct, make_array, make_set, make_map, get_array_item,
        /// format_text, self, literal, enum_literal, reroute, comment,
        /// timeline, call_delegate, bind_delegate, unbind_delegate,
        /// clear_delegate, assign_delegate.
        node_type: String,
        /// call_function / call_parent_function: the UFUNCTION name.
        /// event: an overridable event, e.g. "ReceiveBeginPlay".
        function: Option<String>,
        /// Class the function, event, variable or delegate lives on; also the
        /// target type for cast/class_cast and the class for spawn_actor.
        /// Defaults to this Blueprint.
        class: Option<String>,
        /// variable_get / variable_set: the variable name.
        variable: Option<String>,
        /// custom_event / timeline: the name to give it.
        name: Option<String>,
        /// make_struct / break_struct: the struct, e.g. "HitResult".
        #[serde(rename = "struct")]
        struct_type: Option<String>,
        /// switch_enum / enum_literal: the enum, e.g. "/Script/Engine.ECollisionChannel".
        #[serde(rename = "enum")]
        enum_type: Option<String>,
        /// macro: the macro graph name, e.g. "ForEachLoop", "DoOnce", "Gate".
        /// An unknown name comes back with the list of what the library has.
        #[serde(rename = "macro")]
        macro_name: Option<String>,
        /// macro: the macro library asset (default the engine's StandardMacros).
        library: Option<String>,
        /// Delegate-node family and component_bound_event: the delegate name.
        delegate: Option<String>,
        /// component_bound_event: the component variable to bind on.
        component: Option<String>,
        /// literal: object or actor path the node references.
        object: Option<String>,
        /// cast / class_cast: make it a pure cast with no exec pins.
        pure: Option<bool>,
        /// sequence: how many exec outputs (default 2).
        outputs: Option<i32>,
        /// make_array / make_set / make_map: how many entries (default 1).
        entries: Option<i32>,
        /// format_text: the format string; its {tokens} become argument pins.
        format: Option<String>,
        /// comment: the comment text, and its box size.
        text: Option<String>,
        width: Option<i32>,
        height: Option<i32>,
        /// Graph position.
        x: Option<i32>,
        y: Option<i32>,
    },
    DeleteNode {
        blueprint: String,
        graph: Option<String>,
        /// Node GUID from get_graph.
        node: String,
    },
    /// Connect two pins. The graph schema validates the link, so incompatible
    /// types are rejected with a reason instead of silently corrupting the graph.
    ConnectPins {
        blueprint: String,
        graph: Option<String>,
        from_node: String,
        from_pin: String,
        to_node: String,
        to_pin: String,
    },
    DisconnectPins {
        blueprint: String,
        graph: Option<String>,
        from_node: String,
        from_pin: String,
        to_node: String,
        to_pin: String,
    },
    /// Set a pin's literal default value.
    SetPinValue {
        blueprint: String,
        graph: Option<String>,
        node: String,
        pin: String,
        value: String,
    },
    /// Compile the Blueprint and report its status.
    Compile {
        blueprint: String,
    },
    /// Write the Blueprint package to disk.
    Save {
        blueprint: String,
    },
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum BlueprintDebugOp {
    /// What the debugger knows about this Blueprint: breakpoints and watches
    /// set, whether it has compiled debug data, whether PIE is running, which
    /// instance values are read from, and whether this editor can halt at all.
    Status { blueprint: String },
    ListBreakpoints { blueprint: String },
    /// Whether execution is currently halted on a breakpoint, and where.
    /// Names no Blueprint: it reports whatever is stopped.
    HaltStatus {},
    /// Continue running until the next breakpoint.
    Resume {},
    /// Run the next node, descending into a called function graph.
    StepInto {},
    /// Run the next node in this graph, over any function it calls.
    StepOver {},
    /// Run until the current function graph returns.
    StepOut {},
    /// Abandon the rest of this frame's execution and continue.
    Abort {},
    /// Put a breakpoint on a node. When it hits, execution halts and only
    /// this tool, status and output_log answer until it resumes.
    SetBreakpoint {
        blueprint: String,
        /// Graph name or slash path; the first event graph when omitted.
        graph: Option<String>,
        /// Node GUID from blueprint_query get_graph.
        node: String,
        /// Default true.
        enabled: Option<bool>,
    },
    SetBreakpointEnabled {
        blueprint: String,
        graph: Option<String>,
        node: String,
        enabled: bool,
    },
    RemoveBreakpoint {
        blueprint: String,
        graph: Option<String>,
        node: String,
    },
    ClearBreakpoints { blueprint: String },
    /// Every watched pin with its current value, or why it has none.
    ListWatches {
        blueprint: String,
        /// Read values from this instance instead of the selected one.
        object: Option<String>,
    },
    /// Watch a pin. Values are readable while PIE runs, without halting, for
    /// any pin the compiler kept a class property for.
    AddWatch {
        blueprint: String,
        graph: Option<String>,
        node: String,
        /// Pin name from blueprint_query get_graph.
        pin: String,
        object: Option<String>,
    },
    /// One watched pin's current value.
    ReadWatch {
        blueprint: String,
        graph: Option<String>,
        node: String,
        pin: String,
        object: Option<String>,
    },
    RemoveWatch {
        blueprint: String,
        graph: Option<String>,
        node: String,
        pin: String,
    },
    ClearWatches { blueprint: String },
    /// Pick the instance watch values are read from — during PIE, the path
    /// find_actors reports for the spawned actor.
    SetDebugObject { blueprint: String, object: String },
}

#[tool_router(router = blueprint_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Inspect Blueprints: list assets, inspect variables/graphs/components/interfaces/event dispatchers, or dump a graph's nodes, pins and connections. Node GUIDs from get_graph are the handles used by blueprint_modify."
    )]
    async fn blueprint_query(
        &self,
        Parameters(op): Parameters<BlueprintQuery>,
    ) -> Result<Json<Value>, ErrorData> {
        let body = match op {
            BlueprintQuery::List {
                path_prefix,
                max_results,
            } => json!({
                "operation": "list",
                "path_prefix": path_prefix,
                "max_results": max_results,
            }),
            BlueprintQuery::Inspect { blueprint } => {
                json!({"operation": "inspect", "blueprint": blueprint})
            }
            BlueprintQuery::GetGraph { blueprint, graph } => {
                json!({"operation": "get_graph", "blueprint": blueprint, "graph": graph})
            }
        };
        self.call_plugin("/api/blueprints/query", body)
            .await
            .map(Json)
    }

    #[tool(
        description = "Edit Blueprints: create and reparent assets, add/remove/retype variables, functions with parameters and return values, local variables, components on the construction-script tree, interfaces and event dispatchers; add graph nodes from a wide vocabulary (calls, events, casts, macros such as ForEachLoop and DoOnce, branches, sequences, switches, struct make/break, containers, spawn actor, timelines, delegates, comments), connect pins, set pin defaults, compile and save. Every change is undo-able in the editor. Call compile after edits, then save to persist."
    )]
    async fn blueprint_modify(
        &self,
        Parameters(op): Parameters<BlueprintModify>,
    ) -> Result<Json<Value>, ErrorData> {
        self.call_plugin("/api/blueprints/modify", blueprint_modify_body(op))
            .await
            .map(Json)
    }

    #[tool(
        description = "Blueprint debugging: breakpoints, stepping, watched pins, and the instance their values are read from. Watches work without stopping anything — during PIE a watched pin backed by a class property reports its live value. Breakpoints halt execution, and halt_status/resume/step_into/step_over/step_out/abort drive it from there; while halted only this tool, status and output_log answer, since every other handler would be re-entering the engine from inside a paused Blueprint."
    )]
    async fn blueprint_debug(
        &self,
        Parameters(op): Parameters<BlueprintDebugOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/blueprints/debug", body).await.map(Json)
    }
}

/// Request body for `/api/blueprints/modify`. Split out so the contract
/// fixtures can assert the exact wire shape without a live editor.
pub fn blueprint_modify_body(op: BlueprintModify) -> Value {
    serde_json::to_value(op).expect("BlueprintModify serialises")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn add_node_body_uses_wire_names() {
        let body = blueprint_modify_body(BlueprintModify::AddNode {
            blueprint: "/Game/BP".into(),
            graph: None,
            node_type: "make_struct".into(),
            function: None,
            class: None,
            variable: None,
            name: None,
            struct_type: Some("HitResult".into()),
            enum_type: None,
            macro_name: None,
            library: None,
            delegate: None,
            component: None,
            object: None,
            pure: None,
            outputs: None,
            entries: None,
            format: None,
            text: None,
            width: None,
            height: None,
            x: Some(10),
            y: Some(20),
        });
        assert_eq!(body["operation"], "add_node");
        // The plugin reads "struct", not "struct_type".
        assert_eq!(body["struct"], "HitResult");
        assert_eq!(body["x"], 10);
    }

    #[test]
    fn add_variable_body_renames_type() {
        let body = blueprint_modify_body(BlueprintModify::AddVariable {
            blueprint: "/Game/BP".into(),
            name: "Health".into(),
            var_type: "array<float>".into(),
            default: None,
        });
        assert_eq!(body["type"], "array<float>");
    }
}

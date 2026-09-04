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
    /// paths) and components.
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

#[derive(serde::Deserialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum BlueprintModify {
    /// Create a new Blueprint asset (in memory — follow with `save`).
    Create {
        /// Destination package path, e.g. "/Game/Blueprints/BP_Thing".
        path: String,
        /// Parent class (default "Actor"), e.g. "Pawn", "/Script/Engine.Actor".
        parent_class: Option<String>,
    },
    /// Add a member variable.
    AddVariable {
        blueprint: String,
        name: String,
        /// bool, byte, int, int64, float, string, name, text, vector, vector2d,
        /// rotator, transform, linearcolor, color, or "object:<Class>" / "class:<Class>".
        #[serde(rename = "type")]
        var_type: String,
        /// Default value as text, e.g. "100.0".
        default: Option<String>,
    },
    RemoveVariable {
        blueprint: String,
        name: String,
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
    /// Add a node to a graph. Nodes are created configured, so a call_function
    /// node comes back with the real pins of the target function.
    AddNode {
        blueprint: String,
        graph: Option<String>,
        /// call_function, variable_get, variable_set, branch, or custom_event.
        node_type: String,
        /// call_function: the UFUNCTION name, e.g. "K2_SetActorLocation".
        function: Option<String>,
        /// call_function: class owning the function; defaults to this Blueprint.
        class: Option<String>,
        /// variable_get / variable_set: the variable name.
        variable: Option<String>,
        /// custom_event: the event name.
        name: Option<String>,
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

#[tool_router(router = blueprint_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Inspect Blueprints: list assets, inspect variables/graphs/components, or dump a graph's nodes, pins and connections. Node GUIDs from get_graph are the handles used by blueprint_modify."
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
        description = "Edit Blueprints: create assets, add/remove variables and functions, add/delete graph nodes, connect pins, set pin defaults, compile and save. Every change is undo-able in the editor. Call compile after edits, then save to persist."
    )]
    async fn blueprint_modify(
        &self,
        Parameters(op): Parameters<BlueprintModify>,
    ) -> Result<Json<Value>, ErrorData> {
        self.call_plugin("/api/blueprints/modify", blueprint_modify_body(op))
            .await
            .map(Json)
    }
}

/// Request body for `/api/blueprints/modify`. Split out so the contract
/// fixtures can assert the exact wire shape without a live editor.
pub fn blueprint_modify_body(op: BlueprintModify) -> Value {
    match op {
        BlueprintModify::Create { path, parent_class } => {
            json!({"operation": "create", "path": path, "parent_class": parent_class})
        }
        BlueprintModify::AddVariable {
            blueprint,
            name,
            var_type,
            default,
        } => json!({
            "operation": "add_variable", "blueprint": blueprint,
            "name": name, "type": var_type, "default": default,
        }),
        BlueprintModify::RemoveVariable { blueprint, name } => {
            json!({"operation": "remove_variable", "blueprint": blueprint, "name": name})
        }
        BlueprintModify::AddFunction { blueprint, name } => {
            json!({"operation": "add_function", "blueprint": blueprint, "name": name})
        }
        BlueprintModify::RemoveFunction { blueprint, graph } => {
            json!({"operation": "remove_function", "blueprint": blueprint, "graph": graph})
        }
        BlueprintModify::AddNode {
            blueprint,
            graph,
            node_type,
            function,
            class,
            variable,
            name,
            x,
            y,
        } => json!({
            "operation": "add_node", "blueprint": blueprint, "graph": graph,
            "node_type": node_type, "function": function, "class": class,
            "variable": variable, "name": name, "x": x, "y": y,
        }),
        BlueprintModify::DeleteNode {
            blueprint,
            graph,
            node,
        } => json!({
            "operation": "delete_node", "blueprint": blueprint,
            "graph": graph, "node": node,
        }),
        BlueprintModify::ConnectPins {
            blueprint,
            graph,
            from_node,
            from_pin,
            to_node,
            to_pin,
        } => json!({
            "operation": "connect_pins", "blueprint": blueprint, "graph": graph,
            "from_node": from_node, "from_pin": from_pin,
            "to_node": to_node, "to_pin": to_pin,
        }),
        BlueprintModify::DisconnectPins {
            blueprint,
            graph,
            from_node,
            from_pin,
            to_node,
            to_pin,
        } => json!({
            "operation": "disconnect_pins", "blueprint": blueprint, "graph": graph,
            "from_node": from_node, "from_pin": from_pin,
            "to_node": to_node, "to_pin": to_pin,
        }),
        BlueprintModify::SetPinValue {
            blueprint,
            graph,
            node,
            pin,
            value,
        } => json!({
            "operation": "set_pin_value", "blueprint": blueprint, "graph": graph,
            "node": node, "pin": pin, "value": value,
        }),
        BlueprintModify::Compile { blueprint } => {
            json!({"operation": "compile", "blueprint": blueprint})
        }
        BlueprintModify::Save { blueprint } => {
            json!({"operation": "save", "blueprint": blueprint})
        }
    }
}

//! Reflection tools: read/write any UPROPERTY, call any UFUNCTION.
//! Replaces the Remote Control API the Go reference depended on.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::{Value, json};

use crate::UnrealMcp;

#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct GetPropertyInput {
    /// Full object path, e.g. "/Game/Maps/Map.Map:PersistentLevel.MyActor" or a component path.
    pub object_path: String,
    /// Property name as declared in C++/Blueprint, e.g. "bHidden", "Mobility".
    pub property: String,
}

#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct SetPropertyInput {
    /// Full object path.
    pub object_path: String,
    /// Property name.
    pub property: String,
    /// New value as JSON, shaped like the property type (number, string, bool,
    /// object for structs e.g. {"X":1,"Y":2,"Z":3}, array for containers).
    pub value: Value,
}

#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct CallFunctionInput {
    /// Full object path of the object to call on.
    pub object_path: String,
    /// UFUNCTION name, e.g. "K2_SetActorLocation".
    pub function: String,
    /// Arguments by parameter name (case-insensitive). Omitted params keep their defaults.
    pub args: Option<Value>,
}

/// Request body for `/api/object/get_property`. Split out so the contract
/// fixtures can assert the exact wire shape without a live editor.
pub fn get_property_body(input: &GetPropertyInput) -> Value {
    json!({"object_path": input.object_path, "property": input.property})
}

/// Request body for `/api/object/set_property`.
pub fn set_property_body(input: &SetPropertyInput) -> Value {
    json!({
        "object_path": input.object_path,
        "property": input.property,
        "value": input.value,
    })
}

#[tool_router(router = object_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Read any UPROPERTY from any UObject (actor, component, asset) via engine reflection. Requires the editor running with McpLink."
    )]
    async fn get_property(
        &self,
        Parameters(input): Parameters<GetPropertyInput>,
    ) -> Result<Json<Value>, ErrorData> {
        self.call_plugin("/api/object/get_property", get_property_body(&input))
            .await
            .map(Json)
    }

    #[tool(
        description = "Set any UPROPERTY on any UObject via engine reflection. Undo-able (creates a transaction) and marks the package dirty."
    )]
    async fn set_property(
        &self,
        Parameters(input): Parameters<SetPropertyInput>,
    ) -> Result<Json<Value>, ErrorData> {
        self.call_plugin("/api/object/set_property", set_property_body(&input))
            .await
            .map(Json)
    }

    #[tool(
        description = "Call any UFUNCTION on any UObject, passing arguments by name. Returns the function's return value and out-params."
    )]
    async fn call_function(
        &self,
        Parameters(input): Parameters<CallFunctionInput>,
    ) -> Result<Json<Value>, ErrorData> {
        self.call_plugin(
            "/api/object/call_function",
            json!({
                "object_path": input.object_path,
                "function": input.function,
                "args": input.args.unwrap_or_else(|| json!({})),
            }),
        )
        .await
        .map(Json)
    }
}

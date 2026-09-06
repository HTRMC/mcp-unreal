//! Chooser table interop (requires the McpLinkChooser plugin).
//!
//! Variants serialise straight into the plugin request body, so field names
//! here are the contract with `McpChooserRoutes.cpp`.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum ChooserOp {
    /// Column structs (filter, scoring, output, random) with their cell
    /// shapes, result structs, and input-binding parameter structs.
    ListTypes,
    /// New Chooser Table with its signature: what it returns and what it is
    /// evaluated against.
    Create {
        /// e.g. /Game/Choosers/CHT_Locomotion.
        path: String,
        /// "object" (default), "class" or "none" (outputs only).
        result_type: Option<String>,
        /// The result class, e.g. "AnimSequence".
        output_class: Option<String>,
        /// Context parameters in evaluation order: a class name, or
        /// {"class": …} / {"struct": "/Script/Module.Struct"}.
        context: Option<Vec<Value>>,
    },
    /// Signature, columns (input binding, properties, cells), rows and the
    /// fallback.
    Info {
        chooser: String,
    },
    /// Add a column by struct ("FloatRangeColumn", "BoolColumn",
    /// "GameplayTagColumn", "OutputFloatColumn", "RandomizeColumn", …), bound
    /// to a context property by name chain (`property` "Speed" or
    /// "Pawn.Velocity" on `context_index`) or with a full `input` struct.
    AddColumn {
        chooser: String,
        column: String,
        property: Option<String>,
        context_index: Option<i32>,
        /// Bind to the context object or struct itself.
        bound_to_root: Option<bool>,
        /// An input parameter struct with _structType, e.g.
        /// {"_structType": "/Script/Chooser.FloatContextProperty", "Binding": {"PropertyBindingChain": ["Speed"]}}.
        input: Option<Value>,
        /// Column-wide properties (bWrapInput, MaxDistance, TagMatchType, …).
        properties: Option<Value>,
    },
    /// Change a column's properties, input binding or disabled flag.
    SetColumn {
        chooser: String,
        /// Column index or struct name from info.
        column: Value,
        properties: Option<Value>,
        input: Option<Value>,
        disabled: Option<bool>,
    },
    RemoveColumn {
        chooser: String,
        column: Value,
    },
    /// Add a row: its result (`asset`, `class`, `nested_chooser`, or a
    /// `result` struct with _structType) and `cells` keyed by column index or
    /// struct name, each one element of the column's cell array, e.g.
    /// {"0": {"Min": 0, "Max": 150}, "BoolColumn": "MatchTrue"}.
    AddRow {
        chooser: String,
        asset: Option<String>,
        class: Option<String>,
        nested_chooser: Option<String>,
        result: Option<Value>,
        cells: Option<Value>,
        /// Insert position (default: append).
        row: Option<i32>,
        disabled: Option<bool>,
    },
    /// Change a row's result, cells or disabled flag.
    SetRow {
        chooser: String,
        row: i32,
        asset: Option<String>,
        class: Option<String>,
        nested_chooser: Option<String>,
        result: Option<Value>,
        cells: Option<Value>,
        disabled: Option<bool>,
    },
    RemoveRow {
        chooser: String,
        row: i32,
    },
    /// The result used when no row passes.
    SetFallback {
        chooser: String,
        asset: Option<String>,
        class: Option<String>,
        nested_chooser: Option<String>,
        result: Option<Value>,
    },
    /// Evaluate the table against live context: one entry per declared
    /// parameter — an object path or actor name, or a struct as
    /// {"_structType": "/Script/Module.Struct", ...fields}; output columns
    /// write into the structs, which come back.
    Evaluate {
        chooser: String,
        context: Vec<Value>,
        /// World for actor names (default: editor; "pie" during play).
        world: Option<String>,
        /// Every passing row instead of the first.
        multi: Option<bool>,
    },
    Save {
        chooser: String,
    },
}

#[tool_router(router = chooser_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Chooser tables (needs McpLinkChooser): create a table with its result type and context parameters, add filter / scoring / output / randomize columns bound to context properties, add rows with results and per-column cells, set the fallback, evaluate against live objects or structs, and save."
    )]
    async fn chooser_ops(
        &self,
        Parameters(op): Parameters<ChooserOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/data/chooser", body).await.map(Json)
    }
}

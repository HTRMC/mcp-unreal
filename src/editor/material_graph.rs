//! Material graph editing: expression nodes, links, material inputs.
//!
//! Variants serialise straight into the plugin request body, so field names
//! here are the contract with `McpMaterialGraphRoutes.cpp`.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum MaterialGraphOp {
    /// Expression classes usable with add_expression (short names such as
    /// "Constant3Vector", "TextureSample", "ScalarParameter", "Multiply").
    ListExpressionClasses {
        /// Substring filter, e.g. "Parameter".
        filter: Option<String>,
    },
    /// Every expression with its inputs/outputs and links, plus which
    /// expression feeds each material input (BaseColor, Roughness, ...).
    GetGraph {
        /// Material asset path (not an instance).
        material: String,
    },
    /// Add an expression node. Configure it afterwards with `set_property` on
    /// the returned `path` (e.g. Constant, DefaultValue, ParameterName, Texture).
    AddExpression {
        material: String,
        /// Expression class, short name accepted: "Constant3Vector".
        class: String,
        x: Option<i32>,
        y: Option<i32>,
    },
    DeleteExpression {
        material: String,
        /// Expression name from get_graph.
        expression: String,
    },
    /// Link an expression output into another expression's input.
    Connect {
        material: String,
        from: String,
        /// Output name; empty for the default output (most nodes).
        from_output: Option<String>,
        /// Output index from get_graph for unnamed outputs such as channel masks
        /// (Constant3Vector: 0 = RGB, 1 = R, 2 = G, 3 = B). Overrides from_output.
        from_output_index: Option<u32>,
        to: String,
        /// Input name; defaults to the target's first input (e.g. "A").
        to_input: Option<String>,
    },
    /// Feed a material input from an expression: BaseColor, Metallic, Specular,
    /// Roughness, EmissiveColor, Opacity, OpacityMask, Normal, WorldPositionOffset,
    /// AmbientOcclusion, SubsurfaceColor, Refraction, PixelDepthOffset, ...
    ConnectProperty {
        material: String,
        from: String,
        from_output: Option<String>,
        /// Output index for unnamed outputs (see connect).
        from_output_index: Option<u32>,
        property: String,
    },
    DisconnectProperty {
        material: String,
        property: String,
    },
    /// Auto-arrange the nodes.
    Layout {
        material: String,
    },
    /// Recompile the material and report compile errors.
    Recompile {
        material: String,
    },
}

#[tool_router(router = material_graph_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Edit a Material's node graph: add/delete expression nodes (constants, parameters, texture samples, math), connect expressions to each other and to the material's inputs (BaseColor, Roughness, Normal, ...), inspect the graph, auto-layout, and recompile with error reporting. Node options are set with set_property on each expression's path. Use material_ops for instances and parameter values; save with material_ops save."
    )]
    async fn material_graph(
        &self,
        Parameters(op): Parameters<MaterialGraphOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/materials/graph", body)
            .await
            .map(Json)
    }
}

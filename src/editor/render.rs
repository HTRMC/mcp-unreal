//! Render targets, textures, thumbnails and Material Functions.
//!
//! Variants serialise straight into the plugin request body, so the field
//! names here are the contract with `McpRenderRoutes.cpp` and
//! `McpMaterialFunctionRoutes.cpp`.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum RenderOp {
    /// Render target pixel formats `create_render_target` accepts.
    ListFormats {},
    /// New Render Target 2D asset.
    CreateRenderTarget {
        /// e.g. /Game/RT/RT_Mask.
        path: String,
        width: Option<i32>,
        height: Option<i32>,
        /// ETextureRenderTargetFormat name, default "RTF_RGBA16f".
        format: Option<String>,
        /// [R, G, B, A] in linear space.
        clear_color: Option<Vec<f64>>,
        auto_generate_mips: Option<bool>,
    },
    /// Size and format of an existing render target.
    Info { render_target: String },
    Clear {
        render_target: String,
        color: Option<Vec<f64>>,
    },
    /// Render a material across the whole target — the workhorse for
    /// procedural masks, gradients and noise.
    DrawMaterial {
        render_target: String,
        /// Material or Material Instance path.
        material: String,
    },
    /// One pixel back as linear [R, G, B, A]; the way to check a draw worked.
    ReadPixel {
        render_target: String,
        x: i32,
        y: i32,
    },
    /// Write the target to an image file (.png, .exr or .hdr).
    Export {
        render_target: String,
        /// Full path, e.g. C:/out/mask.png.
        file: String,
    },
    /// Bake the target into a Texture2D asset. An existing texture at `path`
    /// is overwritten in place, which keeps everything referencing it.
    ToTexture {
        render_target: String,
        path: String,
    },
    /// Render an asset's thumbnail — to a file, or into the asset's package
    /// where the content browser shows it.
    GenerateThumbnail {
        asset: String,
        size: Option<i32>,
        /// Write a PNG here instead of caching into the package.
        file: Option<String>,
    },
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum MaterialFunctionOp {
    /// New Material Function asset.
    Create {
        /// e.g. /Game/Materials/MF_Blend.
        path: String,
        description: Option<String>,
        /// Show it in the material palette (default true).
        expose_to_library: Option<bool>,
    },
    /// The function's expressions with their paths and pin names.
    Info { function: String },
    /// Add an expression. FunctionInput and FunctionOutput are the function's
    /// parameters and results; `name` sets theirs.
    AddExpression {
        function: String,
        /// e.g. "FunctionInput", "FunctionOutput", "Add" — material_graph
        /// list_expression_classes lists them all.
        class: String,
        x: Option<i32>,
        y: Option<i32>,
        /// Input/output name, for FunctionInput and FunctionOutput.
        name: Option<String>,
    },
    DeleteExpression {
        function: String,
        expression: String,
    },
    Connect {
        function: String,
        from: String,
        /// Output pin name; the first output when omitted.
        from_output: Option<String>,
        to: String,
        /// Input pin name; the first input when omitted.
        to_input: Option<String>,
    },
    /// Auto-arrange the graph.
    Layout { function: String },
    /// Recompile the function and every material that uses it.
    Update { function: String },
    Save { function: String },
}

#[tool_router(router = render_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Render targets, textures and thumbnails: create a Render Target 2D, clear it, draw a material across it (procedural masks, gradients, noise), read a pixel back to check the result, export it to a PNG/EXR/HDR file, or bake it into a Texture2D asset. Also renders an asset's thumbnail, to a file or into its package. capture_viewport photographs what the editor shows; this is the other direction. Needs a windowed editor — there is no RHI to draw with under -nullrhi, and the tool says so rather than returning black."
    )]
    async fn render_ops(
        &self,
        Parameters(op): Parameters<RenderOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/content/render", body).await.map(Json)
    }

    #[tool(
        description = "Material Functions — the reusable sub-graphs a material calls into. Create the asset, add expressions (FunctionInput and FunctionOutput are its parameters and results), wire them, lay the graph out, then update to recompile the function and every material using it. Expression options are properties on the reported paths, so set_property tunes them, exactly as for material_graph."
    )]
    async fn material_function(
        &self,
        Parameters(op): Parameters<MaterialFunctionOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/materials/function", body)
            .await
            .map(Json)
    }
}

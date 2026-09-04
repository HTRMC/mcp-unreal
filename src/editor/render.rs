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
    ToTexture { render_target: String, path: String },
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
    Info {
        function: String,
    },
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
    Layout {
        function: String,
    },
    /// Recompile the function and every material that uses it.
    Update {
        function: String,
    },
    Save {
        function: String,
    },
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum MaterialLayerOp {
    /// New Material Layer asset — a Material Function that produces one layer's
    /// material attributes. Comes with the MaterialAttributes input and layer
    /// output the Material Editor would add; author the body with
    /// `material_function`.
    CreateLayer {
        /// e.g. /Game/Materials/ML_Rock.
        path: String,
        description: Option<String>,
        expose_to_library: Option<bool>,
        /// Skip the input/output nodes (default false).
        seed_nodes: Option<bool>,
    },
    /// New Material Layer Blend asset — how one layer combines with what is
    /// under it. Comes with "Top Layer" and "Bottom Layer" inputs.
    CreateBlend {
        /// e.g. /Game/Materials/MLB_HeightBlend.
        path: String,
        description: Option<String>,
        expose_to_library: Option<bool>,
        seed_nodes: Option<bool>,
    },
    /// The stack on a Material or Material Instance: each layer with its blend,
    /// name and visibility.
    Info {
        /// A Material or Material Instance path.
        asset: String,
        /// Only for a material with more than one layers node.
        expression: Option<String>,
    },
    /// Add a layer on top of the stack, with the blend that combines it with
    /// everything below. The first layer added is the background layer and
    /// takes no blend.
    AddLayer {
        asset: String,
        expression: Option<String>,
        /// A Material Layer asset; an empty slot when omitted.
        layer: Option<String>,
        /// A Material Layer Blend asset; required in practice for layer 1 up.
        blend: Option<String>,
        /// The label the stack shows.
        name: Option<String>,
    },
    /// Swap the layer function at an index.
    SetLayer {
        asset: String,
        expression: Option<String>,
        index: i32,
        layer: String,
    },
    /// Swap the blend function under a layer. Layer 0 has none.
    SetBlend {
        asset: String,
        expression: Option<String>,
        index: i32,
        blend: String,
    },
    RemoveLayer {
        asset: String,
        expression: Option<String>,
        index: i32,
    },
    /// Reorder a layer. The background layer stays at the bottom.
    MoveLayer {
        asset: String,
        expression: Option<String>,
        from_index: i32,
        to_index: i32,
    },
    SetLayerName {
        asset: String,
        expression: Option<String>,
        index: i32,
        name: String,
    },
    SetLayerVisibility {
        asset: String,
        expression: Option<String>,
        index: i32,
        visible: bool,
    },
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
        self.call_plugin("/api/content/render", body)
            .await
            .map(Json)
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

    #[tool(
        description = "Material Layers — the layer/blend stack. Create the Material Layer and Material Layer Blend assets (they are Material Functions, so material_function authors their bodies), then build the stack on a Material (in its Material Attribute Layers node, placed with material_graph add_expression) or override it per instance on a Material Instance. Layer 0 is the background layer and has no blend under it; every layer above is combined with what is below by its blend function."
    )]
    async fn material_layers(
        &self,
        Parameters(op): Parameters<MaterialLayerOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/materials/layers", body)
            .await
            .map(Json)
    }
}

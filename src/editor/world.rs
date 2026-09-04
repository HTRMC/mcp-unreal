//! World building: landscape terrain and foliage scatter.
//!
//! Variants serialise straight into the plugin request body, so field names
//! here are the contract with `McpLandscapeRoutes.cpp` / `McpFoliageRoutes.cpp`.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum LandscapeOp {
    /// Every landscape in the world, with extent, resolution and layers.
    List { world: Option<String> },
    /// New landscape. Resolution is quads_per_section x sections_per_component
    /// x components, so the defaults give a 64x64-vertex landscape.
    Create {
        world: Option<String>,
        /// Editor label for the actor.
        name: Option<String>,
        /// World location of the landscape's corner [X, Y, Z] in cm.
        location: Option<[f64; 3]>,
        /// Scale [X, Y, Z]; the default [100, 100, 100] gives 1 m per quad.
        scale: Option<[f64; 3]>,
        /// 7, 15, 31, 63 (default), 127 or 255.
        quads_per_section: Option<i32>,
        /// 1 (default) or 4.
        sections_per_component: Option<i32>,
        components_x: Option<i32>,
        components_y: Option<i32>,
        /// Starting height in cm relative to the actor (default 0 — flat).
        height: Option<f64>,
        /// Landscape material asset path.
        material: Option<String>,
    },
    /// Extent, resolution and paintable layers of one landscape.
    Info {
        world: Option<String>,
        /// Actor label or path; optional when the level has exactly one.
        landscape: Option<String>,
    },
    /// Heights in cm relative to the landscape actor, row-major from min_y.
    /// At most 65536 vertices per call.
    GetHeights {
        world: Option<String>,
        landscape: Option<String>,
        /// Vertex coordinates; the whole landscape when omitted.
        min_x: Option<i32>,
        min_y: Option<i32>,
        max_x: Option<i32>,
        max_y: Option<i32>,
    },
    /// Sculpt: flatten a region to one height, or write a height per vertex.
    SetHeights {
        world: Option<String>,
        landscape: Option<String>,
        min_x: Option<i32>,
        min_y: Option<i32>,
        max_x: Option<i32>,
        max_y: Option<i32>,
        /// Flatten the whole region to this height in cm.
        height: Option<f64>,
        /// One height in cm per vertex, row-major from min_y; overrides height.
        heights: Option<Vec<f64>>,
    },
    /// Add a paintable weightmap layer, creating its LayerInfo asset if needed.
    AddLayer {
        world: Option<String>,
        landscape: Option<String>,
        /// Layer name, matching a layer the landscape material samples.
        layer: String,
        /// Where to put the LayerInfo asset (default
        /// /Game/Landscape/LayerInfo/<layer>_LayerInfo).
        layer_info: Option<String>,
    },
    /// Paint a layer's weight over a region.
    PaintLayer {
        world: Option<String>,
        landscape: Option<String>,
        layer: String,
        min_x: Option<i32>,
        min_y: Option<i32>,
        max_x: Option<i32>,
        max_y: Option<i32>,
        /// 0 to 1 (default 1).
        weight: Option<f64>,
    },
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum FoliageOp {
    /// Foliage types in the level with their instance counts.
    ListTypes { world: Option<String> },
    /// Make a foliage type from a Static Mesh. Density, scale range, alignment
    /// and collision are properties on the returned `type` path — set_property
    /// edits them.
    AddType {
        world: Option<String>,
        /// Static Mesh asset path, e.g. /Engine/BasicShapes/Cone.
        static_mesh: String,
    },
    /// Remove a foliage type and every instance of it.
    RemoveType {
        world: Option<String>,
        /// Type name from list_types, or its object path.
        r#type: String,
    },
    /// Place instances: either explicit transforms, or scatter `count` of them
    /// by dropping points onto whatever has collision under the area.
    AddInstances {
        world: Option<String>,
        r#type: String,
        /// Explicit placements, each an object with a "location" [X, Y, Z] and
        /// optionally "rotation" [Pitch, Yaw, Roll] and "scale" [X, Y, Z].
        instances: Option<Vec<Value>>,
        /// How many to scatter when `instances` is omitted (max 10000).
        count: Option<i32>,
        /// Middle of the scatter area [X, Y, Z] in cm.
        center: Option<[f64; 3]>,
        /// Half-size of the scatter area [X, Y, Z]; Z is unused.
        extent: Option<[f64; 3]>,
        /// How far down to look for ground, in cm (default 100000).
        trace_height: Option<f64>,
        /// Tilt each instance to the surface it landed on.
        align_to_normal: Option<bool>,
        /// Random yaw per instance (default true).
        random_yaw: Option<bool>,
        min_scale: Option<f64>,
        max_scale: Option<f64>,
        /// Fixed seed, for a repeatable scatter.
        seed: Option<i32>,
    },
    /// Instance count, and the first `max_results` transforms.
    ListInstances {
        world: Option<String>,
        r#type: String,
        max_results: Option<i32>,
    },
    /// Remove instances of a type: those inside a sphere, or all of them.
    RemoveInstances {
        world: Option<String>,
        r#type: String,
        center: Option<[f64; 3]>,
        radius: Option<f64>,
    },
}

#[tool_router(router = world_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Build and sculpt Landscape terrain: create a landscape at a chosen resolution and scale, read and write heights over any region (heights are centimetres of Z relative to the landscape actor, not raw samples), and add or paint the weightmap layers the landscape material blends. Sculpting a region takes either one flat height or one height per vertex, row-major from min_y."
    )]
    async fn landscape_ops(
        &self,
        Parameters(op): Parameters<LandscapeOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/world/landscape", body)
            .await
            .map(Json)
    }

    #[tool(
        description = "Scatter foliage: make foliage types from Static Meshes, then place instances either at explicit transforms or by scattering a count over an area, dropping each onto whatever has collision beneath it (a landscape or static meshes) with optional surface alignment and random yaw and scale. Foliage type settings are properties on the reported type path, so set_property edits them."
    )]
    async fn foliage_ops(
        &self,
        Parameters(op): Parameters<FoliageOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/world/foliage", body).await.map(Json)
    }
}

//! Static Mesh editing: LODs, collision, Nanite, lightmap UVs, sockets.
//!
//! Variants serialise straight into the plugin request body, so the field
//! names here are the contract with `McpStaticMeshRoutes.cpp`.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

/// One generated LOD.
#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
pub struct LodSpec {
    /// Fraction of triangles to keep, 0 to 1. 1.0 is no reduction.
    pub percent_triangles: Option<f64>,
    /// Screen size at which this LOD takes over, 0 to 1. Ignored when
    /// `auto_screen_size` is on.
    pub screen_size: Option<f64>,
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum StaticMeshOp {
    /// LODs with their vertex counts and screen sizes, material slots,
    /// sockets, Nanite settings and collision counts.
    GetInfo {
        mesh: String,
    },
    /// Rebuild the LOD chain by automatic reduction. The first entry is
    /// LOD 0, so it is usually {"percent_triangles": 1.0}.
    SetLods {
        mesh: String,
        lods: Vec<LodSpec>,
        /// Let the engine choose the screen sizes (default true).
        auto_screen_size: Option<bool>,
    },
    /// Drop every LOD but LOD 0.
    RemoveLods {
        mesh: String,
    },
    /// Use a project LOD group's settings instead of per-mesh ones.
    SetLodGroup {
        mesh: String,
        lod_group: String,
    },
    /// Add one simple collision primitive fitted to the mesh.
    AddCollision {
        mesh: String,
        /// box (default), sphere, capsule, ndop10x, ndop10y, ndop10z, ndop18, ndop26.
        shape: Option<String>,
    },
    /// Generate convex hulls that follow the mesh's shape.
    AddConvexCollision {
        mesh: String,
        /// How many hulls (default 4).
        hull_count: Option<i32>,
        /// Vertices per hull, 6-32 (default 16).
        max_hull_verts: Option<i32>,
        /// Voxel resolution, 10000-1000000 (default 100000).
        hull_precision: Option<i32>,
    },
    RemoveCollision {
        mesh: String,
    },
    /// Nanite settings. Omitted fields keep their current value.
    SetNanite {
        mesh: String,
        enabled: Option<bool>,
        position_precision: Option<i32>,
        keep_percent_triangles: Option<f64>,
    },
    /// Whether the build generates a lightmap UV channel.
    SetLightmapUvs {
        mesh: String,
        generate: Option<bool>,
    },
    /// Add a socket, or move one that exists.
    AddSocket {
        mesh: String,
        socket: String,
        location: Option<[f64; 3]>,
        rotation: Option<[f64; 3]>,
        scale: Option<[f64; 3]>,
        tag: Option<String>,
    },
    /// Change an existing socket; errors if it does not exist.
    SetSocket {
        mesh: String,
        socket: String,
        location: Option<[f64; 3]>,
        rotation: Option<[f64; 3]>,
        scale: Option<[f64; 3]>,
        tag: Option<String>,
    },
    RemoveSocket {
        mesh: String,
        socket: String,
    },
    /// Assign a material to one slot.
    SetMaterial {
        mesh: String,
        /// Slot index from get_info (default 0).
        slot: Option<i32>,
        material: String,
    },
    Save {
        mesh: String,
    },
}

#[tool_router(router = mesh_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Edit Static Meshes: inspect LODs, materials, sockets, Nanite and collision; generate a LOD chain by reduction or apply a LOD group; add simple or convex-decomposition collision; toggle Nanite and lightmap UV generation; add, move and remove sockets; assign materials to slots. Build settings and other per-mesh options are ordinary properties — get_property / set_property reach those."
    )]
    async fn static_mesh_ops(
        &self,
        Parameters(op): Parameters<StaticMeshOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/mesh/static", body).await.map(Json)
    }
}

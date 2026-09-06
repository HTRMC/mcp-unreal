//! Geometry Script interop (requires the McpLinkGeometryScript plugin).
//!
//! Variants serialise straight into the plugin request body, so field names
//! here are the contract with `McpGeometryScriptRoutes.cpp`.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum GeometryScriptOp {
    /// A new, empty dynamic mesh in the transient package; returns its path.
    NewMesh { name: Option<String> },
    /// The Geometry Script functions, as "Library.Function" names or full
    /// signatures, filtered by substring and library.
    ListFunctions {
        contains: Option<String>,
        /// e.g. "MeshPrimitiveFunctions", "MeshBooleanFunctions".
        library: Option<String>,
        /// Return parameter lists instead of names (default false).
        signatures: Option<bool>,
        /// Default 200.
        max_results: Option<i32>,
    },
    /// One function's parameters, types, directions and defaults.
    Signature {
        function: String,
        library: Option<String>,
    },
    /// Call a library function on the mesh. The mesh fills the function's
    /// first dynamic-mesh parameter (TargetMesh); `args` hold the rest by
    /// name — structs as objects, enums by name, other meshes by path.
    Call {
        mesh: String,
        /// e.g. "AppendBox", "ApplyMeshBoolean", "RecomputeNormals".
        function: String,
        /// Only needed when the name exists in several libraries.
        library: Option<String>,
        args: Option<Value>,
    },
    /// Vertex and triangle counts, bounds and the engine's summary line.
    MeshInfo { mesh: String },
    /// Write the mesh into a Static Mesh asset — a new one at `path`, or an
    /// existing one's LOD.
    ToStaticMesh {
        mesh: String,
        path: String,
        lod: Option<i32>,
        recompute_normals: Option<bool>,
        recompute_tangents: Option<bool>,
        /// New assets only.
        nanite: Option<bool>,
        /// Existing assets only: take the mesh's materials.
        replace_materials: Option<bool>,
    },
    /// Read a Static Mesh's LOD into the mesh.
    FromStaticMesh {
        mesh: String,
        static_mesh: String,
        lod: Option<i32>,
    },
    /// Drop the mesh.
    Release { mesh: String },
}

#[tool_router(router = geometry_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Geometry Script (needs McpLinkGeometryScript): make a dynamic mesh, call any UGeometryScriptLibrary function on it by name — primitives, booleans, extrusion, remeshing, UVs, normals, simplification — read counts and bounds, and write it to a Static Mesh asset or read one in."
    )]
    async fn geometry_script_ops(
        &self,
        Parameters(op): Parameters<GeometryScriptOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin_with_timeout(
            "/api/geometry/script",
            body,
            std::time::Duration::from_secs(300),
        )
        .await
        .map(Json)
    }
}

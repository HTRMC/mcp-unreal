//! `chaos_ops` — Chaos destruction: Geometry Collections and fracturing.
//!
//! Needs the McpLinkChaos interop plugin, which pulls in the engine's
//! GeometryCollection, PlanarCut and Fracture plugins.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum ChaosOp {
    /// New Geometry Collection built from one or more Static Meshes. It starts
    /// as a single unfractured bone; the fracture operations split it.
    Create {
        /// e.g. /Game/Destruction/GC_Wall.
        path: String,
        /// Static Mesh asset paths, combined into one collection.
        source_meshes: Vec<String>,
    },
    /// Bone, geometry and vertex counts, the cluster levels, and the materials.
    Info { collection: String },
    /// Scatter Voronoi sites through the selected bones and cut along them —
    /// the Fracture mode's Uniform tool, and the usual first fracture.
    FractureUniform {
        collection: String,
        /// Bone indices; every leaf bone when omitted.
        bones: Option<Vec<i32>>,
        /// Fewest pieces per bone (default 8).
        min_sites: Option<i32>,
        /// Most pieces per bone (default 12).
        max_sites: Option<i32>,
        /// One site cloud across the whole selection rather than one per bone
        /// (default true).
        group_fracture: Option<bool>,
        seed: Option<i32>,
        /// Gap left between pieces, in cm.
        grout: Option<f64>,
        /// Fraction of selected bones actually cut (default 1.0).
        chance_to_fracture: Option<f64>,
        /// Split disconnected pieces of one cut into separate bones.
        split_islands: Option<bool>,
        /// Roughen the cut surfaces. 0 (default) leaves them flat.
        noise_amplitude: Option<f64>,
        noise_frequency: Option<f64>,
        noise_octaves: Option<i32>,
        /// Material slot used for the newly exposed interior faces.
        internal_material_id: Option<i32>,
    },
    /// Cut along Voronoi cells around sites you place yourself, in the
    /// collection's local space — one piece per site.
    FractureVoronoi {
        collection: String,
        bones: Option<Vec<i32>>,
        /// At least two [x, y, z] points.
        sites: Vec<[f64; 3]>,
        seed: Option<i32>,
        grout: Option<f64>,
        chance_to_fracture: Option<f64>,
        split_islands: Option<bool>,
        noise_amplitude: Option<f64>,
        noise_frequency: Option<f64>,
        noise_octaves: Option<i32>,
    },
    /// Slice with randomly oriented planes — slabs and shards rather than
    /// Voronoi chunks.
    FracturePlanar {
        collection: String,
        bones: Option<Vec<i32>>,
        /// Number of cutting planes (default 3).
        planes: Option<i32>,
        seed: Option<i32>,
        grout: Option<f64>,
        chance_to_fracture: Option<f64>,
        split_islands: Option<bool>,
        noise_amplitude: Option<f64>,
        noise_frequency: Option<f64>,
        noise_octaves: Option<i32>,
    },
    Save { collection: String },
}

#[tool_router(router = chaos_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Chaos destruction: build a Geometry Collection from Static Meshes and fracture it. Each fracture splits the selected bones into children, so fracturing twice gives a two-level cluster — which is what the solver breaks apart at runtime. Uniform scatters Voronoi sites for you, Voronoi takes sites you place, and Planar slices with random planes; all three take grout, noise and an island split. Place the result with spawn_actor on GeometryCollectionActor and point its component's RestCollection at the asset. Needs the McpLinkChaos plugin."
    )]
    async fn chaos_ops(
        &self,
        Parameters(op): Parameters<ChaosOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/chaos/ops", body).await.map(Json)
    }
}

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
    /// Bone, geometry and vertex counts, the cluster levels, the materials, and
    /// whether convex collision hulls exist.
    Info { collection: String },
    /// The individual bones, so the clustering operations have indices to take:
    /// each with its name, cluster level, parent and child count.
    Bones {
        collection: String,
        /// Only bones at this cluster level (0 is the root).
        level: Option<i32>,
        /// Only bones with no children — the pieces that actually break off.
        leaves_only: Option<bool>,
        max_results: Option<i32>,
    },
    /// Group a bone's children into clusters automatically — the Fracture
    /// mode's Auto Cluster. A cluster level is what the solver breaks apart
    /// one stage at a time.
    AutoCluster {
        collection: String,
        /// "by_number" (default), "by_fraction", "by_size" or "by_grid".
        method: Option<String>,
        /// Whose children to cluster; 0 (the root) clusters the whole thing.
        cluster_index: Option<i32>,
        /// by_number: how many clusters.
        site_count: Option<i32>,
        /// by_fraction: clusters as a fraction of the bone count.
        site_fraction: Option<f64>,
        /// by_size: approximate cluster size in cm.
        site_size: Option<f64>,
        /// Only cluster pieces that actually touch (default true).
        enforce_connectivity: Option<bool>,
        avoid_isolated: Option<bool>,
        /// by_grid: cells along each axis.
        grid_x: Option<i32>,
        grid_y: Option<i32>,
        grid_z: Option<i32>,
    },
    /// Put the named bones under one new cluster.
    Cluster {
        collection: String,
        /// Bone indices from `bones`.
        bones: Vec<i32>,
    },
    /// Merge the named clusters into one.
    MergeClusters { collection: String, bones: Vec<i32> },
    /// Pull neighbouring pieces into the named clusters, out to `iterations`
    /// rings of neighbours.
    ClusterMagnet {
        collection: String,
        bones: Vec<i32>,
        /// Default 1.
        iterations: Option<i32>,
    },
    /// Delete bones and everything under them.
    DeleteBones { collection: String, bones: Vec<i32> },
    /// Build the non-overlapping convex hulls the solver collides with. A
    /// collection without them falls back to much coarser collision.
    GenerateConvex {
        collection: String,
        /// How much of a hull may be cut away to remove overlap (default 0.3).
        fraction_allow_remove: Option<f64>,
        /// Merge vertices closer than this, in cm. 0 (default) keeps them all.
        simplification_distance: Option<f64>,
        /// How far a cluster hull may exceed the volume under it (default 0.5).
        can_exceed_fraction: Option<f64>,
    },
    /// Simplify existing hulls — fewer faces, cheaper collision.
    SimplifyConvex {
        collection: String,
        /// Restrict to these bones; all of them when omitted.
        bones: Option<Vec<i32>>,
        /// Allowed deviation in cm (default 5).
        error_tolerance: Option<f64>,
        /// Simplify to this triangle count instead of by tolerance.
        target_triangles: Option<i32>,
    },
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
    /// Assign a material slot to the faces a fracture created. A fractured
    /// collection shows its original material on the fresh interior faces
    /// until this runs, which is why cut surfaces look wrong by default.
    SetInteriorMaterial {
        collection: String,
        /// Index into the collection's Materials array; `info` lists them.
        material_id: i32,
        /// "internal" (default — the faces a cut created), "external" or "all".
        faces: Option<String>,
        /// Restrict to these bones; the whole collection when omitted.
        bones: Option<Vec<i32>>,
    },
    /// Give faces UVs by projecting a box onto them — the quick way to make a
    /// tiling interior material look right.
    BoxProjectUvs {
        collection: String,
        faces: Option<String>,
        /// Projection box edge length in cm (default 100).
        box_size: Option<f64>,
        /// UV channel (default 0).
        uv_layer: Option<i32>,
        /// Size the box to the collection's bounds instead of `box_size`.
        fit_to_bounds: Option<bool>,
    },
    /// Pack the faces' UV islands into a non-overlapping atlas — what a baked
    /// interior texture needs.
    LayoutUvs {
        collection: String,
        faces: Option<String>,
        uv_layer: Option<i32>,
        /// Atlas resolution the gutter is measured against (default 1024).
        resolution: Option<i32>,
        /// Pixels left between islands (default 1).
        gutter: Option<f64>,
    },
    Save { collection: String },
}

#[tool_router(router = chaos_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Chaos destruction: build a Geometry Collection from Static Meshes, fracture it, cluster the pieces and give them convex collision. Each fracture splits the selected bones into children, so fracturing twice gives a two-level cluster — which is what the solver breaks apart at runtime. Uniform scatters Voronoi sites for you, Voronoi takes sites you place, and Planar slices with random planes; all three take grout, noise and an island split. Clustering decides what breaks apart together and in what order: auto_cluster groups a bone's children by count, fraction, size or a grid, and cluster / merge_clusters / cluster_magnet shape it by hand from the indices `bones` reports. generate_convex builds the hulls the solver actually collides with, and set_interior_material, box_project_uvs and layout_uvs finish the faces a cut exposed. Place the result with spawn_actor on GeometryCollectionActor and point its component's RestCollection at the asset. Needs the McpLinkChaos plugin."
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

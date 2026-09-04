//! Performance readback and offline builds (lighting, navigation, captures).

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum PerfOp {
    /// Sample the real frame time over a window and return the distribution —
    /// average, median, min, max and the 99th percentile, where hitching
    /// shows. Blocks for roughly `frames` frames. Run it with PIE going to
    /// measure gameplay rather than editor idle.
    FrameStats {
        /// Frames to sample, 2-600 (default 60).
        frames: Option<i32>,
    },
    /// Process memory: physical and virtual, current and peak.
    Memory {},
    /// Start an Unreal Insights trace to a .utrace file.
    TraceStart {
        /// Output file; defaults to the project's Saved/Profiling folder.
        file: Option<String>,
        /// Trace channels, e.g. "default", "cpu,gpu,frame" (default "default").
        channels: Option<String>,
    },
    TraceStop {},
    TraceStatus {},
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum BuildOp {
    /// Build static lighting with Lightmass. Runs out of process — poll
    /// get_log for completion. A headless (-nullrhi) editor cannot do this.
    Lighting {
        /// Preview, Medium, High or Production (default).
        quality: Option<String>,
    },
    /// Rebuild the navigation mesh for the current level.
    Navigation {},
    /// Rebuild BSP geometry.
    Geometry {},
    /// Rebuild BSP geometry in visible levels only.
    VisibleGeometry {},
    /// Build Hierarchical LODs.
    Hlod {},
    /// Recapture every reflection capture in the level.
    ReflectionCaptures {},
    /// Rebuild texture streaming data.
    TextureStreaming {},
    /// Build runtime virtual texture data.
    VirtualTexture {},
    /// Build every landscape in the level.
    Landscape {},
    /// Everything the Build menu's "Build All Levels" does.
    All {
        /// Lighting quality for the lighting stage.
        quality: Option<String>,
    },
}

#[tool_router(router = perf_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Measure performance: sample real frame times over a window and get average/median/min/max/p99 in milliseconds, read process memory, and start or stop an Unreal Insights trace. This is the structured readback that `stat fps` cannot give you, because that draws to the viewport."
    )]
    async fn perf_ops(&self, Parameters(op): Parameters<PerfOp>) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/editor/perf", body).await.map(Json)
    }

    #[tool(
        description = "Run the editor's offline builds through the same entry point as its Build menu: static lighting (Lightmass), navigation mesh, BSP geometry, Hierarchical LODs, reflection captures, texture streaming, virtual textures and landscapes. Every build but lighting finishes before the call returns; lighting hands off to Lightmass, so poll get_log for it."
    )]
    async fn build_level(
        &self,
        Parameters(op): Parameters<BuildOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/editor/build", body).await.map(Json)
    }
}

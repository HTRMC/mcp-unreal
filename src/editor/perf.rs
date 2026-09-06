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
    /// The engine's own memory report (the `MemReport -full` console
    /// command) as text: objects by class with counts and sizes, texture and
    /// render-target memory, pools and the allocator summary. It is written
    /// under Saved/Profiling/MemReports and read back here.
    #[serde(rename = "memreport")]
    MemReport {
        /// The full report (default true); false is the short form.
        full: Option<bool>,
        /// Cap on the text returned (default 60000 characters); the file
        /// path is reported either way.
        max_chars: Option<u32>,
    },
    /// One `stat <group>` read as data — what the viewport overlay would
    /// draw: each stat's inclusive and exclusive time in milliseconds
    /// (average and max over the window) with call counts, plus counters
    /// and memory. "GPU" is `stat gpu` (the GPU profiler's first queue,
    /// reported as busy/wait/idle milliseconds per pass — a rendering
    /// editor only), "SceneRendering", "Game", "Engine", "Memory",
    /// "Physics", "Niagara", "Anim" and so on; an unknown group comes back
    /// with the list. The group is switched off again afterwards unless it
    /// was already on.
    StatGroup {
        group: String,
        /// Frames to accumulate before reading (default 30).
        frames: Option<i32>,
        /// Cap per list (default 100).
        max_stats: Option<u32>,
        /// Leave the group displaying afterwards (default false).
        keep_enabled: Option<bool>,
    },
    /// Start a CSV profiler capture: per-frame timings and counters to a
    /// .csv under Saved/Profiling/CSV, the format Unreal's PerfReportTool
    /// and CSV-to-SVG read.
    CsvStart {
        /// Stop automatically after this many frames (default: until csv_stop).
        frames: Option<i32>,
        /// Output folder (default Saved/Profiling/CSV).
        folder: Option<String>,
        /// Output file name (default: named by date and time).
        file: Option<String>,
    },
    /// Stop the CSV capture and report the file it wrote.
    CsvStop {},
    CsvStatus {},
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
        description = "Measure performance: sample real frame times over a window and get average/median/min/max/p99 in milliseconds, read process memory, read any `stat <group>` (GPU, SceneRendering, Game, Memory, ...) as per-stat numbers instead of an overlay, take the engine's MemReport, capture a CSV profile, and start or stop an Unreal Insights trace. This is the structured readback that `stat fps` cannot give you, because that draws to the viewport."
    )]
    async fn perf_ops(&self, Parameters(op): Parameters<PerfOp>) -> Result<Json<Value>, ErrorData> {
        // A memory report and a stat window both wait on the editor's frames.
        let timeout = std::time::Duration::from_secs(90);
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin_with_timeout("/api/editor/perf", body, timeout)
            .await
            .map(Json)
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

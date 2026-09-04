//! Movie Render Queue (requires the McpLinkMovieRender plugin in the project).
//!
//! Variants serialise straight into the plugin request body, so the field
//! names here are the contract with `McpMovieRenderRoutes.cpp`.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum MovieRenderOp {
    /// Setting classes a config can hold — render passes, output types
    /// (PNG/JPG/EXR/WAV/encoder), anti-aliasing, burn-ins, CVar overrides.
    ListSettingClasses {
        name_contains: Option<String>,
    },
    /// New Movie Pipeline config, seeded with an output setting plus a render
    /// pass and output type (a config missing either produces no files).
    CreateConfig {
        /// e.g. /Game/Cinematics/MRQ_Preview.
        path: String,
        /// Default "MoviePipelineDeferredPassBase".
        render_pass: Option<String>,
        /// Default "MoviePipelineImageSequenceOutput_PNG".
        output_type: Option<String>,
    },
    /// The config's settings with their paths, plus the resolved output
    /// directory, file-name format and resolution.
    ConfigInfo {
        config: String,
    },
    AddSetting {
        config: String,
        class: String,
    },
    RemoveSetting {
        config: String,
        class: String,
    },
    Save {
        config: String,
    },
    /// Queue a render and start it. Asynchronous — poll `render_status`.
    /// Needs a windowed editor, since the render runs through PIE.
    Render {
        config: String,
        /// Level Sequence asset path.
        sequence: String,
        /// Map to render on; the open level when omitted.
        map: Option<String>,
        name: Option<String>,
        /// Drop any jobs already queued (default true).
        clear_queue: Option<bool>,
    },
    /// Whether a render is running, and each queued job's progress.
    RenderStatus {},
}

#[tool_router(router = movie_render_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Movie Render Queue — turning an authored Level Sequence into files on disk, which sequence_ops could not do. Create a render config (render pass plus output type), tune its settings with set_property on the reported paths (output directory, resolution, frame range, codec), then render a sequence on a map and poll for progress. The render runs through PIE, so it needs a windowed editor. Needs the McpLinkMovieRender plugin enabled."
    )]
    async fn movie_render(
        &self,
        Parameters(op): Parameters<MovieRenderOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/render/movie", body).await.map(Json)
    }
}

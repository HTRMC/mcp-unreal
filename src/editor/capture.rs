//! `capture_viewport` — the agent's eyes. Returns a real MCP image block so
//! the model can look at the frame, plus structured metadata.

use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::{CallToolResult, ContentBlock};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::{Value, json};

use crate::{UnrealMcp, error};

#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct CaptureInput {
    /// "viewport" (default): the viewport's current frame. "high_res": the
    /// viewport redrawn at `width` x `height` (or `multiplier` times its
    /// size), the editor's High Resolution Screenshot. "camera": the scene
    /// from a camera at `location`/`rotation`, independent of any viewport.
    pub mode: Option<String>,
    /// Include Slate/UMG overlays (HUD, menus). Requires a running PIE session.
    pub include_ui: Option<bool>,
    /// "jpg" (default, small) or "png" (lossless, larger).
    pub format: Option<String>,
    /// JPEG quality 1-100 (default 85).
    pub quality: Option<u32>,
    /// Downscale so the longest side is at most this many pixels (default
    /// 1280, or no downscale for high_res; 0 disables).
    pub max_dimension: Option<u32>,
    /// Save to this absolute file path instead of returning the image inline.
    pub output_path: Option<String>,
    /// high_res / camera: the render size in pixels (camera default 1280x720).
    pub width: Option<u32>,
    pub height: Option<u32>,
    /// high_res: size as a multiple of the viewport when width/height are
    /// absent (default 2, max 8).
    pub multiplier: Option<f64>,
    /// camera: where the camera stands [X, Y, Z] in cm (required).
    pub location: Option<[f64; 3]>,
    /// camera: where it looks [Pitch, Yaw, Roll] in degrees (default forward).
    pub rotation: Option<[f64; 3]>,
    /// camera: horizontal field of view in degrees (default 90).
    pub fov: Option<f64>,
    /// camera: "auto" (default: PIE if running, else editor), "pie" or "editor".
    pub world: Option<String>,
}

/// The plugin request for a capture (shared with the contract tests).
pub fn capture_body(input: &CaptureInput) -> Value {
    json!({
        "mode": input.mode,
        "include_ui": input.include_ui,
        "format": input.format,
        "quality": input.quality,
        "max_dimension": input.max_dimension,
        "output_path": input.output_path,
        "width": input.width,
        "height": input.height,
        "multiplier": input.multiplier,
        "location": input.location,
        "rotation": input.rotation,
        "fov": input.fov,
        "world": input.world,
    })
}

#[tool_router(router = capture_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Capture the editor or PIE viewport as an image you can look at: the current frame, a high-resolution redraw of it (mode high_res, any size the GPU can render), or the scene from a camera placed anywhere (mode camera, with location, rotation and fov — no viewport needed). Downscaled to 1280px and JPEG-encoded by default to keep it small. Set include_ui=true (PIE only) to include HUD/UMG overlays. Needs a rendering editor — a headless (-nullrhi) editor has no viewport and no RHI."
    )]
    async fn capture_viewport(
        &self,
        Parameters(input): Parameters<CaptureInput>,
    ) -> Result<CallToolResult, ErrorData> {
        let timeout = std::time::Duration::from_secs(90);
        let data = self
            .call_plugin_with_timeout(
                "/api/editor/capture_viewport",
                capture_body(&input),
                timeout,
            )
            .await?;

        let format = data.get("format").and_then(Value::as_str).unwrap_or("jpg");
        let mime = if format == "png" {
            "image/png"
        } else {
            "image/jpeg"
        };
        let mut meta = json!({
            "width": data.get("width"),
            "height": data.get("height"),
            "format": format,
        });

        // Saved to disk: report the path, no inline payload.
        if let Some(file) = data.get("file").and_then(Value::as_str) {
            meta["file"] = json!(file);
            let mut result = CallToolResult::success(vec![ContentBlock::text(format!(
                "Saved viewport capture to {file}"
            ))]);
            result.structured_content = Some(meta);
            return Ok(result);
        }

        let image = data
            .get("image_base64")
            .and_then(Value::as_str)
            .ok_or_else(|| error::internal("plugin returned no image data"))?;

        let mut result = CallToolResult::success(vec![ContentBlock::image(image, mime)]);
        result.structured_content = Some(meta);
        Ok(result)
    }
}

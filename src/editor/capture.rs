//! `capture_viewport` — the agent's eyes. Returns a real MCP image block so
//! the model can look at the frame, plus structured metadata.

use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::{CallToolResult, ContentBlock};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::{Value, json};

use crate::{UnrealMcp, error};

#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct CaptureInput {
    /// Include Slate/UMG overlays (HUD, menus). Requires a running PIE session.
    pub include_ui: Option<bool>,
    /// "jpg" (default, small) or "png" (lossless, larger).
    pub format: Option<String>,
    /// JPEG quality 1-100 (default 85).
    pub quality: Option<u32>,
    /// Downscale so the longest side is at most this many pixels (default 1280; 0 disables).
    pub max_dimension: Option<u32>,
    /// Save to this absolute file path instead of returning the image inline.
    pub output_path: Option<String>,
}

#[tool_router(router = capture_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Capture the editor or PIE viewport as an image you can look at. Downscaled to 1280px and JPEG-encoded by default to keep it small. Set include_ui=true (PIE only) to include HUD/UMG overlays. Needs a rendering editor — a headless (-nullrhi) editor has no viewport."
    )]
    async fn capture_viewport(
        &self,
        Parameters(input): Parameters<CaptureInput>,
    ) -> Result<CallToolResult, ErrorData> {
        let data = self
            .call_plugin(
                "/api/editor/capture_viewport",
                json!({
                    "include_ui": input.include_ui,
                    "format": input.format,
                    "quality": input.quality,
                    "max_dimension": input.max_dimension,
                    "output_path": input.output_path,
                }),
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

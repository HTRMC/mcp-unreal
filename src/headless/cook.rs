//! `cook_project` — content cooking / packaging via RunUAT BuildCookRun.

use std::time::Duration;

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};

use crate::budget::{cap, last_n_lines};
use crate::headless::parsers::parse_build_diagnostics;
use crate::headless::runner;
use crate::{UnrealMcp, error};

const COOK_TIMEOUT: Duration = Duration::from_secs(60 * 60);

#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct CookInput {
    /// Target platform: Win64 (default), Linux, Mac, Android, IOS.
    pub platform: Option<String>,
    /// Build configuration: Development (default), Shipping, Test, DebugGame.
    pub configuration: Option<String>,
    /// Only cook changed content (much faster on repeat runs).
    pub iterate: Option<bool>,
    /// Also stage the cooked content into a packaged build.
    pub stage: Option<bool>,
    /// Cook only these maps (package paths), instead of everything.
    pub maps: Option<Vec<String>>,
}

#[derive(serde::Serialize, schemars::JsonSchema)]
pub struct CookOutput {
    pub success: bool,
    pub exit_code: i32,
    pub timed_out: bool,
    pub duration_secs: u64,
    pub error_count: usize,
    pub errors: Vec<String>,
    pub log_tail: String,
}

#[tool_router(router = headless_cook_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Cook (and optionally stage) project content for a platform via RunUAT BuildCookRun. No editor needed. Full cooks take many minutes — pass iterate=true after the first one."
    )]
    async fn cook_project(
        &self,
        Parameters(input): Parameters<CookInput>,
    ) -> Result<Json<CookOutput>, ErrorData> {
        let project = self.cfg.project.as_ref().ok_or_else(error::no_project)?;
        if !self.cfg.run_uat.exists() {
            return Err(error::missing_tool(&self.cfg.run_uat, "RunUAT"));
        }

        let platform = input.platform.unwrap_or_else(|| {
            if cfg!(windows) {
                "Win64".into()
            } else if cfg!(target_os = "macos") {
                "Mac".into()
            } else {
                "Linux".into()
            }
        });
        let configuration = input.configuration.unwrap_or_else(|| "Development".into());

        let mut args = vec![
            "BuildCookRun".to_string(),
            format!("-project={}", project.uproject.display()),
            "-noP4".into(),
            format!("-platform={platform}"),
            format!("-clientconfig={configuration}"),
            "-cook".into(),
            "-utf8output".into(),
            "-unattended".into(),
        ];
        if input.stage.unwrap_or(false) {
            args.push("-stage".into());
        } else {
            args.push("-skipstage".into());
        }
        if input.iterate.unwrap_or(false) {
            args.push("-iterate".into());
        }
        if let Some(maps) = input.maps.as_ref().filter(|m| !m.is_empty()) {
            args.push(format!("-map={}", maps.join("+")));
        }

        let result = runner::run(&self.cfg.run_uat, &args, COOK_TIMEOUT)
            .await
            .map_err(|e| error::internal(e.to_string()))?;

        let combined = result.combined();
        let diags = parse_build_diagnostics(&combined);
        let (errors, error_count) = cap(diags.errors, 30);

        Ok(Json(CookOutput {
            success: !result.timed_out && result.exit_code == 0,
            exit_code: result.exit_code,
            timed_out: result.timed_out,
            duration_secs: result.duration_secs,
            error_count,
            errors,
            log_tail: last_n_lines(&combined, 50),
        }))
    }
}

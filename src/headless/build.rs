//! `build_project` and `generate_project_files` — headless UBT via Build.bat.

use std::time::Duration;

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};

use crate::budget::{cap, last_n_lines};
use crate::headless::parsers::parse_build_diagnostics;
use crate::headless::runner;
use crate::{UnrealMcp, error};

const BUILD_TIMEOUT: Duration = Duration::from_secs(30 * 60);
const GEN_TIMEOUT: Duration = Duration::from_secs(10 * 60);
const MAX_ERRORS: usize = 50;
const MAX_WARNINGS: usize = 20;
const LOG_TAIL_LINES: usize = 50;

#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct BuildInput {
    /// Build target, e.g. "MyProjectEditor". Defaults to "<ProjectName>Editor".
    pub target: Option<String>,
    /// Build configuration: Development (default), DebugGame, Debug, Shipping, Test.
    pub config: Option<String>,
    /// Platform: Win64 (default), Linux, Mac.
    pub platform: Option<String>,
    /// Clean before building (runs Clean.bat first).
    pub clean: Option<bool>,
}

#[derive(serde::Serialize, schemars::JsonSchema)]
pub struct BuildOutput {
    pub success: bool,
    pub exit_code: i32,
    pub timed_out: bool,
    pub duration_secs: u64,
    pub error_count: usize,
    pub warning_count: usize,
    pub errors: Vec<String>,
    pub warnings: Vec<String>,
    /// Last lines of the raw build log.
    pub log_tail: String,
}

#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct GenerateProjectFilesInput {}

#[derive(serde::Serialize, schemars::JsonSchema)]
pub struct GenerateOutput {
    pub success: bool,
    pub exit_code: i32,
    pub timed_out: bool,
    pub duration_secs: u64,
    pub log_tail: String,
}

#[tool_router(router = headless_build_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Build the UE project via UnrealBuildTool (Build.bat). No editor needed. Returns structured errors/warnings parsed from the build log. Takes minutes on a cold build."
    )]
    async fn build_project(
        &self,
        Parameters(input): Parameters<BuildInput>,
    ) -> Result<Json<BuildOutput>, ErrorData> {
        let project = self.cfg.project.as_ref().ok_or_else(error::no_project)?;
        if !self.cfg.build_bat.exists() {
            return Err(error::missing_tool(&self.cfg.build_bat, "Build.bat"));
        }
        let target = input
            .target
            .or_else(|| self.cfg.default_editor_target())
            .ok_or_else(error::no_project)?;
        let config = input.config.unwrap_or_else(|| "Development".into());
        let platform = input.platform.unwrap_or_else(default_platform);
        let uproject = project.uproject.display().to_string();

        let base_args = vec![
            target.clone(),
            platform.clone(),
            config.clone(),
            format!("-Project={uproject}"),
            "-WaitMutex".into(),
        ];

        if input.clean.unwrap_or(false) && self.cfg.clean_bat.exists() {
            tracing::info!("cleaning {target} before build");
            let _ = runner::run(&self.cfg.clean_bat, &base_args, GEN_TIMEOUT).await;
        }

        let result = runner::run(&self.cfg.build_bat, &base_args, BUILD_TIMEOUT)
            .await
            .map_err(|e| error::internal(e.to_string()))?;

        let combined = result.combined();
        let diags = parse_build_diagnostics(&combined);
        let (errors, error_count) = cap(diags.errors, MAX_ERRORS);
        let (warnings, warning_count) = cap(diags.warnings, MAX_WARNINGS);

        Ok(Json(BuildOutput {
            success: !result.timed_out && result.exit_code == 0,
            exit_code: result.exit_code,
            timed_out: result.timed_out,
            duration_secs: result.duration_secs,
            error_count,
            warning_count,
            errors,
            warnings,
            log_tail: last_n_lines(&combined, LOG_TAIL_LINES),
        }))
    }

    #[tool(
        description = "Regenerate IDE project files (.sln) for the UE project after adding/removing C++ modules or source files."
    )]
    async fn generate_project_files(
        &self,
        Parameters(_input): Parameters<GenerateProjectFilesInput>,
    ) -> Result<Json<GenerateOutput>, ErrorData> {
        let project = self.cfg.project.as_ref().ok_or_else(error::no_project)?;
        if !self.cfg.build_bat.exists() {
            return Err(error::missing_tool(&self.cfg.build_bat, "Build.bat"));
        }
        let args = vec![
            "-projectfiles".into(),
            format!("-project={}", project.uproject.display()),
            "-game".into(),
            "-progress".into(),
        ];
        let result = runner::run(&self.cfg.build_bat, &args, GEN_TIMEOUT)
            .await
            .map_err(|e| error::internal(e.to_string()))?;
        let combined = result.combined();
        Ok(Json(GenerateOutput {
            success: !result.timed_out && result.exit_code == 0,
            exit_code: result.exit_code,
            timed_out: result.timed_out,
            duration_secs: result.duration_secs,
            log_tail: last_n_lines(&combined, 30),
        }))
    }
}

/// Host platform name UBT/UAT expects, used as the default target platform.
pub fn default_platform() -> String {
    if cfg!(windows) {
        "Win64".into()
    } else if cfg!(target_os = "macos") {
        "Mac".into()
    } else {
        "Linux".into()
    }
}

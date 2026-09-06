//! World Partition builders as a subprocess: HLOD generation, minimap,
//! navigation data and actor resaves through `WorldPartitionBuilderCommandlet`,
//! which is how the Build menu and the engine's own scripts run them.

use std::time::Duration;

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::{Value, json};

use crate::budget::last_n_lines;
use crate::headless::parsers::parse_build_diagnostics;
use crate::headless::runner;
use crate::{UnrealMcp, error};

const BUILDER_TIMEOUT: Duration = Duration::from_secs(4 * 60 * 60);

#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct WorldPartitionBuildInput {
    /// The World Partition map's package path, e.g. /Game/Maps/OpenWorld.
    pub map: String,
    /// "hlod" (default), "minimap", "navigation", "resave_actors", or a
    /// builder class name such as WorldPartitionRenameDuplicateBuilder.
    pub builder: Option<String>,
    /// HLOD steps: "setup" (create HLOD actors for the layers), "build"
    /// (generate their meshes), "delete", "finalize". Default setup + build.
    pub steps: Option<Vec<String>>,
    /// Build only this HLOD layer asset (hlod builder).
    pub hlod_layer: Option<String>,
    /// Extra commandlet switches, e.g. ["-Verbose"].
    pub extra_args: Option<Vec<String>>,
    /// Render during the build (HLOD meshes and the minimap need it; default
    /// true).
    pub allow_rendering: Option<bool>,
}

#[tool_router(router = headless_wp_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Run a World Partition builder on a map without the editor open: generate HLODs (setup, build, delete, finalize; one layer or all), the minimap, navigation data, or resave actors. Runs UnrealEditor-Cmd with WorldPartitionBuilderCommandlet and returns the parsed errors and log tail. Long: HLOD builds on a large world take hours."
    )]
    async fn world_partition_build(
        &self,
        Parameters(input): Parameters<WorldPartitionBuildInput>,
    ) -> Result<Json<Value>, ErrorData> {
        let project = self.cfg.project.as_ref().ok_or_else(error::no_project)?;
        if !self.cfg.editor_cmd.exists() {
            return Err(error::no_editor_binary(&self.cfg.editor_cmd));
        }
        let builder_spec = input.builder.as_deref().unwrap_or("hlod").to_string();
        let builder = match builder_spec.to_ascii_lowercase().as_str() {
            "hlod" | "hlods" => "WorldPartitionHLODsBuilder".to_string(),
            "minimap" => "WorldPartitionMiniMapBuilder".to_string(),
            "navigation" | "nav" | "navmesh" => "WorldPartitionNavigationDataBuilder".to_string(),
            "resave_actors" | "resave" => "WorldPartitionResaveActorsBuilder".to_string(),
            _ => builder_spec.clone(),
        };
        let mut args = vec![
            project.uproject.display().to_string(),
            input.map.clone(),
            "-run=WorldPartitionBuilderCommandlet".to_string(),
            format!("-Builder={builder}"),
            "-Unattended".to_string(),
            "-NoShaderCompile".to_string(),
            "-stdout".to_string(),
            "-FullStdOutLogOutput".to_string(),
        ];
        if input.allow_rendering.unwrap_or(true) {
            args.push("-AllowCommandletRendering".to_string());
        }
        if builder == "WorldPartitionHLODsBuilder" {
            let steps = input
                .steps
                .clone()
                .unwrap_or_else(|| vec!["setup".to_string(), "build".to_string()]);
            for step in &steps {
                let switch = match step.to_ascii_lowercase().as_str() {
                    "setup" => "-SetupHLODs",
                    "build" => "-BuildHLODs",
                    "delete" => "-DeleteHLODs",
                    "finalize" | "finalise" => "-FinalizeHLODs",
                    other => {
                        return Err(error::invalid(format!(
                            "unknown HLOD step '{other}' — setup, build, delete or finalize"
                        )));
                    }
                };
                args.push(switch.to_string());
            }
            if let Some(layer) = &input.hlod_layer {
                args.push(format!("-BuildHLODLayer={layer}"));
            }
        }
        if let Some(extra) = &input.extra_args {
            args.extend(extra.iter().cloned());
        }
        let result = runner::run(&self.cfg.editor_cmd, &args, BUILDER_TIMEOUT)
            .await
            .map_err(|e| error::internal(e.to_string()))?;
        let combined = result.combined();
        let diags = parse_build_diagnostics(&combined);
        let (errors, error_count) = crate::budget::cap(diags.errors, 30);
        let (warnings, warning_count) = crate::budget::cap(diags.warnings, 30);
        // The builder's own lines: what it did, and why it stopped.
        let messages: Vec<String> = combined
            .lines()
            .filter(|l| {
                (l.contains("LogWorldPartition") || l.contains("Builder") || l.contains("Error:"))
                    && !l.contains("LogModuleManager")
                    && !l.contains("LogGameFeatures")
                    && !l.contains("LoadErrors")
                    && !l.contains("LogCsvProfiler")
                    && !l.contains("LogInit")
                    && !l.contains("LogWorldPartition: Display: FWorldPartitionClassDescRegistry")
            })
            .map(|l| l.trim().to_string())
            .collect();
        let (messages, message_count) = crate::budget::cap(messages, 40);
        let not_partitioned = combined.contains("not a World Partition")
            || combined.contains("is not partitioned")
            || combined.contains("WorldPartition is null")
            || combined.contains("does not use World Partition")
            || combined.contains("not using World Partition")
            || combined.contains("no World Partition")
            || combined.contains("is not a partitioned world")
            || combined.contains("not partitioned");
        Ok(Json(json!({
            "success": !result.timed_out && result.exit_code == 0 && error_count == 0,
            "exit_code": result.exit_code,
            "timed_out": result.timed_out,
            "duration_secs": result.duration_secs,
            "map": input.map,
            "builder": builder,
            "args": args,
            "error_count": error_count,
            "errors": errors,
            "warning_count": warning_count,
            "warnings": warnings,
            "not_world_partition": not_partitioned,
            "message_count": message_count,
            "messages": messages,
            "log_tail": last_n_lines(&combined, 40),
        })))
    }
}

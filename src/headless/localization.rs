//! `localization_ops` — localization targets, cultures, gather and compile.
//!
//! Split transport, one tool: target and culture edits go to the editor
//! plugin, while gather and compile run the GatherText commandlet as a
//! subprocess. The engine's own in-editor task wrappers all take a parent
//! SWindow for their progress dialog, so they cannot run from a headless
//! editor at all — and a commandlet is what the editor's Localization
//! Dashboard shells out to anyway.

use std::time::Duration;

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::{Value, json};

use crate::budget::last_n_lines;
use crate::headless::parsers::parse_build_diagnostics;
use crate::headless::runner;
use crate::{UnrealMcp, error};

const COMMANDLET_TIMEOUT: Duration = Duration::from_secs(60 * 60);

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum LocalizationOp {
    /// The project's localization targets, their cultures and where each
    /// one's manifest, archives and .locres live.
    ListTargets {},
    /// Culture codes this engine knows, for add_culture.
    ListCultures {
        name_contains: Option<String>,
        max_results: Option<i32>,
    },
    /// New localization target, with its commandlet config written.
    CreateTarget {
        /// e.g. "Game".
        target: String,
        /// The culture the source text is authored in (default "en").
        native_culture: Option<String>,
    },
    TargetInfo { target: String },
    AddCulture { target: String, culture: String },
    RemoveCulture { target: String, culture: String },
    /// Make an already-supported culture the source language.
    SetNativeCulture { target: String, culture: String },
    /// Which sources `gather` collects strings from. The target the engine
    /// ships in BaseEditor.ini has every one of these disabled, so gathering it
    /// untouched produces an empty manifest. Omitted fields are left alone.
    ConfigureGather {
        target: String,
        /// Gather FText from source and config files.
        from_text_files: Option<bool>,
        /// Project-relative directories to scan, e.g. ["Source", "Config"].
        text_search_directories: Option<Vec<String>>,
        /// Gather text properties out of assets.
        from_packages: Option<bool>,
        /// Package wildcards, e.g. ["Content/*"].
        package_include_wildcards: Option<Vec<String>>,
        /// Gather from asset metadata.
        from_metadata: Option<bool>,
    },
    /// Rewrite the target's commandlet config files from its current settings.
    /// `gather` and `compile` read those, so run this after changing a target
    /// with set_property.
    GenerateConfigs { target: String },
    /// Run the GatherText commandlet: collect source strings into the target's
    /// manifest and archives. Minutes on a large project.
    Gather { target: String },
    /// Compile the archives into the .locres files the game loads.
    Compile { target: String },
    /// Write the archives out as PO files, the format translators work in.
    ExportPo { target: String },
    /// Read translated PO files back into the archives.
    ImportPo { target: String },
    /// The source strings a gather found for one culture, with their
    /// translations and which are still untranslated.
    ListTranslations {
        target: String,
        culture: String,
        namespace_contains: Option<String>,
        text_contains: Option<String>,
        /// Only entries whose translation is missing or still the source text.
        untranslated_only: Option<bool>,
        max_results: Option<i32>,
    },
    /// Translate one entry, in the archive `compile` reads.
    SetTranslation {
        target: String,
        culture: String,
        /// Usually "" — the namespace from list_translations.
        namespace: Option<String>,
        key: String,
        translation: String,
    },
}

#[derive(serde::Serialize, schemars::JsonSchema)]
pub struct CommandletOutput {
    pub success: bool,
    pub exit_code: i32,
    pub timed_out: bool,
    pub duration_secs: u64,
    pub target: String,
    /// The config script the commandlet was run against.
    pub config: String,
    pub error_count: usize,
    pub errors: Vec<String>,
    pub log_tail: String,
}

impl UnrealMcp {
    /// Run one GatherText step against a target's config script.
    async fn run_localization_commandlet(
        &self,
        target: &str,
        step: &str,
    ) -> Result<Json<Value>, ErrorData> {
        let project = self.cfg.project.as_ref().ok_or_else(error::no_project)?;
        if !self.cfg.editor_cmd.exists() {
            return Err(error::no_editor_binary(&self.cfg.editor_cmd));
        }
        // The dashboard's own layout: Config/Localization/<Target>_<Step>.ini,
        // written by the plugin's generate_configs.
        let config = format!("Config/Localization/{target}_{step}.ini");
        let absolute = project.root.join(&config);
        if !absolute.exists() {
            return Err(error::invalid(format!(
                "no config script at {} — run localization_ops generate_configs (or create_target) \
                 first; that is what writes it",
                absolute.display()
            )));
        }

        let args = vec![
            project.uproject.display().to_string(),
            "-Run=GatherText".to_string(),
            format!("-Config={config}"),
            "-Unattended".to_string(),
            "-NoShaderCompile".to_string(),
            "-stdout".to_string(),
            "-FullStdOutLogOutput".to_string(),
        ];
        let result = runner::run(&self.cfg.editor_cmd, &args, COMMANDLET_TIMEOUT)
            .await
            .map_err(|e| error::internal(e.to_string()))?;
        let combined = result.combined();
        let diags = parse_build_diagnostics(&combined);
        let (errors, error_count) = crate::budget::cap(diags.errors, 20);

        Ok(Json(json!({
            "success": !result.timed_out && result.exit_code == 0,
            "exit_code": result.exit_code,
            "timed_out": result.timed_out,
            "duration_secs": result.duration_secs,
            "target": target,
            "config": absolute.display().to_string(),
            "error_count": error_count,
            "errors": errors,
            "log_tail": last_n_lines(&combined, 40),
        })))
    }
}

#[tool_router(router = localization_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Localization: list and create targets, manage their cultures and which one is native, point the gather at its sources, then gather source strings into the target's manifest and archives, translate them (in place, or by exporting PO files for translators and importing them back), and compile the result into the .locres files the game loads. Target edits go through the running editor; gather and compile run the GatherText commandlet as a subprocess, which is what the Localization Dashboard does too — the engine's in-editor wrappers need a progress dialog and cannot run headless."
    )]
    async fn localization_ops(
        &self,
        Parameters(op): Parameters<LocalizationOp>,
    ) -> Result<Json<Value>, ErrorData> {
        match &op {
            LocalizationOp::Gather { target } => {
                self.run_localization_commandlet(target, "Gather").await
            }
            LocalizationOp::Compile { target } => {
                self.run_localization_commandlet(target, "Compile").await
            }
            LocalizationOp::ExportPo { target } => {
                self.run_localization_commandlet(target, "Export").await
            }
            LocalizationOp::ImportPo { target } => {
                self.run_localization_commandlet(target, "Import").await
            }
            _ => {
                let body = serde_json::to_value(&op)
                    .map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
                self.call_plugin("/api/workflow/localization", body)
                    .await
                    .map(Json)
            }
        }
    }
}

//! `package_project` — full packaging via RunUAT BuildCookRun.
//!
//! `cook_project` stops at cooked content; this goes all the way to a
//! runnable build: compile, cook, stage, pak, archive — plus dedicated-server
//! targets and "Launch On" deploy/run against a connected device.

use std::path::PathBuf;
use std::time::Duration;

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};

use crate::budget::{cap, last_n_lines};
use crate::headless::parsers::parse_build_diagnostics;
use crate::headless::runner;
use crate::{UnrealMcp, error};

const PACKAGE_TIMEOUT: Duration = Duration::from_secs(3 * 60 * 60);
const MAX_ERRORS: usize = 30;
const LOG_TAIL_LINES: usize = 60;

#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct PackageInput {
    /// Target platform: Win64 (default on Windows), Linux, Mac, Android, IOS.
    pub platform: Option<String>,
    /// Build configuration: Shipping (default), Development, Test, DebugGame.
    pub configuration: Option<String>,
    /// Where the finished build is archived. Defaults to
    /// `<project>/Saved/Packaged/<platform>`. Pass `archive: false` to leave
    /// the build in Saved/StagedBuilds instead.
    pub archive_directory: Option<String>,
    /// Copy the staged build into `archive_directory` (default true).
    pub archive: Option<bool>,
    /// Cook content into .pak files rather than loose files (default true).
    pub pak: Option<bool>,
    /// Use the IoStore container format alongside the paks.
    pub iostore: Option<bool>,
    /// Compress pak/container contents (default true).
    pub compressed: Option<bool>,
    /// Also build, cook and stage the dedicated server target.
    pub server: Option<bool>,
    /// With `server`, package only the server (no client build).
    pub server_only: Option<bool>,
    /// Mark as a distribution build (store submission: signing, no dev flags).
    pub distribution: Option<bool>,
    /// Stage the platform prerequisites installer next to the build.
    pub prereqs: Option<bool>,
    /// Skip the C++ compile step (content-only package of an existing binary).
    pub skip_build: Option<bool>,
    /// Skip cooking and reuse whatever is already cooked.
    pub skip_cook: Option<bool>,
    /// Only cook content that changed since the last cook.
    pub iterate: Option<bool>,
    /// Cook only these maps (package paths) instead of everything.
    pub maps: Option<Vec<String>>,
    /// Deploy the build to a device and run it ("Launch On"). Device id is
    /// `<Platform>@<Name>`, e.g. "Android@Pixel7" — or just the device name.
    pub device: Option<String>,
    /// Extra raw arguments appended to the BuildCookRun command line.
    pub extra_args: Option<Vec<String>>,
}

#[derive(serde::Serialize, schemars::JsonSchema)]
pub struct PackageOutput {
    pub success: bool,
    pub exit_code: i32,
    pub timed_out: bool,
    pub duration_secs: u64,
    /// Where the finished build was archived, when archiving was requested.
    pub archive_directory: Option<String>,
    /// Whether `archive_directory` exists on disk after the run.
    pub archive_exists: bool,
    pub error_count: usize,
    pub errors: Vec<String>,
    /// The BuildCookRun command line, so a failed run can be rerun by hand.
    pub command: String,
    pub log_tail: String,
}

/// Build the BuildCookRun argument list. Split out so the flag combinations
/// are unit-testable without an engine install.
pub(crate) fn package_args(
    uproject: &str,
    project_root: &std::path::Path,
    input: &PackageInput,
) -> (Vec<String>, Option<PathBuf>) {
    let platform = input
        .platform
        .clone()
        .unwrap_or_else(super::build::default_platform);
    let configuration = input
        .configuration
        .clone()
        .unwrap_or_else(|| "Shipping".into());
    let server_only = input.server_only.unwrap_or(false);
    let server = input.server.unwrap_or(false) || server_only;

    let mut args = vec![
        "BuildCookRun".to_string(),
        format!("-project={uproject}"),
        "-noP4".into(),
        "-utf8output".into(),
        "-unattended".into(),
        "-nocompileeditor".into(),
        format!("-platform={platform}"),
        format!("-clientconfig={configuration}"),
    ];

    if input.skip_build.unwrap_or(false) {
        args.push("-skipbuild".into());
    } else {
        args.push("-build".into());
    }
    if input.skip_cook.unwrap_or(false) {
        args.push("-skipcook".into());
    } else {
        args.push("-cook".into());
    }
    if input.iterate.unwrap_or(false) {
        args.push("-iterate".into());
    }
    args.push("-stage".into());

    if input.pak.unwrap_or(true) {
        args.push("-pak".into());
        if input.compressed.unwrap_or(true) {
            args.push("-compressed".into());
        }
    } else {
        args.push("-nopak".into());
    }
    if input.iostore.unwrap_or(false) {
        args.push("-iostore".into());
    }

    if server {
        args.push("-server".into());
        args.push(format!("-serverconfig={configuration}"));
        if server_only {
            args.push("-noclient".into());
        }
    }
    if input.distribution.unwrap_or(false) {
        args.push("-distribution".into());
    }
    if input.prereqs.unwrap_or(false) {
        args.push("-prereqs".into());
    }

    let mut archive_directory = None;
    if input.archive.unwrap_or(true) {
        let dir = input.archive_directory.clone().map(PathBuf::from).unwrap_or_else(|| {
            project_root
                .join("Saved")
                .join("Packaged")
                .join(&platform)
        });
        args.push("-archive".into());
        args.push(format!("-archivedirectory={}", dir.display()));
        archive_directory = Some(dir);
    }

    if let Some(maps) = input.maps.as_ref().filter(|m| !m.is_empty()) {
        args.push(format!("-map={}", maps.join("+")));
    }

    if let Some(device) = input.device.as_ref().filter(|d| !d.is_empty()) {
        // "Launch On": deploy the staged build and start it. UAT wants a
        // fully-qualified <Platform>@<Name>; a bare name is qualified here.
        let device = if device.contains('@') {
            device.clone()
        } else {
            format!("{platform}@{device}")
        };
        args.push("-deploy".into());
        args.push("-run".into());
        args.push(format!("-device={device}"));
    }

    if let Some(extra) = input.extra_args.as_ref() {
        args.extend(extra.iter().cloned());
    }
    (args, archive_directory)
}

#[tool_router(router = headless_package_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Package the project into a runnable build via RunUAT BuildCookRun: compile, cook, stage, pak and archive, optionally including a dedicated server target, a distribution build, or a deploy-and-run on a connected device (Launch On). No editor needed. A full package takes many minutes to hours — pass iterate=true or skip_cook=true on repeats. Use cook_project instead when only cooked content is wanted."
    )]
    async fn package_project(
        &self,
        Parameters(input): Parameters<PackageInput>,
    ) -> Result<Json<PackageOutput>, ErrorData> {
        let project = self.cfg.project.as_ref().ok_or_else(error::no_project)?;
        if !self.cfg.run_uat.exists() {
            return Err(error::missing_tool(&self.cfg.run_uat, "RunUAT"));
        }

        let (args, archive_directory) = package_args(
            &project.uproject.display().to_string(),
            &project.root,
            &input,
        );
        let command = format!("{} {}", self.cfg.run_uat.display(), args.join(" "));

        let result = runner::run(&self.cfg.run_uat, &args, PACKAGE_TIMEOUT)
            .await
            .map_err(|e| error::internal(e.to_string()))?;

        let combined = result.combined();
        let diags = parse_build_diagnostics(&combined);
        let (errors, error_count) = cap(diags.errors, MAX_ERRORS);
        let archive_exists = archive_directory
            .as_ref()
            .is_some_and(|dir| dir.exists());

        Ok(Json(PackageOutput {
            success: !result.timed_out && result.exit_code == 0,
            exit_code: result.exit_code,
            timed_out: result.timed_out,
            duration_secs: result.duration_secs,
            archive_directory: archive_directory.map(|d| d.display().to_string()),
            archive_exists,
            error_count,
            errors,
            command,
            log_tail: last_n_lines(&combined, LOG_TAIL_LINES),
        }))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn input() -> PackageInput {
        PackageInput {
            platform: Some("Win64".into()),
            configuration: None,
            archive_directory: None,
            archive: None,
            pak: None,
            iostore: None,
            compressed: None,
            server: None,
            server_only: None,
            distribution: None,
            prereqs: None,
            skip_build: None,
            skip_cook: None,
            iterate: None,
            maps: None,
            device: None,
            extra_args: None,
        }
    }

    fn args_for(input: &PackageInput) -> Vec<String> {
        package_args("X/Demo.uproject", std::path::Path::new("X"), input).0
    }

    #[test]
    fn defaults_build_cook_stage_pak_and_archive() {
        let args = args_for(&input());
        for expected in [
            "BuildCookRun",
            "-build",
            "-cook",
            "-stage",
            "-pak",
            "-compressed",
            "-archive",
            "-clientconfig=Shipping",
            "-platform=Win64",
        ] {
            assert!(args.iter().any(|a| a == expected), "missing {expected} in {args:?}");
        }
        assert!(args.iter().any(|a| a.starts_with("-archivedirectory=")));
    }

    #[test]
    fn server_only_drops_the_client() {
        let mut i = input();
        i.server_only = Some(true);
        let args = args_for(&i);
        assert!(args.iter().any(|a| a == "-server"));
        assert!(args.iter().any(|a| a == "-noclient"));
        assert!(args.iter().any(|a| a == "-serverconfig=Shipping"));
    }

    #[test]
    fn skipping_build_and_cook_swaps_the_flags() {
        let mut i = input();
        i.skip_build = Some(true);
        i.skip_cook = Some(true);
        let args = args_for(&i);
        assert!(args.iter().any(|a| a == "-skipbuild"));
        assert!(args.iter().any(|a| a == "-skipcook"));
        assert!(!args.iter().any(|a| a == "-build"));
        assert!(!args.iter().any(|a| a == "-cook"));
    }

    #[test]
    fn nopak_suppresses_compression() {
        let mut i = input();
        i.pak = Some(false);
        let args = args_for(&i);
        assert!(args.iter().any(|a| a == "-nopak"));
        assert!(!args.iter().any(|a| a == "-compressed"));
    }

    #[test]
    fn bare_device_name_is_qualified_with_the_platform() {
        let mut i = input();
        i.platform = Some("Android".into());
        i.device = Some("Pixel7".into());
        let args = args_for(&i);
        assert!(args.iter().any(|a| a == "-device=Android@Pixel7"));
        assert!(args.iter().any(|a| a == "-deploy"));
        assert!(args.iter().any(|a| a == "-run"));

        i.device = Some("Android@Other".into());
        let args = args_for(&i);
        assert!(args.iter().any(|a| a == "-device=Android@Other"));
    }

    #[test]
    fn archive_can_be_turned_off() {
        let mut i = input();
        i.archive = Some(false);
        let (args, dir) = package_args("X/Demo.uproject", std::path::Path::new("X"), &i);
        assert!(dir.is_none());
        assert!(!args.iter().any(|a| a == "-archive"));
    }
}

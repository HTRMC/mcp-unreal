use std::env;
use std::path::{Path, PathBuf};

/// The engine version the McpLink plugin's C++ targets. Support for other
/// UE 5.x versions is planned; until then `status` flags a mismatch instead of
/// letting an agent hit confusing errors deeper in.
pub const SUPPORTED_ENGINE: (u64, u64) = (5, 8);

/// The UE project the server operates on.
#[derive(Debug, Clone)]
pub struct ProjectInfo {
    pub uproject: PathBuf,
    pub root: PathBuf,
    pub name: String,
}

/// Server configuration. Loading never fails: missing pieces are detected at
/// tool-call time so the server always starts and can explain what to fix.
#[derive(Debug, Clone)]
pub struct Config {
    pub engine_root: PathBuf,
    pub editor_cmd: PathBuf,
    pub build_bat: PathBuf,
    pub clean_bat: PathBuf,
    pub run_uat: PathBuf,
    pub project: Option<ProjectInfo>,
    pub plugin_port: u16,
    pub docs_cache_dir: PathBuf,
}

impl Config {
    pub fn load() -> Self {
        let engine_root = detect_engine_root();
        let editor_cmd = env::var_os("UE_EDITOR_PATH")
            .map(PathBuf::from)
            .unwrap_or_else(|| editor_cmd_path(&engine_root));
        let batch = engine_root.join("Engine").join("Build").join("BatchFiles");
        let (build_bat, clean_bat, run_uat) = if cfg!(windows) {
            (
                batch.join("Build.bat"),
                batch.join("Clean.bat"),
                batch.join("RunUAT.bat"),
            )
        } else {
            (
                batch.join("Build.sh"),
                batch.join("Clean.sh"),
                batch.join("RunUAT.sh"),
            )
        };
        let plugin_port = env::var("PLUGIN_PORT")
            .ok()
            .and_then(|v| v.parse().ok())
            .unwrap_or(8091);
        let docs_cache_dir = env::var_os("MCP_UNREAL_DOCS_CACHE")
            .map(PathBuf::from)
            .unwrap_or_else(|| {
                dirs::data_local_dir()
                    .unwrap_or_else(|| PathBuf::from("."))
                    .join("mcp-unreal")
            });

        Self {
            engine_root,
            editor_cmd,
            build_bat,
            clean_bat,
            run_uat,
            project: detect_project(),
            plugin_port,
            docs_cache_dir,
        }
    }

    /// Default editor target name, e.g. `MyProjectEditor`.
    pub fn default_editor_target(&self) -> Option<String> {
        self.project.as_ref().map(|p| format!("{}Editor", p.name))
    }
}

fn detect_engine_root() -> PathBuf {
    if let Some(root) = env::var_os("UE_ENGINE_ROOT") {
        return PathBuf::from(root);
    }
    discover_engine_roots()
        .into_iter()
        .next()
        .unwrap_or_else(fallback_engine_root)
}

/// Every UE install we can find, best match first: the version McpLink targets,
/// then the newest. Sources are the Epic Launcher's own install manifest (which
/// knows about installs on any drive) followed by the conventional locations
/// for the platform.
pub fn discover_engine_roots() -> Vec<PathBuf> {
    let mut roots: Vec<PathBuf> = Vec::new();
    for candidate in launcher_installs()
        .into_iter()
        .chain(conventional_installs())
    {
        if is_engine_root(&candidate) && !roots.contains(&candidate) {
            roots.push(candidate);
        }
    }
    roots.sort_by_key(|root| {
        let version = engine_version_at(root);
        let is_supported =
            version.is_some_and(|(major, minor, _)| (major, minor) == SUPPORTED_ENGINE);
        // false sorts before true, so negate; then newest version first.
        (
            !is_supported,
            std::cmp::Reverse(version.unwrap_or((0, 0, 0))),
        )
    });
    roots
}

/// An engine install is anything with a readable `Engine/Build/Build.version`,
/// which covers both Launcher installs and source builds.
fn is_engine_root(path: &Path) -> bool {
    path.join("Engine")
        .join("Build")
        .join("Build.version")
        .exists()
}

/// `(major, minor, patch)` of an install, from its `Build.version`.
pub fn engine_version_at(engine_root: &Path) -> Option<(u64, u64, u64)> {
    let path = engine_root
        .join("Engine")
        .join("Build")
        .join("Build.version");
    parse_build_version(&std::fs::read_to_string(path).ok()?)
}

pub(crate) fn parse_build_version(text: &str) -> Option<(u64, u64, u64)> {
    let v: serde_json::Value = serde_json::from_str(text).ok()?;
    Some((
        v.get("MajorVersion")?.as_u64()?,
        v.get("MinorVersion")?.as_u64()?,
        v.get("PatchVersion")?.as_u64()?,
    ))
}

/// Engine installs recorded by the Epic Games Launcher.
#[cfg(windows)]
fn launcher_installs() -> Vec<PathBuf> {
    let program_data = env::var_os("PROGRAMDATA")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(r"C:\ProgramData"));
    read_launcher_manifest(
        &program_data
            .join("Epic")
            .join("UnrealEngineLauncher")
            .join("LauncherInstalled.dat"),
    )
}

#[cfg(target_os = "macos")]
fn launcher_installs() -> Vec<PathBuf> {
    read_launcher_manifest(Path::new(
        "/Users/Shared/Epic Games/UnrealEngineLauncher/LauncherInstalled.dat",
    ))
}

/// No Epic Launcher on Linux — installs are source builds found by convention.
#[cfg(all(not(windows), not(target_os = "macos")))]
fn launcher_installs() -> Vec<PathBuf> {
    Vec::new()
}

#[cfg(any(windows, target_os = "macos"))]
fn read_launcher_manifest(path: &Path) -> Vec<PathBuf> {
    std::fs::read_to_string(path)
        .ok()
        .map(|text| parse_launcher_manifest(&text))
        .unwrap_or_default()
}

/// Install locations of engine artifacts (`UE_5.8`, ...) in the launcher
/// manifest. The same file lists marketplace content, which is filtered out by
/// the `UE_` prefix.
pub(crate) fn parse_launcher_manifest(text: &str) -> Vec<PathBuf> {
    let Ok(manifest) = serde_json::from_str::<serde_json::Value>(text) else {
        return Vec::new();
    };
    manifest
        .get("InstallationList")
        .and_then(|list| list.as_array())
        .map(|entries| {
            entries
                .iter()
                .filter(|entry| {
                    entry
                        .get("AppName")
                        .and_then(|n| n.as_str())
                        .is_some_and(|name| name.starts_with("UE_"))
                })
                .filter_map(|entry| {
                    entry
                        .get("InstallLocation")
                        .and_then(|p| p.as_str())
                        .filter(|p| !p.is_empty())
                        .map(PathBuf::from)
                })
                .collect()
        })
        .unwrap_or_default()
}

fn conventional_installs() -> Vec<PathBuf> {
    let mut out = Vec::new();
    if cfg!(windows) {
        for drive in ['C', 'D', 'E'] {
            out.extend(engine_dirs_in(&PathBuf::from(format!(
                r"{drive}:\Program Files\Epic Games"
            ))));
        }
    } else if cfg!(target_os = "macos") {
        out.extend(engine_dirs_in(Path::new("/Users/Shared/Epic Games")));
        out.extend(engine_dirs_in(Path::new("/Applications/Epic Games")));
    } else {
        for path in [
            "/opt/UnrealEngine",
            "/opt/unreal-engine",
            "/usr/local/UnrealEngine",
        ] {
            out.push(PathBuf::from(path));
        }
        if let Some(home) = dirs::home_dir() {
            out.push(home.join("UnrealEngine"));
            out.extend(engine_dirs_in(&home.join("Epic Games")));
        }
    }
    out
}

/// Immediate `UE_*` subdirectories of an Epic Games install folder.
fn engine_dirs_in(dir: &Path) -> Vec<PathBuf> {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return Vec::new();
    };
    entries
        .filter_map(|entry| entry.ok())
        .map(|entry| entry.path())
        .filter(|path| {
            path.file_name()
                .and_then(|n| n.to_str())
                .is_some_and(|name| name.starts_with("UE_"))
        })
        .collect()
}

/// Where to point the user when no install was found, so error messages name a
/// plausible path for their platform rather than nothing at all.
fn fallback_engine_root() -> PathBuf {
    if cfg!(windows) {
        PathBuf::from(r"C:\Program Files\Epic Games\UE_5.8")
    } else if cfg!(target_os = "macos") {
        PathBuf::from("/Users/Shared/Epic Games/UE_5.8")
    } else {
        PathBuf::from("/opt/UnrealEngine")
    }
}

fn editor_cmd_path(engine_root: &Path) -> PathBuf {
    let bin = engine_root.join("Engine").join("Binaries");
    if cfg!(windows) {
        bin.join("Win64").join("UnrealEditor-Cmd.exe")
    } else if cfg!(target_os = "macos") {
        bin.join("Mac").join("UnrealEditor-Cmd")
    } else {
        bin.join("Linux").join("UnrealEditor-Cmd")
    }
}

fn detect_project() -> Option<ProjectInfo> {
    if let Some(spec) = env::var_os("MCP_UNREAL_PROJECT") {
        let spec = PathBuf::from(spec);
        if spec.extension().is_some_and(|e| e == "uproject") {
            return project_from_uproject(&spec);
        }
        if let Some(found) = uproject_in_dir(&spec) {
            return project_from_uproject(&found);
        }
        tracing::warn!(
            "MCP_UNREAL_PROJECT is set to {} but no .uproject was found there",
            spec.display()
        );
        return None;
    }
    // Walk up from cwd looking for a .uproject.
    let mut dir = env::current_dir().ok()?;
    for _ in 0..12 {
        if let Some(found) = uproject_in_dir(&dir) {
            return project_from_uproject(&found);
        }
        if !dir.pop() {
            break;
        }
    }
    None
}

fn uproject_in_dir(dir: &Path) -> Option<PathBuf> {
    let entries = std::fs::read_dir(dir).ok()?;
    entries
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .find(|p| p.extension().is_some_and(|e| e == "uproject"))
}

fn project_from_uproject(uproject: &Path) -> Option<ProjectInfo> {
    let name = uproject.file_stem()?.to_string_lossy().into_owned();
    let root = uproject.parent()?.to_path_buf();
    Some(ProjectInfo {
        uproject: uproject.to_path_buf(),
        root,
        name,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn editor_target_derived_from_project_name() {
        let cfg = Config {
            engine_root: PathBuf::new(),
            editor_cmd: PathBuf::new(),
            build_bat: PathBuf::new(),
            clean_bat: PathBuf::new(),
            run_uat: PathBuf::new(),
            project: Some(ProjectInfo {
                uproject: PathBuf::from("X/Demo.uproject"),
                root: PathBuf::from("X"),
                name: "Demo".into(),
            }),
            plugin_port: 8091,
            docs_cache_dir: PathBuf::new(),
        };
        assert_eq!(cfg.default_editor_target().as_deref(), Some("DemoEditor"));
    }

    #[test]
    fn parses_build_version() {
        let text = r#"{"MajorVersion": 5, "MinorVersion": 8, "PatchVersion": 2,
            "Changelist": 56702186, "BranchName": "++UE5+Release-5.8"}"#;
        assert_eq!(parse_build_version(text), Some((5, 8, 2)));
        assert_eq!(parse_build_version("not json"), None);
        assert_eq!(parse_build_version(r#"{"MajorVersion": 5}"#), None);
    }

    #[test]
    fn parses_launcher_manifest_and_skips_marketplace_entries() {
        let text = r#"{
            "InstallationList": [
                {
                    "InstallLocation": "D:\\Program Files\\Epic Games\\UE_5.8",
                    "AppName": "UE_5.8",
                    "AppVersion": "5.8.2-56702186+++UE5+Release-5.8-Windows"
                },
                {
                    "InstallLocation": "C:\\Program Files\\Epic Games\\UE_5.6",
                    "AppName": "UE_5.6"
                },
                {
                    "InstallLocation": "C:\\Marketplace\\SomePack",
                    "AppName": "SomePack_5.8"
                }
            ]
        }"#;
        let roots = parse_launcher_manifest(text);
        assert_eq!(
            roots,
            vec![
                PathBuf::from(r"D:\Program Files\Epic Games\UE_5.8"),
                PathBuf::from(r"C:\Program Files\Epic Games\UE_5.6"),
            ]
        );
    }

    #[test]
    fn launcher_manifest_tolerates_garbage() {
        assert!(parse_launcher_manifest("").is_empty());
        assert!(parse_launcher_manifest("{}").is_empty());
        assert!(parse_launcher_manifest(r#"{"InstallationList": 3}"#).is_empty());
    }

    #[test]
    fn detected_engine_root_is_a_real_install_or_the_platform_fallback() {
        // Whatever the machine has, discovery must only ever return installs
        // that actually exist, and the fallback must be absolute.
        for root in discover_engine_roots() {
            assert!(
                is_engine_root(&root),
                "{} is not an engine root",
                root.display()
            );
        }
        assert!(fallback_engine_root().is_absolute());
    }
}

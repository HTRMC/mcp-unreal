//! `status` — server health and live-probed capabilities. Agents should call
//! this first: it reports what works right now and how to fix what doesn't.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};

use crate::UnrealMcp;

#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct StatusInput {}

#[derive(serde::Serialize, schemars::JsonSchema)]
pub struct ProjectStatus {
    pub uproject: String,
    pub name: String,
}

/// What the background update run last concluded. Reported here rather than
/// checked here: `status` never goes to the network.
#[derive(serde::Serialize, schemars::JsonSchema)]
pub struct UpdateReport {
    /// `apply`, `check` or `off`, from `MCP_UNREAL_AUTO_UPDATE`.
    pub channel: String,
    pub current: String,
    pub latest: Option<String>,
    /// `up_to_date`, `update_available`, `staged`, `applied`, `blocked`,
    /// `unavailable`, or `checking` when no run has finished yet.
    pub state: String,
    pub detail: Option<String>,
}

#[derive(serde::Serialize, schemars::JsonSchema)]
pub struct StatusOutput {
    pub server_version: String,
    pub engine_root: String,
    /// Whether `engine_root` was pinned, discovered, or is only a fallback
    /// guess because no install was found.
    pub engine_root_source: String,
    /// Every UE install discovery could find, best match first.
    pub engine_installs_found: Vec<String>,
    pub engine_version: Option<String>,
    pub editor_cmd_found: bool,
    pub project: Option<ProjectStatus>,
    pub plugin_port: u16,
    pub plugin_online: bool,
    /// Raw status payload from the McpLink plugin when it is reachable
    /// (PIE state, plugin version, loaded feature providers).
    pub plugin_status: Option<serde_json::Value>,
    /// What works right now: headless_build, headless_test, editor_* groups.
    pub features: Vec<String>,
    /// Remediation hints for anything offline.
    pub hints: Vec<String>,
    pub update: UpdateReport,
}

#[tool_router(router = status_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Check server health: UE install, project detection, editor plugin connectivity, and the list of currently available features. Call this first."
    )]
    async fn status(
        &self,
        Parameters(_input): Parameters<StatusInput>,
    ) -> Result<Json<StatusOutput>, ErrorData> {
        let cfg = &self.cfg;
        let mut features = Vec::new();
        let mut hints = Vec::new();

        let editor_cmd_found = cfg.editor_cmd.exists();
        let engine_version = engine_version(&cfg.engine_root);
        let installs = crate::config::discover_engine_roots();

        // A fallback root is a guess. Left unlabelled it reads as a detection,
        // and every headless tool then fails against a path that was never
        // there — so say what was searched and how to pin it.
        match cfg.engine_root_source {
            crate::config::EngineRootSource::Fallback => hints.push(format!(
                "no UE install found — {} is a fallback guess, not a detected install. \
                 Searched: {}. Set UE_ENGINE_ROOT to the folder containing Engine/ \
                 (a source build or an install outside these locations needs this)",
                cfg.engine_root.display(),
                crate::config::engine_search_locations().join(", "),
            )),
            crate::config::EngineRootSource::Pinned if !editor_cmd_found => hints.push(format!(
                "UE_ENGINE_ROOT points at {}, which does not look like an engine root \
                 (no Engine/Binaries/…/UnrealEditor-Cmd). It should be the folder \
                 containing Engine/, not the Engine/ folder itself{}",
                cfg.engine_root.display(),
                if installs.is_empty() {
                    String::new()
                } else {
                    format!(
                        " — discovery did find {}",
                        installs
                            .iter()
                            .map(|p| p.display().to_string())
                            .collect::<Vec<_>>()
                            .join(", ")
                    )
                },
            )),
            _ => {}
        }

        // McpLink's C++ targets one engine version; say so up front rather than
        // letting a mismatch surface as a compile or route error later.
        if let Some((major, minor, _)) = crate::config::engine_version_at(&cfg.engine_root) {
            let (want_major, want_minor) = crate::config::SUPPORTED_ENGINE;
            if (major, minor) != (want_major, want_minor) {
                hints.push(format!(
                    "engine at {} is UE {major}.{minor}, but McpLink targets UE {want_major}.{want_minor} — \
                     the plugin will not build against this version; set UE_ENGINE_ROOT to a {want_major}.{want_minor} install",
                    cfg.engine_root.display()
                ));
            }
        }

        if editor_cmd_found {
            if cfg.project.is_some() {
                features.push("headless_build".to_string());
                features.push("headless_test".to_string());
            } else {
                hints.push(
                    "no UE project detected — set MCP_UNREAL_PROJECT or run from a project dir"
                        .to_string(),
                );
            }
        } else {
            hints.push(format!(
                "UnrealEditor-Cmd not found at {} — set UE_ENGINE_ROOT",
                cfg.editor_cmd.display()
            ));
        }

        let (plugin_online, plugin_status) = match self.editor.ping().await {
            Ok(v) => (true, Some(v)),
            Err(e) => {
                hints.push(format!(
                    "editor plugin offline on port {} ({e}) — open the Unreal Editor with the McpLink plugin enabled for editor tools",
                    cfg.plugin_port
                ));
                (false, None)
            }
        };
        if plugin_online {
            features.push("editor_control".to_string());
            // Interop plugins register their own route groups.
            let routes: Vec<String> = plugin_status
                .as_ref()
                .and_then(|v| v.get("routes"))
                .and_then(|r| r.as_array())
                .map(|a| {
                    a.iter()
                        .filter_map(|x| x.as_str().map(str::to_string))
                        .collect()
                })
                .unwrap_or_default();
            for (prefix, feature) in [
                ("/api/audio/metasound", "metasounds"),
                ("/api/render/movie", "movie_render_queue"),
                ("/api/chaos/", "chaos_destruction"),
                ("/api/niagara/", "niagara"),
                ("/api/gas/", "gameplay_abilities"),
                ("/api/pcg/", "pcg"),
                ("/api/python/", "python"),
                ("/api/toolsets/", "toolset_registry"),
                ("/api/geometry/", "geometry_script"),
                ("/api/anim/control_rig", "control_rig"),
                ("/api/world/snapshot", "level_snapshots"),
                ("/api/anim/ik_rig", "ik_rig"),
                ("/api/vp/remote_control", "remote_control"),
                ("/api/vp/live_link", "live_link"),
                ("/api/anim/pose_search", "pose_search"),
                ("/api/mass/", "mass"),
                ("/api/data/chooser", "chooser"),
                ("/api/cameras/", "gameplay_cameras"),
            ] {
                if routes.iter().any(|r| r.starts_with(prefix)) {
                    features.push(feature.to_string());
                }
            }
        }
        // The server and the plugin are one contract, so a mismatched pair
        // fails as a 404 on a route the server is sure exists. Say which is
        // behind rather than letting it surface that way.
        let server_version = env!("CARGO_PKG_VERSION");
        if let Some(plugin_version) = plugin_status
            .as_ref()
            .and_then(|v| v.get("plugin_version"))
            .and_then(|v| v.as_str())
            && plugin_version != server_version
        {
            hints.push(format!(
                "version mismatch: this server is {server_version}, the McpLink plugin \
                 in the editor is {plugin_version} — they ship as a pair, so install \
                 both from the same release; a tool whose route only exists on one \
                 side of the pair will fail"
            ));
        }

        if cfg.engine_root.join("Engine").join("Source").exists() {
            features.push(if self.docs.is_ready() {
                "engine_source_lookup".to_string()
            } else {
                "engine_source_lookup (index builds on first use)".to_string()
            });
        } else {
            hints.push(format!(
                "no Engine/Source under {} — lookup_class and search_api will not work",
                cfg.engine_root.display()
            ));
        }

        let update_env = crate::update::UpdateEnv::from_config(cfg);
        // A state file written before the updater was switched off describes a
        // world that is no longer being kept current, so it is not reported.
        let update_state = match update_env.channel {
            crate::update::Channel::Off => None,
            _ => crate::update::read_state(&update_env),
        };
        if let Some(state) = &update_state
            && let Some(detail) = &state.detail
            && matches!(state.state.as_str(), "update_available" | "blocked")
        {
            hints.push(detail.clone());
        }

        Ok(Json(StatusOutput {
            server_version: server_version.to_string(),
            engine_root: cfg.engine_root.display().to_string(),
            engine_root_source: cfg.engine_root_source.as_str().to_string(),
            engine_installs_found: installs.iter().map(|p| p.display().to_string()).collect(),
            engine_version,
            editor_cmd_found,
            project: cfg.project.as_ref().map(|p| ProjectStatus {
                uproject: p.uproject.display().to_string(),
                name: p.name.clone(),
            }),
            plugin_port: cfg.plugin_port,
            plugin_online,
            plugin_status,
            features,
            hints,
            update: UpdateReport {
                channel: update_env.channel.as_str().to_string(),
                current: server_version.to_string(),
                latest: update_state.as_ref().and_then(|s| s.latest.clone()),
                // Nothing is being checked when the updater is switched off,
                // and a stale state file from before it was switched off is
                // not what is happening now either.
                state: match update_env.channel {
                    crate::update::Channel::Off => "off".to_string(),
                    _ => update_state
                        .as_ref()
                        .map(|s| s.state.clone())
                        .unwrap_or_else(|| "checking".to_string()),
                },
                detail: update_state.as_ref().and_then(|s| s.detail.clone()),
            },
        }))
    }
}

pub fn engine_version(engine_root: &std::path::Path) -> Option<String> {
    let (major, minor, patch) = crate::config::engine_version_at(engine_root)?;
    Some(format!("{major}.{minor}.{patch}"))
}

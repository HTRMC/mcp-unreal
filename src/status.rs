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

#[derive(serde::Serialize, schemars::JsonSchema)]
pub struct StatusOutput {
    pub server_version: String,
    pub engine_root: String,
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
                ("/api/niagara/", "niagara"),
                ("/api/gas/", "gameplay_abilities"),
                ("/api/pcg/", "pcg"),
                ("/api/python/", "python"),
            ] {
                if routes.iter().any(|r| r.starts_with(prefix)) {
                    features.push(feature.to_string());
                }
            }
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

        Ok(Json(StatusOutput {
            server_version: env!("CARGO_PKG_VERSION").to_string(),
            engine_root: cfg.engine_root.display().to_string(),
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
        }))
    }
}

pub fn engine_version(engine_root: &std::path::Path) -> Option<String> {
    let (major, minor, patch) = crate::config::engine_version_at(engine_root)?;
    Some(format!("{major}.{minor}.{patch}"))
}

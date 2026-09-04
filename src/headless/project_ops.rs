//! `project_ops` (.uproject) and `config_ops` (.ini) — file-level project
//! configuration. Both work with the editor closed; enabling a plugin or
//! changing config generally needs an editor restart to take effect.

use std::path::PathBuf;

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::{Value, json};

use crate::headless::ini;
use crate::{UnrealMcp, error};

#[derive(serde::Deserialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum ProjectOp {
    /// Engine association, description, modules and plugin list from the .uproject.
    GetInfo {},
    /// Plugins referenced by the .uproject and whether each is enabled.
    ListPlugins {},
    /// Enable a plugin (adds the entry if absent). Requires an editor restart.
    EnablePlugin {
        /// Plugin name as it appears in its .uplugin, e.g. "EnhancedInput".
        name: String,
    },
    /// Disable a plugin. Requires an editor restart.
    DisablePlugin { name: String },
}

#[derive(serde::Deserialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum ConfigOp {
    /// List the sections present in a config file.
    ListSections {
        /// Config file name, e.g. "DefaultEngine.ini" (default), "DefaultGame.ini".
        file: Option<String>,
    },
    /// Read every value for a key (UE allows repeated keys).
    Get {
        /// Section without brackets, e.g. "/Script/Engine.RendererSettings".
        section: String,
        key: String,
        file: Option<String>,
    },
    /// Set a key, replacing an existing plain assignment or appending.
    Set {
        section: String,
        key: String,
        value: String,
        file: Option<String>,
    },
    /// Remove every entry for a key in a section.
    Delete {
        section: String,
        key: String,
        file: Option<String>,
    },
}

impl UnrealMcp {
    fn uproject_path(&self) -> Result<PathBuf, ErrorData> {
        Ok(self
            .cfg
            .project
            .as_ref()
            .ok_or_else(error::no_project)?
            .uproject
            .clone())
    }

    fn read_uproject(&self) -> Result<(PathBuf, Value), ErrorData> {
        let path = self.uproject_path()?;
        let text = std::fs::read_to_string(&path)
            .map_err(|e| error::internal(format!("cannot read {}: {e}", path.display())))?;
        let value: Value = serde_json::from_str(&text)
            .map_err(|e| error::internal(format!("{} is not valid JSON: {e}", path.display())))?;
        Ok((path, value))
    }

    fn write_uproject(&self, path: &std::path::Path, value: &Value) -> Result<(), ErrorData> {
        // UE writes .uproject with tab indentation; match it to keep diffs clean.
        let mut text = String::new();
        let formatter = serde_json::ser::PrettyFormatter::with_indent(b"\t");
        let mut ser =
            serde_json::Serializer::with_formatter(unsafe { text.as_mut_vec() }, formatter);
        serde::Serialize::serialize(value, &mut ser)
            .map_err(|e| error::internal(format!("cannot serialize .uproject: {e}")))?;
        text.push('\n');
        std::fs::write(path, text)
            .map_err(|e| error::internal(format!("cannot write {}: {e}", path.display())))
    }

    fn config_path(&self, file: Option<String>) -> Result<PathBuf, ErrorData> {
        let project = self.cfg.project.as_ref().ok_or_else(error::no_project)?;
        let name = file.unwrap_or_else(|| "DefaultEngine.ini".to_string());
        if name.contains('/') || name.contains('\\') || name.contains("..") {
            return Err(error::invalid(
                "'file' must be a bare config file name such as DefaultEngine.ini",
            ));
        }
        Ok(project.root.join("Config").join(name))
    }
}

#[tool_router(router = project_ops_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Read and modify the .uproject file: project info, and enabling/disabling plugins. Plugin changes need an editor restart and usually a rebuild."
    )]
    async fn project_ops(
        &self,
        Parameters(op): Parameters<ProjectOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let (path, mut project) = self.read_uproject()?;

        match op {
            ProjectOp::GetInfo {} => Ok(Json(json!({
                "uproject": path.display().to_string(),
                "engine_association": project.get("EngineAssociation"),
                "description": project.get("Description"),
                "modules": project.get("Modules"),
                "plugin_count": project.get("Plugins").and_then(Value::as_array).map_or(0, Vec::len),
            }))),

            ProjectOp::ListPlugins {} => {
                let plugins: Vec<Value> = project
                    .get("Plugins")
                    .and_then(Value::as_array)
                    .map(|list| {
                        list.iter()
                            .map(|p| {
                                json!({
                                    "name": p.get("Name"),
                                    "enabled": p.get("Enabled").and_then(Value::as_bool).unwrap_or(true),
                                })
                            })
                            .collect()
                    })
                    .unwrap_or_default();
                Ok(Json(json!({"plugins": plugins, "total": plugins.len()})))
            }

            ProjectOp::EnablePlugin { name } => {
                if name.trim().is_empty() {
                    return Err(error::invalid("'name' must be a plugin name"));
                }
                set_plugin_enabled(&mut project, &name, true);
                self.write_uproject(&path, &project)?;
                Ok(Json(json!({
                    "plugin": name,
                    "enabled": true,
                    "message": "restart the editor (and rebuild if the plugin has C++ modules) for this to take effect",
                })))
            }

            ProjectOp::DisablePlugin { name } => {
                if name.trim().is_empty() {
                    return Err(error::invalid("'name' must be a plugin name"));
                }
                set_plugin_enabled(&mut project, &name, false);
                self.write_uproject(&path, &project)?;
                Ok(Json(json!({
                    "plugin": name,
                    "enabled": false,
                    "message": "restart the editor for this to take effect",
                })))
            }
        }
    }

    #[tool(
        description = "Read and modify UE .ini config files (DefaultEngine.ini, DefaultGame.ini, ...): list_sections, get, set, delete. Repeated keys are preserved; comments and formatting survive edits."
    )]
    async fn config_ops(
        &self,
        Parameters(op): Parameters<ConfigOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let file = match &op {
            ConfigOp::ListSections { file }
            | ConfigOp::Get { file, .. }
            | ConfigOp::Set { file, .. }
            | ConfigOp::Delete { file, .. } => file.clone(),
        };
        let path = self.config_path(file)?;
        let text = std::fs::read_to_string(&path).unwrap_or_default();

        match op {
            ConfigOp::ListSections { .. } => {
                let names: Vec<String> = ini::sections(&text)
                    .into_iter()
                    .map(|(name, entries)| format!("{name} ({} keys)", entries.len()))
                    .collect();
                Ok(Json(
                    json!({"file": path.display().to_string(), "sections": names}),
                ))
            }

            ConfigOp::Get { section, key, .. } => {
                let values = ini::get(&text, &section, &key);
                Ok(Json(json!({
                    "file": path.display().to_string(),
                    "section": section,
                    "key": key,
                    "values": values,
                    "found": !values.is_empty(),
                })))
            }

            ConfigOp::Set {
                section,
                key,
                value,
                ..
            } => {
                let updated = ini::set(&text, &section, &key, &value);
                if let Some(dir) = path.parent() {
                    let _ = std::fs::create_dir_all(dir);
                }
                std::fs::write(&path, updated).map_err(|e| {
                    error::internal(format!("cannot write {}: {e}", path.display()))
                })?;
                Ok(Json(json!({
                    "file": path.display().to_string(),
                    "section": section,
                    "key": key,
                    "value": value,
                    "message": "restart the editor (or reload config) for this to take effect",
                })))
            }

            ConfigOp::Delete { section, key, .. } => {
                let (updated, removed) = ini::remove(&text, &section, &key);
                std::fs::write(&path, updated).map_err(|e| {
                    error::internal(format!("cannot write {}: {e}", path.display()))
                })?;
                Ok(Json(json!({
                    "file": path.display().to_string(),
                    "section": section,
                    "key": key,
                    "removed": removed,
                })))
            }
        }
    }
}

fn set_plugin_enabled(project: &mut Value, name: &str, enabled: bool) {
    let plugins = project
        .as_object_mut()
        .expect("uproject root is an object")
        .entry("Plugins")
        .or_insert_with(|| Value::Array(Vec::new()));
    let Some(list) = plugins.as_array_mut() else {
        return;
    };
    for entry in list.iter_mut() {
        if entry
            .get("Name")
            .and_then(Value::as_str)
            .is_some_and(|n| n.eq_ignore_ascii_case(name))
        {
            if let Some(object) = entry.as_object_mut() {
                object.insert("Enabled".into(), Value::Bool(enabled));
            }
            return;
        }
    }
    list.push(json!({"Name": name, "Enabled": enabled}));
}

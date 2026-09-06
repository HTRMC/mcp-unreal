//! The engine's Toolset Registry, bridged (requires the McpLinkToolsets
//! plugin, which pulls in the engine's ToolsetRegistry plugin; the toolsets
//! themselves come from Epic's `AllToolsets` plugin or the individual
//! `*Toolset` plugins, or from a project's own `UToolsetDefinition` classes).
//!
//! Variants serialise straight into the plugin request body, so field names
//! here are the contract with `McpToolsetRoutes.cpp`.

use std::time::Duration;

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

/// How long `execute` waits for a tool by default. Most tools answer at once;
/// the asynchronous ones (starting PIE, rendering an asset thumbnail,
/// building a semantic search index) take seconds to minutes.
const DEFAULT_EXECUTE_TIMEOUT_SECS: u64 = 120;
const MAX_EXECUTE_TIMEOUT_SECS: u64 = 3600;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum ToolsetOp {
    /// Every toolset the registry holds: name, version, description, whether
    /// it is enabled, how many tools it exposes (and how many the project's
    /// block/allow lists hide), plus the registry's own block and allow
    /// patterns.
    ListToolsets {
        /// Substring filter on toolset name or description.
        name_contains: Option<String>,
        /// Also list each toolset's tools with their descriptions (default false).
        include_tools: Option<bool>,
    },
    /// The tools themselves, as `Toolset.Tool` names with descriptions —
    /// what `execute` takes.
    ListTools {
        /// Only this toolset's tools (a name from `list_toolsets`).
        toolset: Option<String>,
        /// Substring filter on tool name or description, e.g. "Dataflow" or "camera".
        name_contains: Option<String>,
        /// Include each tool's full `inputSchema` (default false — use
        /// `get_tool` for one tool's schema; a whole toolset's is large).
        include_schemas: Option<bool>,
        /// Cap on tools returned (default 500).
        max_results: Option<u32>,
    },
    /// One tool's full entry: description and the JSON schema of its
    /// `arguments`, as the registry generated it from the UFunction.
    GetTool {
        /// Full name from `list_tools` ("EditorApp.GetCameraTransform"); a
        /// bare tool name works when exactly one toolset defines it.
        tool: String,
    },
    /// Run a tool. Its return value comes back as `result`, converted to
    /// JSON by the registry (objects and actors as paths, structs as
    /// objects, maps keyed by their keys). A tool that raises a script error
    /// fails the call with that message.
    Execute {
        /// Full name from `list_tools`, e.g. "EditorApp.GetSelectedActors".
        tool: String,
        /// Arguments keyed by the tool's parameter names — `get_tool` reports
        /// them under `inputSchema.properties`. Omit for a tool with none.
        arguments: Option<Value>,
        /// Seconds to wait for an asynchronous tool (default 120, max 3600).
        timeout_secs: Option<u64>,
    },
    /// Agent Skill assets in the project — the prompts and instructions a
    /// project authors for AI agents, each with its brief description.
    ListSkills {},
    /// A skill's full instructions.
    GetSkill {
        /// Skill path from `list_skills`, e.g. "/Game/Skills/MySkill.MySkill_C".
        path: String,
    },
}

#[tool_router(router = toolset_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "The engine's own Toolset Registry (UE 5.8, experimental): list, describe and execute every AI-callable tool the editor has registered — Epic's toolsets (Dataflow graphs, Chaos Cloth assets, Game Feature plugins, plugin creation, MVVM, Data Registries, World Conditions, Conversations, Live Coding, semantic asset search, gameplay cues, config settings with schemas, the Slate driver, Sequencer and Control Rig, editor camera and content browser, and more: 52 toolsets and some 830 C++ and Python tools with AllToolsets enabled), any toolset the project defines (UToolsetDefinition subclasses, Python toolsets), and Agent Skill assets — Epic's and the project's own instructions for agents. Tool names are `<Toolset>.<Tool>` with a dotted toolset name (`EditorToolset.EditorAppToolset.GetCameraTransform`, `animation_toolset.toolsets.sequencer.SequencerTools.create_camera`); a bare tool name works when only one toolset defines it. Use list_toolsets to see what is registered, list_tools/get_tool for a tool's argument schema, then execute with arguments keyed by parameter name. Tools run under the registry's own block/allow lists and manage their own transactions. Needs the McpLinkToolsets plugin, which enables the engine's ToolsetRegistry plugin; enable Epic's AllToolsets plugin (or individual *Toolset plugins) for Epic's toolsets."
    )]
    async fn toolset_ops(
        &self,
        Parameters(op): Parameters<ToolsetOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let timeout = match &op {
            ToolsetOp::Execute { timeout_secs, .. } => Some(Duration::from_secs(
                timeout_secs
                    .unwrap_or(DEFAULT_EXECUTE_TIMEOUT_SECS)
                    .clamp(1, MAX_EXECUTE_TIMEOUT_SECS),
            )),
            _ => None,
        };
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        match timeout {
            Some(timeout) => self
                .call_plugin_with_timeout("/api/toolsets/ops", body, timeout)
                .await
                .map(Json),
            None => self.call_plugin("/api/toolsets/ops", body).await.map(Json),
        }
    }
}

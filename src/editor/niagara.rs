//! Niagara interop (requires the McpLinkNiagara plugin in the project).
//!
//! Variants serialise straight into the plugin request body, so field names
//! here are the contract with `McpNiagaraRoutes.cpp`.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum NiagaraOp {
    /// Find Niagara system assets. Engine templates live under "/Niagara"
    /// (e.g. /Niagara/DefaultAssets/Templates/Systems/SimpleExplosion).
    ListSystems {
        /// Package path to search under (default "/Game").
        path_prefix: Option<String>,
        max_results: Option<u32>,
    },
    /// A system's emitters and exposed user parameters with their types and defaults.
    SystemInfo {
        /// System asset path.
        system: String,
    },
    /// Spawn a system into the world, at a location or attached to an actor.
    /// Returns the component path used by the other operations.
    Spawn {
        system: String,
        /// "auto" (default: PIE if running, else editor), "pie", or "editor".
        world: Option<String>,
        /// [X, Y, Z]; relative to the attach actor when `attach_to` is set.
        location: Option<[f64; 3]>,
        /// [Pitch, Yaw, Roll].
        rotation: Option<[f64; 3]>,
        scale: Option<[f64; 3]>,
        /// Actor path or label to attach to (its root component).
        attach_to: Option<String>,
        /// Socket name on the attach component.
        socket: Option<String>,
        /// Destroy the component when the system completes (default false so it can be inspected).
        auto_destroy: Option<bool>,
        /// Activate immediately (default true).
        auto_activate: Option<bool>,
        /// User parameter values keyed by name ("SpawnRate" or "User.SpawnRate"):
        /// number, bool, [x,y] / [x,y,z] / [x,y,z,w] array, or object path.
        parameters: Option<Value>,
    },
    /// Every live Niagara component in a world with its system and state.
    ListComponents { world: Option<String> },
    /// One component's state plus its current override parameter values.
    ComponentInfo {
        /// Component object path from spawn / list_components, or an actor path/label
        /// (first Niagara component on it).
        component: Option<String>,
        actor: Option<String>,
        world: Option<String>,
    },
    /// Set user parameters on a live component (typed from the system's exposed parameters).
    SetParameters {
        component: Option<String>,
        actor: Option<String>,
        world: Option<String>,
        parameters: Value,
        /// Restart the simulation after applying (default false).
        reset: Option<bool>,
    },
    Activate {
        component: Option<String>,
        actor: Option<String>,
        world: Option<String>,
        /// Reset the simulation on activation.
        reset: Option<bool>,
    },
    Deactivate {
        component: Option<String>,
        actor: Option<String>,
        world: Option<String>,
    },
    SetPaused {
        component: Option<String>,
        actor: Option<String>,
        world: Option<String>,
        paused: bool,
    },
    /// Destroy the component.
    Destroy {
        component: Option<String>,
        actor: Option<String>,
        world: Option<String>,
    },
}

#[tool_router(router = niagara_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Niagara VFX: list systems (engine templates under /Niagara), inspect emitters and user parameters, spawn systems at a location or attached to an actor, set typed user parameters on live components, activate/deactivate/pause/destroy them, and list what is playing in a world. Needs the McpLinkNiagara plugin enabled in the project."
    )]
    async fn niagara_ops(
        &self,
        Parameters(op): Parameters<NiagaraOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/niagara/ops", body).await.map(Json)
    }
}

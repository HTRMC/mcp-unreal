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

/// Authoring, as opposed to `NiagaraOp`'s driving of live systems.
///
/// A system holds emitters; each emitter holds four script stacks and a list
/// of renderers. A stack is an ordered list of modules, and a module has typed
/// inputs — the same shape the Niagara editor's stack view shows.
#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum NiagaraAuthorOp {
    /// The script stacks a module can be added to, and whether each belongs to
    /// the system or to an emitter.
    ListStages {},
    /// Niagara module scripts available to add to a stack.
    ListModuleScripts {
        name_contains: Option<String>,
        /// Only modules valid in this stage, e.g. "particle_update".
        stage: Option<String>,
        /// Include modules the library hides (default false).
        include_non_library: Option<bool>,
        max_results: Option<u32>,
    },
    /// Renderer classes `add_renderer` accepts (sprite, mesh, ribbon, light, ...).
    ListRendererClasses {},
    /// New Niagara system asset with the default system scripts.
    CreateSystem {
        /// e.g. /Game/FX/NS_Sparks.
        path: String,
    },
    /// New standalone Niagara emitter asset, usable as a source for add_emitter.
    CreateEmitter {
        path: String,
        /// Start from the default module and renderer set (default true).
        add_defaults: Option<bool>,
    },
    /// The whole authoring view of a system: every emitter, its four script
    /// stacks with their ordered modules and typed inputs, and its renderers.
    Stack {
        system: String,
        /// Include each module's input names and types (default true).
        include_inputs: Option<bool>,
    },
    /// Add an emitter to a system from an emitter asset.
    AddEmitter {
        system: String,
        /// Emitter asset path; engine templates live under /Niagara.
        emitter_asset: String,
        /// Name for the emitter inside the system.
        name: Option<String>,
        /// Copy the emitter rather than inheriting from it (default true).
        create_copy: Option<bool>,
    },
    RemoveEmitter {
        system: String,
        /// Emitter name or id from `stack`.
        emitter: String,
    },
    RenameEmitter {
        system: String,
        emitter: String,
        name: String,
    },
    SetEmitterEnabled {
        system: String,
        emitter: String,
        enabled: bool,
    },
    /// Add a module script to one script stack.
    AddModule {
        system: String,
        /// "system_spawn", "system_update", "emitter_spawn", "emitter_update",
        /// "particle_spawn" or "particle_update".
        stage: String,
        /// Required for the emitter-scoped stages.
        emitter: Option<String>,
        /// Module script path, from list_module_scripts.
        module: String,
        /// Position in the stack; appended when omitted.
        index: Option<i32>,
        /// Name for the module in the stack.
        name: Option<String>,
    },
    /// Remove a module and relink the stack around it.
    RemoveModule {
        system: String,
        stage: String,
        emitter: Option<String>,
        /// Module id or name from `stack`.
        module: String,
    },
    SetModuleEnabled {
        system: String,
        stage: String,
        emitter: Option<String>,
        module: String,
        enabled: bool,
    },
    /// Set a literal value on one of a module's inputs.
    SetModuleInput {
        system: String,
        stage: String,
        emitter: Option<String>,
        module: String,
        /// Input name from `stack`, e.g. "SpawnRate".
        input: String,
        /// Number, bool, or [x,y] / [x,y,z] / [x,y,z,w], matching the input type.
        value: Value,
    },
    /// Add a renderer to an emitter. Its options are ordinary properties on the
    /// reported path, so set_property tunes them.
    AddRenderer {
        system: String,
        emitter: String,
        /// e.g. "NiagaraSpriteRendererProperties".
        class: String,
    },
    RemoveRenderer {
        system: String,
        emitter: String,
        /// Renderer index from `stack`.
        index: i32,
    },
    /// Compile the system and report each script's compile status plus any
    /// error messages. Run this after authoring — it is what surfaces a broken
    /// stack. Compilation is asynchronous: when `compiling` comes back true,
    /// poll `compile_status`.
    Compile {
        system: String,
        force: Option<bool>,
    },
    /// The same report as `compile` without requesting a new compile.
    CompileStatus {
        system: String,
    },
    Save {
        system: String,
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

    #[tool(
        description = "Author Niagara systems rather than just drive them: create system and emitter assets, add/remove/rename/enable emitters, read the full stack (every emitter's emitter-spawn/update and particle-spawn/update module lists with typed inputs, plus renderers), add and remove modules from any stack, set literal module input values, add and remove renderers, then compile and save. Renderer and module *options* are ordinary properties on the reported paths, so set_property tunes them. Compile after authoring — that is what reports a broken stack. Needs the McpLinkNiagara plugin enabled."
    )]
    async fn niagara_author(
        &self,
        Parameters(op): Parameters<NiagaraAuthorOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/niagara/author", body).await.map(Json)
    }
}

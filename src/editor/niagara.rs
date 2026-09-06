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
// set_module_input carries every way an input can be driven, which makes it
// much larger than the other variants. Boxing it would turn the generated tool
// schema into a $ref indirection, and the schema is the contract with the MCP
// client, so the size stays: one of these exists per tool call.
#[allow(clippy::large_enum_variant)]
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
    /// Dynamic input scripts — the graphs a module input can be driven by
    /// instead of a literal (Random Range Float, Curve, Multiply Float, ...),
    /// each with the type it produces. `list_input_options` answers the same
    /// question for one specific input, already filtered to what fits it.
    ListDynamicInputs {
        name_contains: Option<String>,
        /// Only dynamic inputs producing this type, e.g. "float", "Vector", "LinearColor".
        output_type: Option<String>,
        /// Include scripts the library hides (default false).
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
    ///
    /// Each input reports its `path` (what `set_module_input` takes), type
    /// and `mode` — "local" with its `value`, "linked" with the parameter it
    /// reads, "dynamic" with the dynamic input script and that script's own
    /// `inputs` nested underneath (paths like "Drag/Minimum"), "data_interface",
    /// "object_asset", "expression", or "default_function" — plus `can_reset`,
    /// `enum_options`, `static` for static switches and any edit condition.
    Stack {
        system: String,
        /// Include each module's inputs with their current values (default true).
        /// Reading them builds the editor's stack view, so pass false for a
        /// quick module and renderer listing.
        include_inputs: Option<bool>,
        /// Also list inputs the stack panel hides — the other branches of a
        /// static switch (default false). They report `visible: false`.
        include_hidden: Option<bool>,
    },
    /// One module with its full input tree — the same detail `stack` gives,
    /// for a single module.
    GetModule {
        system: String,
        stage: String,
        emitter: Option<String>,
        /// Module id or name from `stack`.
        module: String,
        include_hidden: Option<bool>,
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
        /// Report the new module's hidden inputs too (default false).
        include_hidden: Option<bool>,
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
    /// Set one of a module's inputs, exactly as the stack panel would: a
    /// literal `value`, a `dynamic_input` script (with its own `inputs` set
    /// in the same call), a `link` to a parameter, an HLSL `expression`, a
    /// `data_interface` class with `properties`, an `object_asset`, or
    /// `reset` back to the module's default. Pass one of those per call.
    /// Whatever drove the input before is replaced — a `value` on an input a
    /// template handed to a dynamic input turns it back into a literal.
    /// Inputs nested under a dynamic input are addressed by path, e.g.
    /// "Drag/Minimum"; `stack` and `get_module` print every path.
    SetModuleInput {
        system: String,
        stage: String,
        emitter: Option<String>,
        module: String,
        /// Input path from `stack`: "SpawnRate", or "Drag/Minimum" for the
        /// Minimum input of the dynamic input driving Drag.
        input: String,
        /// Literal: number, bool, enum name, or [x,y] / [x,y,z] / [x,y,z,w],
        /// matching the input type (static switches included).
        value: Option<Value>,
        /// Dynamic input script — an asset path, or a name such as
        /// "Random Range Float"; `list_input_options` lists what fits.
        dynamic_input: Option<String>,
        /// With `dynamic_input`: literal values for the new dynamic input's own
        /// inputs, keyed by name, e.g. {"Minimum": 1, "Maximum": 4}.
        inputs: Option<Value>,
        /// Parameter to read instead of a value: "Emitter.SpawnRate",
        /// "User.Strength", "Particles.Age", "Engine.Owner.Velocity", ... A new
        /// name in a readable namespace creates that parameter (a "User."
        /// name becomes an exposed user parameter).
        link: Option<String>,
        /// Custom HLSL expression producing the input's type.
        expression: Option<String>,
        /// Data interface class for a data-interface input, e.g.
        /// "NiagaraDataInterfaceCurve"; `list_input_options` names the classes that fit.
        data_interface: Option<String>,
        /// Property values to apply to the input's data interface (with
        /// `data_interface`, or alone when it already has one).
        properties: Option<Value>,
        /// Asset path for an object input (a mesh, texture, ...).
        object_asset: Option<String>,
        /// Reset the input to the module's default.
        reset: Option<bool>,
        /// Toggle the input's edit condition (its inline checkbox), on its own
        /// or alongside a value.
        edit_condition_enabled: Option<bool>,
        /// Report hidden nested inputs in the result (default false).
        include_hidden: Option<bool>,
    },
    /// What one input can be given: the dynamic inputs whose output fits its
    /// type, the parameters it can link to (and any conversion script the
    /// editor would insert), the namespaces a new parameter may be created
    /// in, the data-interface classes that fit, and its enum options.
    ListInputOptions {
        system: String,
        stage: String,
        emitter: Option<String>,
        module: String,
        /// Input path from `stack`, e.g. "SpawnRate" or "Drag/Minimum".
        input: String,
        name_contains: Option<String>,
        /// Include dynamic inputs the library hides (default false).
        include_non_library: Option<bool>,
        max_results: Option<u32>,
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
        description = "Author Niagara systems rather than just drive them: create system and emitter assets, add/remove/rename/enable emitters, read the full stack (every emitter's emitter-spawn/update and particle-spawn/update module lists, plus renderers) with each module's inputs as the Niagara editor shows them — current value and mode (literal, linked parameter, dynamic input with its own nested inputs, data interface, expression), add and remove modules from any stack, and set any input at any depth: a literal, a dynamic input such as Random Range Float with its sub-inputs, a link to a parameter, an HLSL expression, a data interface with properties, or a reset to default. list_input_options says what fits a given input. Renderer options are ordinary properties on the reported paths, so set_property tunes them. Compile after authoring — that is what reports a broken stack. Needs the McpLinkNiagara plugin enabled."
    )]
    async fn niagara_author(
        &self,
        Parameters(op): Parameters<NiagaraAuthorOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/niagara/author", body)
            .await
            .map(Json)
    }
}

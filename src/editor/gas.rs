//! Gameplay Ability System interop (requires the McpLinkGAS plugin).
//!
//! Variants serialise straight into the plugin request body, so field names
//! here are the contract with `McpGasRoutes.cpp`.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum GasOp {
    /// Loaded GameplayAbility / GameplayEffect / AttributeSet classes.
    ListClasses {
        /// "abilities" (default), "effects", or "attribute_sets".
        kind: Option<String>,
        /// Substring filter on the class name.
        filter: Option<String>,
    },
    /// Everything about an actor's ability system: attribute sets with current
    /// and base values, granted abilities, active effects, owned tags.
    GetState {
        /// Actor path or editor label.
        actor: String,
        /// "auto" (default: PIE if running, else editor), "pie", or "editor".
        world: Option<String>,
    },
    /// Grant an ability class to the actor.
    GiveAbility {
        actor: String,
        world: Option<String>,
        /// Ability class: "GA_Jump", "/Game/Abilities/GA_Jump", or a native class name.
        ability: String,
        level: Option<i32>,
        input_id: Option<i32>,
    },
    /// Remove a granted ability by class or by spec handle (from get_state).
    RemoveAbility {
        actor: String,
        world: Option<String>,
        ability: Option<String>,
        handle: Option<String>,
    },
    /// Try to activate a granted ability by class, or every ability matching a tag.
    ActivateAbility {
        actor: String,
        world: Option<String>,
        ability: Option<String>,
        tag: Option<String>,
    },
    CancelAbilities {
        actor: String,
        world: Option<String>,
    },
    /// Apply a GameplayEffect class to the actor (from itself, or from `source`).
    /// Instant effects apply and return no active handle.
    ApplyEffect {
        actor: String,
        world: Option<String>,
        /// GameplayEffect class.
        effect: String,
        level: Option<f32>,
        /// Actor whose ability system is the instigator (default: the target itself).
        source: Option<String>,
    },
    /// Remove active effects by handle (from get_state) or by effect class.
    RemoveEffect {
        actor: String,
        world: Option<String>,
        handle: Option<String>,
        effect: Option<String>,
    },
    /// Set an attribute's base value, e.g. "Health" or "MyAttributeSet.Health".
    SetAttribute {
        actor: String,
        world: Option<String>,
        attribute: String,
        value: f64,
    },
    /// Add a loose gameplay tag (must be registered in the project's tag list).
    AddTag {
        actor: String,
        world: Option<String>,
        tag: String,
        count: Option<i32>,
    },
    RemoveTag {
        actor: String,
        world: Option<String>,
        tag: String,
        count: Option<i32>,
    },
}

#[tool_router(router = gas_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Gameplay Ability System: read an actor's attributes, granted abilities, active effects and tags; grant/activate/cancel abilities; apply/remove gameplay effects; set attribute base values; add/remove loose tags; list ability, effect and attribute-set classes. Works on any actor with an AbilitySystemComponent (usually during PIE). Needs the McpLinkGAS plugin enabled in the project."
    )]
    async fn gas_ops(&self, Parameters(op): Parameters<GasOp>) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/gas/ops", body).await.map(Json)
    }
}

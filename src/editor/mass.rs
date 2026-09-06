//! Mass Entity interop (requires the McpLinkMass plugin).
//!
//! Variants serialise straight into the plugin request body, so field names
//! here are the contract with `McpMassRoutes.cpp`.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

/// One entity type a spawner produces.
#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
pub struct MassEntityType {
    /// Mass Entity Config asset path.
    pub config: String,
    /// Share of the count (normalised across types; default 1).
    #[serde(skip_serializing_if = "Option::is_none")]
    pub proportion: Option<f64>,
}

/// One spawn data generator on a spawner.
#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
pub struct MassGenerator {
    /// A class from list_generators, e.g. "MassEntityEQSSpawnPointsGenerator".
    pub class: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub proportion: Option<f64>,
    /// Editable properties of the generator, by name.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub properties: Option<Value>,
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum MassOp {
    /// Every trait class (Mass Entity Config building blocks).
    ListTraits,
    /// Every spawn data generator class.
    ListGenerators,
    /// New Mass Entity Config asset, optionally inheriting a parent's traits.
    CreateConfig {
        /// e.g. /Game/Mass/EC_Crowd.
        path: String,
        parent: Option<String>,
    },
    /// Parent, own traits with their properties, and the combined trait set.
    ConfigInfo {
        config: String,
    },
    /// Add a trait (or return the existing one of that class) and set its
    /// properties.
    AddTrait {
        config: String,
        /// A class from list_traits, e.g. "MassAssortedFragmentsTrait".
        #[serde(rename = "trait")]
        trait_: String,
        properties: Option<Value>,
    },
    /// Set properties on a trait the config already has.
    SetTrait {
        config: String,
        #[serde(rename = "trait")]
        trait_: String,
        properties: Value,
    },
    RemoveTrait {
        config: String,
        #[serde(rename = "trait")]
        trait_: String,
    },
    /// Build the entity template against a world and report whether it
    /// validates.
    ValidateConfig {
        config: String,
        world: Option<String>,
    },
    SaveConfig {
        config: String,
    },
    /// Place a Mass Spawner actor with entity types and generators.
    CreateSpawner {
        world: Option<String>,
        name: Option<String>,
        location: Option<[f64; 3]>,
        rotation: Option<[f64; 3]>,
        count: Option<i32>,
        auto_spawn: Option<bool>,
        entity_types: Option<Vec<MassEntityType>>,
        generators: Option<Vec<MassGenerator>>,
    },
    /// Rewrite a spawner's count, entity types or generators (arrays given
    /// replace the existing ones).
    ConfigureSpawner {
        actor: String,
        world: Option<String>,
        count: Option<i32>,
        auto_spawn: Option<bool>,
        entity_types: Option<Vec<MassEntityType>>,
        generators: Option<Vec<MassGenerator>>,
    },
    SpawnerInfo {
        actor: String,
        world: Option<String>,
    },
    /// Run a spawner in a playing world (PIE); generators may spawn over a
    /// few frames.
    DoSpawning {
        actor: String,
        world: Option<String>,
    },
    DoDespawning {
        actor: String,
        world: Option<String>,
    },
    /// Entity count and every spawner in a world.
    EntityStats {
        world: Option<String>,
    },
}

#[tool_router(router = mass_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Mass Entity (needs McpLinkMass): create Mass Entity Config assets and add, edit or remove traits on them; place Mass Spawner actors with entity types and spawn data generators; spawn or despawn in PIE; and read entity counts."
    )]
    async fn mass_ops(&self, Parameters(op): Parameters<MassOp>) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/mass/ops", body).await.map(Json)
    }
}

//! Actor tools: discover, spawn, delete, transform, inspect components.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::{Value, json};

use crate::UnrealMcp;

/// Which world to act on: "auto" (PIE if playing, else editor), "pie", "editor".
#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct WorldSel {
    /// "auto" (default), "pie" (errors if PIE is not running), or "editor".
    pub world: Option<String>,
}

#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct ListActorsInput {
    pub world: Option<String>,
    /// Only actors of this class or its subclasses, e.g. "StaticMeshActor", "/Game/BP_Enemy".
    pub class_filter: Option<String>,
    /// Substring match on actor label or name.
    pub name_filter: Option<String>,
    /// Max actors returned (default 200, cap 1000).
    pub max_results: Option<u32>,
}

#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct SpawnActorInput {
    pub world: Option<String>,
    /// Class to spawn: "StaticMeshActor", "/Script/Engine.PointLight", or a Blueprint path "/Game/BP_Thing".
    pub class: String,
    /// World location [X, Y, Z] in cm. Defaults to origin.
    pub location: Option<[f64; 3]>,
    /// Rotation [Pitch, Yaw, Roll] in degrees.
    pub rotation: Option<[f64; 3]>,
    /// Scale [X, Y, Z]. Defaults to [1,1,1].
    pub scale: Option<[f64; 3]>,
    /// Editor label for the new actor.
    pub name: Option<String>,
}

#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct DeleteActorsInput {
    pub world: Option<String>,
    /// Object paths or editor labels of actors to delete.
    pub actors: Vec<String>,
}

#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct MoveActorInput {
    pub world: Option<String>,
    /// Object path or editor label.
    pub actor: String,
    /// New world location [X, Y, Z] in cm.
    pub location: Option<[f64; 3]>,
    /// New rotation [Pitch, Yaw, Roll] in degrees.
    pub rotation: Option<[f64; 3]>,
    /// New scale [X, Y, Z].
    pub scale: Option<[f64; 3]>,
}

#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct ActorComponentsInput {
    pub world: Option<String>,
    /// Object path or editor label.
    pub actor: String,
}

#[tool_router(router = actor_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "List actors in the current level with class, path, and transform. Use this to discover actor paths for the other tools."
    )]
    async fn get_level_actors(
        &self,
        Parameters(input): Parameters<ListActorsInput>,
    ) -> Result<Json<Value>, ErrorData> {
        self.call_plugin(
            "/api/actors/list",
            json!({
                "world": input.world,
                "class_filter": input.class_filter,
                "name_filter": input.name_filter,
                "max_results": input.max_results,
            }),
        )
        .await
        .map(Json)
    }

    #[tool(description = "Spawn an actor by class at a transform. Undo-able.")]
    async fn spawn_actor(
        &self,
        Parameters(input): Parameters<SpawnActorInput>,
    ) -> Result<Json<Value>, ErrorData> {
        self.call_plugin(
            "/api/actors/spawn",
            json!({
                "world": input.world,
                "class": input.class,
                "location": input.location,
                "rotation": input.rotation,
                "scale": input.scale,
                "name": input.name,
            }),
        )
        .await
        .map(Json)
    }

    #[tool(description = "Delete one or more actors by object path or editor label. Undo-able.")]
    async fn delete_actors(
        &self,
        Parameters(input): Parameters<DeleteActorsInput>,
    ) -> Result<Json<Value>, ErrorData> {
        self.call_plugin(
            "/api/actors/delete",
            json!({"world": input.world, "actors": input.actors}),
        )
        .await
        .map(Json)
    }

    #[tool(
        description = "Set an actor's location, rotation, and/or scale. Undo-able. Pass only the components you want to change."
    )]
    async fn move_actor(
        &self,
        Parameters(input): Parameters<MoveActorInput>,
    ) -> Result<Json<Value>, ErrorData> {
        self.call_plugin(
            "/api/actors/transform",
            json!({
                "world": input.world,
                "actor": input.actor,
                "location": input.location,
                "rotation": input.rotation,
                "scale": input.scale,
            }),
        )
        .await
        .map(Json)
    }

    #[tool(
        description = "Get an actor's component hierarchy: names, classes, paths, relative transforms, visibility, attachment."
    )]
    async fn get_actor_components(
        &self,
        Parameters(input): Parameters<ActorComponentsInput>,
    ) -> Result<Json<Value>, ErrorData> {
        self.call_plugin(
            "/api/actors/components",
            json!({"world": input.world, "actor": input.actor}),
        )
        .await
        .map(Json)
    }
}

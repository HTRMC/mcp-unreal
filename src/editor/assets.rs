//! Asset discovery tools backed by the editor's asset registry.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::{Value, json};

use crate::UnrealMcp;

#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct SearchAssetsInput {
    /// Package path to search under (default "/Game"). Use "/Engine" for engine content.
    pub path_prefix: Option<String>,
    /// Restrict to this asset class and its subclasses, e.g. "StaticMesh", "Material", "Blueprint".
    pub class: Option<String>,
    /// Substring match on asset name.
    pub name_contains: Option<String>,
    /// Max results (default 50, cap 500).
    pub max_results: Option<u32>,
}

#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct AssetInfoInput {
    /// Asset path, e.g. "/Game/Meshes/SM_Rock" (the .AssetName suffix is optional).
    pub path: String,
}

#[tool_router(router = asset_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Search project assets by path, class, and name. Returns name, class, package and object path."
    )]
    async fn search_assets(
        &self,
        Parameters(input): Parameters<SearchAssetsInput>,
    ) -> Result<Json<Value>, ErrorData> {
        self.call_plugin(
            "/api/assets/search",
            json!({
                "path_prefix": input.path_prefix,
                "class": input.class,
                "name_contains": input.name_contains,
                "max_results": input.max_results,
            }),
        )
        .await
        .map(Json)
    }

    #[tool(description = "Get an asset's metadata plus its dependencies and referencers.")]
    async fn get_asset_info(
        &self,
        Parameters(input): Parameters<AssetInfoInput>,
    ) -> Result<Json<Value>, ErrorData> {
        self.call_plugin("/api/assets/info", json!({"path": input.path}))
            .await
            .map(Json)
    }
}

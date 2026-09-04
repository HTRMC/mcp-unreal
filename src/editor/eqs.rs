//! `eqs_ops` — Environment Query System assets.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum EqsOp {
    /// Every generator and test class this project has, which is what an
    /// option and its tests are built from. Call this first — the names are
    /// what `add_option` and `add_test` take.
    ListClasses {
        /// Substring match on the class name, e.g. "Distance".
        name_filter: Option<String>,
    },
    /// Create an Environment Query asset with an empty graph and its root node.
    Create { path: String },
    /// The query's options, each with its generator, its ordered tests, and
    /// the object paths to edit them through `set_property`.
    Info { query: String },
    /// Add an option: one generator producing candidate items.
    AddOption {
        query: String,
        /// Generator class name or path, e.g. "EnvQueryGenerator_SimpleGrid".
        generator: String,
    },
    RemoveOption {
        query: String,
        /// Index from `info`.
        option: u32,
    },
    /// Add a test that scores or filters an option's items. Tests run in the
    /// order added; tune one with `set_property` on its `object_path`.
    AddTest {
        query: String,
        option: u32,
        /// Test class name or path, e.g. "EnvQueryTest_Distance".
        test: String,
    },
    RemoveTest {
        query: String,
        option: u32,
        test_index: u32,
    },
    /// Turn a test off without removing it. A disabled test is left out of the
    /// compiled query.
    SetTestEnabled {
        query: String,
        option: u32,
        test_index: u32,
        enabled: bool,
    },
    /// Rebuild the runtime options from the graph. Every editing operation
    /// does this already; call it after editing a generator or test through
    /// `set_property`.
    Compile { query: String },
    Save { query: String },
    /// Run the query and get its scored items back — where the AI would
    /// actually stand, best first. Needs no PIE: the editor world has an AI
    /// system of its own, so a query runs against the level as it is open.
    Run {
        query: String,
        /// The actor to run as. Every context resolves relative to it, starting
        /// with Querier, so the same query gives different answers per actor.
        querier: String,
        /// "all_matching" (default) keeps every item that passed, scored and
        /// sorted — what you want when inspecting a query. "single_best",
        /// "random_best_5pct" and "random_best_25pct" pick one item the way
        /// gameplay would: the pick is item 0, and the rest of the scored set
        /// comes back with it because an editor build keeps it for the EQS
        /// debugger.
        run_mode: Option<String>,
        /// "auto" (default), "pie" or "editor".
        world: Option<String>,
        /// Cap on returned items, 1..500. Default 50.
        max_items: Option<u32>,
    },
}

#[tool_router(router = eqs_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Author Environment Query System assets: an EQS query is a list of options, each one a generator that produces candidate items (a grid of points, actors of a class) plus ordered tests that score and filter them — how AI picks where to stand or what to shoot. Create a query, add options and tests, enable or disable tests, then compile and save. Generator and test settings are ordinary properties: info reports each one's object_path, and set_property edits it exactly as the details panel would, after which compile folds it into the runtime query. run executes the query as a chosen actor and returns the scored items, best first, which is how you check that a query actually picks the spots you meant — it works in the editor world without entering PIE."
    )]
    async fn eqs_ops(&self, Parameters(op): Parameters<EqsOp>) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/ai/eqs", body).await.map(Json)
    }
}

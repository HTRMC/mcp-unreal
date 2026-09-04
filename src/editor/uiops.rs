//! `ui_ops` — asset editors, and driving the editor's own Slate widgets.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum UiOp {
    /// Open an asset's editor. Some engine code only finishes an asset when
    /// its editor constructs itself — a fresh Material Layer gets its input
    /// and output nodes that way.
    OpenAsset { asset: String },
    CloseAsset { asset: String },
    CloseAll {},
    /// Which assets currently have an editor open. An open editor also lists
    /// its own transient preview objects.
    ListOpen {},

    /// Every widget a locator resolves to, with its text, visibility, whether
    /// it can be interacted with, and its screen rectangle. Call this first:
    /// the acting operations take an `index` into exactly this list.
    FindWidgets {
        /// A widget's driver id. The safer handle when a widget has one.
        id: Option<String>,
        /// A widget path instead: `<SType>` matches by widget type, `#Id` by
        /// driver id, a bare word by tag, `/` is a direct child and `//` any
        /// descendant — e.g. `<SWindow>//<SDockTab>`. It may not begin or end
        /// with `/`.
        path: Option<String>,
    },
    /// Click a widget. Slow paths like `<SWindow>//<SButton>` walk the whole
    /// arranged UI, so scope the locator when you can.
    Click {
        id: Option<String>,
        path: Option<String>,
        /// Which match to act on when the locator resolves to several, in
        /// find_widgets order. Default 0.
        index: Option<u32>,
        /// "left" (default), "right" or "middle".
        button: Option<String>,
    },
    DoubleClick {
        id: Option<String>,
        path: Option<String>,
        index: Option<u32>,
        button: Option<String>,
    },
    Hover {
        id: Option<String>,
        path: Option<String>,
        index: Option<u32>,
    },
    Focus {
        id: Option<String>,
        path: Option<String>,
        index: Option<u32>,
    },
    /// Type text into a widget, as keystrokes rather than a value assignment.
    Type {
        id: Option<String>,
        path: Option<String>,
        index: Option<u32>,
        text: String,
    },
    /// Send one key to a widget, e.g. Enter, Escape, Tab, LeftControl.
    PressKey {
        id: Option<String>,
        path: Option<String>,
        index: Option<u32>,
        key: String,
    },
    /// Scroll a scrollable widget by a delta (positive scrolls down).
    Scroll {
        id: Option<String>,
        path: Option<String>,
        index: Option<u32>,
        delta: Option<f64>,
    },
}

#[tool_router(router = ui_ops_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Drive the editor's own UI: find Slate widgets by id or type path and click, double-click, hover, focus, type into, key or scroll them — plus opening, closing and listing asset editors. Opening an editor is worth doing on its own, because some assets are only finished off when their editor constructs them. Widget work needs a windowed editor (a -nullrhi editor arranges no geometry) and goes through the engine's AutomationDriver on a worker thread. Call find_widgets first: a locator can match many widgets, and the acting operations take an index into that list."
    )]
    async fn ui_ops(&self, Parameters(op): Parameters<UiOp>) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/editor/ui_ops", body).await.map(Json)
    }
}

//! Play-In-Editor control, player/camera control, and input injection —
//! the tools that let an agent actually play the game in the editor.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::{Value, json};

use crate::UnrealMcp;

#[derive(serde::Deserialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum PieOp {
    /// Start a Play-In-Editor session.
    Start {
        /// Block until the engine reports PIE started (recommended) instead of returning immediately.
        wait: Option<bool>,
        /// Map to play, e.g. "/Game/Maps/TestMap". Defaults to the current level.
        map: Option<String>,
        /// Simulate In Editor (no player possession) instead of Play In Editor.
        simulate: Option<bool>,
        /// Spawn the player here [X, Y, Z] instead of at a Player Start.
        location: Option<[f64; 3]>,
        /// Facing for that spawn [Pitch, Yaw, Roll].
        rotation: Option<[f64; 3]>,
        /// Number of client windows, 1-8 (default 1). More than one makes it a
        /// networked session.
        players: Option<i32>,
        /// "standalone" (default), "listen_server" or "client".
        net_mode: Option<String>,
        /// Also launch a separate server even when the net mode does not need one.
        dedicated_server: Option<bool>,
        /// Run every client inside this editor process (default true). Only
        /// in-process clients are reachable by the other tools.
        one_process: Option<bool>,
    },
    /// Stop the running PIE session.
    Stop {
        /// Block until the engine reports PIE ended.
        wait: Option<bool>,
    },
    /// Whether PIE is running, and which map.
    Status {},
    /// Pause the running PIE session.
    Pause {},
    /// Resume a paused PIE session.
    Resume {},
}

#[derive(serde::Deserialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum PlayerOp {
    /// Player pawn state: path, class, location, rotation, velocity, control rotation. Requires PIE.
    GetInfo { player_index: Option<u32> },
    /// Move the player pawn directly (bypasses movement input). Requires PIE.
    Teleport {
        /// Destination [X, Y, Z] in cm.
        location: [f64; 3],
        /// Optional facing [Pitch, Yaw, Roll] in degrees.
        rotation: Option<[f64; 3]>,
        player_index: Option<u32>,
    },
    /// Set the view direction (control rotation). Requires PIE.
    SetControlRotation {
        /// [Pitch, Yaw, Roll] in degrees.
        rotation: [f64; 3],
        player_index: Option<u32>,
    },
    /// Possess a different pawn. Requires PIE.
    Possess {
        /// Pawn object path or editor label.
        actor: String,
        player_index: Option<u32>,
    },
    /// Editor viewport camera position (works without PIE).
    GetCamera {},
    /// Move the editor viewport camera (works without PIE).
    SetCamera {
        location: Option<[f64; 3]>,
        rotation: Option<[f64; 3]>,
    },
}

#[derive(serde::Deserialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum InputOp {
    /// Hold a key down until released or the timeout elapses.
    PressKey {
        /// UE key name: "W", "SpaceBar", "LeftShift", "Gamepad_FaceButton_Bottom".
        key: String,
        /// Auto-release after this many milliseconds (safety net).
        timeout_ms: Option<u64>,
        player_index: Option<u32>,
    },
    /// Release a held key.
    ReleaseKey {
        key: String,
        player_index: Option<u32>,
    },
    /// Press and release a key over a few frames (a single "keystroke").
    TapKey {
        key: String,
        /// Frames to hold before releasing (default 2).
        hold_frames: Option<u32>,
        player_index: Option<u32>,
    },
    /// Hold an analog axis at a value, re-injected every frame.
    SetAxis {
        /// Analog key name, e.g. "Gamepad_LeftX", "Gamepad_RightY".
        axis: String,
        /// -1.0 to 1.0. Zero clears the axis.
        value: f64,
        timeout_ms: Option<u64>,
        player_index: Option<u32>,
    },
    /// Mouse look. "once" delivers the total delta spread over `frames`;
    /// "continuous" injects dx/dy every frame until cleared with dx=dy=0.
    MouseMove {
        /// Horizontal delta in pixels (positive = right).
        dx: f64,
        /// Vertical delta in pixels (positive = down).
        dy: f64,
        /// "once" (default) or "continuous".
        mode: Option<String>,
        /// Frames to spread a "once" move over (default 1).
        frames: Option<u32>,
        player_index: Option<u32>,
    },
    /// Mouse button press, release, or click.
    MouseButton {
        /// "left" (default), "right", "middle", "thumb1", "thumb2".
        button: Option<String>,
        /// "press", "release", or "click" (default press).
        action: Option<String>,
        player_index: Option<u32>,
    },
    /// Scroll the mouse wheel.
    MouseWheel {
        /// Positive scrolls up.
        delta: f64,
        player_index: Option<u32>,
    },
    /// Inject an Enhanced Input action directly by asset path — use when a key
    /// binding is unknown, or to drive an action with a 2D/3D value.
    InjectAction {
        /// Input Action asset path, e.g. "/Game/Input/IA_Move".
        action: String,
        /// Scalar, or [X, Y] / [X, Y, Z] for axis actions. Defaults to 1.0.
        value: Option<Value>,
        /// "hold" (default, injected every frame until released) or "trigger" (one shot).
        mode: Option<String>,
        timeout_ms: Option<u64>,
        player_index: Option<u32>,
    },
    /// Stop a held Enhanced Input action injection.
    ReleaseAction {
        action: String,
        player_index: Option<u32>,
    },
    /// Release every held key, axis, mouse delta and action.
    ReleaseAll { player_index: Option<u32> },
    /// What is currently held, including what the engine reports as actually down.
    GetState { player_index: Option<u32> },
}

#[tool_router(router = play_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Control Play-In-Editor sessions: start, stop, status, pause, resume. Pass wait=true on start/stop to block until the transition completes instead of polling."
    )]
    async fn pie_control(
        &self,
        Parameters(op): Parameters<PieOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body = match op {
            PieOp::Start {
                wait,
                map,
                simulate,
                location,
                rotation,
                players,
                net_mode,
                dedicated_server,
                one_process,
            } => json!({
                "operation": "start", "wait": wait, "map": map, "simulate": simulate,
                "location": location, "rotation": rotation, "players": players,
                "net_mode": net_mode, "dedicated_server": dedicated_server,
                "one_process": one_process,
            }),
            PieOp::Stop { wait } => json!({"operation": "stop", "wait": wait}),
            PieOp::Status {} => json!({"operation": "status"}),
            PieOp::Pause {} => json!({"operation": "pause"}),
            PieOp::Resume {} => json!({"operation": "resume"}),
        };
        self.call_plugin("/api/editor/pie_control", body)
            .await
            .map(Json)
    }

    #[tool(
        description = "Inspect and control the player pawn during PIE (get_info, teleport, set_control_rotation, possess), or move the editor viewport camera (get_camera, set_camera — no PIE needed)."
    )]
    async fn player_control(
        &self,
        Parameters(op): Parameters<PlayerOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body = match op {
            PlayerOp::GetInfo { player_index } => {
                json!({"operation": "get_info", "player_index": player_index})
            }
            PlayerOp::Teleport {
                location,
                rotation,
                player_index,
            } => json!({
                "operation": "teleport",
                "location": location,
                "rotation": rotation,
                "player_index": player_index,
            }),
            PlayerOp::SetControlRotation {
                rotation,
                player_index,
            } => json!({
                "operation": "set_control_rotation",
                "rotation": rotation,
                "player_index": player_index,
            }),
            PlayerOp::Possess {
                actor,
                player_index,
            } => json!({"operation": "possess", "actor": actor, "player_index": player_index}),
            PlayerOp::GetCamera {} => json!({"operation": "get_camera"}),
            PlayerOp::SetCamera { location, rotation } => {
                json!({"operation": "set_camera", "location": location, "rotation": rotation})
            }
        };
        self.call_plugin("/api/editor/player_control", body)
            .await
            .map(Json)
    }

    #[tool(
        description = "Play the game: inject keyboard, mouse and gamepad input into a running PIE session. Held keys and axes are re-injected every frame until released, so the character keeps moving. Drives both legacy and Enhanced Input, and works even when the PIE window is not focused. Requires PIE (start it with pie_control). Injected input auto-releases after 30s of inactivity."
    )]
    async fn input_inject(
        &self,
        Parameters(op): Parameters<InputOp>,
    ) -> Result<Json<Value>, ErrorData> {
        self.call_plugin("/api/input/inject", input_body(op))
            .await
            .map(Json)
    }
}

/// Request body for `/api/input/inject`. Split out so the contract fixtures can
/// assert the exact wire shape without a live editor.
pub fn input_body(op: InputOp) -> Value {
    #[allow(clippy::let_and_return)]
    let body = match op {
        InputOp::PressKey {
            key,
            timeout_ms,
            player_index,
        } => json!({
            "operation": "press_key", "key": key,
            "timeout_ms": timeout_ms, "player_index": player_index,
        }),
        InputOp::ReleaseKey { key, player_index } => {
            json!({"operation": "release_key", "key": key, "player_index": player_index})
        }
        InputOp::TapKey {
            key,
            hold_frames,
            player_index,
        } => json!({
            "operation": "tap_key", "key": key,
            "hold_frames": hold_frames, "player_index": player_index,
        }),
        InputOp::SetAxis {
            axis,
            value,
            timeout_ms,
            player_index,
        } => json!({
            "operation": "set_axis", "axis": axis, "value": value,
            "timeout_ms": timeout_ms, "player_index": player_index,
        }),
        InputOp::MouseMove {
            dx,
            dy,
            mode,
            frames,
            player_index,
        } => json!({
            "operation": "mouse_move", "dx": dx, "dy": dy,
            "mode": mode, "frames": frames, "player_index": player_index,
        }),
        InputOp::MouseButton {
            button,
            action,
            player_index,
        } => json!({
            "operation": "mouse_button", "button": button,
            "action": action, "player_index": player_index,
        }),
        InputOp::MouseWheel {
            delta,
            player_index,
        } => json!({"operation": "mouse_wheel", "delta": delta, "player_index": player_index}),
        InputOp::InjectAction {
            action,
            value,
            mode,
            timeout_ms,
            player_index,
        } => json!({
            "operation": "inject_action", "action": action, "value": value,
            "mode": mode, "timeout_ms": timeout_ms, "player_index": player_index,
        }),
        InputOp::ReleaseAction {
            action,
            player_index,
        } => json!({"operation": "release_action", "action": action, "player_index": player_index}),
        InputOp::ReleaseAll { player_index } => {
            json!({"operation": "release_all", "player_index": player_index})
        }
        InputOp::GetState { player_index } => {
            json!({"operation": "get_state", "player_index": player_index})
        }
    };
    body
}

//! Sequencer authoring for Level Sequences and UMG widget animations.
//!
//! Variants serialise straight into the plugin request body, so field names
//! here are the contract with `McpSequenceRoutes.cpp`.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

/// A pin on a sequence event's custom event node.
#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
pub struct EventParameter {
    pub name: String,
    /// bool, int, int64, float, string, name, text, vector, rotator,
    /// transform or "object:<Class>".
    #[serde(rename = "type")]
    pub param_type: String,
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum SequenceOp {
    /// Track classes usable with add_track (short names such as "Transform",
    /// "SkeletalAnimation", "CameraCut", "Audio", "Fade").
    ListTrackClasses {
        /// Substring filter, e.g. "Camera".
        filter: Option<String>,
    },
    /// New Level Sequence asset. Playback defaults to 5 seconds at 30 fps.
    Create {
        /// Asset path, e.g. /Game/Cine/LS_Shot.
        path: String,
        /// Timeline frames per second (default 30).
        display_rate: Option<f64>,
        /// Internal resolution in ticks per second (default 24000 — leave alone
        /// unless you know you need it).
        tick_resolution: Option<f64>,
        start_frame: Option<f64>,
        start_seconds: Option<f64>,
        end_frame: Option<f64>,
        end_seconds: Option<f64>,
        /// Length from the start, in seconds; overrides end_frame/end_seconds.
        duration_seconds: Option<f64>,
    },
    /// Rates, playback range, marked frames, every binding with its tracks and
    /// sections, and each section's channels.
    Inspect {
        sequence: String,
        /// Include each channel's keyframes (times and values).
        include_keys: Option<bool>,
    },
    SetPlaybackRange {
        sequence: String,
        start_frame: Option<f64>,
        start_seconds: Option<f64>,
        end_frame: Option<f64>,
        end_seconds: Option<f64>,
    },
    SetDisplayRate {
        sequence: String,
        /// Frames per second, e.g. 24 or 29.97.
        display_rate: f64,
    },
    AddMarkedFrame {
        sequence: String,
        label: Option<String>,
        frame: Option<f64>,
        seconds: Option<f64>,
    },
    /// Bind a level actor (or one of its components) so tracks can animate it.
    AddBinding {
        sequence: String,
        /// Actor object path or editor label.
        actor: String,
        /// Component name to bind instead of the actor itself.
        component: Option<String>,
        /// Spawn a copy when the sequence plays instead of possessing the level
        /// actor.
        spawnable: Option<bool>,
        /// Which world to look the actor up in: "auto", "pie", "editor".
        world: Option<String>,
    },
    RemoveBinding {
        sequence: String,
        /// Binding GUID or display name.
        binding: String,
    },
    SetBindingName {
        sequence: String,
        binding: String,
        name: String,
    },
    /// Add a track. Property tracks are easier to add with add_property_track,
    /// which picks the class and channel count from the property itself.
    AddTrack {
        sequence: String,
        /// Track class: "Transform", "SkeletalAnimation", "CameraCut", "Audio",
        /// "Fade", "Slomo", or a full class path.
        track: String,
        /// Binding GUID or name; omit for a root (sequence-wide) track.
        binding: Option<String>,
        /// Property path for property tracks, e.g. "RelativeLocation".
        property: Option<String>,
        /// Display name shown in Sequencer.
        name: Option<String>,
        /// Also add a section spanning the range (default true).
        with_section: Option<bool>,
        start_frame: Option<f64>,
        start_seconds: Option<f64>,
        end_frame: Option<f64>,
        end_seconds: Option<f64>,
    },
    /// Add the right property track for a property on the bound object, with a
    /// section spanning the playback range — e.g. "RelativeLocation" on a
    /// component binding, "Intensity" on a light.
    AddPropertyTrack {
        sequence: String,
        binding: String,
        /// Property path on the bound class; dotted paths reach into structs
        /// ("RelativeLocation.X").
        property: String,
        start_frame: Option<f64>,
        start_seconds: Option<f64>,
        end_frame: Option<f64>,
        end_seconds: Option<f64>,
    },
    RemoveTrack {
        sequence: String,
        /// Track path or name from inspect.
        track: String,
    },
    AddSection {
        sequence: String,
        track: String,
        start_frame: Option<f64>,
        start_seconds: Option<f64>,
        end_frame: Option<f64>,
        end_seconds: Option<f64>,
        /// Row within the track, for overlapping sections.
        row_index: Option<i32>,
    },
    SetSectionRange {
        sequence: String,
        section: String,
        start_frame: Option<f64>,
        start_seconds: Option<f64>,
        end_frame: Option<f64>,
        end_seconds: Option<f64>,
    },
    RemoveSection {
        sequence: String,
        /// Section path from inspect (names repeat across tracks).
        section: String,
    },
    /// Cut to a bound camera for a stretch of the timeline.
    AddCameraCut {
        sequence: String,
        /// Binding GUID or name of the camera actor.
        camera_binding: String,
        start_frame: Option<f64>,
        start_seconds: Option<f64>,
        end_frame: Option<f64>,
        end_seconds: Option<f64>,
    },
    /// Key a channel. Channel names come from inspect ("Location.X"); a
    /// section with a single channel calls it "Value". Times are display-rate
    /// frames or seconds.
    AddKey {
        sequence: String,
        section: String,
        channel: String,
        frame: Option<f64>,
        seconds: Option<f64>,
        /// The keyed value: number, bool, string or enumerator name.
        value: Option<Value>,
        /// "auto" (default), "linear", "constant" or "cubic" — float and double
        /// channels only.
        interpolation: Option<String>,
        /// Several keys at once: [{"frame": 0, "value": 0}, ...]. Each entry may
        /// carry its own interpolation.
        keys: Option<Vec<Value>>,
    },
    /// Delete the key at a time, or every key on the channel when no time is given.
    RemoveKeys {
        sequence: String,
        section: String,
        channel: String,
        frame: Option<f64>,
        seconds: Option<f64>,
    },
    /// The value a channel holds where it has no keys.
    SetChannelDefault {
        sequence: String,
        section: String,
        channel: String,
        value: Option<Value>,
    },
    /// Nest another sequence: a section on the Subsequences track (root, or
    /// on a binding) that plays it, for its own length unless an end is given.
    AddSubsequence {
        sequence: String,
        /// The Level Sequence asset to nest.
        subsequence: String,
        /// Binding GUID or name; omit for the root Subsequences track.
        binding: Option<String>,
        start_frame: Option<f64>,
        start_seconds: Option<f64>,
        end_frame: Option<f64>,
        end_seconds: Option<f64>,
        /// Row within the track (default: a new row).
        row_index: Option<i32>,
    },
    /// Shake a camera: a Camera Shake track on the camera's binding with a
    /// section playing a CameraShakeBase class over the range.
    AddCameraShake {
        sequence: String,
        /// Binding GUID or name of the camera actor.
        binding: String,
        /// A CameraShakeBase subclass — a Blueprint camera shake asset path
        /// or a native class.
        shake_class: String,
        start_frame: Option<f64>,
        start_seconds: Option<f64>,
        end_frame: Option<f64>,
        end_seconds: Option<f64>,
        /// Intensity multiplier (default 1).
        play_scale: Option<f64>,
        /// "camera_local" (default), "world" or "user_defined".
        play_space: Option<String>,
        /// [Pitch, Yaw, Roll] for a user_defined play space.
        play_space_rotation: Option<[f64; 3]>,
        row_index: Option<i32>,
    },
    /// Fire an event into the sequence's director Blueprint: a custom event
    /// node is created there (with the bound object as a pin on a binding's
    /// track) and the key calls it. `parameters` add pins the event carries
    /// and `payload` sets what the sequence passes into them; wire the logic
    /// under the node with blueprint_modify on the reported
    /// `director_blueprint`.
    AddEvent {
        sequence: String,
        /// Binding GUID or name; omit for a sequence-wide event.
        binding: Option<String>,
        /// Name of the custom event (default SequenceEvent_N).
        name: Option<String>,
        /// When a one-shot trigger fires.
        frame: Option<f64>,
        seconds: Option<f64>,
        /// Fire every evaluation while a section spanning start..end is
        /// active, instead of once.
        repeat: Option<bool>,
        start_frame: Option<f64>,
        start_seconds: Option<f64>,
        end_frame: Option<f64>,
        end_seconds: Option<f64>,
        /// Pins to add to the event: [{"name": "Strength", "type": "float"}],
        /// types bool, int, int64, float, string, name, text, vector,
        /// rotator, transform or "object:<Class>".
        parameters: Option<Vec<EventParameter>>,
        /// Values passed for those pins, keyed by name: {"Strength": 2.5}.
        payload: Option<Value>,
    },
    /// Spawn a LevelSequenceActor so the sequence plays in the level.
    AddToLevel {
        sequence: String,
        world: Option<String>,
        /// Actor name and label.
        name: Option<String>,
        auto_play: Option<bool>,
        /// 0 plays once, -1 loops forever.
        loop_count: Option<i32>,
    },
    Save {
        sequence: String,
    },
    /// Every animation on a Widget Blueprint.
    ListWidgetAnimations {
        /// Widget Blueprint asset path, e.g. /Game/UI/WBP_Menu.
        widget_blueprint: String,
    },
    /// New UMG animation. Address it afterwards as
    /// "<widget_blueprint>:<name>" wherever `sequence` is taken.
    CreateWidgetAnimation {
        widget_blueprint: String,
        /// Animation name; also becomes a Blueprint variable.
        name: String,
        /// Frames per second (default 60).
        display_rate: Option<f64>,
        /// Length in seconds (default 5).
        duration_seconds: Option<f64>,
    },
    RemoveWidgetAnimation {
        widget_blueprint: String,
        animation: String,
    },
    /// Bind a widget of the animation's own Widget Blueprint so tracks can
    /// animate it. "Self" binds the user widget itself.
    AddWidgetBinding {
        /// The animation, as "<widget_blueprint>:<name>".
        sequence: String,
        /// Widget name from widget_blueprint_query inspect, or "Self".
        widget: String,
    },
}

#[tool_router(router = sequence_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Author Sequencer content: Level Sequence cinematics and UMG widget animations, which share the same MovieScene machinery. Create a sequence or a widget animation, bind level actors and their components (or widgets, for an animation), add transform/animation/camera-cut/audio and property tracks, add sections, keyframe any channel, nest subsequences, add camera shakes, and fire events into the director Blueprint with typed payloads. A widget animation is addressed as \"/Game/UI/WBP_Menu:Anim_FadeIn\" wherever `sequence` is taken. Frame numbers are in the sequence's display rate, and seconds work everywhere a frame does. Section and track options that are plain UPROPERTYs (a skeletal animation section's Params.Animation, an audio section's Sound, easing) are set with set_property on the path each operation reports. add_to_level spawns a LevelSequenceActor so PIE plays a Level Sequence; play a widget animation with call_function PlayAnimation on the widget."
    )]
    async fn sequence_ops(
        &self,
        Parameters(op): Parameters<SequenceOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/sequences/ops", body).await.map(Json)
    }
}

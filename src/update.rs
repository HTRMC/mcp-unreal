//! Auto-update: moves the server binary and the McpLink plugin to the same
//! release, or moves neither.
//!
//! The two halves are one contract — the Rust tools and the plugin routes are
//! checked against each other in `tests/fixtures/contract/` — so a server that
//! runs ahead of its plugin fails as a 404 on a route it is sure exists.
//! Updating one half on its own would manufacture exactly the breakage that
//! updating by hand avoids, since whoever downloads by hand takes both from
//! the same release page. So when a plugin install is present, both move
//! together in one window or neither moves.
//!
//! That window is "the editor is closed". Windows keeps a loaded DLL locked,
//! so the plugin can only be replaced while the editor is not running, and the
//! binary is held back to the same moment even though it could swap itself at
//! any time. Holding it back is what keeps the pair honest.
//!
//! Nothing here affects the running process. The MCP client owns this
//! process's stdio and exiting to re-exec would look like a crash, so an
//! applied update lands on the next launch.

use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};

use crate::config::Config;
use crate::editor::client::EditorClient;

/// How long to wait before asking GitHub again. Applying an already-staged
/// update needs no network, so this only paces the check itself.
const CHECK_INTERVAL: Duration = Duration::from_secs(24 * 60 * 60);

/// A stale lock is a crashed run, not a live one.
const LOCK_STALE_AFTER: Duration = Duration::from_secs(10 * 60);

/// Release assets are a few MB. This is a sanity bound, not a real limit.
const MAX_ASSET_BYTES: u64 = 256 * 1024 * 1024;

const CONNECT_TIMEOUT: Duration = Duration::from_secs(10);
const DOWNLOAD_TIMEOUT: Duration = Duration::from_secs(300);

/// What the updater is allowed to do, from `MCP_UNREAL_AUTO_UPDATE`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Channel {
    /// Download, and swap both halves the next time the editor is closed.
    Apply,
    /// Look, report through `status`, change nothing on disk.
    Check,
    /// No network, no writes.
    Off,
}

impl Channel {
    pub fn from_env() -> Self {
        Self::parse(&std::env::var("MCP_UNREAL_AUTO_UPDATE").unwrap_or_default())
    }

    /// Anything unrecognised is `apply`: the setting exists to turn the
    /// updater down, so a typo must not silently turn it off.
    fn parse(value: &str) -> Self {
        match value.trim().to_ascii_lowercase().as_str() {
            "off" | "false" | "0" | "no" | "never" => Self::Off,
            "check" | "notify" => Self::Check,
            _ => Self::Apply,
        }
    }

    pub fn as_str(self) -> &'static str {
        match self {
            Self::Apply => "apply",
            Self::Check => "check",
            Self::Off => "off",
        }
    }
}

/// What the last background run concluded. `status` reads this file rather
/// than going to the network itself, so a status call stays instant.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct UpdateState {
    /// Unix seconds of the last completed check, absent if none has finished.
    pub checked_at: Option<u64>,
    /// Newest published release, as a bare version.
    pub latest: Option<String>,
    /// `up_to_date`, `update_available`, `staged`, `applied`, `blocked`,
    /// `unavailable` or `checking`.
    pub state: String,
    /// Why, in a sentence — the reason a blocked update is blocked, or what an
    /// applied one changed.
    pub detail: Option<String>,
}

impl UpdateState {
    fn new(state: &str, detail: Option<String>) -> Self {
        Self {
            checked_at: now_secs(),
            latest: None,
            state: state.to_string(),
            detail,
        }
    }
}

/// One release asset, as the GitHub API reports it.
#[derive(Debug, Clone, Deserialize)]
struct Asset {
    name: String,
    browser_download_url: String,
    #[serde(default)]
    size: u64,
    /// `sha256:<hex>`. GitHub records this for every asset, which is why the
    /// release workflow ships no `.sha256` sidecars.
    #[serde(default)]
    digest: Option<String>,
}

#[derive(Debug, Clone, Deserialize)]
struct Release {
    tag_name: String,
    #[serde(default)]
    assets: Vec<Asset>,
}

/// Everything the updater needs, resolved once so the background task never
/// reaches back into process state.
pub struct UpdateEnv {
    pub channel: Channel,
    pub api_base: String,
    pub token: Option<String>,
    pub dir: PathBuf,
    /// `owner/repo` the releases come from, taken from the crate's own
    /// `repository` field so a fork updates from the fork.
    pub repo: Option<(String, String)>,
    pub current: String,
    /// Engine line the installed plugin must match, e.g. `5.8`.
    pub engine_line: Option<String>,
    pub project_root: Option<PathBuf>,
}

impl UpdateEnv {
    pub fn from_config(cfg: &Config) -> Self {
        let dir = std::env::var_os("MCP_UNREAL_UPDATE_DIR")
            .map(PathBuf::from)
            .unwrap_or_else(|| {
                dirs::data_local_dir()
                    .unwrap_or_else(|| PathBuf::from("."))
                    .join("mcp-unreal")
                    .join("update")
            });
        Self {
            channel: Channel::from_env(),
            api_base: std::env::var("MCP_UNREAL_UPDATE_API")
                .unwrap_or_else(|_| "https://api.github.com".to_string()),
            // Not read from GH_TOKEN on purpose: attaching a token the user
            // set for something else to requests they did not ask for is a
            // surprise. This one exists for private repos and forks.
            token: std::env::var("MCP_UNREAL_UPDATE_TOKEN")
                .ok()
                .filter(|t| !t.trim().is_empty()),
            dir,
            repo: parse_repo(env!("CARGO_PKG_REPOSITORY")),
            current: env!("CARGO_PKG_VERSION").to_string(),
            engine_line: crate::config::engine_version_at(&cfg.engine_root)
                .map(|(major, minor, _)| format!("{major}.{minor}")),
            project_root: cfg.project.as_ref().map(|p| p.root.clone()),
        }
    }

    fn state_path(&self) -> PathBuf {
        self.dir.join("state.json")
    }

    fn staged_dir(&self, version: &str) -> PathBuf {
        self.dir.join("staged").join(version)
    }
}

/// Read what the last run concluded. Never fails: a missing or unreadable
/// state file just means nothing is known yet.
pub fn read_state(env: &UpdateEnv) -> Option<UpdateState> {
    let text = std::fs::read_to_string(env.state_path()).ok()?;
    serde_json::from_str(&text).ok()
}

fn write_state(env: &UpdateEnv, state: &UpdateState) {
    if let Err(e) = std::fs::create_dir_all(&env.dir) {
        tracing::debug!("update: cannot create {}: {e}", env.dir.display());
        return;
    }
    match serde_json::to_string_pretty(state) {
        Ok(text) => {
            if let Err(e) = std::fs::write(env.state_path(), text) {
                tracing::debug!("update: cannot write state: {e}");
            }
        }
        Err(e) => tracing::debug!("update: cannot serialise state: {e}"),
    }
}

/// Start the update run in the background. Returns immediately; the MCP
/// service must never wait on this.
pub fn spawn(cfg: &Config) {
    let env = UpdateEnv::from_config(cfg);
    let editor = EditorClient::new(cfg.plugin_port);
    if env.channel == Channel::Off {
        tracing::debug!("update: disabled by MCP_UNREAL_AUTO_UPDATE=off");
        return;
    }
    tokio::spawn(async move {
        if let Err(e) = run(&env, &editor).await {
            // An update failing is never worth failing a session over.
            tracing::info!("update: {e}");
            write_state(&env, &UpdateState::new("unavailable", Some(e.to_string())));
        }
    });
}

#[derive(Debug, thiserror::Error)]
pub enum UpdateError {
    #[error("{0}")]
    Message(String),
    #[error("io: {0}")]
    Io(#[from] std::io::Error),
}

fn err(msg: impl Into<String>) -> UpdateError {
    UpdateError::Message(msg.into())
}

type Result<T> = std::result::Result<T, UpdateError>;

async fn run(env: &UpdateEnv, editor: &EditorClient) -> Result<()> {
    let _lock = Lock::acquire(&env.dir)?;

    let current = parse_version(&env.current)
        .ok_or_else(|| err(format!("own version {} is not x.y.z", env.current)))?;

    // A staged update needs no network to finish, so try that first: the
    // common case is "downloaded yesterday, the editor was open, now it is
    // closed".
    let previous = read_state(env);
    if let Some(staged) = previous
        .as_ref()
        .filter(|s| s.state == "staged")
        .and_then(|s| s.latest.clone())
        && parse_version(&staged).is_some_and(|v| v > current)
        && env.staged_dir(&staged).is_dir()
    {
        return finish(env, editor, &staged).await;
    }

    if let Some(checked) = previous.as_ref().and_then(|s| s.checked_at)
        && now_secs().is_some_and(|now| now.saturating_sub(checked) < CHECK_INTERVAL.as_secs())
    {
        tracing::debug!("update: checked recently, skipping");
        return Ok(());
    }

    let (owner, repo) = env
        .repo
        .clone()
        .ok_or_else(|| err("no repository recorded in the crate metadata"))?;

    let http = reqwest::Client::builder()
        .connect_timeout(CONNECT_TIMEOUT)
        .timeout(DOWNLOAD_TIMEOUT)
        .user_agent(format!("mcp-unreal/{}", env.current))
        .build()
        .map_err(|e| err(e.to_string()))?;

    let release = fetch_latest(&http, env, &owner, &repo).await?;
    let latest_raw = release.tag_name.trim_start_matches('v').to_string();
    let latest = parse_version(&latest_raw)
        .ok_or_else(|| err(format!("release tag {} is not vX.Y.Z", release.tag_name)))?;

    if latest <= current {
        let mut state = UpdateState::new("up_to_date", None);
        state.latest = Some(latest_raw);
        write_state(env, &state);
        tracing::debug!("update: {} is current", env.current);
        return Ok(());
    }

    tracing::info!(
        "update: {} is available (running {})",
        latest_raw,
        env.current
    );

    if env.channel == Channel::Check {
        let mut state = UpdateState::new(
            "update_available",
            Some(format!(
                "{latest_raw} is published; this server is {}. Set MCP_UNREAL_AUTO_UPDATE=apply to install it automatically, or download it from the releases page",
                env.current
            )),
        );
        state.latest = Some(latest_raw);
        write_state(env, &state);
        return Ok(());
    }

    // Decide the plugin half before downloading anything: if the pair cannot
    // move together there is no point fetching either half.
    let plugin = plugin_plan(env);
    if let PluginPlan::Blocked(reason) = &plugin {
        let mut state = UpdateState::new(
            "blocked",
            Some(format!(
                "{latest_raw} is published, but the McpLink plugin cannot be updated automatically ({reason}), \
                 and the server is not updated on its own because the two must match. Update both by hand from the releases page"
            )),
        );
        state.latest = Some(latest_raw);
        write_state(env, &state);
        tracing::info!("update: not applying — {reason}");
        return Ok(());
    }

    download(&http, env, &release, &latest_raw, &plugin).await?;
    finish(env, editor, &latest_raw).await
}

async fn fetch_latest(
    http: &reqwest::Client,
    env: &UpdateEnv,
    owner: &str,
    repo: &str,
) -> Result<Release> {
    let url = format!(
        "{}/repos/{owner}/{repo}/releases/latest",
        env.api_base.trim_end_matches('/')
    );
    let mut req = http
        .get(&url)
        .header("Accept", "application/vnd.github+json")
        .header("X-GitHub-Api-Version", "2022-11-28");
    if let Some(token) = &env.token {
        req = req.bearer_auth(token);
    }
    let resp = req.send().await.map_err(|e| err(e.to_string()))?;
    let status = resp.status();
    if status == reqwest::StatusCode::NOT_FOUND {
        return Err(err(
            "no published release is visible — a private repository needs MCP_UNREAL_UPDATE_TOKEN",
        ));
    }
    if !status.is_success() {
        return Err(err(format!("GitHub answered HTTP {status}")));
    }
    resp.json::<Release>()
        .await
        .map_err(|e| err(format!("unreadable release JSON: {e}")))
}

// --- what can be updated ----------------------------------------------------

/// The plugin half of a release: which installed plugin folders it would
/// replace, or why it cannot.
#[derive(Debug, Clone, PartialEq, Eq)]
enum PluginPlan {
    /// No plugin install to keep in step with, so the server can move alone.
    NotInstalled,
    /// These plugin folder names are installed and replaceable.
    Update(Vec<String>),
    Blocked(String),
}

fn plugin_plan(env: &UpdateEnv) -> PluginPlan {
    let Some(root) = &env.project_root else {
        // Nothing is configured to have a plugin in it. There is no installed
        // half to skew against, so this is not a blocker.
        return PluginPlan::NotInstalled;
    };
    let plugins = root.join("Plugins");
    if !plugins.is_dir() {
        return PluginPlan::NotInstalled;
    }
    match installed_plugins(&plugins) {
        Ok(found) if found.is_empty() => PluginPlan::NotInstalled,
        Ok(found) => {
            if !cfg!(windows) {
                return PluginPlan::Blocked(
                    "the release ships a prebuilt plugin for Windows only; on this platform it is built from source"
                        .to_string(),
                );
            }
            if env.engine_line.is_none() {
                return PluginPlan::Blocked(
                    "the engine version could not be read, so the matching plugin build cannot be chosen"
                        .to_string(),
                );
            }
            PluginPlan::Update(found)
        }
        Err(reason) => PluginPlan::Blocked(reason),
    }
}

/// The McpLink plugin folders under a project's `Plugins/`, or the reason one
/// of them must not be touched.
///
/// Two refusals matter. A junction or symlink is how a development checkout is
/// wired (`tools/setup-dev.ps1` links these straight at the source tree), so
/// replacing one would overwrite the repository the developer is working in. A
/// folder with no `Binaries/` was compiled by hand rather than installed from
/// a release, and its owner did not ask for a prebuilt one.
fn installed_plugins(plugins_dir: &Path) -> std::result::Result<Vec<String>, String> {
    let mut found = Vec::new();
    let entries = std::fs::read_dir(plugins_dir)
        .map_err(|e| format!("cannot read {}: {e}", plugins_dir.display()))?;
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().to_string();
        if !name.starts_with("McpLink") {
            continue;
        }
        let path = entry.path();
        let meta = std::fs::symlink_metadata(&path)
            .map_err(|e| format!("cannot inspect {}: {e}", path.display()))?;
        if meta.file_type().is_symlink() {
            return Err(format!(
                "{name} is a junction into a source checkout, which an update must not overwrite"
            ));
        }
        if !meta.is_dir() {
            continue;
        }
        if !path.join("Binaries").is_dir() {
            return Err(format!(
                "{name} has no Binaries/, so it was built from source rather than installed from a release"
            ));
        }
        found.push(name);
    }
    found.sort();
    Ok(found)
}

// --- download and stage -----------------------------------------------------

async fn download(
    http: &reqwest::Client,
    env: &UpdateEnv,
    release: &Release,
    version: &str,
    plugin: &PluginPlan,
) -> Result<()> {
    let server_name = server_asset_name(version, std::env::consts::OS, std::env::consts::ARCH)
        .ok_or_else(|| {
            err(format!(
                "no release build for {}-{}",
                std::env::consts::OS,
                std::env::consts::ARCH
            ))
        })?;

    let mut wanted = vec![server_name];
    if let PluginPlan::Update(_) = plugin {
        let line = env.engine_line.as_deref().unwrap_or_default();
        wanted.push(plugin_asset_name(version, line));
    }

    let staged = env.staged_dir(version);
    // A half-finished stage from a previous run must not be mistaken for a
    // complete one.
    let _ = std::fs::remove_dir_all(&staged);
    std::fs::create_dir_all(&staged)?;

    for name in &wanted {
        let asset = release
            .assets
            .iter()
            .find(|a| &a.name == name)
            .ok_or_else(|| err(format!("release {version} has no asset named {name}")))?;
        if asset.size > MAX_ASSET_BYTES {
            return Err(err(format!(
                "{name} is implausibly large ({} bytes)",
                asset.size
            )));
        }
        let bytes = fetch_asset(http, env, asset).await?;
        verify_digest(asset.digest.as_deref(), &bytes).map_err(|e| err(format!("{name}: {e}")))?;
        std::fs::write(staged.join(name), &bytes)?;
        tracing::debug!("update: staged {name} ({} bytes)", bytes.len());
    }

    let mut state = UpdateState::new(
        "staged",
        Some(format!(
            "{version} is downloaded and will be installed the next time this server starts with the editor closed"
        )),
    );
    state.latest = Some(version.to_string());
    write_state(env, &state);
    Ok(())
}

async fn fetch_asset(http: &reqwest::Client, env: &UpdateEnv, asset: &Asset) -> Result<Vec<u8>> {
    let mut req = http
        .get(&asset.browser_download_url)
        .header("Accept", "application/octet-stream");
    if let Some(token) = &env.token {
        req = req.bearer_auth(token);
    }
    let resp = req.send().await.map_err(|e| err(e.to_string()))?;
    if !resp.status().is_success() {
        return Err(err(format!(
            "downloading {} answered HTTP {}",
            asset.name,
            resp.status()
        )));
    }
    Ok(resp.bytes().await.map_err(|e| err(e.to_string()))?.to_vec())
}

/// Compare an asset against the digest the release API published for it.
///
/// This proves the bytes are the ones GitHub recorded, so a mangled or
/// truncated download is caught. It is not a signature: it says nothing that
/// the API response itself did not say.
fn verify_digest(digest: Option<&str>, bytes: &[u8]) -> std::result::Result<(), String> {
    let Some(digest) = digest else {
        // Older releases predate the field. The size check and the archive
        // parse still have to pass, and the transport was TLS.
        return Ok(());
    };
    // The field is `sha256:<hex>`. An algorithm this build cannot compute has
    // to be refused rather than compared as if it were hex, which would pass
    // nothing and fail everything for a confusing reason.
    let expected = match digest.split_once(':') {
        Some((algorithm, hex)) if algorithm.eq_ignore_ascii_case("sha256") => hex,
        Some((algorithm, _)) => {
            return Err(format!(
                "the release records a {algorithm} digest, which this build cannot verify"
            ));
        }
        None => digest,
    };
    let actual = sha256_hex(bytes);
    if actual.eq_ignore_ascii_case(expected) {
        Ok(())
    } else {
        Err(format!(
            "sha256 is {actual}, but the release records {expected}"
        ))
    }
}

fn sha256_hex(bytes: &[u8]) -> String {
    use sha2::{Digest, Sha256};
    let mut hasher = Sha256::new();
    hasher.update(bytes);
    hasher
        .finalize()
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect()
}

// --- apply ------------------------------------------------------------------

/// Install a staged release, if the editor is closed. Leaves the stage alone
/// otherwise so the next start can try again.
async fn finish(env: &UpdateEnv, editor: &EditorClient, version: &str) -> Result<()> {
    if editor.ping().await.is_ok() {
        tracing::debug!("update: {version} is staged; waiting for the editor to close");
        return Ok(());
    }

    let staged = env.staged_dir(version);
    let plugin = plugin_plan(env);
    if let PluginPlan::Blocked(reason) = &plugin {
        return Err(err(format!("cannot install {version}: {reason}")));
    }

    let mut swapped_plugins = Vec::new();
    let result = (|| -> Result<()> {
        if let PluginPlan::Update(names) = &plugin {
            let line = env.engine_line.as_deref().unwrap_or_default();
            let zip = staged.join(plugin_asset_name(version, line));
            let root = env
                .project_root
                .as_ref()
                .ok_or_else(|| err("no project root"))?
                .join("Plugins");
            swapped_plugins = install_plugins(&zip, &root, names)?;
        }
        install_server(&staged, version)
    })();

    match result {
        Ok(()) => {
            for swap in &swapped_plugins {
                let _ = std::fs::remove_dir_all(&swap.backup);
            }
            let _ = std::fs::remove_dir_all(&staged);
            let mut state = UpdateState::new(
                "applied",
                Some(format!(
                    "{version} is installed and takes effect the next time this server and the editor start"
                )),
            );
            state.latest = Some(version.to_string());
            write_state(env, &state);
            tracing::info!("update: installed {version}; it takes effect on the next start");
            Ok(())
        }
        Err(e) => {
            // The pair moves together or not at all, so anything already
            // swapped goes back.
            for swap in swapped_plugins.iter().rev() {
                let _ = std::fs::remove_dir_all(&swap.live);
                let _ = std::fs::rename(&swap.backup, &swap.live);
            }
            Err(e)
        }
    }
}

/// A plugin folder that has been replaced, and where its predecessor went.
struct PluginSwap {
    live: PathBuf,
    backup: PathBuf,
}

/// Replace installed plugin folders from the release zip.
///
/// The zip's root is one folder per plugin, which is the shape of `Plugins/`
/// already, so this extracts beside the live folders and then swaps by rename:
/// renames inside one directory are as close to atomic as this gets, and they
/// leave the old folder intact for rollback. Only folders that are already
/// installed are written — a release carrying more interop plugins than the
/// user chose must not install the rest.
fn install_plugins(
    zip_path: &Path,
    plugins_dir: &Path,
    names: &[String],
) -> Result<Vec<PluginSwap>> {
    let staging = plugins_dir.join(".mcp-update");
    let _ = std::fs::remove_dir_all(&staging);
    std::fs::create_dir_all(&staging)?;

    let extracted = extract_zip(zip_path, &staging, names)?;
    for name in names {
        if !extracted.contains(name) {
            let _ = std::fs::remove_dir_all(&staging);
            return Err(err(format!("the release zip has no {name} folder")));
        }
    }

    let mut swaps = Vec::new();
    for name in names {
        let live = plugins_dir.join(name);
        let backup = plugins_dir.join(format!("{name}.mcpbak"));
        let _ = std::fs::remove_dir_all(&backup);
        std::fs::rename(&live, &backup)?;
        if let Err(e) = std::fs::rename(staging.join(name), &live) {
            // Put this one back before the caller unwinds the rest.
            let _ = std::fs::rename(&backup, &live);
            return Err(e.into());
        }
        swaps.push(PluginSwap { live, backup });
    }
    let _ = std::fs::remove_dir_all(&staging);
    Ok(swaps)
}

/// Unpack the entries under `allowed` top-level folders into `dest`, and
/// report which of them were seen.
///
/// Archive entries are attacker-controlled input by construction, so paths go
/// through `enclosed_name`, which refuses absolute paths and anything that
/// climbs out of the destination with `..`.
fn extract_zip(
    zip_path: &Path,
    dest: &Path,
    allowed: &[String],
) -> Result<std::collections::BTreeSet<String>> {
    let file = std::fs::File::open(zip_path)?;
    let mut archive = zip::ZipArchive::new(std::io::BufReader::new(file))
        .map_err(|e| err(format!("unreadable zip {}: {e}", zip_path.display())))?;

    let mut seen = std::collections::BTreeSet::new();
    for index in 0..archive.len() {
        let mut entry = archive
            .by_index(index)
            .map_err(|e| err(format!("unreadable zip entry: {e}")))?;
        let Some(relative) = entry.enclosed_name() else {
            return Err(err(format!(
                "zip entry {} escapes the destination",
                entry.name()
            )));
        };
        let Some(top) = relative.components().next() else {
            continue;
        };
        let top = top.as_os_str().to_string_lossy().to_string();
        if !allowed.contains(&top) {
            continue;
        }
        seen.insert(top);

        let out = dest.join(&relative);
        if entry.is_dir() {
            std::fs::create_dir_all(&out)?;
            continue;
        }
        if let Some(parent) = out.parent() {
            std::fs::create_dir_all(parent)?;
        }
        let mut writer = std::io::BufWriter::new(std::fs::File::create(&out)?);
        std::io::copy(&mut entry, &mut writer)?;
        writer.flush()?;
    }
    Ok(seen)
}

/// Put the new binary where this process's binary is, for the next launch.
fn install_server(staged: &Path, version: &str) -> Result<()> {
    let exe = std::env::current_exe()?;
    let name = if cfg!(windows) {
        "mcp-unreal.exe"
    } else {
        "mcp-unreal"
    };
    let archive = staged.join(
        server_asset_name(version, std::env::consts::OS, std::env::consts::ARCH)
            .ok_or_else(|| err("no release build for this platform"))?,
    );

    // Unpack next to the running binary rather than in the cache directory:
    // the swap below is a rename, and a rename cannot cross a filesystem.
    let incoming = exe.with_file_name(format!("{name}.new"));
    let _ = std::fs::remove_file(&incoming);
    extract_binary(&archive, name, &incoming)?;

    let result = replace_running_exe(&exe, &incoming);
    if result.is_err() {
        let _ = std::fs::remove_file(&incoming);
    }
    result
}

/// Pull the single executable out of a release archive.
fn extract_binary(archive: &Path, name: &str, dest: &Path) -> Result<()> {
    let file = std::fs::File::open(archive)?;
    let found = if archive.extension().is_some_and(|e| e == "zip") {
        let mut zip = zip::ZipArchive::new(std::io::BufReader::new(file))
            .map_err(|e| err(format!("unreadable zip: {e}")))?;
        let mut found = false;
        for index in 0..zip.len() {
            let mut entry = zip
                .by_index(index)
                .map_err(|e| err(format!("unreadable zip entry: {e}")))?;
            let is_match = entry
                .enclosed_name()
                .is_some_and(|p| p.file_name().is_some_and(|f| f == name));
            if is_match && !entry.is_dir() {
                write_executable(&mut entry, dest)?;
                found = true;
                break;
            }
        }
        found
    } else {
        let mut tar = tar::Archive::new(flate2::read::GzDecoder::new(file));
        let mut found = false;
        for entry in tar.entries()? {
            let mut entry = entry?;
            let is_match = entry
                .path()
                .map(|p| p.file_name().is_some_and(|f| f == name))
                .unwrap_or(false);
            if is_match {
                write_executable(&mut entry, dest)?;
                found = true;
                break;
            }
        }
        found
    };
    if !found {
        return Err(err(format!(
            "{} contains no {name}",
            archive.file_name().unwrap_or_default().to_string_lossy()
        )));
    }
    Ok(())
}

fn write_executable(reader: &mut impl Read, dest: &Path) -> Result<()> {
    let mut writer = std::io::BufWriter::new(std::fs::File::create(dest)?);
    std::io::copy(reader, &mut writer)?;
    writer.flush()?;
    drop(writer);
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(dest, std::fs::Permissions::from_mode(0o755))?;
    }
    Ok(())
}

/// Swap a new binary in for the one this process is running from.
///
/// Windows will not let a running image be written or deleted, but it will let
/// it be *renamed*, which is the whole trick: move the running binary aside,
/// move the new one into its place, and delete the leftover on a later start
/// once nothing has it open. Unix replaces the directory entry and the running
/// process keeps its inode, so the same sequence works there and the leftover
/// can go immediately.
fn replace_running_exe(exe: &Path, incoming: &Path) -> Result<()> {
    let previous = exe.with_file_name(format!(
        "{}.old",
        exe.file_name().unwrap_or_default().to_string_lossy()
    ));
    let _ = std::fs::remove_file(&previous);
    std::fs::rename(exe, &previous)?;
    if let Err(e) = std::fs::rename(incoming, exe) {
        // Nothing has been installed, so put the running binary's name back.
        let _ = std::fs::rename(&previous, exe);
        return Err(e.into());
    }
    // Fails on Windows while this process is alive; cleaned up next start.
    let _ = std::fs::remove_file(&previous);
    Ok(())
}

/// Delete the previous binary left behind by an earlier swap. Called at
/// startup, when the file is no longer the running image.
pub fn clean_previous_binary() {
    let Ok(exe) = std::env::current_exe() else {
        return;
    };
    let previous = exe.with_file_name(format!(
        "{}.old",
        exe.file_name().unwrap_or_default().to_string_lossy()
    ));
    if previous.exists() {
        let _ = std::fs::remove_file(&previous);
    }
}

// --- single-writer lock -----------------------------------------------------

/// Several MCP sessions run several copies of this server, and they start
/// together often enough that two of them racing on the same swap is a real
/// case rather than a theoretical one.
struct Lock(PathBuf);

impl Lock {
    fn acquire(dir: &Path) -> Result<Self> {
        std::fs::create_dir_all(dir)?;
        let path = dir.join("update.lock");
        match std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&path)
        {
            Ok(mut file) => {
                let _ = write!(file, "{}", std::process::id());
                Ok(Self(path))
            }
            Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => {
                let stale = std::fs::metadata(&path)
                    .and_then(|m| m.modified())
                    .map(|t| t.elapsed().unwrap_or_default() > LOCK_STALE_AFTER)
                    .unwrap_or(false);
                if stale {
                    // Whoever held this did not live to release it.
                    std::fs::remove_file(&path)?;
                    return Self::acquire(dir);
                }
                Err(err("another mcp-unreal is already updating"))
            }
            Err(e) => Err(e.into()),
        }
    }
}

impl Drop for Lock {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.0);
    }
}

// --- small pure helpers -----------------------------------------------------

fn now_secs() -> Option<u64> {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .ok()
        .map(|d| d.as_secs())
}

/// `1.2.3` into something orderable. Deliberately strict: release tags are
/// validated as `\d+\.\d+\.\d+` by tools/bump-version.ps1, so anything else is
/// a tag this updater should not act on.
pub fn parse_version(text: &str) -> Option<(u64, u64, u64)> {
    let mut parts = text.trim().trim_start_matches('v').split('.');
    let major = parts.next()?.parse().ok()?;
    let minor = parts.next()?.parse().ok()?;
    let patch = parts.next()?.parse().ok()?;
    if parts.next().is_some() {
        return None;
    }
    Some((major, minor, patch))
}

/// `owner`/`repo` out of a GitHub URL.
fn parse_repo(url: &str) -> Option<(String, String)> {
    let rest = url
        .trim_end_matches('/')
        .trim_end_matches(".git")
        .split("github.com")
        .nth(1)?
        .trim_start_matches(['/', ':']);
    let mut parts = rest.split('/');
    let owner = parts.next().filter(|s| !s.is_empty())?;
    let repo = parts.next().filter(|s| !s.is_empty())?;
    if parts.next().is_some() {
        return None;
    }
    Some((owner.to_string(), repo.to_string()))
}

/// The release asset holding the server for a host, matching the target
/// triples the release workflow builds.
pub fn server_asset_name(version: &str, os: &str, arch: &str) -> Option<String> {
    let (triple, ext) = match (os, arch) {
        ("windows", "x86_64") => ("x86_64-pc-windows-msvc", "zip"),
        ("macos", "aarch64") => ("aarch64-apple-darwin", "tar.gz"),
        ("macos", "x86_64") => ("x86_64-apple-darwin", "tar.gz"),
        ("linux", "x86_64") => ("x86_64-unknown-linux-gnu", "tar.gz"),
        _ => return None,
    };
    Some(format!("mcp-unreal-{version}-{triple}.{ext}"))
}

/// The release asset holding the prebuilt plugin, as tools/package-plugin.ps1
/// names it: the engine line is part of the name because the binaries only
/// work on the line they were compiled against.
pub fn plugin_asset_name(version: &str, engine_line: &str) -> String {
    format!("McpLink-{version}-UE{engine_line}-Win64.zip")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_versions_and_rejects_anything_else() {
        assert_eq!(parse_version("0.1.0"), Some((0, 1, 0)));
        assert_eq!(parse_version("v1.2.3"), Some((1, 2, 3)));
        assert_eq!(parse_version(" 10.20.30 "), Some((10, 20, 30)));
        assert_eq!(parse_version("0.2.0-rc1"), None);
        assert_eq!(parse_version("0.1"), None);
        assert_eq!(parse_version("0.1.0.1"), None);
        assert_eq!(parse_version(""), None);
    }

    #[test]
    fn orders_versions_numerically_not_lexically() {
        // The bug this guards: "0.10.0" sorts before "0.9.0" as a string.
        assert!(parse_version("0.10.0") > parse_version("0.9.0"));
        assert!(parse_version("1.0.0") > parse_version("0.99.99"));
    }

    #[test]
    fn parses_the_repository_out_of_crate_metadata() {
        assert_eq!(
            parse_repo("https://github.com/HTRMC/mcp-unreal"),
            Some(("HTRMC".to_string(), "mcp-unreal".to_string()))
        );
        assert_eq!(
            parse_repo("https://github.com/HTRMC/mcp-unreal.git"),
            Some(("HTRMC".to_string(), "mcp-unreal".to_string()))
        );
        assert_eq!(parse_repo("https://gitlab.com/a/b"), None);
        assert_eq!(parse_repo("https://github.com/HTRMC"), None);
    }

    #[test]
    fn the_crates_own_repository_resolves() {
        assert_eq!(
            parse_repo(env!("CARGO_PKG_REPOSITORY")),
            Some(("HTRMC".to_string(), "mcp-unreal".to_string()))
        );
    }

    #[test]
    fn asset_names_match_what_the_release_workflow_publishes() {
        assert_eq!(
            server_asset_name("0.1.0", "windows", "x86_64").unwrap(),
            "mcp-unreal-0.1.0-x86_64-pc-windows-msvc.zip"
        );
        assert_eq!(
            server_asset_name("0.1.0", "linux", "x86_64").unwrap(),
            "mcp-unreal-0.1.0-x86_64-unknown-linux-gnu.tar.gz"
        );
        assert_eq!(
            server_asset_name("0.1.0", "macos", "aarch64").unwrap(),
            "mcp-unreal-0.1.0-aarch64-apple-darwin.tar.gz"
        );
        assert_eq!(server_asset_name("0.1.0", "freebsd", "x86_64"), None);
        assert_eq!(
            plugin_asset_name("0.1.0", "5.8"),
            "McpLink-0.1.0-UE5.8-Win64.zip"
        );
    }

    #[test]
    fn channel_parses_the_settings_and_defaults_to_apply() {
        assert_eq!(Channel::parse(""), Channel::Apply);
        assert_eq!(Channel::parse("apply"), Channel::Apply);
        assert_eq!(Channel::parse(" CHECK "), Channel::Check);
        assert_eq!(Channel::parse("notify"), Channel::Check);
        assert_eq!(Channel::parse("off"), Channel::Off);
        assert_eq!(Channel::parse("no"), Channel::Off);
        // A typo must not silently disable updates.
        assert_eq!(Channel::parse("offf"), Channel::Apply);
    }

    #[test]
    fn digest_verification_accepts_the_recorded_hash_and_rejects_a_changed_byte() {
        // sha256("mcp") — computed here rather than pasted, so the test proves
        // the comparison, not a constant.
        let bytes = b"mcp-unreal release bytes";
        let digest = format!("sha256:{}", sha256_hex(bytes));
        assert!(verify_digest(Some(&digest), bytes).is_ok());
        assert!(verify_digest(Some(&digest.to_uppercase()), bytes).is_ok());
        assert!(verify_digest(Some(&digest), b"tampered").is_err());
        // A digest this build cannot compute must not be waved through.
        assert!(verify_digest(Some("sha512:00ff"), bytes).is_err());
        // A release that predates the digest field is not a failure.
        assert!(verify_digest(None, bytes).is_ok());
    }

    #[test]
    fn sha256_matches_a_known_vector() {
        assert_eq!(
            sha256_hex(b"abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
    }

    #[test]
    fn a_plugin_folder_without_binaries_blocks_the_pair() {
        let tmp = tempfile::tempdir().unwrap();
        let plugins = tmp.path().join("Plugins");
        std::fs::create_dir_all(plugins.join("McpLink").join("Source")).unwrap();
        let reason = installed_plugins(&plugins).unwrap_err();
        assert!(reason.contains("built from source"), "{reason}");
    }

    #[test]
    fn an_installed_plugin_is_updatable_and_unrelated_folders_are_ignored() {
        let tmp = tempfile::tempdir().unwrap();
        let plugins = tmp.path().join("Plugins");
        std::fs::create_dir_all(plugins.join("McpLink").join("Binaries")).unwrap();
        std::fs::create_dir_all(plugins.join("McpLinkNiagara").join("Binaries")).unwrap();
        std::fs::create_dir_all(plugins.join("SomeoneElsesPlugin")).unwrap();
        assert_eq!(
            installed_plugins(&plugins).unwrap(),
            vec!["McpLink".to_string(), "McpLinkNiagara".to_string()]
        );
    }

    #[test]
    fn no_plugins_directory_is_not_an_error() {
        let tmp = tempfile::tempdir().unwrap();
        let plugins = tmp.path().join("Plugins");
        std::fs::create_dir_all(&plugins).unwrap();
        assert!(installed_plugins(&plugins).unwrap().is_empty());
    }

    #[test]
    fn the_lock_excludes_a_second_run_and_frees_on_drop() {
        let tmp = tempfile::tempdir().unwrap();
        let first = Lock::acquire(tmp.path()).unwrap();
        assert!(Lock::acquire(tmp.path()).is_err());
        drop(first);
        assert!(Lock::acquire(tmp.path()).is_ok());
    }

    #[test]
    fn extraction_writes_only_the_folders_that_are_installed() {
        let tmp = tempfile::tempdir().unwrap();
        let zip_path = tmp.path().join("plugins.zip");
        write_test_zip(
            &zip_path,
            &[
                ("McpLink/McpLink.uplugin", b"{}".as_slice()),
                ("McpLink/Binaries/Win64/x.dll", b"dll".as_slice()),
                ("McpLinkNiagara/McpLinkNiagara.uplugin", b"{}".as_slice()),
            ],
        );
        let dest = tmp.path().join("out");
        let seen = extract_zip(&zip_path, &dest, &["McpLink".to_string()]).unwrap();

        assert_eq!(seen, ["McpLink".to_string()].into_iter().collect());
        assert!(dest.join("McpLink/Binaries/Win64/x.dll").is_file());
        // The release carries an interop plugin this project never installed.
        assert!(!dest.join("McpLinkNiagara").exists());
    }

    #[test]
    fn extraction_refuses_an_entry_that_climbs_out_of_the_destination() {
        let tmp = tempfile::tempdir().unwrap();
        let zip_path = tmp.path().join("evil.zip");
        write_test_zip(&zip_path, &[("../../escaped.txt", b"no".as_slice())]);
        let dest = tmp.path().join("out");
        let result = extract_zip(&zip_path, &dest, &["McpLink".to_string()]);
        assert!(result.is_err(), "a path-traversal entry must be refused");
    }

    #[test]
    fn a_plugin_swap_is_reversible() {
        let tmp = tempfile::tempdir().unwrap();
        let plugins = tmp.path().join("Plugins");
        std::fs::create_dir_all(plugins.join("McpLink").join("Binaries")).unwrap();
        std::fs::write(plugins.join("McpLink").join("marker.txt"), "old").unwrap();

        let zip_path = tmp.path().join("plugins.zip");
        write_test_zip(&zip_path, &[("McpLink/marker.txt", b"new".as_slice())]);

        let names = vec!["McpLink".to_string()];
        let swaps = install_plugins(&zip_path, &plugins, &names).unwrap();
        assert_eq!(
            std::fs::read_to_string(plugins.join("McpLink/marker.txt")).unwrap(),
            "new"
        );

        // What finish() does when the server half fails after this point.
        for swap in swaps.iter().rev() {
            std::fs::remove_dir_all(&swap.live).unwrap();
            std::fs::rename(&swap.backup, &swap.live).unwrap();
        }
        assert_eq!(
            std::fs::read_to_string(plugins.join("McpLink/marker.txt")).unwrap(),
            "old"
        );
    }

    #[test]
    fn a_missing_folder_in_the_zip_leaves_every_plugin_untouched() {
        let tmp = tempfile::tempdir().unwrap();
        let plugins = tmp.path().join("Plugins");
        std::fs::create_dir_all(plugins.join("McpLink").join("Binaries")).unwrap();
        std::fs::create_dir_all(plugins.join("McpLinkGAS").join("Binaries")).unwrap();

        let zip_path = tmp.path().join("plugins.zip");
        write_test_zip(&zip_path, &[("McpLink/marker.txt", b"new".as_slice())]);

        let names = vec!["McpLink".to_string(), "McpLinkGAS".to_string()];
        assert!(install_plugins(&zip_path, &plugins, &names).is_err());
        assert!(plugins.join("McpLink").is_dir());
        assert!(plugins.join("McpLinkGAS").is_dir());
        assert!(!plugins.join("McpLink.mcpbak").exists());
    }

    #[test]
    fn the_binary_swap_installs_the_new_file_and_leaves_the_old_recoverable() {
        let tmp = tempfile::tempdir().unwrap();
        let exe = tmp.path().join("mcp-unreal.exe");
        std::fs::write(&exe, "old binary").unwrap();
        let incoming = tmp.path().join("mcp-unreal.exe.new");
        std::fs::write(&incoming, "new binary").unwrap();

        replace_running_exe(&exe, &incoming).unwrap();
        assert_eq!(std::fs::read_to_string(&exe).unwrap(), "new binary");
        assert!(!incoming.exists());
    }

    #[test]
    fn the_server_binary_is_found_in_both_archive_shapes() {
        let tmp = tempfile::tempdir().unwrap();

        let zip_path = tmp
            .path()
            .join("mcp-unreal-0.2.0-x86_64-pc-windows-msvc.zip");
        write_test_zip(
            &zip_path,
            &[
                (
                    "mcp-unreal-0.2.0-x86_64-pc-windows-msvc/README.md",
                    b"docs".as_slice(),
                ),
                (
                    "mcp-unreal-0.2.0-x86_64-pc-windows-msvc/mcp-unreal.exe",
                    b"WINDOWS".as_slice(),
                ),
            ],
        );
        let out = tmp.path().join("from-zip");
        extract_binary(&zip_path, "mcp-unreal.exe", &out).unwrap();
        assert_eq!(std::fs::read_to_string(&out).unwrap(), "WINDOWS");

        let tar_path = tmp
            .path()
            .join("mcp-unreal-0.2.0-x86_64-unknown-linux-gnu.tar.gz");
        write_test_tar_gz(
            &tar_path,
            &[
                (
                    "mcp-unreal-0.2.0-x86_64-unknown-linux-gnu/LICENSE",
                    b"MIT".as_slice(),
                ),
                (
                    "mcp-unreal-0.2.0-x86_64-unknown-linux-gnu/mcp-unreal",
                    b"POSIX".as_slice(),
                ),
            ],
        );
        let out = tmp.path().join("from-tar");
        extract_binary(&tar_path, "mcp-unreal", &out).unwrap();
        assert_eq!(std::fs::read_to_string(&out).unwrap(), "POSIX");

        assert!(extract_binary(&zip_path, "not-in-there", tmp.path().join("x").as_path()).is_err());
    }

    // --- the network path ---------------------------------------------------
    //
    // These drive run() and download() against a mock GitHub. They never reach
    // the install step: install_server() replaces std::env::current_exe, which
    // under `cargo test` is the test binary itself. The install half is covered
    // by the swap and rollback tests above, which are handed explicit paths.

    fn test_env(dir: &Path, api_base: String, channel: Channel) -> UpdateEnv {
        UpdateEnv {
            channel,
            api_base,
            token: None,
            dir: dir.to_path_buf(),
            repo: Some(("o".to_string(), "r".to_string())),
            current: "0.1.0".to_string(),
            engine_line: Some("5.8".to_string()),
            project_root: None,
        }
    }

    /// An editor client pointed at a port nothing is listening on, which is
    /// what "the editor is closed" looks like.
    fn offline_editor() -> EditorClient {
        EditorClient::new(9)
    }

    async fn mock_release(assets: serde_json::Value) -> wiremock::MockServer {
        use wiremock::matchers::{method, path};
        use wiremock::{Mock, ResponseTemplate};

        let server = wiremock::MockServer::start().await;
        Mock::given(method("GET"))
            .and(path("/repos/o/r/releases/latest"))
            .respond_with(ResponseTemplate::new(200).set_body_json(serde_json::json!({
                "tag_name": "v0.2.0",
                "assets": assets,
            })))
            .mount(&server)
            .await;
        server
    }

    #[tokio::test]
    async fn a_newer_release_is_reported_without_touching_disk_on_the_check_channel() {
        let tmp = tempfile::tempdir().unwrap();
        let server = mock_release(serde_json::json!([])).await;
        let env = test_env(tmp.path(), server.uri(), Channel::Check);

        run(&env, &offline_editor()).await.unwrap();

        let state = read_state(&env).unwrap();
        assert_eq!(state.state, "update_available");
        assert_eq!(state.latest.as_deref(), Some("0.2.0"));
        assert!(state.detail.unwrap().contains("0.2.0"));
        // Nothing was downloaded: check looks and reports, it does not fetch.
        assert!(!tmp.path().join("staged").exists());
    }

    #[tokio::test]
    async fn the_current_version_is_reported_up_to_date() {
        let tmp = tempfile::tempdir().unwrap();
        let server = mock_release(serde_json::json!([])).await;
        let mut env = test_env(tmp.path(), server.uri(), Channel::Check);
        env.current = "0.2.0".to_string();

        run(&env, &offline_editor()).await.unwrap();

        assert_eq!(read_state(&env).unwrap().state, "up_to_date");
    }

    #[tokio::test]
    async fn an_older_release_never_downgrades() {
        let tmp = tempfile::tempdir().unwrap();
        let server = mock_release(serde_json::json!([])).await;
        let mut env = test_env(tmp.path(), server.uri(), Channel::Check);
        env.current = "0.9.0".to_string();

        run(&env, &offline_editor()).await.unwrap();

        assert_eq!(read_state(&env).unwrap().state, "up_to_date");
    }

    #[tokio::test]
    async fn a_recent_check_is_not_repeated() {
        let tmp = tempfile::tempdir().unwrap();
        // Nothing is listening on the API base, so a request would fail the run.
        let env = test_env(tmp.path(), "http://127.0.0.1:9".to_string(), Channel::Check);
        write_state(
            &env,
            &UpdateState {
                checked_at: now_secs(),
                latest: Some("0.1.0".to_string()),
                state: "up_to_date".to_string(),
                detail: None,
            },
        );

        run(&env, &offline_editor()).await.unwrap();
        assert_eq!(read_state(&env).unwrap().state, "up_to_date");
    }

    #[tokio::test]
    async fn a_plugin_that_cannot_be_updated_stops_the_server_half_too() {
        let tmp = tempfile::tempdir().unwrap();
        let project = tmp.path().join("project");
        // A source checkout: Source but no Binaries.
        std::fs::create_dir_all(project.join("Plugins/McpLink/Source")).unwrap();

        let server = mock_release(serde_json::json!([])).await;
        let mut env = test_env(tmp.path(), server.uri(), Channel::Apply);
        env.project_root = Some(project);

        run(&env, &offline_editor()).await.unwrap();

        let state = read_state(&env).unwrap();
        assert_eq!(state.state, "blocked");
        let detail = state.detail.unwrap();
        assert!(detail.contains("built from source"), "{detail}");
        // The invariant under test: the server half is not fetched either.
        assert!(!tmp.path().join("staged").exists());
    }

    #[tokio::test]
    async fn assets_are_staged_only_when_their_recorded_digest_matches() {
        use wiremock::matchers::{method, path};
        use wiremock::{Mock, ResponseTemplate};

        let tmp = tempfile::tempdir().unwrap();
        let server = wiremock::MockServer::start().await;
        let body = b"a release archive".to_vec();
        Mock::given(method("GET"))
            .and(path("/download/server.zip"))
            .respond_with(ResponseTemplate::new(200).set_body_bytes(body.clone()))
            .mount(&server)
            .await;

        let env = test_env(tmp.path(), server.uri(), Channel::Apply);
        let asset_name = server_asset_name("0.2.0", std::env::consts::OS, std::env::consts::ARCH)
            .expect("the host is one of the release targets");

        let good = Release {
            tag_name: "v0.2.0".to_string(),
            assets: vec![Asset {
                name: asset_name.clone(),
                browser_download_url: format!("{}/download/server.zip", server.uri()),
                size: body.len() as u64,
                digest: Some(format!("sha256:{}", sha256_hex(&body))),
            }],
        };
        let http = reqwest::Client::new();
        download(&http, &env, &good, "0.2.0", &PluginPlan::NotInstalled)
            .await
            .unwrap();
        assert!(env.staged_dir("0.2.0").join(&asset_name).is_file());
        assert_eq!(read_state(&env).unwrap().state, "staged");

        let mut tampered = good.clone();
        tampered.assets[0].digest = Some(format!("sha256:{}", sha256_hex(b"different bytes")));
        let error = download(&http, &env, &tampered, "0.2.0", &PluginPlan::NotInstalled)
            .await
            .unwrap_err()
            .to_string();
        assert!(error.contains("sha256"), "{error}");
        // A failed verification leaves nothing behind to be installed later.
        assert!(!env.staged_dir("0.2.0").join(&asset_name).exists());
    }

    #[tokio::test]
    async fn a_private_or_releaseless_repository_says_so() {
        use wiremock::matchers::{method, path};
        use wiremock::{Mock, ResponseTemplate};

        let tmp = tempfile::tempdir().unwrap();
        let server = wiremock::MockServer::start().await;
        Mock::given(method("GET"))
            .and(path("/repos/o/r/releases/latest"))
            .respond_with(ResponseTemplate::new(404))
            .mount(&server)
            .await;

        let env = test_env(tmp.path(), server.uri(), Channel::Apply);
        let error = run(&env, &offline_editor()).await.unwrap_err().to_string();
        assert!(error.contains("MCP_UNREAL_UPDATE_TOKEN"), "{error}");
    }

    #[tokio::test]
    async fn a_staged_update_is_installed_without_asking_github_again() {
        let tmp = tempfile::tempdir().unwrap();
        // Nothing is listening on the API base: reaching it would fail this run.
        let env = test_env(tmp.path(), "http://127.0.0.1:9".to_string(), Channel::Apply);
        std::fs::create_dir_all(env.staged_dir("0.2.0")).unwrap();
        write_state(
            &env,
            &UpdateState {
                checked_at: now_secs(),
                latest: Some("0.2.0".to_string()),
                state: "staged".to_string(),
                detail: None,
            },
        );

        let outcome = run(&env, &offline_editor()).await;

        // The stage is empty, so installing it fails — but the failure has to
        // come from the local archive, proving the run went straight to the
        // install instead of back to GitHub.
        let message = outcome.unwrap_err().to_string();
        assert!(!message.contains("HTTP"), "{message}");
    }

    fn write_test_zip(path: &Path, entries: &[(&str, &[u8])]) {
        let file = std::fs::File::create(path).unwrap();
        let mut zip = zip::ZipWriter::new(file);
        let options: zip::write::FileOptions<'_, ()> =
            zip::write::FileOptions::default().compression_method(zip::CompressionMethod::Deflated);
        for (name, body) in entries {
            zip.start_file(*name, options).unwrap();
            zip.write_all(body).unwrap();
        }
        zip.finish().unwrap();
    }

    fn write_test_tar_gz(path: &Path, entries: &[(&str, &[u8])]) {
        let file = std::fs::File::create(path).unwrap();
        let encoder = flate2::write::GzEncoder::new(file, flate2::Compression::fast());
        let mut builder = tar::Builder::new(encoder);
        for (name, body) in entries {
            let mut header = tar::Header::new_gnu();
            header.set_size(body.len() as u64);
            header.set_mode(0o755);
            header.set_cksum();
            builder.append_data(&mut header, *name, *body).unwrap();
        }
        builder.into_inner().unwrap().finish().unwrap();
    }
}

use rmcp::ServiceExt;
use tracing_subscriber::EnvFilter;

#[tokio::main]
async fn main() {
    if std::env::args().any(|a| a == "--version" || a == "-V") {
        println!("mcp-unreal {}", env!("CARGO_PKG_VERSION"));
        return;
    }

    // stdout is reserved for JSON-RPC; ALL logging goes to stderr.
    let filter =
        EnvFilter::try_from_env("MCP_UNREAL_LOG_LEVEL").unwrap_or_else(|_| EnvFilter::new("info"));
    tracing_subscriber::fmt()
        .with_env_filter(filter)
        .with_writer(std::io::stderr)
        .with_ansi(false)
        .init();

    let cfg = mcp_unreal::config::Config::load();
    tracing::info!(
        engine_root = %cfg.engine_root.display(),
        project = ?cfg.project.as_ref().map(|p| p.uproject.display().to_string()),
        plugin_port = cfg.plugin_port,
        "starting mcp-unreal {}",
        env!("CARGO_PKG_VERSION")
    );

    // A previous self-update leaves the old binary behind, because Windows
    // will not delete a running image. It is deletable now.
    mcp_unreal::update::clean_previous_binary();
    // Checks and installs in the background: an update must never delay, or
    // fail, the session that discovers it.
    mcp_unreal::update::spawn(&cfg);

    let server = mcp_unreal::UnrealMcp::new(cfg);
    match server.serve(rmcp::transport::stdio()).await {
        Ok(service) => {
            if let Err(e) = service.waiting().await {
                tracing::error!("server terminated with error: {e}");
                std::process::exit(1);
            }
        }
        Err(e) => {
            tracing::error!("failed to start MCP server: {e}");
            std::process::exit(1);
        }
    }
}

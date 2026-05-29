// Entry point for the sweep service. Loads config, polls positions,
// triggers sweeps when a position crosses the liquidation threshold.

mod watcher;

#[tokio::main]
async fn main() {
    let cfg = load_config();
    watcher::run(cfg).await;
}

fn load_config() -> String {
    std::fs::read_to_string("../config/thresholds.toml")
        .unwrap_or_default()
}

# T1a: The Autonomous Daemon

T1a is an ultra-lightweight, autonomous AI agent built in pure C11. Based on the [noclaw](https://github.com/angristan/noclaw) architecture, it is designed for maximum efficiency with a near-zero resource footprint. T1a acts as a high-performance command unit, capable of running indefinitely as a background daemon to manage tasks, execute tools, and assist via Telegram, CLI, or HTTP gateway.

## Key Capabilities

- **Ultra-Efficient**: ~80KB binary, <5MB RAM usage. Zero external runtime dependencies.
- **Daemon-Ready (Always-On)**: Self-healing process management, signal handling (`SIGINT`/`SIGTERM`), automated resource cleanup. Native `systemd` integration for 24/7 autonomous operation.
- **Full MCP Support**: Native, robust Model Context Protocol (MCP) client. Drives complex toolchains (Filesystem, Search, Tavily, Sequential Thinking, Memory Graph) with full handshake and timeout management.
- **Self-Managing Memory**: Flat-file memory system with semantic-like keyword search, automatic pruning, and memory compaction to prevent context bloat and OOM crashes.
- **Provider Fallback**: Primary provider (e.g. OpenRouter) with configurable fallback (e.g. Anthropic direct). Automatic switch on failure for resilience.
- **Robust JSON/HTTP**: Standard-compliant JSON-RPC layer with strict character escaping. Compatible with OpenRouter, OpenAI, Anthropic, and Gemini.

## Architecture

T1a utilizes a modular, function-pointer based architecture for extreme flexibility without bloat:

- **Core**: Event loop driven by non-blocking I/O (via `select`).
- **Providers**: Native TLS support for Anthropic (Claude 3.5 Sonnet) and OpenAI-compatible APIs (OpenRouter, Llama 3.3, Gemini 2.0 Flash, etc.).
- **Tools**: Built-in shell, filesystem I/O, and dynamic MCP proxying.
- **Persistence**: Memories stored at `workspace/memories.tsv` with auto-garbage collection.

## Quick Start

### Build
```bash
make release
```

### Configuration
Managed via `~/.noclaw/config.json`. T1a auto-generates optimized defaults on first run.

```bash
# Initialize configuration
./noclaw onboard --api-key sk-or-... --provider openrouter
```

### Execution

```bash
# Run as an interactive CLI agent
./noclaw agent

# Run as a background daemon (Telegram mode)
./noclaw agent --channel telegram &

# Run HTTP gateway
./noclaw gateway
```

### Setup Always-On (Systemd)
To ensure T1a remains operational after reboots or process failures:
```bash
mkdir -p ~/.config/systemd/user/
cat > ~/.config/systemd/user/t1a.service <<EOF
[Unit]
Description=T1a AI Agent - Always On
After=network.target

[Service]
Environment=NOCLAW_TELEGRAM_TOKEN=your_token_here
WorkingDirectory=$(pwd)
ExecStart=$(pwd)/noclaw agent --channel telegram
Restart=always
RestartSec=10
StandardOutput=append:~/t1a_final.log
StandardError=append:~/t1a_final.log

[Install]
WantedBy=default.target
EOF

systemctl --user daemon-reload
systemctl --user enable t1a.service
systemctl --user start t1a.service
```

### Control
- **Check Status**: `systemctl --user status t1a`
- **Restart Process**: `systemctl --user restart t1a`
- **Monitor Logs**: `tail -f ~/t1a_final.log`

## Daemon Features

- **Graceful Shutdown**: Catches termination signals to kill child MCP processes, preventing zombies.
- **Anti-Stuck Logic**: Detects and breaks infinite reasoning loops.
- **Heartbeat**: Periodic self-checks to ensure system stability.

## MCP Toolsets
T1a drives a suite of MCP servers (configured in `~/.noclaw/mcp.json`):
1. **Tavily**: Web research, crawling, and content extraction.
2. **Sequential Thinking**: Structured, reflective problem-solving.
3. **Memory Graph**: Knowledge graph-based long-term memory.

## Philosophy

- **Efficiency First**: No wasted cycles, no unnecessary allocations.
- **Robustness**: Designed to run for months without restarting. Handles malformed data and network instability gracefully.
- **Pragmatism**: Solves complex problems using standard syscalls and minimalist C.

---
*"Wong edan mah ajaib."*

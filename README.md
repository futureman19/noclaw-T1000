# T1a: The Autonomous Daemon

T1a is an ultra-lightweight, autonomous AI agent built in pure C11. Based on the [noclaw](https://github.com/angristan/noclaw) architecture, it is designed for maximum efficiency with a near-zero resource footprint. T1a acts as a high-performance command unit, capable of running indefinitely as a background daemon to manage tasks, execute tools, and assist via Telegram, CLI, or HTTP gateway.

## Key Capabilities

- **Ultra-Efficient**: ~108KB binary, <5MB RAM usage. Zero external runtime dependencies.
- **Daemon-Ready (Always-On)**: Native integration with `systemd` for 24/7 autonomous operation. Features self-healing process management and automated resource cleanup.
- **Full MCP Support**: Native, robust Model Context Protocol (MCP) client. Supports persistent connections to servers like Tavily, Sequential Thinking, and Memory Graph.
- **Extensive Internal Toolset**: 13+ built-in tools for rapid, zero-dependency task execution.
- **Self-Managing Memory**: High-density flat-file memory system with automatic pruning and Memory Compaction to prevent context bloat.
- **Provider Resilience**: Primary provider with configurable fallback (e.g. OpenRouter -> OpenAI). Automatic switch on failure.
- **Robust JSON/HTTP**: Standard-compliant JSON-RPC layer with strict character escaping for maximum compatibility across LLM providers.

## Architecture

T1a utilizes a modular, function-pointer based architecture for extreme flexibility without bloat:

- **Core**: Event loop driven by non-blocking I/O.
- **Providers**: Native TLS support for OpenAI-compatible APIs (Llama 3.3, Gemini 2.0 Flash, etc.).
- **Tools**: Built-in shell, filesystem I/O, and native MCP orchestration.
- **Persistence**: Persistent memories stored at `workspace/memories.tsv` with auto-garbage collection.

## 🛠️ Toolsets

### Internal Tools (Native C)
1. **`shell`**: Execute system commands.
2. **`file_read` / `file_write`**: Direct filesystem access.
3. **`memory_store` / `memory_recall`**: Persistent context management.
4. **`get_time`**: Local system clock with timezone support (zero network dependency).
5. **`http_fetch`**: Direct URL fetching using internal HTTP client.
6. **`list_dir`**: Structured directory listing.
7. **`sys_info`**: System health, uptime, and resource monitoring.
8. **`calc`**: Precise math expression evaluator.
9. **`env_get`**: Safe environment variable retrieval.
10. **`base64`**: Encode/decode data.
11. **`hash`**: MD5/SHA256 checksum computation.

### MCP Toolsets
T1a drives a suite of MCP servers (configured in `~/.noclaw/mcp.json`):
1. **Tavily**: Web research, crawling, and content extraction.
2. **Sequential Thinking**: Structured, reflective problem-solving.
3. **Memory Graph**: Knowledge graph-based long-term memory.

## 🚀 Quick Start

### Build
```bash
make clean && make release
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

## Philosophy

- **Efficiency First**: No wasted cycles, no unnecessary allocations.
- **Robustness**: Designed to handle malformed data and network instability gracefully.
- **Pragmatism**: Solves complex problems using standard syscalls and minimalist C.

---
*"Wong edan mah ajaib."*

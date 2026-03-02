# T1a: The Autonomous Daemon

T1a is an ultra-lightweight, autonomous AI agent built in pure C11. Based on the `noclaw` architecture, it is designed for maximum efficiency with a near-zero resource footprint. T1a acts as a high-performance command unit, capable of running indefinitely as a background daemon to manage tasks, execute tools, and assist via CLI or HTTP gateway.

## Key Capabilities

- **Ultra-Efficient**: ~100KB binary, <5MB RAM usage. Zero external runtime dependencies.
- **Daemon-Ready**: Built for 24/7 operation with self-healing process management, signal handling (`SIGINT`/`SIGTERM`), and automated resource cleanup.
- **Full MCP Support**: Native, robust Model Context Protocol (MCP) client. Drives complex toolchains (Postgres, Filesystem, Search) with full handshake and timeout management.
- **Self-Managing Memory**: Implements a "flat-file" memory system with automatic pruning and trimming to prevent disk bloat over time.
- **Resilient Autonomy**: Features a sliding-window context buffer with memory compaction, preventing OOM crashes during long-running sessions.

## Architecture

T1a utilizes a modular, function-pointer based architecture for extreme flexibility without bloat:

- **Core**: Event loop driven by non-blocking I/O (via `select`).
- **Providers**: Native support for Anthropic (Claude 3.5 Sonnet) and OpenAI-compatible APIs.
- **Tools**: Built-in shell, filesystem I/O, and dynamic MCP proxying.
- **Memory**: Persistent flat-file backend with semantic-like keyword search and auto-garbage collection.

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

# Run as a background daemon (Telegram/Gateway mode)
./noclaw agent --channel telegram &
```

## Daemon Features

- **Graceful Shutdown**: Catches termination signals to kill child MCP processes, preventing zombies.
- **Anti-Stuck Logic**: Detects and breaks infinite reasoning loops.
- **Heartbeat**: Periodic self-checks to ensure system stability.

## Philosophy

- **Efficiency First**: No wasted cycles, no unnecessary allocations.
- **Robustness**: Designed to run for months without restarting.
- **Pragmatism**: Solves problems using standard syscalls and simple algorithms.

---
*"Wong edan mah ajaib."*

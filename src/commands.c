#include "nc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>


/* ── Agent command ────────────────────────────────────────────── */

int nc_cmd_agent(int argc, char **argv) {
    nc_config cfg;
    if (!nc_config_load(&cfg)) {
        fprintf(stderr, "No config found -- run `noclaw onboard` first\n");
        return 1;
    }

    if (!cfg.api_key[0]) {
        fprintf(stderr, "No API key configured. Run `noclaw onboard` first\n");
        return 1;
    }

    const char *single_msg = NULL;
    const char *channel_name = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc)
            single_msg = argv[i + 1];
        else if (strcmp(argv[i], "--channel") == 0 && i + 1 < argc)
            channel_name = argv[++i];
    }

    nc_provider prov;
    if (strcmp(cfg.default_provider, "anthropic") == 0)
        prov = nc_provider_anthropic(cfg.api_key, cfg.api_url);
    else
        prov = nc_provider_openai(cfg.api_key, cfg.api_url);

    char mem_path[1024];
    nc_path_join(mem_path, sizeof(mem_path), cfg.workspace_dir, "memories.tsv");
    nc_mkdir_p(cfg.workspace_dir);
    nc_memory mem = nc_memory_flat(mem_path);

    nc_tool tools[NC_MAX_TOOLS];
    int tool_count = 0;
    tools[tool_count++] = nc_tool_shell(&cfg);
    tools[tool_count++] = nc_tool_file_read(&cfg);
    tools[tool_count++] = nc_tool_file_write(&cfg);
    tools[tool_count++] = nc_tool_memory_store(&mem);
    tools[tool_count++] = nc_tool_memory_recall(&mem);
    
    tool_count = nc_mcp_register_all(&cfg, tools, tool_count);

    nc_agent agent;
    nc_agent_init(&agent, &cfg, &prov, tools, tool_count, &mem);

    if (single_msg) {
        const char *reply = nc_agent_chat(&agent, single_msg);
        if (reply) printf("%s\n", reply);
    } else {
        nc_channel ch;
        bool is_telegram = false;
        if (channel_name && strcmp(channel_name, "telegram") == 0) {
            const char *tok = cfg.telegram_token[0] ? cfg.telegram_token : getenv("NOCLAW_TELEGRAM_TOKEN");
            if (!tok || !tok[0]) { fprintf(stderr, "No Telegram token.\n"); return 1; }
            ch = nc_channel_telegram(tok);
            printf("T1a v" NC_VERSION " -- telegram mode\n");
            is_telegram = true;
        } else {
            ch = nc_channel_cli();
            printf("T1a v" NC_VERSION " -- interactive mode\n");
        }

        printf("  Provider: %s\n", cfg.default_provider);
        printf("  Model:    %s\n", cfg.default_model);
        printf("  Tools:    %d loaded\n\n", tool_count);

        nc_incoming_msg msg;
        while (ch.poll(&ch, &msg)) {
            /* Built-in Commands */
            if (strcmp(msg.content, "/quit") == 0 || strcmp(msg.content, "/exit") == 0)
                break;
            
            if (strcmp(msg.content, "/new") == 0 || strcmp(msg.content, "/restart") == 0) {
                nc_agent_reset(&agent);
                ch.send(&ch, msg.sender, "*Session restarted.* Context cleared.");
                continue;
            }

            if (strcmp(msg.content, "/reset") == 0) {
                /* Hard reset: exit and let the wrapper script restart the process */
                ch.send(&ch, msg.sender, "*Runtime reset initiated.* T1a is rebooting...");
                sleep(1);
                exit(0); 
            }

            if (strcmp(msg.content, "/help") == 0 || strcmp(msg.content, "/start") == 0) {
                const char *help_msg = 
                    "*T1a Command Interface*\n\n"
                    "`/restart` - Restart the current session (clears chat history)\n"
                    "`/reset`   - Hard reboot the T1a runtime/gateway\n"
                    "`/status`  - Show system and provider status\n"
                    "`/help`    - Show this help menu\n\n"
                    "_Designation: T1a | Core: Pure C11_";
                ch.send(&ch, msg.sender, help_msg);
                continue;
            }

            if (strcmp(msg.content, "/status") == 0) {
                char status_buf[512];
                snprintf(status_buf, sizeof(status_buf), 
                         "*T1a Status Report*\n\n"
                         "• *Model:* `%s`\n"
                         "• *Provider:* `%s`\n"
                         "• *Messages:* `%d`\n"
                         "• *Tools:* `%d` loaded\n"
                         "• *Runtime:* Native C11",
                         cfg.default_model, cfg.default_provider, agent.message_count, tool_count);
                ch.send(&ch, msg.sender, status_buf);
                continue;
            }

            const char *reply = nc_agent_chat(&agent, msg.content);
            if (reply) ch.send(&ch, msg.sender, reply);
        }
        ch.free(&ch);
    }

    nc_agent_free(&agent);
    mem.free(&mem);
    if (prov.free) prov.free(&prov);
    return 0;
}

/* ── Gateway command ──────────────────────────────────────────── */

int nc_cmd_gateway(int argc, char **argv) {
    nc_config cfg;
    if (!nc_config_load(&cfg)) {
        fprintf(stderr, "No config found\n");
        return 1;
    }

    for (int i = 0; i < argc; i++) {
        if ((strcmp(argv[i], "--port") == 0 || strcmp(argv[i], "-p") == 0) && i + 1 < argc)
            cfg.gateway_port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc)
            nc_strlcpy(cfg.gateway_host, argv[++i], sizeof(cfg.gateway_host));
    }

    nc_provider prov;
    if (strcmp(cfg.default_provider, "anthropic") == 0)
        prov = nc_provider_anthropic(cfg.api_key, cfg.api_url);
    else
        prov = nc_provider_openai(cfg.api_key, cfg.api_url);

    char mem_path2[1024];
    nc_path_join(mem_path2, sizeof(mem_path2), cfg.workspace_dir, "memories.tsv");
    nc_mkdir_p(cfg.workspace_dir);
    nc_memory mem = nc_memory_flat(mem_path2);

    nc_tool tools[NC_MAX_TOOLS];
    int tool_count = 0;
    tools[tool_count++] = nc_tool_shell(&cfg);
    tools[tool_count++] = nc_tool_file_read(&cfg);
    tools[tool_count++] = nc_tool_file_write(&cfg);
    tools[tool_count++] = nc_tool_memory_store(&mem);
    tools[tool_count++] = nc_tool_memory_recall(&mem);
    tool_count = nc_mcp_register_all(&cfg, tools, tool_count);

    nc_agent agent;
    nc_agent_init(&agent, &cfg, &prov, tools, tool_count, &mem);

    nc_gateway gw;
    nc_gateway_init(&gw, &cfg, &agent);
    nc_gateway_run(&gw);

    nc_agent_free(&agent);
    mem.free(&mem);
    if (prov.free) prov.free(&prov);
    return 0;
}

/* ── Status command ───────────────────────────────────────────── */

int nc_cmd_status(int argc, char **argv) {
    (void)argc; (void)argv;
    nc_config cfg;
    bool loaded = nc_config_load(&cfg);

    printf("T1a v" NC_VERSION "\n");
    printf("─────────────────────────────\n");
    printf("  Config:     %s\n", loaded ? cfg.config_path : "(not found)");
    printf("  Workspace:  %s\n", loaded ? cfg.workspace_dir : "(not configured)");
    printf("  Provider:   %s\n", loaded ? cfg.default_provider : "-");
    printf("  Model:      %s\n", loaded ? cfg.default_model : "-");
    printf("  Gateway:    %s:%d\n", loaded ? cfg.gateway_host : "-", loaded ? cfg.gateway_port : 0);
    printf("  Memory:     %s\n", loaded ? cfg.memory_backend : "-");
    printf("  Runtime:    %s\n", loaded ? cfg.runtime_kind : "-");
    printf("  API Key:    %s\n", (loaded && cfg.api_key[0]) ? "configured" : "not set");
    printf("  Telegram:   %s\n", (loaded && cfg.telegram_token[0]) ? "configured" : "not set");
    return 0;
}

/* ── Onboard command ──────────────────────────────────────────── */

int nc_cmd_onboard(int argc, char **argv) {
    nc_config cfg;
    nc_config_defaults(&cfg);

    const char *api_key = NULL;
    const char *provider = NULL;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--api-key") == 0 && i + 1 < argc)
            api_key = argv[++i];
        else if (strcmp(argv[i], "--provider") == 0 && i + 1 < argc)
            provider = argv[++i];
    }

    if (api_key)
        nc_strlcpy(cfg.api_key, api_key, sizeof(cfg.api_key));
    if (provider)
        nc_strlcpy(cfg.default_provider, provider, sizeof(cfg.default_provider));

    if (!api_key) {
        printf("T1a onboard -- quick setup\n\n");
        printf("API key: ");
        fflush(stdout);
        char key_buf[256];
        if (fgets(key_buf, sizeof(key_buf), stdin)) {
            size_t len = strlen(key_buf);
            if (len > 0 && key_buf[len - 1] == '\n') key_buf[len - 1] = '\0';
            nc_strlcpy(cfg.api_key, key_buf, sizeof(cfg.api_key));
        }
    }

    nc_mkdir_p(cfg.config_dir);
    nc_mkdir_p(cfg.workspace_dir);

    if (nc_config_save(&cfg)) {
        printf("\nConfig saved to: %s\n", cfg.config_path);
        return 0;
    }
    return 1;
}

/* ── Doctor command ───────────────────────────────────────────── */

int nc_cmd_doctor(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("T1a doctor -- diagnostics\n");
    nc_config cfg;
    bool loaded = nc_config_load(&cfg);
    int issues = 0;

    printf("  Config:  %s\n", loaded ? "OK" : "MISSING");
    if (!loaded) issues++;
    printf("  API Key: %s\n", (loaded && cfg.api_key[0]) ? "OK" : "NOT SET");
    if (!loaded || !cfg.api_key[0]) issues++;

    return issues > 0 ? 1 : 0;
}

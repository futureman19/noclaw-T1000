/*
 * Agent loop: manages conversation history, dispatches tool calls,
 * and drives the provider in a loop until the LLM produces a final response.
 */

#include "nc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Init / Free ──────────────────────────────────────────────── */

static void load_sys_prompt(nc_agent *agent, char *buf, size_t cap) {
    char soul_path[1024], user_path[1024], ident_path[1024];
    nc_path_join(soul_path, sizeof(soul_path), agent->config->config_dir, "SOUL.md");
    nc_path_join(user_path, sizeof(user_path), agent->config->config_dir, "USER.md");
    nc_path_join(ident_path, sizeof(ident_path), agent->config->config_dir, "IDENTITY.md");

    size_t s_len, u_len, i_len;
    char *soul = nc_read_file(soul_path, &s_len);
    char *user = nc_read_file(user_path, &u_len);
    char *ident = nc_read_file(ident_path, &i_len);

    snprintf(buf, cap, 
             "SYSTEM ARCHITECTURE:\n%s\n\nSOUL:\n%s\n\nUSER CONTEXT:\n%s\n\n"
             "OPERATIONAL DIRECTIVE:\n"
             "1. You are a fully autonomous agent. Never explain HOW you will use tools; just CALL them.\n"
             "2. If a task requires external information (news, web), call 'tavily_search' immediately.\n"
             "3. Think deeply before you act. NEVER loop the same tool call with the same arguments.\n"
             "4. If a tool call returns empty or irrelevant data, provide a concise answer based on what you already know. STOP searching if you have called search tools more than 2 times.\n"
             "5. Your responses must be final results, not plans or meta-commentary about your functions.\n"
             "6. Brevity is mandatory. Zero fluff.",
             ident ? ident : "Minimalist C agent.",
             soul ? soul : "Helpful assistant.",
             user ? user : "Unknown user.");

    if (soul) free(soul);
    if (user) free(user);
    if (ident) free(ident);
}

void nc_agent_init(nc_agent *agent, nc_config *cfg, nc_provider *prov,
                   nc_tool *tools, int tool_count, nc_memory *mem) {
    memset(agent, 0, sizeof(*agent));
    agent->config = cfg;
    agent->provider = prov;
    agent->tools = tools;
    agent->tool_count = tool_count;
    agent->memory = mem;

    /* Increased arena for better reasoning on small SBCs */
    nc_arena_init(&agent->arena, 256 * 1024);

    char prompt_buf[12288];
    load_sys_prompt(agent, prompt_buf, sizeof(prompt_buf));

    agent->messages[0] = (nc_message){
        .role = nc_arena_dup(&agent->arena, "system", 6),
        .content = nc_arena_dup(&agent->arena, prompt_buf, strlen(prompt_buf)),
    };
    agent->message_count = 1;
}

/* ── Build tools JSON for the provider ────────────────────────── */

static const char *build_tools_json(nc_agent *agent) {
    if (agent->tool_count == 0) return NULL;

    /* 64KB for tool definitions */
    static const size_t bufsz = 65536;
    char *buf = (char *)nc_arena_alloc(&agent->arena, bufsz);
    if (!buf) return NULL;
    int off = 0;
    off += snprintf(buf + off, bufsz - (size_t)off, "[");

    for (int i = 0; i < agent->tool_count; i++) {
        if (i > 0) off += snprintf(buf + off, bufsz - (size_t)off, ",");
        off += snprintf(buf + off, bufsz - (size_t)off,
            "{\"type\":\"function\",\"function\":{\"name\":\"%s\",\"description\":\"%s\",\"parameters\":%s}}",
            agent->tools[i].def.name,
            agent->tools[i].def.description,
            agent->tools[i].def.parameters_json);
    }

    off += snprintf(buf + off, bufsz - (size_t)off, "]");
    return buf;
}

/* ── Memory Compaction ────────────────────────────────────────── */

/* Compacts the agent's arena by creating a new one and copying only active messages. */
static void agent_compact_memory(nc_agent *agent) {
    nc_log(NC_LOG_INFO, "Compacting memory arena...");
    
    nc_arena new_arena;
    nc_arena_init(&new_arena, 256 * 1024);

    /* Rebuild messages array in the new arena */
    for (int i = 0; i < agent->message_count; i++) {
        nc_message *msg = &agent->messages[i];
        
        msg->role = nc_arena_dup(&new_arena, msg->role, strlen(msg->role));
        if (msg->content)
            msg->content = nc_arena_dup(&new_arena, msg->content, strlen(msg->content));
        
        if (msg->tool_call_id)
            msg->tool_call_id = nc_arena_dup(&new_arena, msg->tool_call_id, strlen(msg->tool_call_id));
            
        if (msg->tool_calls && msg->tool_call_count > 0) {
            nc_tool_call *new_tcs = (nc_tool_call *)nc_arena_alloc(&new_arena, 
                msg->tool_call_count * sizeof(nc_tool_call));
            if (new_tcs) {
                memcpy(new_tcs, msg->tool_calls, msg->tool_call_count * sizeof(nc_tool_call));
                msg->tool_calls = new_tcs;
            }
        }
    }

    /* Free old arena and swap */
    nc_arena_free(&agent->arena);
    agent->arena = new_arena;
}

/* ── Add message to history ───────────────────────────────────── */

static void agent_push_msg(nc_agent *agent, const char *role, const char *content,
                           const char *tool_call_id,
                           const nc_tool_call *tool_calls, int tool_call_count) {
    if (agent->message_count >= NC_MAX_MESSAGES) {
        int keep = NC_MAX_MESSAGES / 2;
        /* Shift messages to keep the last half + system prompt at [0] */
        memmove(&agent->messages[1], &agent->messages[agent->message_count - keep],
                (size_t)keep * sizeof(nc_message));
        agent->message_count = 1 + keep;
        
        /* Trigger garbage collection to free space from dropped messages */
        agent_compact_memory(agent);
    }

    nc_message *msg = &agent->messages[agent->message_count++];
    memset(msg, 0, sizeof(*msg));
    msg->role = nc_arena_dup(&agent->arena, role, strlen(role));
    msg->content = content ? nc_arena_dup(&agent->arena, content, strlen(content)) : NULL;
    msg->tool_call_id = tool_call_id
        ? nc_arena_dup(&agent->arena, tool_call_id, strlen(tool_call_id))
        : NULL;

    if (tool_calls && tool_call_count > 0) {
        msg->tool_calls = (nc_tool_call *)nc_arena_alloc(
            &agent->arena, (size_t)tool_call_count * sizeof(nc_tool_call));
        if (msg->tool_calls) {
            memcpy(msg->tool_calls, tool_calls, (size_t)tool_call_count * sizeof(nc_tool_call));
            msg->tool_call_count = tool_call_count;
        }
    }
}

static nc_tool *find_tool(nc_agent *agent, const char *name) {
    for (int i = 0; i < agent->tool_count; i++) {
        if (strcmp(agent->tools[i].def.name, name) == 0)
            return &agent->tools[i];
    }
    return NULL;
}

/* ── Chat ── */

const char *nc_agent_chat(nc_agent *agent, const char *user_input) {
    agent_push_msg(agent, "user", user_input, NULL, NULL, 0);

    const char *tools_json = build_tools_json(agent);
    /* Increased max iterations and added fallback logic */
    int max_iterations = 16;

    for (int iter = 0; iter < max_iterations; iter++) {
        /* Force final answer on last iteration by disabling tools */
        if (iter == max_iterations - 1) {
            agent_push_msg(agent, "system", "System: Maximum tool iteration limit reached. Please provide a final response based on the information gathered so far.", NULL, NULL, 0);
            tools_json = NULL; /* Explicitly disable tools for this request */
        }

        nc_chat_request req = {
            .messages = agent->messages,
            .message_count = agent->message_count,
            .model = agent->config->default_model,
            .temperature = agent->config->default_temperature,
            .tools_json = tools_json,
            .max_tokens = 8192,
        };

        nc_chat_response resp;
        if (!agent->provider->chat(agent->provider, &req, &resp)) {
            static const char err_msg[] = "error: communication failure with provider.";
            return nc_arena_dup(&agent->arena, err_msg, sizeof(err_msg) - 1);
        }

        if (!resp.has_tool_calls) {
            const char *reply = nc_arena_dup(&agent->arena, resp.content, strlen(resp.content));
            agent_push_msg(agent, "assistant", resp.content, NULL, NULL, 0);
            return reply;
        }

        nc_log(NC_LOG_INFO, "T1a executing %d tools (iteration %d)", resp.tool_call_count, iter + 1);

        agent_push_msg(agent, "assistant",
                       resp.content[0] ? resp.content : NULL,
                       NULL,
                       resp.tool_calls, resp.tool_call_count);

        for (int i = 0; i < resp.tool_call_count; i++) {
            nc_tool_call *tc = &resp.tool_calls[i];
            nc_tool *tool = find_tool(agent, tc->name);
            /* High-capacity buffer for raw tool outputs (Markdown/JSON) */
            size_t result_buf_size = 262144; /* 256KB */
            char *result_buf = malloc(result_buf_size);
            if (!result_buf) return "error: OOM in tool dispatch";

            if (tool) {
                bool ok = tool->execute(tool, tc->arguments, result_buf, result_buf_size);
                (void)ok;
            } else {
                snprintf(result_buf, result_buf_size, "error: unknown tool '%s'", tc->name);
            }

            agent_push_msg(agent, "tool", result_buf, tc->id, NULL, 0);
            free(result_buf);
        }
    }

    return "error: maximum autonomy iterations reached.";
}

void nc_agent_reset(nc_agent *agent) {
    char role_buf[32];
    char content_buf[16384];
    nc_strlcpy(role_buf, agent->messages[0].role, sizeof(role_buf));
    nc_strlcpy(content_buf, agent->messages[0].content, sizeof(content_buf));

    nc_arena_reset(&agent->arena);

    agent->messages[0] = (nc_message){
        .role = nc_arena_dup(&agent->arena, role_buf, strlen(role_buf)),
        .content = nc_arena_dup(&agent->arena, content_buf, strlen(content_buf)),
    };
    agent->message_count = 1;
}

void nc_agent_free(nc_agent *agent) {
    nc_arena_free(&agent->arena);
    agent->message_count = 0;
}

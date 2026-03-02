#include "nc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/select.h>
#include <signal.h>

/* ── Process & Transport ──────────────────────────────────────── */

typedef struct {
    char name[64];
    pid_t pid;
    int fd_in;  /* Write to server */
    int fd_out; /* Read from server */
    int id_counter;
    bool initialized;
    nc_arena arena; /* For server-lifetime allocations */
    
    /* Read buffering */
    char rb[8192];
    size_t rb_len;
} mcp_server;

/* Global registry of running servers */
#define MAX_MCP_SERVERS 8
static mcp_server *g_servers[MAX_MCP_SERVERS];
static int g_server_count = 0;

static mcp_server *mcp_server_alloc(const char *name) {
    if (g_server_count >= MAX_MCP_SERVERS) return NULL;
    mcp_server *s = calloc(1, sizeof(mcp_server));
    if (!s) return NULL;
    nc_strlcpy(s->name, name, sizeof(s->name));
    nc_arena_init(&s->arena, 64 * 1024);
    g_servers[g_server_count++] = s;
    return s;
}

static bool mcp_proc_start(mcp_server *s, const char *cmd, char *const argv[], char *const envp[]) {
    int in_pipe[2], out_pipe[2]; /* Parent writes to in_pipe[1], reads from out_pipe[0] */

    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0) return false;

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return false;
    }

    if (pid == 0) {
        /* Child */
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        /* Close unused FDs */
        close(in_pipe[1]);
        close(out_pipe[0]);
        /* Close all other FDs to be safe (3..1024) */
        for (int i = 3; i < 1024; i++) close(i);

        execve(cmd, argv, envp);
        perror("execve failed");
        _exit(1);
    }

    /* Parent */
    close(in_pipe[0]);
    close(out_pipe[1]);
    s->pid = pid;
    s->fd_in = in_pipe[1];
    s->fd_out = out_pipe[0];
    
    /* Ignore SIGPIPE to avoid crashing if server dies */
    signal(SIGPIPE, SIG_IGN);
    
    return true;
}

/* ── JSON-RPC ─────────────────────────────────────────────────── */

/* Read a single line (JSON-RPC message) from server.
   Handles buffering. Returns a string allocated in the arena. */
static char *mcp_read_msg(mcp_server *s, nc_arena *msg_arena) {
    char *line = NULL;
    size_t line_len = 0;
    size_t line_cap = 0;
    
    while (1) {
        /* Check if we have a newline in the buffer */
        char *nl = memchr(s->rb, '\n', s->rb_len);
        if (nl) {
            size_t len = nl - s->rb;
            /* Copy line to arena */
            size_t total_len = line_len + len;
            if (line_cap <= total_len) {
                size_t new_cap = total_len + 1024;
                char *new_line = nc_arena_alloc(msg_arena, new_cap);
                if (line) memcpy(new_line, line, line_len);
                line = new_line;
                line_cap = new_cap;
            }
            memcpy(line + line_len, s->rb, len);
            line[line_len + len] = '\0';
            
            /* Shift buffer */
            size_t remain = s->rb_len - (len + 1);
            memmove(s->rb, nl + 1, remain);
            s->rb_len = remain;
            
            return line;
        }

        /* No newline, consume all and read more */
        if (s->rb_len > 0) {
            size_t total_len = line_len + s->rb_len;
            if (line_cap <= total_len) {
                size_t new_cap = total_len + 4096;
                char *new_line = nc_arena_alloc(msg_arena, new_cap);
                if (line) memcpy(new_line, line, line_len);
                line = new_line;
                line_cap = new_cap;
            }
            memcpy(line + line_len, s->rb, s->rb_len);
            line_len += s->rb_len;
            s->rb_len = 0;
        }

        /* Read more */
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s->fd_out, &fds);
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 }; /* 5s timeout */

        int ret = select(s->fd_out + 1, &fds, NULL, NULL, &tv);
        if (ret <= 0) break; /* Timeout or error */

        ssize_t n = read(s->fd_out, s->rb, sizeof(s->rb));
        if (n <= 0) break; /* EOF or error */
        s->rb_len = n;
    }
    
    return line; /* Might be partial if EOF reached without newline */
}

static nc_json *mcp_rpc_call(mcp_server *s, const char *method, const char *params_json, nc_arena *arena) {
    int id = ++s->id_counter;
    char *req = nc_arena_alloc(arena, 8192 + (params_json ? strlen(params_json) : 0));
    
    if (params_json) {
        sprintf(req, "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s,\"id\":%d}\n", 
                 method, params_json, id);
    } else {
        sprintf(req, "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"id\":%d}\n", 
                 method, id);
    }

    /* Send */
    if (write(s->fd_in, req, strlen(req)) < 0) return NULL;
    /* nc_log(NC_LOG_DEBUG, "MCP -> %s: %s", s->name, req); */

    /* Read loop until we match ID */
    while (1) {
        char *line = mcp_read_msg(s, arena);
        if (!line) return NULL;
        
        /* nc_log(NC_LOG_DEBUG, "MCP <- %s: %s", s->name, line); */

        nc_json *root = nc_json_parse(arena, line, strlen(line));
        if (!root) continue;

        /* Check for matching ID */
        nc_json *jid = nc_json_get(root, "id");
        if (jid && jid->type == NC_JSON_NUMBER && (int)jid->number == id) {
            nc_json *err = nc_json_get(root, "error");
            if (err) {
                nc_str msg = nc_json_str(nc_json_get(err, "message"), "Unknown error");
                nc_log(NC_LOG_ERROR, "MCP Error from %s: %.*s", s->name, NC_STR_ARG(msg));
                return NULL;
            }
            return nc_json_get(root, "result");
        }
    }
}

static void mcp_rpc_notify(mcp_server *s, const char *method, const char *params_json) {
    char req[8192];
    if (params_json) {
        snprintf(req, sizeof(req), 
                 "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}\n", 
                 method, params_json);
    } else {
        snprintf(req, sizeof(req), 
                 "{\"jsonrpc\":\"2.0\",\"method\":\"%s\"}\n", 
                 method);
    }
    write(s->fd_in, req, strlen(req));
}

/* ── Handshake ────────────────────────────────────────────────── */

static bool mcp_handshake(mcp_server *s) {
    nc_arena a;
    nc_arena_init(&a, 32 * 1024);

    /* 1. Send initialize */
    const char *init_params = 
        "{\"protocolVersion\":\"2024-11-05\","
        "\"capabilities\":{\"roots\":{\"listChanged\":true}},"
        "\"clientInfo\":{\"name\":\"noclaw\",\"version\":\"" NC_VERSION "\"}}";
    
    nc_json *res = mcp_rpc_call(s, "initialize", init_params, &a);
    if (!res) {
        nc_log(NC_LOG_ERROR, "MCP handshake failed for %s", s->name);
        nc_arena_free(&a);
        return false;
    }

    /* 2. Send initialized notification */
    mcp_rpc_notify(s, "notifications/initialized", NULL);

    s->initialized = true;
    nc_log(NC_LOG_INFO, "MCP server '%s' initialized", s->name);
    
    nc_arena_free(&a);
    return true;
}

/* ── JSON Stringify Helper ────────────────────────────────────── */

static void json_stringify(nc_json *node, char *buf, size_t *off, size_t cap) {
    if (*off >= cap) return;
    int n;
    
    switch (node->type) {
        case NC_JSON_NULL: 
            n = snprintf(buf + *off, cap - *off, "null"); 
            if (n > 0) *off += n;
            break;
        case NC_JSON_BOOL: 
            n = snprintf(buf + *off, cap - *off, "%s", node->boolean ? "true" : "false"); 
            if (n > 0) *off += n;
            break;
        case NC_JSON_NUMBER: 
            n = snprintf(buf + *off, cap - *off, "%g", node->number); 
            if (n > 0) *off += n;
            break;
        case NC_JSON_STRING: 
            if (*off < cap) buf[(*off)++] = '"';
            for (size_t i = 0; i < node->string.len && *off < cap; i++) {
                char c = node->string.ptr[i];
                if (c == '"') n = snprintf(buf + *off, cap - *off, "\\\"");
                else if (c == '\\') n = snprintf(buf + *off, cap - *off, "\\\\");
                else if (c == '\n') n = snprintf(buf + *off, cap - *off, "\\n");
                else {
                     buf[(*off)++] = c; 
                     n = 0;
                }
                if (n > 0) *off += n;
            }
            if (*off < cap) buf[(*off)++] = '"';
            break;
        case NC_JSON_ARRAY:
            if (*off < cap) buf[(*off)++] = '[';
            for (int i = 0; i < node->array.count; i++) {
                if (i > 0 && *off < cap) buf[(*off)++] = ',';
                json_stringify(&node->array.items[i], buf, off, cap);
            }
            if (*off < cap) buf[(*off)++] = ']';
            break;
        case NC_JSON_OBJECT:
             if (*off < cap) buf[(*off)++] = '{';
             for (int i = 0; i < node->object.count; i++) {
                 if (i > 0 && *off < cap) buf[(*off)++] = ',';
                 n = snprintf(buf + *off, cap - *off, "\"%.*s\":", NC_STR_ARG(node->object.keys[i]));
                 if (n > 0) *off += n;
                 json_stringify(&node->object.vals[i], buf, off, cap);
             }
             if (*off < cap) buf[(*off)++] = '}';
             break;
    }
    if (*off < cap) buf[*off] = '\0';
}

/* ── Tool Execution ───────────────────────────────────────────── */

static bool mcp_tool_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    mcp_server *s = (mcp_server *)self->ctx;
    nc_arena a;
    nc_arena_init(&a, out_cap + 8192);

    /* Construct params for tools/call */
    /* Escape args_json if needed? No, it's already a JSON string representation of an object. */
    /* Wait, the params argument must be a JSON object string. */
    /* tools/call params: { name: "toolname", arguments: { ... } } */
    /* args_json comes from the LLM, it IS the arguments object string. */
    
    char *params = nc_arena_alloc(&a, strlen(args_json) + 1024);
    sprintf(params, "{\"name\":\"%s\",\"arguments\":%s}", self->def.name, args_json);

    nc_json *res = mcp_rpc_call(s, "tools/call", params, &a);
    if (!res) {
        snprintf(out, out_cap, "error: MCP call failed");
        nc_arena_free(&a);
        return false;
    }

    /* Result schema: { content: [{type: "text", text: "..."}] } */
    nc_json *content = nc_json_get(res, "content");
    out[0] = '\0';
    
    if (content && content->type == NC_JSON_ARRAY) {
        size_t off = 0;
        for (int i = 0; i < content->array.count; i++) {
            nc_json *item = &content->array.items[i];
            nc_str type = nc_json_str(nc_json_get(item, "type"), "");
            if (nc_str_eql(type, "text")) {
                nc_str text = nc_json_str(nc_json_get(item, "text"), "");
                size_t avail = out_cap - off - 1;
                size_t cplen = text.len < avail ? text.len : avail;
                memcpy(out + off, text.ptr, cplen);
                off += cplen;
                if (off >= out_cap - 1) break;
            }
        }
        out[off] = '\0';
    } else {
        snprintf(out, out_cap, "ok (empty result)");
    }

    bool is_error = nc_json_bool(nc_json_get(res, "isError"), false);
    
    nc_arena_free(&a);
    return !is_error;
}

/* ── Tool Discovery ───────────────────────────────────────────── */

static int mcp_discover_tools(mcp_server *s, nc_tool *tools, int count) {
    nc_arena a;
    nc_arena_init(&a, 64 * 1024);

    nc_json *res = mcp_rpc_call(s, "tools/list", NULL, &a);
    if (!res) {
        nc_arena_free(&a);
        return count;
    }

    nc_json *tools_arr = nc_json_get(res, "tools");
    if (tools_arr && tools_arr->type == NC_JSON_ARRAY) {
        for (int i = 0; i < tools_arr->array.count && count < NC_MAX_TOOLS; i++) {
            nc_json *t = &tools_arr->array.items[i];
            nc_str name = nc_json_str(nc_json_get(t, "name"), "");
            nc_str desc = nc_json_str(nc_json_get(t, "description"), "");
            nc_json *schema = nc_json_get(t, "inputSchema");

            if (name.len > 0) {
                /* Copy to server arena to persist */
                char *name_copy = nc_arena_dup(&s->arena, name.ptr, name.len);
                char *desc_copy = nc_arena_dup(&s->arena, desc.ptr, desc.len);
                
                /* Serialize schema */
                char *schema_json = nc_arena_alloc(&s->arena, 8192);
                size_t off = 0;
                if (schema) json_stringify(schema, schema_json, &off, 8192);
                else strcpy(schema_json, "{}");

                tools[count].def.name = name_copy;
                tools[count].def.description = desc_copy;
                tools[count].def.parameters_json = schema_json;
                tools[count].ctx = s;
                tools[count].execute = mcp_tool_execute;
                tools[count].free = NULL;

                nc_log(NC_LOG_INFO, "  + Registered MCP tool: %s", name_copy);
                count++;
            }
        }
    }

    nc_arena_free(&a);
    return count;
}

/* ── Public API ───────────────────────────────────────────────── */

int nc_mcp_register_all(const nc_config *cfg, nc_tool *tools, int start_idx) {
    char mcp_path[1024];
    nc_path_join(mcp_path, sizeof(mcp_path), cfg->config_dir, "mcp.json");
    
    size_t len;
    char *data = nc_read_file(mcp_path, &len);
    if (!data) return start_idx;

    nc_arena a;
    nc_arena_init(&a, len * 2 + 4096);
    nc_json *root = nc_json_parse(&a, data, len);
    if (!root) { free(data); nc_arena_free(&a); return start_idx; }

    nc_json *servers = nc_json_get(root, "mcpServers");
    if (!servers || servers->type != NC_JSON_OBJECT) { free(data); nc_arena_free(&a); return start_idx; }

    int count = start_idx;

    for (int i = 0; i < servers->object.count; i++) {
        nc_str sname = servers->object.keys[i];
        nc_json *scfg = &servers->object.vals[i];
        
        nc_str cmd_str = nc_json_str(nc_json_get(scfg, "command"), "");
        if (cmd_str.len == 0) continue;

        char name_buf[64];
        snprintf(name_buf, sizeof(name_buf), "%.*s", NC_STR_ARG(sname));

        mcp_server *s = mcp_server_alloc(name_buf);
        if (!s) { nc_log(NC_LOG_ERROR, "Max MCP servers reached"); break; }

        /* Prepare argv */
        char *argv[64];
        int argc = 0;
        argv[argc++] = nc_arena_dup(&s->arena, cmd_str.ptr, cmd_str.len);

        nc_json *args = nc_json_get(scfg, "args");
        if (args && args->type == NC_JSON_ARRAY) {
            for (int j = 0; j < args->array.count && argc < 63; j++) {
                nc_str arg = nc_json_str(&args->array.items[j], "");
                argv[argc++] = nc_arena_dup(&s->arena, arg.ptr, arg.len);
            }
        }
        argv[argc] = NULL;

        /* Prepare envp */
        char *envp[128];
        int envc = 0;
        
        envp[envc++] = "TERM=xterm-256color";
        
        const char *inherit[] = {"PATH", "HOME", "USER", "LANG", "SHELL", NULL};
        for (const char **k = inherit; *k; k++) {
            const char *v = getenv(*k);
            if (v && envc < 127) {
                char *entry = nc_arena_alloc(&s->arena, strlen(*k) + strlen(v) + 2);
                sprintf(entry, "%s=%s", *k, v);
                envp[envc++] = entry;
            }
        }

        nc_json *env_obj = nc_json_get(scfg, "env");
        if (env_obj && env_obj->type == NC_JSON_OBJECT) {
            for (int k = 0; k < env_obj->object.count && envc < 127; k++) {
                nc_str key = env_obj->object.keys[k];
                nc_str val = nc_json_str(&env_obj->object.vals[k], "");
                char *entry = nc_arena_alloc(&s->arena, key.len + val.len + 2);
                sprintf(entry, "%.*s=%.*s", NC_STR_ARG(key), NC_STR_ARG(val));
                envp[envc++] = entry;
            }
        }
        envp[envc] = NULL;

        nc_log(NC_LOG_INFO, "Starting MCP server '%s'...", s->name);
        if (mcp_proc_start(s, argv[0], argv, envp)) {
            if (mcp_handshake(s)) {
                count = mcp_discover_tools(s, tools, count);
            } else {
                nc_log(NC_LOG_ERROR, "MCP Handshake failed for %s", s->name);
                kill(s->pid, SIGTERM);
            }
        } else {
            nc_log(NC_LOG_ERROR, "Failed to spawn MCP server %s", s->name);
        }
    }

    free(data);
    nc_arena_free(&a);
    return count;
}

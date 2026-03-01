#include "nc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>

typedef struct {
    char name[64];
    char command[1024];
} mcp_server_ctx;

static bool mcp_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    mcp_server_ctx *mcp = (mcp_server_ctx *)self->ctx;
    nc_log(NC_LOG_INFO, "  -> calling MCP server '%s' with args: %s", mcp->name, args_json);
    
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0) {
        nc_strlcpy(out, "error: failed to create pipes", out_cap);
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        nc_strlcpy(out, "error: failed to fork", out_cap);
        return false;
    }

    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[1]);
        close(out_pipe[0]);
        
        execl("/bin/sh", "sh", "-c", mcp->command, (char *)NULL);
        _exit(1);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);

    char *request = malloc(strlen(args_json) + 1024);
    if (!request) {
        nc_strlcpy(out, "error: OOM for request", out_cap);
        return false;
    }
    
    /* FIX: Correctly structure JSON-RPC call with arguments */
    snprintf(request, strlen(args_json) + 1024, 
             "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"params\":{\"name\":\"%s\",\"arguments\":%s},\"id\":1}\n", 
             self->def.name, args_json);
    
    write(in_pipe[1], request, strlen(request));
    close(in_pipe[1]);
    free(request);

    char *raw_res = malloc(64 * 1024);
    if (!raw_res) {
        nc_strlcpy(out, "error: OOM for response", out_cap);
        close(out_pipe[0]);
        return false;
    }

    size_t total = 0;
    ssize_t n;
    while ((n = read(out_pipe[0], raw_res + total, (64 * 1024) - total - 1)) > 0) {
        total += (size_t)n;
        if (total >= (64 * 1024) - 1) break;
    }
    raw_res[total] = '\0';
    close(out_pipe[0]);
    waitpid(pid, NULL, 0);

    if (total == 0) {
        nc_strlcpy(out, "error: empty response from MCP server", out_cap);
        free(raw_res);
        return false;
    }

    nc_arena parse_arena;
    nc_arena_init(&parse_arena, total * 2 + 4096);
    nc_json *root = nc_json_parse(&parse_arena, raw_res, total);
    
    if (root) {
        nc_json *result = nc_json_get(root, "result");
        if (result) {
            nc_json *content = nc_json_get(result, "content");
            if (content && content->type == NC_JSON_ARRAY && content->array.count > 0) {
                size_t out_pos = 0;
                for (int i = 0; i < content->array.count; i++) {
                    nc_json *item = &content->array.items[i];
                    nc_str text = nc_json_str(nc_json_get(item, "text"), "");
                    if (text.len > 0) {
                        size_t to_copy = (text.len < out_cap - out_pos - 1) ? text.len : (out_cap - out_pos - 1);
                        memcpy(out + out_pos, text.ptr, to_copy);
                        out_pos += to_copy;
                    }
                }
                out[out_pos] = '\0';
                free(raw_res);
                nc_arena_free(&parse_arena);
                return true;
            }
        }
        
        nc_json *error = nc_json_get(root, "error");
        if (error) {
            nc_str msg = nc_json_str(nc_json_get(error, "message"), "unknown MCP error");
            snprintf(out, out_cap, "MCP Error: " NC_STR_FMT, NC_STR_ARG(msg));
            free(raw_res);
            nc_arena_free(&parse_arena);
            return false;
        }
    }

    nc_strlcpy(out, raw_res, out_cap);
    free(raw_res);
    nc_arena_free(&parse_arena);
    return true; 
}

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
    for (int i = 0; i < servers->object.count && count < NC_MAX_TOOLS; i++) {
        nc_str name = servers->object.keys[i];
        nc_json *s_cfg = &servers->object.vals[i];
        nc_str cmd = nc_json_str(nc_json_get(s_cfg, "command"), "");
        
        if (cmd.len > 0) {
            mcp_server_ctx *ctx = malloc(sizeof(mcp_server_ctx));
            nc_strlcpy(ctx->name, name.ptr, name.len + 1);
            
            nc_json *args = nc_json_get(s_cfg, "args");
            char full_cmd[2048];
            nc_strlcpy(full_cmd, cmd.ptr, cmd.len + 1);
            
            if (args && args->type == NC_JSON_ARRAY) {
                for (int j = 0; j < args->array.count; j++) {
                    nc_str arg = nc_json_str(&args->array.items[j], "");
                    if (arg.len > 0) {
                        strcat(full_cmd, " ");
                        strcat(full_cmd, arg.ptr);
                    }
                }
            }
            
            nc_json *env_obj = nc_json_get(s_cfg, "env");
            if (env_obj && env_obj->type == NC_JSON_OBJECT) {
                char env_prefix[1024] = "";
                for (int k = 0; k < env_obj->object.count; k++) {
                    nc_str key = env_obj->object.keys[k];
                    nc_str val = nc_json_str(&env_obj->object.vals[k], "");
                    char pair[256];
                    snprintf(pair, sizeof(pair), " " NC_STR_FMT "=" NC_STR_FMT, NC_STR_ARG(key), NC_STR_ARG(val));
                    strcat(env_prefix, pair);
                }
                char final_cmd[3072];
                snprintf(final_cmd, sizeof(final_cmd), "%s %s", env_prefix, full_cmd);
                nc_strlcpy(ctx->command, final_cmd, sizeof(ctx->command));
            } else {
                nc_strlcpy(ctx->command, full_cmd, sizeof(ctx->command));
            }

            /* FIX: Pass proper tool definitions for each proxy */
            tools[count].def.name = strdup(ctx->name);
            tools[count].def.description = "MCP Server Proxy";
            
            /* Give it a more flexible parameter schema for proxying */
            if (strcmp(ctx->name, "sequentialthinking") == 0) {
                tools[count].def.parameters_json = "{\"type\":\"object\",\"properties\":{\"thought\":{\"type\":\"string\"},\"thoughtNumber\":{\"type\":\"integer\"},\"totalThoughts\":{\"type\":\"integer\"},\"nextThoughtNeeded\":{\"type\":\"boolean\"}}}";
            } else if (strcmp(ctx->name, "tavily_remote_mcp") == 0) {
                tools[count].def.parameters_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}}}";
            } else {
                tools[count].def.parameters_json = "{\"type\":\"object\"}";
            }

            tools[count].ctx = ctx;
            tools[count].execute = mcp_execute;
            tools[count].free = NULL;
            count++;
        }
    }

    free(data);
    nc_arena_free(&a);
    return count;
}

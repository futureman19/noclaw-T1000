#include "nc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

typedef struct {
    char name[64];
    char command[1024];
} mcp_server_ctx;

static bool mcp_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    mcp_server_ctx *mcp = (mcp_server_ctx *)self->ctx;
    
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0) return false;

    pid_t pid = fork();
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[1]);
        close(out_pipe[0]);
        
        execl("/bin/sh", "sh", "-c", mcp->command, (char *)NULL);
        exit(1);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);

    char request[10240];
    snprintf(request, sizeof(request), 
             "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"params\":{\"name\":\"%s\",\"arguments\":%s},\"id\":1}\n", 
             self->def.name, args_json);
    
    write(in_pipe[1], request, strlen(request));
    close(in_pipe[1]);

    size_t total = 0;
    char buf[1024];
    ssize_t n;
    while ((n = read(out_pipe[0], buf, sizeof(buf))) > 0 && total < out_cap - 1) {
        memcpy(out + total, buf, (size_t)n);
        total += (size_t)n;
    }
    out[total] = '\0';
    close(out_pipe[0]);
    waitpid(pid, NULL, 0);

    return total > 0; 
}

int nc_mcp_register_all(const nc_config *cfg, nc_tool *tools, int start_idx) {
    char mcp_path[1024];
    nc_path_join(mcp_path, sizeof(mcp_path), cfg->config_dir, "mcp.json");
    
    size_t len;
    char *data = nc_read_file(mcp_path, &len);
    if (!data) return start_idx;

    nc_arena a;
    nc_arena_init(&a, len * 2 + 1024);
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
            
            /* Build full command with args if present */
            nc_json *args = nc_json_get(s_cfg, "args");
            char full_cmd[1024];
            nc_strlcpy(full_cmd, cmd.ptr, cmd.len + 1);
            
            if (args && args->type == NC_JSON_ARRAY) {
                for (int j = 0; j < args->array.count; j++) {
                    nc_str arg = nc_json_str(&args->array.items[j], "");
                    strcat(full_cmd, " ");
                    strcat(full_cmd, arg.ptr);
                }
            }
            nc_strlcpy(ctx->command, full_cmd, sizeof(ctx->command));

            tools[count].def.name = strdup(ctx->name);
            tools[count].def.description = "MCP Server Proxy";
            tools[count].def.parameters_json = "{\"type\":\"object\"}";
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

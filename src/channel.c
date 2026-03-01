#include "nc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* ── CLI channel ──────────────────────────────────────────────── */

static bool cli_poll(nc_channel *self, nc_incoming_msg *out) {
    (void)self;
    memset(out, 0, sizeof(*out));

    printf("you> ");
    fflush(stdout);

    if (!fgets(out->content, sizeof(out->content), stdin))
        return false;

    size_t len = strlen(out->content);
    if (len > 0 && out->content[len - 1] == '\n')
        out->content[len - 1] = '\0';

    if (out->content[0] == '\0')
        return false;

    nc_strlcpy(out->sender, "cli", sizeof(out->sender));
    nc_strlcpy(out->channel_name, "cli", sizeof(out->channel_name));
    return true;
}

static bool cli_send(nc_channel *self, const char *to, const char *text) {
    (void)self;
    (void)to;
    printf("noclaw> %s\n", text);
    return true;
}

static void cli_free(nc_channel *self) {
    (void)self;
}

nc_channel nc_channel_cli(void) {
    return (nc_channel){
        .name = "cli",
        .ctx  = NULL,
        .poll = cli_poll,
        .send = cli_send,
        .free = cli_free,
    };
}

/* ── Telegram channel (Bot API, long polling) ─────────────────── */

typedef struct {
    char token[256];
    long long last_update_id;
} tg_ctx;

static void tg_send_action(tg_ctx *ctx, const char *chat_id, const char *action) {
    char url[512];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendChatAction", ctx->token);
    
    char body[256];
    nc_jw w;
    nc_jw_init(&w, body, sizeof(body));
    nc_jw_obj_open(&w);
    nc_jw_raw(&w, "chat_id", chat_id);
    nc_jw_str(&w, "action", action);
    nc_jw_obj_close(&w);

    const char *headers[] = { "Content-Type: application/json" };
    nc_http_response resp;
    nc_http_post(url, body, w.len, headers, 1, &resp);
    nc_http_response_free(&resp);
}

static bool tg_poll(nc_channel *self, nc_incoming_msg *out) {
    tg_ctx *ctx = (tg_ctx *)self->ctx;
    memset(out, 0, sizeof(*out));

    char url[512];
    snprintf(url, sizeof(url),
        "https://api.telegram.org/bot%s/getUpdates?offset=%lld&timeout=30&allowed_updates=[\"message\"]",
        ctx->token, ctx->last_update_id + 1);

    nc_http_response resp;
    if (!nc_http_get(url, NULL, 0, &resp) || resp.status != 200) {
        nc_http_response_free(&resp);
        return false;
    }

    nc_arena a;
    nc_arena_init(&a, resp.body_len * 2 + 512);
    nc_json *root = nc_json_parse(&a, resp.body, resp.body_len);
    nc_http_response_free(&resp);

    if (!root || !nc_json_bool(nc_json_get(root, "ok"), false)) {
        nc_arena_free(&a);
        return false;
    }

    nc_json *result = nc_json_get(root, "result");
    if (!result || result->type != NC_JSON_ARRAY || result->array.count == 0) {
        nc_arena_free(&a);
        return false;
    }

    nc_json *update = &result->array.items[0];
    long long uid = (long long)nc_json_num(nc_json_get(update, "update_id"), 0);
    if (uid > 0) ctx->last_update_id = uid;

    nc_json *message = nc_json_get(update, "message");
    if (!message) {
        nc_arena_free(&a);
        return false;
    }

    nc_json *chat = nc_json_get(message, "chat");
    long long chat_id = (long long)nc_json_num(nc_json_get(chat, "id"), 0);
    nc_str text = nc_json_str(nc_json_get(message, "text"), "");

    if (text.len == 0) {
        nc_arena_free(&a);
        return false;
    }

    snprintf(out->sender, sizeof(out->sender), "%lld", chat_id);
    size_t cplen = text.len < sizeof(out->content) - 1 ? text.len : sizeof(out->content) - 1;
    memcpy(out->content, text.ptr, cplen);
    out->content[cplen] = '\0';
    nc_strlcpy(out->channel_name, "telegram", sizeof(out->channel_name));

    /* Send 'typing' action as soon as we get a message */
    tg_send_action(ctx, out->sender, "typing");

    nc_arena_free(&a);
    return true;
}

static bool tg_send(nc_channel *self, const char *to, const char *text) {
    tg_ctx *ctx = (tg_ctx *)self->ctx;

    char url[512];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", ctx->token);

    char body[8192];
    nc_jw w;
    nc_jw_init(&w, body, sizeof(body));
    nc_jw_obj_open(&w);
    nc_jw_raw(&w, "chat_id", to);
    nc_jw_str(&w, "text", text);
    nc_jw_str(&w, "parse_mode", "Markdown");
    nc_jw_obj_close(&w);

    const char *headers[] = { "Content-Type: application/json" };
    nc_http_response resp;
    bool ok = nc_http_post(url, body, w.len, headers, 1, &resp);
    int status = resp.status;
    if (!ok || status != 200)
        nc_log(NC_LOG_WARN, "Telegram sendMessage failed: HTTP %d", status);
    nc_http_response_free(&resp);
    return ok && status == 200;
}

static void tg_free(nc_channel *self) {
    free(self->ctx);
    self->ctx = NULL;
}

nc_channel nc_channel_telegram(const char *bot_token) {
    tg_ctx *ctx = (tg_ctx *)calloc(1, sizeof(tg_ctx));
    nc_strlcpy(ctx->token, bot_token, sizeof(ctx->token));
    return (nc_channel){
        .name = "telegram",
        .ctx  = ctx,
        .poll = tg_poll,
        .send = tg_send,
        .free = tg_free,
    };
}

/* ── Discord / Slack (Disabled to keep T1a minimalist) ────────── */

nc_channel nc_channel_discord(const char *bot_token) {
    (void)bot_token;
    nc_log(NC_LOG_ERROR, "Discord channel disabled in this build.");
    return (nc_channel){0};
}

nc_channel nc_channel_slack(const char *bot_token) {
    (void)bot_token;
    nc_log(NC_LOG_ERROR, "Slack channel disabled in this build.");
    return (nc_channel){0};
}

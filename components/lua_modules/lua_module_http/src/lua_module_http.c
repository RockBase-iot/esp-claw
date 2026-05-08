/*
 * SPDX-FileCopyrightText: 2026 RockBase IoT (Chengdu) CO., LTD.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal HTTP client Lua module.
 *
 * Lua API:
 *   http.request{url=..., method="GET"|"POST"|"PATCH"|..., body=..., headers={k=v},
 *                timeout_ms=3000, max_body_bytes=8192}
 *     -> table { ok=bool, status=int, body=string|nil, error=string|nil }
 *   http.get(url[, opts])         -- shortcut for {url=url, method="GET"}
 *   http.post(url, body[, opts])  -- shortcut for {url=url, method="POST", body=body}
 *
 * Designed for short LAN HTTP probes (NMMiner /probe, /alive, /api/...).
 * Body is capped to avoid heap blow-ups; on overflow ok=true, body is
 * truncated and result.truncated=true is set.
 */

#include "lua_module_http.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cap_lua.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "lauxlib.h"

#define LUA_MODULE_HTTP_DEFAULT_TIMEOUT_MS 3000
#define LUA_MODULE_HTTP_DEFAULT_BODY_CAP   8192
#define LUA_MODULE_HTTP_HARD_BODY_CAP      (64 * 1024)

static const char *TAG = "lua_http";

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool truncated;
} lua_http_resp_t;

static esp_err_t lua_http_event_handler(esp_http_client_event_t *event)
{
    lua_http_resp_t *resp;

    if (!event || event->user_data == NULL) {
        return ESP_OK;
    }
    resp = (lua_http_resp_t *)event->user_data;

    if (event->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    if (event->data_len <= 0 || event->data == NULL) {
        return ESP_OK;
    }
    if (resp->cap == 0) {
        resp->truncated = true;
        return ESP_OK;
    }

    size_t avail = (resp->len < resp->cap) ? (resp->cap - resp->len - 1) : 0;
    size_t to_copy = (size_t)event->data_len;
    if (to_copy > avail) {
        to_copy = avail;
        resp->truncated = true;
    }
    if (to_copy > 0 && resp->buf) {
        memcpy(resp->buf + resp->len, event->data, to_copy);
        resp->len += to_copy;
        resp->buf[resp->len] = '\0';
    } else if (to_copy == 0 && (size_t)event->data_len > 0) {
        resp->truncated = true;
    }
    return ESP_OK;
}

static const char *lua_http_method_name(esp_http_client_method_t m)
{
    switch (m) {
    case HTTP_METHOD_GET:    return "GET";
    case HTTP_METHOD_POST:   return "POST";
    case HTTP_METHOD_PATCH:  return "PATCH";
    case HTTP_METHOD_PUT:    return "PUT";
    case HTTP_METHOD_DELETE: return "DELETE";
    case HTTP_METHOD_HEAD:   return "HEAD";
    default:                 return "GET";
    }
}

static esp_http_client_method_t lua_http_parse_method(const char *s)
{
    if (!s || !s[0]) {
        return HTTP_METHOD_GET;
    }
    if (!strcasecmp(s, "GET"))    return HTTP_METHOD_GET;
    if (!strcasecmp(s, "POST"))   return HTTP_METHOD_POST;
    if (!strcasecmp(s, "PATCH"))  return HTTP_METHOD_PATCH;
    if (!strcasecmp(s, "PUT"))    return HTTP_METHOD_PUT;
    if (!strcasecmp(s, "DELETE")) return HTTP_METHOD_DELETE;
    if (!strcasecmp(s, "HEAD"))   return HTTP_METHOD_HEAD;
    return HTTP_METHOD_GET;
}

/* Read string field from a table at given index (positive). */
static const char *lua_http_opt_field_string(lua_State *L, int idx, const char *key,
                                             const char *defval, size_t *out_len)
{
    const char *s = defval;
    size_t len = (defval ? strlen(defval) : 0);
    lua_getfield(L, idx, key);
    if (lua_isstring(L, -1)) {
        s = lua_tolstring(L, -1, &len);
    }
    /* Note: caller must finish using s before this string is popped from
     * the Lua stack. We pop here and rely on the immediate copy by the
     * esp_http_client_set_* APIs. */
    lua_pop(L, 1);
    if (out_len) {
        *out_len = len;
    }
    return s;
}

static int lua_http_opt_field_int(lua_State *L, int idx, const char *key, int defval)
{
    int v = defval;
    lua_getfield(L, idx, key);
    if (lua_isnumber(L, -1)) {
        v = (int)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);
    return v;
}

static int lua_http_request_impl(lua_State *L, int opts_idx)
{
    /* opts_idx must reference a table holding the request options. */
    luaL_checktype(L, opts_idx, LUA_TTABLE);

    /* Pull simple scalar fields up front. */
    size_t url_len = 0;
    const char *url = lua_http_opt_field_string(L, opts_idx, "url", NULL, &url_len);
    if (!url || !url[0]) {
        lua_newtable(L);
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "ok");
        lua_pushstring(L, "missing url");
        lua_setfield(L, -2, "error");
        return 1;
    }
    /* Stash url in our own buffer because the original Lua string was popped. */
    char *url_dup = strdup(url);
    if (!url_dup) {
        lua_newtable(L);
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "ok");
        lua_pushstring(L, "out of memory");
        lua_setfield(L, -2, "error");
        return 1;
    }

    size_t method_len = 0;
    const char *method_str = lua_http_opt_field_string(L, opts_idx, "method", "GET", &method_len);
    esp_http_client_method_t method = lua_http_parse_method(method_str);

    int timeout_ms = lua_http_opt_field_int(L, opts_idx, "timeout_ms",
                                            LUA_MODULE_HTTP_DEFAULT_TIMEOUT_MS);
    if (timeout_ms < 100) {
        timeout_ms = 100;
    } else if (timeout_ms > 60000) {
        timeout_ms = 60000;
    }

    int body_cap_arg = lua_http_opt_field_int(L, opts_idx, "max_body_bytes",
                                              LUA_MODULE_HTTP_DEFAULT_BODY_CAP);
    if (body_cap_arg < 256) {
        body_cap_arg = 256;
    }
    if (body_cap_arg > LUA_MODULE_HTTP_HARD_BODY_CAP) {
        body_cap_arg = LUA_MODULE_HTTP_HARD_BODY_CAP;
    }

    /* Body string (kept on the Lua stack so the pointer stays valid). */
    const char *body = NULL;
    size_t body_len = 0;
    lua_getfield(L, opts_idx, "body");
    if (lua_isstring(L, -1)) {
        body = lua_tolstring(L, -1, &body_len);
    }
    /* Leave on top: we'll pop after esp_http_client_perform. */
    int body_stack_idx = lua_gettop(L);

    lua_http_resp_t resp = {
        .buf = malloc((size_t)body_cap_arg + 1),
        .len = 0,
        .cap = (size_t)body_cap_arg + 1,
        .truncated = false,
    };
    if (!resp.buf) {
        free(url_dup);
        lua_pop(L, 1); /* body */
        lua_newtable(L);
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "ok");
        lua_pushstring(L, "out of memory");
        lua_setfield(L, -2, "error");
        return 1;
    }
    resp.buf[0] = '\0';

    esp_http_client_config_t config = {
        .url = url_dup,
        .method = method,
        .timeout_ms = timeout_ms,
        .event_handler = lua_http_event_handler,
        .user_data = &resp,
        .disable_auto_redirect = false,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(resp.buf);
        free(url_dup);
        lua_pop(L, 1); /* body */
        lua_newtable(L);
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "ok");
        lua_pushstring(L, "esp_http_client_init failed");
        lua_setfield(L, -2, "error");
        return 1;
    }

    /* Apply caller-supplied headers. */
    lua_getfield(L, opts_idx, "headers");
    if (lua_istable(L, -1)) {
        int hdr_idx = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, hdr_idx) != 0) {
            if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
                esp_http_client_set_header(client,
                                           lua_tostring(L, -2),
                                           lua_tostring(L, -1));
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1); /* headers */

    if (body && body_len > 0) {
        esp_http_client_set_post_field(client, body, (int)body_len);
        if (method == HTTP_METHOD_POST || method == HTTP_METHOD_PATCH || method == HTTP_METHOD_PUT) {
            /* Default content-type if user did not provide one. */
            esp_http_client_set_header(client, "Content-Type", "application/json");
        }
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    /* body string (Lua-managed) no longer needed. */
    lua_pop(L, 1);
    (void)body_stack_idx;
    free(url_dup);

    lua_newtable(L);
    lua_pushboolean(L, err == ESP_OK);
    lua_setfield(L, -2, "ok");
    lua_pushinteger(L, status);
    lua_setfield(L, -2, "status");
    lua_pushstring(L, lua_http_method_name(method));
    lua_setfield(L, -2, "method");
    if (resp.len > 0) {
        lua_pushlstring(L, resp.buf, resp.len);
        lua_setfield(L, -2, "body");
    }
    if (resp.truncated) {
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, "truncated");
    }
    if (err != ESP_OK) {
        lua_pushstring(L, esp_err_to_name(err));
        lua_setfield(L, -2, "error");
    }
    free(resp.buf);

    if (err != ESP_OK) {
        ESP_LOGD(TAG, "request failed: %s", esp_err_to_name(err));
    }
    return 1;
}

static int lua_http_request(lua_State *L)
{
    return lua_http_request_impl(L, 1);
}

/* http.get(url[, opts]) -> result */
static int lua_http_get(lua_State *L)
{
    const char *url = luaL_checkstring(L, 1);

    /* Build a temporary opts table from positional args. */
    lua_newtable(L);
    int t = lua_gettop(L);
    if (lua_istable(L, 2)) {
        /* Shallow-copy fields from caller opts into our table. */
        lua_pushnil(L);
        while (lua_next(L, 2) != 0) {
            lua_pushvalue(L, -2);
            lua_insert(L, -2);
            lua_settable(L, t);
        }
    }
    lua_pushstring(L, url);
    lua_setfield(L, t, "url");
    lua_pushstring(L, "GET");
    lua_setfield(L, t, "method");
    return lua_http_request_impl(L, t);
}

/* http.post(url, body[, opts]) -> result */
static int lua_http_post(lua_State *L)
{
    const char *url = luaL_checkstring(L, 1);
    size_t blen = 0;
    const char *body = luaL_optlstring(L, 2, "", &blen);

    lua_newtable(L);
    int t = lua_gettop(L);
    if (lua_istable(L, 3)) {
        lua_pushnil(L);
        while (lua_next(L, 3) != 0) {
            lua_pushvalue(L, -2);
            lua_insert(L, -2);
            lua_settable(L, t);
        }
    }
    lua_pushstring(L, url);
    lua_setfield(L, t, "url");
    lua_pushstring(L, "POST");
    lua_setfield(L, t, "method");
    lua_pushlstring(L, body, blen);
    lua_setfield(L, t, "body");
    return lua_http_request_impl(L, t);
}

int luaopen_http(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        { "request", lua_http_request },
        { "get",     lua_http_get     },
        { "post",    lua_http_post    },
        { NULL, NULL },
    };

    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    return 1;
}

esp_err_t lua_module_http_register(void)
{
    return cap_lua_register_module("http", luaopen_http);
}

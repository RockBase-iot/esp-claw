/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_skill.h"
#include "cap_skill_mgr.h"

static const char *CAP_SKILL_ACTIVATE = "activate_skill";
static const char *CAP_SKILL_DEACTIVATE = "deactivate_skill";
static const char *CAP_SKILL_LIST = "list_skill";
static const char *CAP_SKILL_REGISTER = "register_skill";
static const char *CAP_SKILL_UNREGISTER = "unregister_skill";

#define CAP_SKILL_MAX_CATALOG_LEN 16384
#define CAP_SKILL_MAX_PATH_LEN    128

static const char *cap_skill_root_dir(void)
{
    return claw_skill_get_skills_root_dir();
}

static void cap_skill_free_string_array(char **items, size_t count)
{
    size_t i;

    if (!items) {
        return;
    }

    for (i = 0; i < count; i++) {
        free(items[i]);
    }
    free(items);
}

static bool cap_skill_cap_group_exists(cJSON *cap_groups, const char *value)
{
    cJSON *item = NULL;

    if (!cap_groups || !value) {
        return false;
    }
    cJSON_ArrayForEach(item, cap_groups) {
        if (cJSON_IsString(item) && item->valuestring && strcmp(item->valuestring, value) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t cap_skill_copy_cap_groups(cJSON *dst, cJSON *src)
{
    cJSON *item = NULL;

    if (!dst) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!src) {
        return ESP_OK;
    }
    if (!cJSON_IsArray(src)) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON_ArrayForEach(item, src) {
        cJSON *copy = NULL;
        if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0]) {
            return ESP_ERR_INVALID_ARG;
        }
        if (cap_skill_cap_group_exists(dst, item->valuestring)) {
            continue;
        }
        copy = cJSON_CreateString(item->valuestring);
        if (!copy) {
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(dst, copy);
    }
    return ESP_OK;
}

static esp_err_t cap_skill_sync_session_visible_groups(const char *session_id)
{
    char **group_ids = NULL;
    size_t group_count = 0;
    esp_err_t err = ESP_OK;

    if (!session_id || !session_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    err = claw_skill_load_active_cap_groups(session_id, &group_ids, &group_count);
    if (err == ESP_ERR_NOT_FOUND) {
        return claw_cap_set_session_llm_visible_groups(session_id, NULL, 0);
    }
    if (err != ESP_OK) {
        return err;
    }

    err = claw_cap_set_session_llm_visible_groups(session_id,
                                                  (const char *const *)group_ids,
                                                  group_count);
    cap_skill_free_string_array(group_ids, group_count);
    return err;
}

static esp_err_t cap_skill_build_result(const char *action,
                                        const char *session_id,
                                        const char *skill_id,
                                        cJSON *skill_ids,
                                        bool all,
                                        char *output,
                                        size_t output_size)
{
    char **active_skill_ids = NULL;
    size_t active_skill_count = 0;
    cJSON *root = NULL;
    cJSON *active = NULL;
    cJSON *requested = NULL;
    char *rendered = NULL;
    esp_err_t err;
    size_t i;

    if (!action || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    err = claw_skill_load_active_skill_ids(session_id, &active_skill_ids, &active_skill_count);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        return err;
    }

    root = cJSON_CreateObject();
    active = cJSON_CreateArray();
    if (!root || !active) {
        cJSON_Delete(root);
        cJSON_Delete(active);
        cap_skill_free_string_array(active_skill_ids, active_skill_count);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "action", action);
    cJSON_AddStringToObject(root, "session_id", session_id ? session_id : "");
    if (skill_id) {
        cJSON_AddStringToObject(root, "skill_id", skill_id);
    }
    if (skill_ids) {
        requested = cJSON_Duplicate(skill_ids, true);
        if (!requested) {
            cJSON_Delete(root);
            cap_skill_free_string_array(active_skill_ids, active_skill_count);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToObject(root, "skill_ids", requested);
    }
    if (all) {
        cJSON_AddBoolToObject(root, "all", true);
    }
    cJSON_AddBoolToObject(root, "ok", true);
    for (i = 0; i < active_skill_count; i++) {
        cJSON_AddItemToArray(active, cJSON_CreateString(active_skill_ids[i]));
    }
    cJSON_AddItemToObject(root, "active_skill_ids", active);

    rendered = cJSON_PrintUnformatted(root);
    if (!rendered) {
        cJSON_Delete(root);
        cap_skill_free_string_array(active_skill_ids, active_skill_count);
        return ESP_ERR_NO_MEM;
    }

    snprintf(output, output_size, "%s", rendered);
    free(rendered);
    cJSON_Delete(root);
    cap_skill_free_string_array(active_skill_ids, active_skill_count);
    return ESP_OK;
}

static void cap_skill_write_error(char *output,
                                  size_t output_size,
                                  const char *error,
                                  const char *skill_id)
{
    cJSON *root = NULL;
    char *rendered = NULL;

    if (!output || output_size == 0) {
        return;
    }

    root = cJSON_CreateObject();
    if (!root) {
        snprintf(output,
                 output_size,
                 "{\"ok\":false,\"error\":\"%s\"}",
                 error ? error : "unknown error");
        return;
    }

    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", error ? error : "unknown error");
    if (skill_id && skill_id[0]) {
        cJSON_AddStringToObject(root, "skill_id", skill_id);
    }

    rendered = cJSON_PrintUnformatted(root);
    if (rendered) {
        snprintf(output, output_size, "%s", rendered);
        free(rendered);
    } else {
        snprintf(output,
                 output_size,
                 "{\"ok\":false,\"error\":\"%s\"}",
                 error ? error : "unknown error");
    }
    cJSON_Delete(root);
}

static esp_err_t cap_skill_read_file_dup(const char *path, char **out_text)
{
    FILE *file = NULL;
    long size;
    char *text = NULL;
    size_t read_bytes;

    if (!path || !out_text) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_text = NULL;

    file = fopen(path, "rb");
    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    size = ftell(file);
    if (size < 0 || size > CAP_SKILL_MAX_CATALOG_LEN) {
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return ESP_FAIL;
    }

    text = calloc(1, (size_t)size + 1);
    if (!text) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    read_bytes = fread(text, 1, (size_t)size, file);
    fclose(file);
    text[read_bytes] = '\0';
    *out_text = text;
    return ESP_OK;
}

static esp_err_t cap_skill_write_file_text(const char *path, const char *text)
{
    FILE *file = NULL;

    if (!path || !text) {
        return ESP_ERR_INVALID_ARG;
    }

    file = fopen(path, "wb");
    if (!file) {
        return ESP_FAIL;
    }
    if (fputs(text, file) < 0) {
        fclose(file);
        return ESP_FAIL;
    }
    fclose(file);
    return ESP_OK;
}

static bool cap_skill_path_is_valid(const char *skill_id, const char *path)
{
    char expected[CAP_SKILL_MAX_PATH_LEN];

    if (!skill_id || !skill_id[0] || !path || !path[0]) {
        return false;
    }
    if (path[0] == '/' || strstr(path, "..") != NULL || strchr(path, '\\') != NULL || strchr(skill_id, '/') || strchr(skill_id, '\\')) {
        return false;
    }
    if (snprintf(expected, sizeof(expected), "%s/SKILL.md", skill_id) >= (int)sizeof(expected)) {
        return false;
    }
    return strcmp(path, expected) == 0;
}

static bool cap_skill_file_exists(const char *path)
{
    struct stat st = {0};

    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static esp_err_t cap_skill_ensure_parent_dirs(const char *path)
{
    char dir_path[CAP_SKILL_MAX_PATH_LEN];
    size_t i;

    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (snprintf(dir_path, sizeof(dir_path), "%s", path) >= (int)sizeof(dir_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (i = 1; dir_path[i] != '\0'; i++) {
        if (dir_path[i] != '/') {
            continue;
        }
        dir_path[i] = '\0';
        if (mkdir(dir_path, 0755) != 0 && errno != EEXIST) {
            return ESP_FAIL;
        }
        dir_path[i] = '/';
    }

    return ESP_OK;
}

static esp_err_t cap_skill_load_catalog_json(char **out_text, cJSON **out_catalog)
{
    char *catalog_text = NULL;
    cJSON *catalog = NULL;
    esp_err_t err;

    if (!out_text || !out_catalog) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_text = NULL;
    *out_catalog = NULL;

    catalog_text = calloc(1, CAP_SKILL_MAX_CATALOG_LEN);
    if (!catalog_text) {
        return ESP_ERR_NO_MEM;
    }

    err = claw_skill_render_catalog_json(catalog_text, CAP_SKILL_MAX_CATALOG_LEN);
    if (err != ESP_OK) {
        free(catalog_text);
        return err;
    }

    catalog = cJSON_Parse(catalog_text);
    if (!catalog || !cJSON_IsObject(catalog)) {
        cJSON_Delete(catalog);
        free(catalog_text);
        return ESP_ERR_INVALID_STATE;
    }

    *out_text = catalog_text;
    *out_catalog = catalog;
    return ESP_OK;
}

static esp_err_t cap_skill_build_runtime_markdown(const char *skill_id,
                                                  const char *summary,
                                                  cJSON *cap_groups_input,
                                                  char **out_text)
{
    cJSON *meta = NULL;
    cJSON *metadata = NULL;
    cJSON *cap_groups = NULL;
    char *meta_text = NULL;
    char *markdown = NULL;

    if (!skill_id || !skill_id[0] || !summary || !summary[0] || !out_text) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_text = NULL;

    meta = cJSON_CreateObject();
    metadata = cJSON_CreateObject();
    cap_groups = cJSON_CreateArray();
    if (!meta || !metadata || !cap_groups) {
        cJSON_Delete(meta);
        cJSON_Delete(metadata);
        cJSON_Delete(cap_groups);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(meta, "name", skill_id);
    cJSON_AddStringToObject(meta, "description", summary);
    if (cap_skill_copy_cap_groups(cap_groups, cap_groups_input) != ESP_OK) {
        cJSON_Delete(meta);
        cJSON_Delete(metadata);
        cJSON_Delete(cap_groups);
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_AddItemToObject(metadata, "cap_groups", cap_groups);
    cJSON_AddStringToObject(metadata, "manage_mode", "runtime");
    cJSON_AddItemToObject(meta, "metadata", metadata);
    meta_text = cJSON_PrintUnformatted(meta);
    cJSON_Delete(meta);
    if (!meta_text) {
        return ESP_ERR_NO_MEM;
    }

    markdown = malloc(strlen(meta_text) + strlen(skill_id) + strlen(summary) + 96);
    if (!markdown) {
        free(meta_text);
        return ESP_ERR_NO_MEM;
    }
    snprintf(markdown,
             strlen(meta_text) + strlen(skill_id) + strlen(summary) + 96,
             "---\n%s\n---\n\n# %s\n\n%s\n",
             meta_text,
             skill_id,
             summary);
    free(meta_text);
    *out_text = markdown;
    return ESP_OK;
}

static esp_err_t cap_skill_build_catalog_result(const char *action,
                                                cJSON *skill,
                                                const char *skill_id,
                                                char *output,
                                                size_t output_size)
{
    cJSON *root = NULL;
    cJSON *catalog = NULL;
    cJSON *skills = NULL;
    char *catalog_text = NULL;
    char *rendered = NULL;
    esp_err_t err;

    if (!action || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_skill_load_catalog_json(&catalog_text, &catalog);
    if (err != ESP_OK) {
        return err;
    }
    free(catalog_text);

    skills = cJSON_DetachItemFromObjectCaseSensitive(catalog, "skills");
    cJSON_Delete(catalog);
    if (!cJSON_IsArray(skills)) {
        cJSON_Delete(skills);
        return ESP_ERR_INVALID_STATE;
    }

    root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(skills);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "action", action);
    if (skill) {
        cJSON_AddItemToObject(root, "skill", skill);
    } else if (skill_id && skill_id[0]) {
        cJSON_AddStringToObject(root, "skill_id", skill_id);
    }
    cJSON_AddItemToObject(root, "skills", skills);

    rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) {
        return ESP_ERR_NO_MEM;
    }

    snprintf(output, output_size, "%s", rendered);
    free(rendered);
    return ESP_OK;
}

static esp_err_t cap_skill_activate_execute(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char *output,
                                            size_t output_size)
{
    cJSON *root = NULL;
    cJSON *skill_ids_item = NULL;
    cJSON *skill_item = NULL;
    char activated_skill_id[64] = {0};
    esp_err_t err;

    if (!ctx || !ctx->session_id || !ctx->session_id[0] || !output || output_size == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"invalid input json\"}");
        return ESP_ERR_INVALID_ARG;
    }
    skill_ids_item = cJSON_GetObjectItemCaseSensitive(root, "skill_ids");

    if (!cJSON_IsArray(skill_ids_item) || cJSON_GetArraySize(skill_ids_item) <= 0) {
        cJSON_Delete(root);
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"skill_ids is required\"}");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON_ArrayForEach(skill_item, skill_ids_item) {
        if (!cJSON_IsString(skill_item) || !skill_item->valuestring || !skill_item->valuestring[0]) {
            cJSON_Delete(root);
            snprintf(output, output_size, "{\"ok\":false,\"error\":\"skill_ids must contain non-empty strings\"}");
            return ESP_ERR_INVALID_ARG;
        }

        snprintf(activated_skill_id, sizeof(activated_skill_id), "%s", skill_item->valuestring);
        err = claw_skill_activate_for_session(ctx->session_id, activated_skill_id);
        if (err != ESP_OK) {
            cJSON_Delete(root);
            snprintf(output,
                     output_size,
                     "{\"ok\":false,\"error\":\"failed to activate skill\",\"skill_id\":\"%s\"}",
                     activated_skill_id);
            return err;
        }
    }

    err = cap_skill_sync_session_visible_groups(ctx->session_id);
    if (err != ESP_OK) {
        snprintf(output,
                 output_size,
                 "{\"ok\":false,\"error\":\"failed to sync capability visibility\",\"skill_id\":\"%s\"}",
                 activated_skill_id);
        cJSON_Delete(root);
        return err;
    }

    err = cap_skill_build_result(CAP_SKILL_ACTIVATE,
                                 ctx->session_id,
                                 NULL,
                                 skill_ids_item,
                                 false,
                                 output,
                                 output_size);
    cJSON_Delete(root);
    return err;
}

static esp_err_t cap_skill_deactivate_execute(const char *input_json,
                                              const claw_cap_call_context_t *ctx,
                                              char *output,
                                              size_t output_size)
{
    cJSON *root = NULL;
    cJSON *skill_ids_item = NULL;
    cJSON *all_item = NULL;
    cJSON *skill_item = NULL;
    char target_skill_id[64] = {0};
    bool all_requested = false;
    esp_err_t err = ESP_OK;

    if (!ctx || !ctx->session_id || !ctx->session_id[0] || !output || output_size == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"invalid input json\"}");
        return ESP_ERR_INVALID_ARG;
    }
    skill_ids_item = cJSON_GetObjectItemCaseSensitive(root, "skill_ids");
    all_item = cJSON_GetObjectItemCaseSensitive(root, "all");
    all_requested = cJSON_IsBool(all_item) && cJSON_IsTrue(all_item);

    if (cJSON_IsArray(skill_ids_item) && cJSON_GetArraySize(skill_ids_item) > 0) {
        char guard_reason[256] = {0};

        cJSON_ArrayForEach(skill_item, skill_ids_item) {
            char current_skill_id[64] = {0};

            if (!cJSON_IsString(skill_item) || !skill_item->valuestring || !skill_item->valuestring[0]) {
                cJSON_Delete(root);
                snprintf(output, output_size, "{\"ok\":false,\"error\":\"skill_ids must contain non-empty strings\"}");
                return ESP_ERR_INVALID_ARG;
            }
            snprintf(current_skill_id, sizeof(current_skill_id), "%s", skill_item->valuestring);

            err = claw_skill_check_deactivate_allowed(ctx->session_id,
                                                      current_skill_id,
                                                      guard_reason, sizeof(guard_reason));
            if (err != ESP_OK) {
                cJSON_Delete(root);
                cJSON *resp = cJSON_CreateObject();
                char *rendered = NULL;
                if (resp) {
                    cJSON_AddBoolToObject(resp, "ok", false);
                    cJSON_AddStringToObject(resp, "error", "deactivate blocked by skill guard");
                    cJSON_AddStringToObject(resp, "skill_id", current_skill_id);
                    cJSON_AddStringToObject(resp, "reason", guard_reason);
                    rendered = cJSON_PrintUnformatted(resp);
                    cJSON_Delete(resp);
                }
                if (rendered) {
                    snprintf(output, output_size, "%s", rendered);
                    free(rendered);
                } else {
                    snprintf(output, output_size,
                             "{\"ok\":false,\"error\":\"deactivate blocked\",\"skill_id\":\"%s\"}",
                             current_skill_id);
                }
                return ESP_OK;
            }
        }

        cJSON_ArrayForEach(skill_item, skill_ids_item) {
            snprintf(target_skill_id, sizeof(target_skill_id), "%s", skill_item->valuestring);
            err = claw_skill_deactivate_for_session(ctx->session_id, target_skill_id);
            if (err != ESP_OK) {
                cJSON_Delete(root);
                snprintf(output,
                         output_size,
                         "{\"ok\":false,\"error\":\"failed to deactivate skill\",\"skill_id\":\"%s\"}",
                         target_skill_id);
                return err;
            }
        }
    } else if (all_requested) {
        char **active_ids = NULL;
        size_t active_count = 0;
        esp_err_t list_err = claw_skill_load_active_skill_ids(ctx->session_id,
                                                              &active_ids,
                                                              &active_count);
        if (list_err != ESP_OK && list_err != ESP_ERR_NOT_FOUND) {
            cJSON_Delete(root);
            snprintf(output, output_size,
                     "{\"ok\":false,\"error\":\"failed to enumerate active skills\","
                     "\"all\":true,\"detail\":\"%s\"}",
                     esp_err_to_name(list_err));
            return list_err;
        }

        char first_block_id[64] = {0};
        char first_block_reason[256] = {0};
        for (size_t i = 0; i < active_count; i++) {
            if (!active_ids[i] || !active_ids[i][0]) {
                continue;
            }
            char r[256] = {0};
            esp_err_t g = claw_skill_check_deactivate_allowed(ctx->session_id,
                                                              active_ids[i],
                                                              r, sizeof(r));
            if (g != ESP_OK) {
                snprintf(first_block_id, sizeof(first_block_id), "%s", active_ids[i]);
                snprintf(first_block_reason, sizeof(first_block_reason), "%s", r);
                break;
            }
        }
        for (size_t i = 0; i < active_count; i++) {
            free(active_ids[i]);
        }
        free(active_ids);

        if (first_block_id[0]) {
            cJSON_Delete(root);
            cJSON *resp = cJSON_CreateObject();
            char *rendered = NULL;
            if (resp) {
                cJSON_AddBoolToObject(resp, "ok", false);
                cJSON_AddStringToObject(resp, "error", "deactivate blocked by skill guard");
                cJSON_AddBoolToObject(resp, "all", true);
                cJSON_AddStringToObject(resp, "blocked_by", first_block_id);
                cJSON_AddStringToObject(resp, "reason", first_block_reason);
                rendered = cJSON_PrintUnformatted(resp);
                cJSON_Delete(resp);
            }
            if (rendered) {
                snprintf(output, output_size, "%s", rendered);
                free(rendered);
            } else {
                snprintf(output, output_size,
                         "{\"ok\":false,\"error\":\"deactivate blocked\","
                         "\"all\":true,\"blocked_by\":\"%s\"}",
                         first_block_id);
            }
            return ESP_OK;
        }
        err = claw_skill_clear_active_for_session(ctx->session_id);
        snprintf(target_skill_id, sizeof(target_skill_id), "%s", "all");
    } else {
        cJSON_Delete(root);
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"skill_ids or all=true is required\"}");
        return ESP_ERR_INVALID_ARG;
    }

    if (err != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output,
                 output_size,
                 "{\"ok\":false,\"error\":\"failed to deactivate skill\",\"skill_id\":\"%s\"}",
                 target_skill_id);
        return err;
    }
    err = cap_skill_sync_session_visible_groups(ctx->session_id);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output,
                 output_size,
                 "{\"ok\":false,\"error\":\"failed to sync capability visibility\",\"skill_id\":\"%s\"}",
                 target_skill_id);
        return err;
    }

    err = cap_skill_build_result(CAP_SKILL_DEACTIVATE,
                                 ctx->session_id,
                                 NULL,
                                 cJSON_IsArray(skill_ids_item) && cJSON_GetArraySize(skill_ids_item) > 0 ? skill_ids_item : NULL,
                                 all_requested,
                                 output,
                                 output_size);
    cJSON_Delete(root);
    return err;
}

static esp_err_t cap_skill_list_execute(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output,
                                        size_t output_size)
{
    (void)input_json;
    (void)ctx;

    return cap_skill_build_catalog_result(CAP_SKILL_LIST, NULL, NULL, output, output_size);
}

static esp_err_t cap_skill_register_execute(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char *output,
                                            size_t output_size)
{
    char skill_path[CAP_SKILL_MAX_PATH_LEN];
    char *markdown = NULL;
    cJSON *root = NULL;
    cJSON *skill_id_item = NULL;
    cJSON *file_item = NULL;
    cJSON *summary_item = NULL;
    cJSON *cap_groups_item = NULL;
    cJSON *skill = NULL;
    claw_skill_catalog_entry_t existing_entry;
    esp_err_t err;

    (void)ctx;

    root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        cap_skill_write_error(output, output_size, "invalid input json", NULL);
        return ESP_ERR_INVALID_ARG;
    }

    skill_id_item = cJSON_GetObjectItemCaseSensitive(root, "skill_id");
    file_item = cJSON_GetObjectItemCaseSensitive(root, "file");
    summary_item = cJSON_GetObjectItemCaseSensitive(root, "summary");
    cap_groups_item = cJSON_GetObjectItemCaseSensitive(root, "cap_groups");
    if (!cJSON_IsString(skill_id_item) || !skill_id_item->valuestring || !skill_id_item->valuestring[0] ||
            !cJSON_IsString(file_item) || !file_item->valuestring || !file_item->valuestring[0] ||
            !cJSON_IsString(summary_item) || !summary_item->valuestring || !summary_item->valuestring[0]) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "skill_id, file and summary are required", NULL);
        return ESP_ERR_INVALID_ARG;
    }
    if (cap_groups_item && !cJSON_IsArray(cap_groups_item)) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "cap_groups must be an array", skill_id_item->valuestring);
        return ESP_ERR_INVALID_ARG;
    }

    if (!cap_skill_path_is_valid(skill_id_item->valuestring, file_item->valuestring)) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "file must be <skill_id>/SKILL.md", skill_id_item->valuestring);
        return ESP_ERR_INVALID_ARG;
    }
    if (claw_skill_get_catalog_entry(skill_id_item->valuestring, &existing_entry) == ESP_OK) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "duplicate skill_id", skill_id_item->valuestring);
        return ESP_ERR_INVALID_ARG;
    }

    {
        const char *root_dir = cap_skill_root_dir();
        if (!root_dir) {
            cJSON_Delete(root);
            cap_skill_write_error(output, output_size, "skill storage is not initialized", skill_id_item->valuestring);
            return ESP_ERR_INVALID_STATE;
        }
        if (snprintf(skill_path, sizeof(skill_path), "%s/%s", root_dir, file_item->valuestring) >= (int)sizeof(skill_path)) {
            cJSON_Delete(root);
            cap_skill_write_error(output, output_size, "file path is too long", skill_id_item->valuestring);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    if (!cap_skill_file_exists(skill_path)) {
        err = cap_skill_ensure_parent_dirs(skill_path);
        if (err != ESP_OK) {
            cJSON_Delete(root);
            cap_skill_write_error(output, output_size, "failed to prepare skill directory", skill_id_item->valuestring);
            return err;
        }
    } else {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "skill markdown file already exists", skill_id_item->valuestring);
        return ESP_ERR_INVALID_STATE;
    }

    err = cap_skill_build_runtime_markdown(skill_id_item->valuestring,
                                           summary_item->valuestring,
                                           cap_groups_item,
                                           &markdown);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "failed to build skill markdown", skill_id_item->valuestring);
        return err;
    }

    err = cap_skill_write_file_text(skill_path, markdown);
    if (err != ESP_OK) {
        free(markdown);
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "failed to write skill markdown", skill_id_item->valuestring);
        return err;
    }
    free(markdown);

    err = claw_skill_reload_registry();
    if (err != ESP_OK) {
        remove(skill_path);
        (void)claw_skill_reload_registry();
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "failed to reload skill registry", skill_id_item->valuestring);
        return err;
    }

    skill = cJSON_CreateObject();
    if (!skill) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "out of memory", skill_id_item->valuestring);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(skill, "id", skill_id_item->valuestring);
    cJSON_AddStringToObject(skill, "file", file_item->valuestring);
    cJSON_AddStringToObject(skill, "summary", summary_item->valuestring);
    cJSON_AddStringToObject(skill, "manage_mode", "runtime");
    if (cap_groups_item) {
        cJSON *cap_groups_out = cJSON_Duplicate(cap_groups_item, true);
        if (cap_groups_out) {
            cJSON_AddItemToObject(skill, "cap_groups", cap_groups_out);
        }
    }

    cJSON_Delete(root);
    return cap_skill_build_catalog_result(CAP_SKILL_REGISTER, skill, NULL, output, output_size);
}

static esp_err_t cap_skill_unregister_execute(const char *input_json,
                                              const claw_cap_call_context_t *ctx,
                                              char *output,
                                              size_t output_size)
{
    char skill_path[CAP_SKILL_MAX_PATH_LEN];
    char *old_markdown = NULL;
    cJSON *root = NULL;
    cJSON *skill_id_item = NULL;
    const char *skill_id = NULL;
    claw_skill_catalog_entry_t entry;
    esp_err_t err;

    (void)ctx;

    root = cJSON_Parse(input_json ? input_json : "{}");
    skill_id_item = root ? cJSON_GetObjectItemCaseSensitive(root, "skill_id") : NULL;
    if (!cJSON_IsString(skill_id_item) || !skill_id_item->valuestring || !skill_id_item->valuestring[0]) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "skill_id is required", NULL);
        return ESP_ERR_INVALID_ARG;
    }

    skill_id = skill_id_item->valuestring;
    err = claw_skill_get_catalog_entry(skill_id, &entry);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "skill not found", skill_id);
        return err;
    }
    if (entry.manage_mode != CLAW_SKILL_MANAGE_MODE_RUNTIME) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "skill is readonly and cannot be unregistered", skill_id);
        return ESP_ERR_NOT_SUPPORTED;
    }

    {
        const char *root_dir = cap_skill_root_dir();
        if (!root_dir) {
            cJSON_Delete(root);
            cap_skill_write_error(output, output_size, "skill storage is not initialized", skill_id);
            return ESP_ERR_INVALID_STATE;
        }
        if (snprintf(skill_path, sizeof(skill_path), "%s/%s", root_dir, entry.file) >= (int)sizeof(skill_path)) {
            cJSON_Delete(root);
            cap_skill_write_error(output, output_size, "file path is too long", skill_id);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    err = cap_skill_read_file_dup(skill_path, &old_markdown);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "failed to read skill markdown", skill_id);
        return err;
    }
    if (remove(skill_path) != 0) {
        free(old_markdown);
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "failed to delete skill markdown", skill_id);
        return ESP_FAIL;
    }

    err = claw_skill_reload_registry();
    if (err != ESP_OK) {
        if (cap_skill_write_file_text(skill_path, old_markdown) == ESP_OK) {
            (void)claw_skill_reload_registry();
        }
        free(old_markdown);
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "failed to reload skill registry", skill_id);
        return err;
    }

    free(old_markdown);
    cJSON_Delete(root);
    return cap_skill_build_catalog_result(CAP_SKILL_UNREGISTER, NULL, skill_id, output, output_size);
}

static const claw_cap_descriptor_t s_skill_descriptors[] = {
    {
        .id = "list_skill",
        .name = "list_skill",
        .family = "skill",
        .description = "List all skills discovered from markdown files under the skills root directory.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        /* The skills catalog is already injected into prompt context, so keep this for non-LLM callers only. */
        .cap_flags = 0,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_skill_list_execute,
    },
    {
        .id = "register_skill",
        .name = "register_skill",
        .family = "skill",
        .description = "Create one runtime-managed skill markdown file and reload the in-memory skill registry.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"skill_id\":{\"type\":\"string\"},"
        "\"file\":{\"type\":\"string\",\"pattern\":\"^[^/]+/SKILL\\\\.md$\"},\"summary\":{\"type\":\"string\"},"
        "\"cap_groups\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},"
        "\"required\":[\"skill_id\",\"file\",\"summary\"]}",
        .execute = cap_skill_register_execute,
    },
    {
        .id = "unregister_skill",
        .name = "unregister_skill",
        .family = "skill",
        .description = "Delete one runtime-managed skill markdown file and reload the in-memory skill registry.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"skill_id\":{\"type\":\"string\"}},\"required\":[\"skill_id\"]}",
        .execute = cap_skill_unregister_execute,
    },
    {
        .id = "activate_skill",
        .name = "activate_skill",
        .family = "skill",
        .description = "Activate one or more skills from skill_ids and load their skill documentation into the prompt. "
                       "When multiple skills are needed, activate all required skills in one call whenever possible.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"skill_ids\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"minItems\":1}},\"required\":[\"skill_ids\"]}",
        .execute = cap_skill_activate_execute,
    },
    {
        .id = "deactivate_skill",
        .name = "deactivate_skill",
        .family = "skill",
        .description = "Deactivate one or more skills from skill_ids, or use all=true to clear active skills and "
                       "remove their skill documentation from the prompt. When multiple skills are needed, "
                       "deactivate all required skills in one call whenever possible.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"skill_ids\":{\"type\":\"array\","
        "\"items\":{\"type\":\"string\"},\"minItems\":1},\"all\":{\"type\":\"boolean\"}}}",
        .execute = cap_skill_deactivate_execute,
    },
};

static const claw_cap_group_t s_skill_group = {
    .group_id = "cap_skill",
    .descriptors = s_skill_descriptors,
    .descriptor_count = sizeof(s_skill_descriptors) / sizeof(s_skill_descriptors[0]),
};

esp_err_t cap_skill_mgr_register_group(void)
{
    if (claw_cap_group_exists(s_skill_group.group_id)) {
        return ESP_OK;
    }

    return claw_cap_register_group(&s_skill_group);
}

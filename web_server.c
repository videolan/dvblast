#include "web_server.h"
#include "web_state.h"
#include "web_manager.h"
#include "web_ui_embed.h"
#include "softcam_csa.h"

#include <microhttpd.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POST_BUFFER_MAX (1024 * 1024)

typedef struct {
    char *data;
    size_t size;
    char method[16];
    char url[256];
} http_request_ctx_t;

static struct MHD_Daemon *g_mhd_daemon = NULL;

static void add_cors_headers(struct MHD_Response *response) {
    if (!response) return;
    MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
    MHD_add_response_header(response, "Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS, HEAD");
    MHD_add_response_header(response, "Access-Control-Allow-Headers", "Content-Type, Authorization");
    MHD_add_response_header(response, "Server", "DVBlast-Web-Gateway/2.0");
}

static int send_response(struct MHD_Connection *conn, int status_code,
                         const char *content_type, const void *data, size_t len) {
    struct MHD_Response *resp = MHD_create_response_from_buffer(len, (void *)data, MHD_RESPMEM_MUST_COPY);
    if (!resp) return MHD_NO;
    MHD_add_response_header(resp, "Content-Type", content_type);
    add_cors_headers(resp);
    int ret = MHD_queue_response(conn, status_code, resp);
    MHD_destroy_response(resp);
    return ret;
}

static int send_json(struct MHD_Connection *conn, int status_code, json_t *root) {
    char *str = json_dumps(root, JSON_INDENT(2) | JSON_PRESERVE_ORDER);
    json_decref(root);
    if (!str) return send_response(conn, 500, "text/plain", "JSON Encoding Error", 19);

    int ret = send_response(conn, status_code, "application/json", str, strlen(str));
    free(str);
    return ret;
}

static int send_json_ok(struct MHD_Connection *conn, const char *msg) {
    json_t *r = json_object();
    json_object_set_new(r, "ok", json_true());
    if (msg) json_object_set_new(r, "message", json_string(msg));
    return send_json(conn, 200, r);
}

static int send_json_error(struct MHD_Connection *conn, int status_code, const char *err) {
    json_t *r = json_object();
    json_object_set_new(r, "ok", json_false());
    json_object_set_new(r, "error", json_string(err ? err : "Unknown Error"));
    return send_json(conn, status_code, r);
}

static int handle_get_request(struct MHD_Connection *conn, const char *url) {
    /* UI Page */
    if (strcmp(url, "/") == 0 || strcmp(url, "/index.html") == 0) {
        return send_response(conn, 200, "text/html; charset=utf-8", web_ui_html, web_ui_html_len);
    }

    /* System Telemetry */
    if (strcmp(url, "/api/v1/system") == 0 || strcmp(url, "/api/v1/telemetry") == 0) {
        return send_json(conn, 200, web_state_telemetry_to_json());
    }

    /* Configured Adapters */
    if (strcmp(url, "/api/v1/adapters") == 0 || strcmp(url, "/api/v1/frontend/channels") == 0) {
        return send_json(conn, 200, web_state_adapters_to_json());
    }

    /* Hardware Detection */
    if (strcmp(url, "/api/v1/hardware") == 0 || strcmp(url, "/api/v1/hardware/tuners") == 0) {
        return send_json(conn, 200, web_state_hardware_to_json());
    }

    /* Configured Streams */
    if (strcmp(url, "/api/v1/streams") == 0 || strcmp(url, "/api/v1/mux/outputs") == 0) {
        return send_json(conn, 200, web_state_streams_to_json());
    }

    /* SoftCAM Keys */
    if (strcmp(url, "/api/v1/softcam") == 0 || strcmp(url, "/api/v1/descramble/keys") == 0) {
        return send_json(conn, 200, softcam_csa_to_json());
    }

    /* Full System Configuration */
    if (strcmp(url, "/api/v1/config") == 0 || strcmp(url, "/api/v1/system/config") == 0) {
        return send_json(conn, 200, web_state_config_to_json());
    }

    /* Logs */
    if (strcmp(url, "/api/v1/logs") == 0 || strncmp(url, "/api/v1/system/logs", 19) == 0) {
        return send_json(conn, 200, web_state_logs_to_json(100));
    }

    return send_json_error(conn, 404, "Endpoint Not Found");
}

static int handle_post_request(struct MHD_Connection *conn, const char *url, const char *body) {
    json_error_t jerr;
    json_t *payload = NULL;
    if (body && *body) {
        payload = json_loads(body, 0, &jerr);
    }

    /* ===================================================================== */
    /*  ADAPTERS API                                                         */
    /* ===================================================================== */

    if (strcmp(url, "/api/v1/adapters") == 0 || strcmp(url, "/api/v1/frontend/tune") == 0) {
        if (!payload || !json_is_object(payload)) {
            if (payload) json_decref(payload);
            return send_json_error(conn, 400, "Invalid JSON body for adapter configuration");
        }

        web_adapter_t ad;
        memset(&ad, 0, sizeof(ad));

        const char *id = json_string_value(json_object_get(payload, "id"));
        const char *nm = json_string_value(json_object_get(payload, "name"));
        const char *del = json_string_value(json_object_get(payload, "delsys"));
        const char *pol = json_string_value(json_object_get(payload, "polarization"));
        const char *lnb = json_string_value(json_object_get(payload, "lnb_type"));
        const char *mod = json_string_value(json_object_get(payload, "modulation"));

        if (!id || !*id) {
            json_decref(payload);
            return send_json_error(conn, 400, "Missing required field: 'id'");
        }

        if (json_is_true(json_object_get(payload, "remove"))) {
            bool ok = web_manager_delete_adapter(id);
            json_decref(payload);
            return ok ? send_json_ok(conn, "Adapter removed") : send_json_error(conn, 404, "Adapter not found");
        }

        snprintf(ad.id, sizeof(ad.id), "%s", id);
        snprintf(ad.name, sizeof(ad.name), "%s", (nm && *nm) ? nm : id);
        ad.enabled = !json_is_false(json_object_get(payload, "enabled"));
        ad.adapter_idx = (int)json_integer_value(json_object_get(payload, "adapter_idx"));
        ad.frontend_idx = (int)json_integer_value(json_object_get(payload, "frontend_idx"));
        snprintf(ad.delsys, sizeof(ad.delsys), "%s", (del && *del) ? del : "DVBT2");

        json_t *j_freq = json_object_get(payload, "frequency");
        double freq_val = 0.0;
        if (json_is_string(j_freq)) {
            freq_val = atof(json_string_value(j_freq));
        } else if (json_is_number(j_freq)) {
            freq_val = json_number_value(j_freq);
        }
        if (freq_val > 0.0 && freq_val < 1000.0) {
            freq_val *= 1000000.0; /* e.g. 498 MHz -> 498000000 Hz */
        } else if (freq_val >= 1000.0 && freq_val < 1000000.0) {
            freq_val *= 1000.0;    /* e.g. 498000 kHz -> 498000000 Hz */
        }
        ad.frequency = (uint64_t)freq_val;

        ad.symbol_rate = (uint32_t)json_integer_value(json_object_get(payload, "symbol_rate"));
        ad.bandwidth = (int)json_integer_value(json_object_get(payload, "bandwidth"));
        if (ad.bandwidth <= 0) ad.bandwidth = 8;
        snprintf(ad.polarization, sizeof(ad.polarization), "%s", (pol && *pol) ? pol : "H");

        if (strcmp(ad.polarization, "V") == 0 || strcmp(ad.polarization, "13") == 0) {
            ad.voltage = 13;
        } else {
            ad.voltage = 18;
        }

        ad.tone = (int)json_integer_value(json_object_get(payload, "tone"));
        ad.diseqc = (int)json_integer_value(json_object_get(payload, "diseqc"));
        if (lnb) snprintf(ad.lnb_type, sizeof(ad.lnb_type), "%s", lnb);
        if (mod) snprintf(ad.modulation, sizeof(ad.modulation), "%s", mod);
        ad.multistream_id = (int)json_integer_value(json_object_get(payload, "multistream_id"));

        bool ok = web_manager_add_adapter(&ad);
        json_decref(payload);
        return ok ? send_json_ok(conn, "Adapter saved and configured") : send_json_error(conn, 500, "Failed to configure adapter");
    }

    if (strcmp(url, "/api/v1/adapters/delete") == 0) {
        if (!payload || !json_is_object(payload)) {
            if (payload) json_decref(payload);
            return send_json_error(conn, 400, "Invalid JSON body");
        }
        const char *id = json_string_value(json_object_get(payload, "id"));
        if (!id || !*id) {
            json_decref(payload);
            return send_json_error(conn, 400, "Missing required field: 'id'");
        }
        bool ok = web_manager_delete_adapter(id);
        json_decref(payload);
        return ok ? send_json_ok(conn, "Adapter deleted") : send_json_error(conn, 404, "Adapter not found");
    }

    if (strcmp(url, "/api/v1/adapters/toggle") == 0) {
        if (!payload || !json_is_object(payload)) {
            if (payload) json_decref(payload);
            return send_json_error(conn, 400, "Invalid JSON body");
        }
        const char *id = json_string_value(json_object_get(payload, "id"));
        bool enabled = !json_is_false(json_object_get(payload, "enabled"));
        if (!id || !*id) {
            json_decref(payload);
            return send_json_error(conn, 400, "Missing required field: 'id'");
        }
        bool ok = web_manager_toggle_adapter(id, enabled);
        json_decref(payload);
        return ok ? send_json_ok(conn, "Adapter state toggled") : send_json_error(conn, 404, "Adapter not found");
    }

    if (strcmp(url, "/api/v1/adapters/restart") == 0) {
        if (!payload || !json_is_object(payload)) {
            if (payload) json_decref(payload);
            return send_json_error(conn, 400, "Invalid JSON body");
        }
        const char *id = json_string_value(json_object_get(payload, "id"));
        if (!id || !*id) {
            json_decref(payload);
            return send_json_error(conn, 400, "Missing required field: 'id'");
        }
        bool ok = web_manager_restart_adapter(id);
        json_decref(payload);
        return ok ? send_json_ok(conn, "Adapter restart triggered") : send_json_error(conn, 404, "Adapter not found");
    }

    if (strcmp(url, "/api/v1/adapters/scan") == 0 || strcmp(url, "/api/v1/frontend/scan") == 0) {
        int adapter_idx = 0;
        if (payload && json_is_object(payload)) {
            const char *id = json_string_value(json_object_get(payload, "id"));
            if (id) {
                web_adapter_t *ad = web_state_find_adapter(id);
                if (ad) adapter_idx = ad->adapter_idx;
            } else {
                adapter_idx = (int)json_integer_value(json_object_get(payload, "adapter_idx"));
            }
        }
        if (payload) json_decref(payload);

        web_manager_trigger_scan(adapter_idx);
        json_t *res = json_object();
        json_object_set_new(res, "ok", json_true());
        json_object_set_new(res, "adapters", web_state_adapters_to_json());
        return send_json(conn, 200, res);
    }

    /* ===================================================================== */
    /*  STREAMS API                                                          */
    /* ===================================================================== */

    if (strcmp(url, "/api/v1/streams") == 0 || strcmp(url, "/api/v1/mux/outputs") == 0) {
        if (!payload || !json_is_object(payload)) {
            if (payload) json_decref(payload);
            return send_json_error(conn, 400, "Invalid JSON body for stream");
        }

        web_stream_config_t st;
        memset(&st, 0, sizeof(st));

        const char *id = json_string_value(json_object_get(payload, "id"));
        const char *nm = json_string_value(json_object_get(payload, "name"));
        const char *tp = json_string_value(json_object_get(payload, "type"));
        const char *in = json_string_value(json_object_get(payload, "input_url"));
        const char *out = json_string_value(json_object_get(payload, "output_url"));
        const char *ip = json_string_value(json_object_get(payload, "ip"));
        const char *bk = json_string_value(json_object_get(payload, "biss_key"));

        if (!id || !*id) {
            json_decref(payload);
            return send_json_error(conn, 400, "Missing required field: 'id'");
        }

        snprintf(st.id, sizeof(st.id), "%s", id);
        snprintf(st.name, sizeof(st.name), "%s", (nm && *nm) ? nm : id);
        snprintf(st.type, sizeof(st.type), "%s", (tp && *tp) ? tp : "spts");
        if (in) snprintf(st.input_url, sizeof(st.input_url), "%s", in);
        if (out) snprintf(st.output_url, sizeof(st.output_url), "%s", out);
        if (bk) snprintf(st.biss_key, sizeof(st.biss_key), "%s", bk);

        st.enabled = !json_is_false(json_object_get(payload, "enabled"));
        st.adapter_idx = (int)json_integer_value(json_object_get(payload, "adapter_idx"));
        st.service_id = (uint16_t)json_integer_value(json_object_get(payload, "service_id"));
        st.port = (int)json_integer_value(json_object_get(payload, "port"));
        if (st.port <= 0) st.port = 5000;

        if (ip && *ip) {
            snprintf(st.ip, sizeof(st.ip), "%s", ip);
        } else if (out && *out) {
            /* Parse IP from output_url e.g. udp://239.255.1.1:5000 */
            const char *p = strstr(out, "://");
            p = p ? (p + 3) : out;
            char hostbuf[64];
            snprintf(hostbuf, sizeof(hostbuf), "%s", p);
            char *colon = strchr(hostbuf, ':');
            if (colon) {
                *colon = '\0';
                st.port = atoi(colon + 1);
            }
            snprintf(st.ip, sizeof(st.ip), "%s", hostbuf);
        } else {
            snprintf(st.ip, sizeof(st.ip), "239.255.1.1");
        }

        st.ttl = (int)json_integer_value(json_object_get(payload, "ttl"));
        if (st.ttl <= 0) st.ttl = 16;
        st.is_rtp = json_is_true(json_object_get(payload, "is_rtp"));
        st.pacing = !json_is_false(json_object_get(payload, "pacing"));

        bool ok = web_manager_add_stream(&st);
        json_decref(payload);
        return ok ? send_json_ok(conn, "Stream saved and route configured") : send_json_error(conn, 500, "Failed to configure stream");
    }

    if (strcmp(url, "/api/v1/streams/delete") == 0 || strcmp(url, "/api/v1/mux/outputs/delete") == 0) {
        if (!payload || !json_is_object(payload)) {
            if (payload) json_decref(payload);
            return send_json_error(conn, 400, "Invalid JSON body");
        }
        const char *id = json_string_value(json_object_get(payload, "id"));
        if (!id || !*id) {
            json_decref(payload);
            return send_json_error(conn, 400, "Missing required field: 'id'");
        }
        bool ok = web_manager_delete_stream(id);
        json_decref(payload);
        return ok ? send_json_ok(conn, "Stream deleted") : send_json_error(conn, 404, "Stream not found");
    }

    if (strcmp(url, "/api/v1/streams/toggle") == 0) {
        if (!payload || !json_is_object(payload)) {
            if (payload) json_decref(payload);
            return send_json_error(conn, 400, "Invalid JSON body");
        }
        const char *id = json_string_value(json_object_get(payload, "id"));
        bool enabled = !json_is_false(json_object_get(payload, "enabled"));
        if (!id || !*id) {
            json_decref(payload);
            return send_json_error(conn, 400, "Missing required field: 'id'");
        }
        bool ok = web_manager_toggle_stream(id, enabled);
        json_decref(payload);
        return ok ? send_json_ok(conn, "Stream state toggled") : send_json_error(conn, 404, "Stream not found");
    }

    /* ===================================================================== */
    /*  SOFTCAM API                                                          */
    /* ===================================================================== */

    if (strcmp(url, "/api/v1/softcam") == 0 || strcmp(url, "/api/v1/descramble/keys") == 0) {
        if (!payload || !json_is_object(payload)) {
            if (payload) json_decref(payload);
            return send_json_error(conn, 400, "Invalid JSON body for SoftCAM key");
        }
        const char *id = json_string_value(json_object_get(payload, "id"));
        const char *nm = json_string_value(json_object_get(payload, "name"));
        uint16_t sid = (uint16_t)json_integer_value(json_object_get(payload, "service_id"));
        const char *key = json_string_value(json_object_get(payload, "key"));
        bool en = !json_is_false(json_object_get(payload, "enabled"));

        if (!id || !key) {
            json_decref(payload);
            return send_json_error(conn, 400, "Missing 'id' or 'key'");
        }
        bool ok = softcam_csa_set_key(id, nm, sid, key, en);
        web_state_save_json(NULL);
        json_decref(payload);
        return ok ? send_json_ok(conn, "SoftCAM key saved") : send_json_error(conn, 500, "Failed to set SoftCAM key");
    }

    if (strcmp(url, "/api/v1/softcam/delete") == 0 || strcmp(url, "/api/v1/descramble/keys/delete") == 0) {
        if (!payload || !json_is_object(payload)) {
            if (payload) json_decref(payload);
            return send_json_error(conn, 400, "Invalid JSON body");
        }
        const char *id = json_string_value(json_object_get(payload, "id"));
        if (!id) {
            json_decref(payload);
            return send_json_error(conn, 400, "Missing key 'id'");
        }
        bool ok = softcam_csa_del_key(id);
        web_state_save_json(NULL);
        json_decref(payload);
        return ok ? send_json_ok(conn, "SoftCAM key deleted") : send_json_error(conn, 404, "Key not found");
    }

    /* ===================================================================== */
    /*  CONFIGURATION API                                                    */
    /* ===================================================================== */

    if (strcmp(url, "/api/v1/config") == 0 || strcmp(url, "/api/v1/system/config") == 0) {
        if (!payload || !json_is_object(payload)) {
            if (payload) json_decref(payload);
            return send_json_error(conn, 400, "Invalid JSON payload for config");
        }
        char tmp_path[300];
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", g_gateway.config_file);
        int res = json_dump_file(payload, tmp_path, JSON_INDENT(2));
        json_decref(payload);

        if (res == 0) {
            rename(tmp_path, g_gateway.config_file);
            web_state_load_json(g_gateway.config_file);
            web_manager_sync_outputs();
            return send_json_ok(conn, "Configuration saved and applied");
        }
        return send_json_error(conn, 500, "Failed to write configuration file");
    }

    if (payload) json_decref(payload);
    return send_json_error(conn, 404, "Endpoint Not Found");
}

static enum MHD_Result http_handler(void *cls, struct MHD_Connection *conn,
                                    const char *url, const char *method,
                                    const char *version, const char *upload_data,
                                    size_t *upload_data_size, void **con_cls) {
    (void)cls; (void)version;

    if (*con_cls == NULL) {
        http_request_ctx_t *ctx = calloc(1, sizeof(http_request_ctx_t));
        if (!ctx) return MHD_NO;
        snprintf(ctx->method, sizeof(ctx->method), "%s", method);
        snprintf(ctx->url, sizeof(ctx->url), "%s", url);
        *con_cls = ctx;
        return MHD_YES;
    }

    http_request_ctx_t *ctx = (http_request_ctx_t *)*con_cls;

    if (*upload_data_size != 0) {
        if (ctx->size + *upload_data_size > POST_BUFFER_MAX) {
            *upload_data_size = 0;
            return MHD_NO;
        }
        ctx->data = realloc(ctx->data, ctx->size + *upload_data_size + 1);
        memcpy(ctx->data + ctx->size, upload_data, *upload_data_size);
        ctx->size += *upload_data_size;
        ctx->data[ctx->size] = '\0';
        *upload_data_size = 0;
        return MHD_YES;
    }

    /* Handle OPTIONS pre-flight */
    if (strcasecmp(method, "OPTIONS") == 0) {
        struct MHD_Response *resp = MHD_create_response_from_buffer(0, "", MHD_RESPMEM_PERSISTENT);
        add_cors_headers(resp);
        int ret = MHD_queue_response(conn, 204, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    if (strcasecmp(method, "GET") == 0 || strcasecmp(method, "HEAD") == 0) {
        return handle_get_request(conn, url);
    } else if (strcasecmp(method, "POST") == 0) {
        return handle_post_request(conn, url, ctx->data);
    }

    return send_json_error(conn, 405, "Method Not Allowed");
}

static void request_completed(void *cls, struct MHD_Connection *conn,
                              void **con_cls, enum MHD_RequestTerminationCode toe) {
    (void)cls; (void)conn; (void)toe;
    http_request_ctx_t *ctx = (http_request_ctx_t *)*con_cls;
    if (ctx) {
        if (ctx->data) free(ctx->data);
        free(ctx);
        *con_cls = NULL;
    }
}

bool web_server_start(int port, const char *host) {
    if (g_mhd_daemon) {
        web_state_log("WARN", "Web server already running");
        return true;
    }

    (void)host;
    g_mhd_daemon = MHD_start_daemon(
        MHD_USE_SELECT_INTERNALLY,
        port,
        NULL, NULL,
        &http_handler, NULL,
        MHD_OPTION_NOTIFY_COMPLETED, &request_completed, NULL,
        MHD_OPTION_END
    );

    if (!g_mhd_daemon) {
        web_state_log("ERROR", "Failed to start HTTP server on port %d", port);
        return false;
    }

    web_state_log("INFO", "HTTP Server running at http://0.0.0.0:%d", port);
    return true;
}

void web_server_stop(void) {
    if (g_mhd_daemon) {
        MHD_stop_daemon(g_mhd_daemon);
        g_mhd_daemon = NULL;
        web_state_log("INFO", "HTTP Server stopped");
    }
}

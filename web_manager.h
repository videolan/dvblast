#ifndef DVBLAST_WEB_MANAGER_H
#define DVBLAST_WEB_MANAGER_H

#include "web_state.h"

typedef enum {
    WEB_ACTION_NONE = 0,
    WEB_ACTION_TUNE,
    WEB_ACTION_DISABLE,
    WEB_ACTION_RESTART,
    WEB_ACTION_SCAN
} web_action_type_t;

void web_manager_handle_async_action(void);
bool web_manager_submit_action(web_action_type_t type, int adapter_idx, const char *adapter_id);

/* Adapter Management */
bool web_manager_add_adapter(const web_adapter_t *ad);
bool web_manager_delete_adapter(const char *id);
bool web_manager_toggle_adapter(const char *id, bool enable);
bool web_manager_restart_adapter(const char *id);
bool web_manager_tune_adapter(const char *id, int adapter_idx);
void web_manager_trigger_scan(int adapter_idx);

/* Stream Management */
bool web_manager_add_stream(const web_stream_config_t *stream);
bool web_manager_delete_stream(const char *id);
bool web_manager_toggle_stream(const char *id, bool enabled);
void web_manager_sync_outputs(void);

/* Multi-Adapter Worker Management */
void web_manager_sync_worker(int adapter_idx);
void web_manager_sync_workers(void);
void web_manager_poll_secondary_telemetry(void);
void web_manager_cleanup(void);

#endif /* DVBLAST_WEB_MANAGER_H */

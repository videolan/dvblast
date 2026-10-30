#include "web_manager.h"
#include "dvblast.h"
#include "softcam_csa.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/dvb/frontend.h>
#include <pthread.h>

#ifndef MAX_DTV_STATS
#define MAX_DTV_STATS 4
#endif

extern int i_frontend;
extern pthread_t g_main_thread_id;
void demux_ExtractDiscoveredChannels(void);
void demux_TriggerScan(void);
void web_manager_notify_async(void);

typedef struct {
    int adapter_idx;
    char adapter_id[64];
    pid_t pid;
    bool running;
    uint64_t frequency;
    int bandwidth;
    char delsys[16];
} dvb_worker_proc_t;

static dvb_worker_proc_t g_workers[MAX_WEB_ADAPTERS];

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    web_action_type_t type;
    int adapter_idx;
    char adapter_id[64];
    bool done;
    bool success;
} web_action_queue_t;

static web_action_queue_t g_async_action = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .type = WEB_ACTION_NONE,
};

static void web_manager_stop_worker_internal(int adapter_idx) {
    for (int i = 0; i < MAX_WEB_ADAPTERS; i++) {
        if (g_workers[i].adapter_idx == adapter_idx && g_workers[i].running) {
            if (g_workers[i].pid > 0) {
                web_state_log("INFO", "Stopping worker PID %d for adapter %d", g_workers[i].pid, adapter_idx);
                kill(g_workers[i].pid, SIGTERM);
                int status = 0;
                waitpid(g_workers[i].pid, &status, WNOHANG);
            }
            g_workers[i].running = false;
            g_workers[i].pid = 0;
            break;
        }
    }
    char conf_path[64], sock_path[64];
    snprintf(conf_path, sizeof(conf_path), "/tmp/dvblast_ad%d.conf", adapter_idx);
    snprintf(sock_path, sizeof(sock_path), "/tmp/dvblast_ad%d.sock", adapter_idx);
    unlink(conf_path);
    unlink(sock_path);
}

static bool web_manager_read_frontend_telemetry(int fd, bool *out_locked, uint16_t *out_sig_pct,
                                               float *out_snr_db, float *out_sig_dbm, uint32_t *out_ber) {
    if (fd < 0) return false;

    fe_status_t status = 0;
    ioctl(fd, FE_READ_STATUS, &status);

    int sig_percent = 0;
    int snr_percent = 0;
    float sig_dbm = -100.0f;
    float snr_db = 0.0f;
    int signal_read = 0;

    struct dtv_property props[2];
    memset(props, 0, sizeof(props));
    props[0].cmd = DTV_STAT_SIGNAL_STRENGTH;
    props[1].cmd = DTV_STAT_CNR;
    struct dtv_properties cmdseq = { .num = 2, .props = props };

    if (ioctl(fd, FE_GET_PROPERTY, &cmdseq) == 0) {
        for (int i = 0; i < (int)cmdseq.num; i++) {
            struct dtv_property *p = &props[i];
            if (p->result != 0) continue;
            for (int l = 0; l < (int)p->u.st.len && l < MAX_DTV_STATS; l++) {
                int64_t sval = p->u.st.stat[l].svalue;
                uint32_t uval = p->u.st.stat[l].uvalue;
                int scale = p->u.st.stat[l].scale;
                if (scale == FE_SCALE_NOT_AVAILABLE) continue;

                if (p->cmd == DTV_STAT_SIGNAL_STRENGTH) {
                    if (scale == FE_SCALE_DECIBEL) {
                        sig_dbm = (float)sval / 1000.0f;
                        int dbm_int = (int)sig_dbm;
                        sig_percent = dbm_int < -100 ? 0 : (dbm_int < 0 ? (dbm_int + 100) : 99);
                    } else if (scale == FE_SCALE_RELATIVE) {
                        sig_percent = (int)(uval * 99 / 65535);
                        sig_dbm = -100.0f + (float)sig_percent;
                    }
                    if (sig_percent > 0) signal_read = 1;
                } else if (p->cmd == DTV_STAT_CNR) {
                    if (scale == FE_SCALE_DECIBEL) {
                        snr_db = (float)sval / 1000.0f;
                        snr_percent = (int)(snr_db * 100.0f / 35.0f);
                        if (snr_percent < 0) snr_percent = 0;
                        if (snr_percent > 99) snr_percent = 99;
                    } else if (scale == FE_SCALE_RELATIVE) {
                        snr_percent = (int)(uval * 99 / 65535);
                        snr_db = (float)snr_percent * 0.35f;
                    }
                }
            }
        }
    }

    if (!signal_read) {
        uint16_t leg_sig = 0, leg_snr = 0;
        if (ioctl(fd, FE_READ_SIGNAL_STRENGTH, &leg_sig) == 0 && leg_sig > 0) {
            sig_percent = (int)(leg_sig * 99 / 65535);
            sig_dbm = -100.0f + (float)sig_percent;
            signal_read = 1;
        }
        if (ioctl(fd, FE_READ_SNR, &leg_snr) == 0 && leg_snr > 0) {
            snr_percent = (int)(leg_snr * 99 / 65535);
            snr_db = (float)snr_percent * 0.35f;
        }
    }

    uint32_t ber = 0;
    ioctl(fd, FE_READ_BER, &ber);

    /* Si2168 demodulator quirk: status may return 0 even when locked with RF signal */
    if ((status & FE_HAS_LOCK) == 0 && signal_read && sig_percent > 10) {
        status |= (FE_HAS_SIGNAL | FE_HAS_CARRIER | FE_HAS_VITERBI | FE_HAS_SYNC | FE_HAS_LOCK);
    }

    *out_locked = (status & FE_HAS_LOCK) != 0;
    *out_sig_pct = (uint16_t)sig_percent;
    *out_snr_db = snr_db;
    *out_sig_dbm = sig_dbm;
    *out_ber = ber;
    return true;
}

void web_manager_sync_worker(int adapter_idx) {
    if (adapter_idx == i_adapter) {
        return; /* Primary adapter runs directly inside Master process */
    }

    web_adapter_t *ad = web_state_find_adapter_by_idx(adapter_idx);
    if (!ad || !ad->enabled) {
        web_manager_stop_worker_internal(adapter_idx);
        return;
    }

    /* Count active enabled streams for this adapter */
    pthread_mutex_lock(&g_gateway.lock);
    int active_streams = 0;
    web_stream_config_t streams_for_ad[MAX_WEB_STREAMS];
    for (int i = 0; i < g_gateway.num_streams; i++) {
        if (g_gateway.streams[i].enabled && g_gateway.streams[i].adapter_idx == adapter_idx) {
            streams_for_ad[active_streams++] = g_gateway.streams[i];
        }
    }
    pthread_mutex_unlock(&g_gateway.lock);

    /* Write worker configuration file (idle comment if no streams configured yet) */
    char conf_path[64], sock_path[64];
    snprintf(conf_path, sizeof(conf_path), "/tmp/dvblast_ad%d.conf", adapter_idx);
    snprintf(sock_path, sizeof(sock_path), "/tmp/dvblast_ad%d.sock", adapter_idx);

    FILE *fp = fopen(conf_path, "w");
    if (!fp) {
        web_state_log("ERROR", "Failed to write worker config file %s", conf_path);
        return;
    }
    if (active_streams == 0) {
        fprintf(fp, "# idle\n");
    } else {
        for (int i = 0; i < active_streams; i++) {
            fprintf(fp, "%s:%d 1 %d\n", streams_for_ad[i].ip, streams_for_ad[i].port, streams_for_ad[i].service_id);
        }
    }
    fclose(fp);

    const char *target_delsys = (ad->delsys[0] ? ad->delsys : "DVBT2");
    int target_bw = (ad->bandwidth > 0 ? ad->bandwidth : 8);

    /* Check if worker is already running with matching RF parameters */
    dvb_worker_proc_t *existing_worker = NULL;
    for (int i = 0; i < MAX_WEB_ADAPTERS; i++) {
        if (g_workers[i].adapter_idx == adapter_idx && g_workers[i].running && g_workers[i].pid > 0) {
            existing_worker = &g_workers[i];
            break;
        }
    }

    if (existing_worker) {
        if (kill(existing_worker->pid, 0) == 0 &&
            existing_worker->frequency == ad->frequency &&
            existing_worker->bandwidth == target_bw &&
            strcmp(existing_worker->delsys, target_delsys) == 0) {
            /* Same RF tuning: reload outputs via SIGHUP without dropping lock */
            kill(existing_worker->pid, SIGHUP);
            web_state_log("INFO", "Sent SIGHUP reload to worker PID %d (Adapter %s, %d streams)",
                          existing_worker->pid, ad->id, active_streams);
            return;
        }
        /* Parameters changed or process dead: stop old worker before respawning */
        web_manager_stop_worker_internal(adapter_idx);
    }

    pid_t pid = fork();
    if (pid == 0) {
        char sz_ad[16], sz_fe[16], sz_freq[32], sz_bw[16];
        snprintf(sz_ad, sizeof(sz_ad), "%d", ad->adapter_idx);
        snprintf(sz_fe, sizeof(sz_fe), "%d", ad->frontend_idx);
        snprintf(sz_freq, sizeof(sz_freq), "%llu", (unsigned long long)ad->frequency);
        snprintf(sz_bw, sizeof(sz_bw), "%d", target_bw);

        char *argv[] = {
            "./dvblast",
            "--disable-web",
            "-a", sz_ad,
            "-n", sz_fe,
            "-f", sz_freq,
            "-b", sz_bw,
            "-5", (char *)target_delsys,
            "-u",
            "-U",
            "-c", conf_path,
            "-r", sock_path,
            "-q",
            NULL
        };

        execv("./dvblast", argv);
        _exit(127);
    } else if (pid > 0) {
        for (int i = 0; i < MAX_WEB_ADAPTERS; i++) {
            if (!g_workers[i].running || g_workers[i].adapter_idx == adapter_idx) {
                g_workers[i].adapter_idx = adapter_idx;
                snprintf(g_workers[i].adapter_id, sizeof(g_workers[i].adapter_id), "%s", ad->id);
                g_workers[i].pid = pid;
                g_workers[i].running = true;
                g_workers[i].frequency = ad->frequency;
                g_workers[i].bandwidth = target_bw;
                snprintf(g_workers[i].delsys, sizeof(g_workers[i].delsys), "%s", target_delsys);
                break;
            }
        }
        web_state_log("INFO", "Spawned isolated worker PID %d for Adapter %s (/dev/dvb/adapter%d, %d streams, freq=%llu Hz)",
                      pid, ad->id, ad->adapter_idx, active_streams, (unsigned long long)ad->frequency);
    }
}

void web_manager_sync_workers(void) {
    pthread_mutex_lock(&g_gateway.lock);
    int ad_count = g_gateway.num_adapters;
    int ad_indices[MAX_WEB_ADAPTERS];
    for (int i = 0; i < ad_count; i++) {
        ad_indices[i] = g_gateway.adapters[i].adapter_idx;
    }
    pthread_mutex_unlock(&g_gateway.lock);

    for (int i = 0; i < ad_count; i++) {
        if (ad_indices[i] != i_adapter) {
            web_manager_sync_worker(ad_indices[i]);
        }
    }
}

void web_manager_poll_secondary_telemetry(void) {
    /* Reap and auto-restart any died child worker processes */
    for (int i = 0; i < MAX_WEB_ADAPTERS; i++) {
        if (g_workers[i].running && g_workers[i].pid > 0) {
            int status = 0;
            pid_t res = waitpid(g_workers[i].pid, &status, WNOHANG);
            if (res > 0 || (res < 0 && errno == ECHILD)) {
                web_state_log("WARN", "Worker PID %d for adapter %d exited, restarting",
                              g_workers[i].pid, g_workers[i].adapter_idx);
                g_workers[i].running = false;
                g_workers[i].pid = 0;
                web_manager_sync_worker(g_workers[i].adapter_idx);
            }
        }
    }

    pthread_mutex_lock(&g_gateway.lock);
    int ad_count = g_gateway.num_adapters;
    web_adapter_t adapters[MAX_WEB_ADAPTERS];
    memcpy(adapters, g_gateway.adapters, ad_count * sizeof(web_adapter_t));
    pthread_mutex_unlock(&g_gateway.lock);

    for (int i = 0; i < ad_count; i++) {
        if (!adapters[i].enabled || adapters[i].adapter_idx == i_adapter) {
            continue;
        }

        char dev_path[64];
        snprintf(dev_path, sizeof(dev_path), "/dev/dvb/adapter%d/frontend%d",
                 adapters[i].adapter_idx, adapters[i].frontend_idx);

        int fd = open(dev_path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }

        bool locked = false;
        uint16_t sig_pct = 0;
        float snr_db = 0.0f;
        float sig_dbm = -100.0f;
        uint32_t ber = 0;

        web_manager_read_frontend_telemetry(fd, &locked, &sig_pct, &snr_db, &sig_dbm, &ber);
        close(fd);

        web_state_update_adapter_signal_ext(adapters[i].adapter_idx, locked, sig_pct,
                                            snr_db, sig_dbm, ber, 0);
    }
}

void web_manager_cleanup(void) {
    for (int i = 0; i < MAX_WEB_ADAPTERS; i++) {
        if (g_workers[i].running && g_workers[i].pid > 0) {
            kill(g_workers[i].pid, SIGTERM);
            int status = 0;
            waitpid(g_workers[i].pid, &status, WNOHANG);
            g_workers[i].running = false;
        }
    }
}

static void *scan_secondary_adapter_thread(void *arg) {
    int adapter_idx = (int)(intptr_t)arg;
    web_adapter_t *ad = web_state_find_adapter_by_idx(adapter_idx);
    if (!ad || ad->frequency <= 0) return NULL;

    web_state_log("INFO", "Starting isolated background scan for Adapter %s (/dev/dvb/adapter%d, freq=%llu Hz)",
                  ad->id, ad->adapter_idx, (unsigned long long)ad->frequency);

    /* Stop worker temporarily to free frontend/dvr */
    web_manager_stop_worker_internal(adapter_idx);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "./dvblast --disable-web -a %d -n %d -f %llu -b %d -5 %s -u -q -x text -Q 3500 2>&1",
             ad->adapter_idx, ad->frontend_idx, (unsigned long long)ad->frequency,
             ad->bandwidth > 0 ? ad->bandwidth : 8,
             (ad->delsys[0] ? ad->delsys : "DVBT2"));

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        web_state_log("ERROR", "Failed to start scanner on Adapter %d", adapter_idx);
        web_manager_sync_worker(adapter_idx);
        return NULL;
    }

    char line[256];
    int current_sid = 0;
    ad->num_channels = 0;
    memset(ad->channels, 0, sizeof(ad->channels));

    while (fgets(line, sizeof(line), fp)) {
        char *p_sid = strstr(line, "service sid=");
        if (p_sid) {
            current_sid = atoi(p_sid + 12);
        }
        char *p_svc = strstr(line, "service=\"");
        if (p_svc && current_sid > 0) {
            p_svc += 9;
            char *end = strchr(p_svc, '"');
            if (end) {
                *end = '\0';
                channel_info_t ch;
                memset(&ch, 0, sizeof(ch));
                ch.service_id = (uint16_t)current_sid;
                snprintf(ch.name, sizeof(ch.name), "%s", p_svc);
                web_state_add_adapter_channel(adapter_idx, &ch);
                current_sid = 0;
            }
        }
    }
    pclose(fp);

    web_state_save_json(NULL);
    web_state_log("INFO", "Scan complete for Adapter %d, discovered %d services",
                  adapter_idx, ad->num_channels);

    /* Restore streaming worker if active streams exist */
    web_manager_sync_worker(adapter_idx);
    return NULL;
}

static bool web_manager_tune_adapter_internal(const char *adapter_id, int adapter_idx) {
    web_adapter_t *ad = NULL;
    if (adapter_id && *adapter_id) {
        ad = web_state_find_adapter(adapter_id);
    }
    if (!ad) {
        ad = web_state_find_adapter_by_idx(adapter_idx);
    }
    if (!ad) return false;

    /* If this is a secondary adapter, manage via worker process, NOT Master's frontend! */
    if (ad->adapter_idx != i_adapter) {
        web_manager_sync_worker(ad->adapter_idx);
        return true;
    }

    /* If primary adapter is already active on this frequency and bandwidth, skip redundant retune */
    if (i_frontend >= 0 && i_adapter == ad->adapter_idx && i_frequency == (int)ad->frequency &&
        i_bandwidth == ad->bandwidth) {
        web_state_log("INFO", "Primary Adapter %d already tuned to %llu Hz", ad->adapter_idx, (unsigned long long)ad->frequency);
        return true;
    }

    web_state_log("INFO", "Tuning Primary Adapter %d (%s, id=%s): Freq=%llu Hz, BW=%d MHz, Delsys=%s",
                  ad->adapter_idx, ad->name, ad->id, (unsigned long long)ad->frequency, ad->bandwidth, ad->delsys);

    i_adapter = ad->adapter_idx;
    i_fenum = ad->frontend_idx;
    i_frequency = (int)ad->frequency;
    i_bandwidth = ad->bandwidth;
    if (ad->symbol_rate > 0) i_srate = (int)ad->symbol_rate;
    if (ad->delsys[0]) {
        free(psz_delsys);
        psz_delsys = strdup(ad->delsys);
    }
    if (ad->voltage > 0) i_voltage = ad->voltage;
    i_satnum = ad->diseqc;
    b_tone = ad->tone;

    /* Reset channels when tuning transponder */
    ad->num_channels = 0;
    memset(ad->channels, 0, sizeof(ad->channels));

    bool ok = dvb_TuneFrontend(ad->adapter_idx, ad->frontend_idx, ad->frequency,
                               ad->bandwidth, ad->delsys, ad->multistream_id,
                               ad->modulation, NULL, NULL, (int)ad->symbol_rate,
                               ad->voltage, ad->diseqc, ad->tone);
    if (ok) {
        demux_TriggerScan();
    }
    return ok;
}

static bool web_manager_disable_adapter_internal(const char *id) {
    if (!id || !*id) return false;
    web_adapter_t *ad = web_state_find_adapter(id);
    if (!ad) return false;

    ad->enabled = false;
    web_state_log("INFO", "Disabling Adapter %s (/dev/dvb/adapter%d), releasing hardware...", ad->id, ad->adapter_idx);
    if (i_adapter == ad->adapter_idx) {
        dvb_CloseDevices();
        demux_ResetTables();
    } else {
        web_manager_stop_worker_internal(ad->adapter_idx);
    }

    ad->locked = false;
    ad->signal_percent = 0;
    ad->signal_dbm = -100.0f;
    ad->snr_db = 0.0f;
    ad->snr_raw = 0;
    ad->ber = 0;
    ad->ucb = 0;
    ad->bitrate_bps = 0;
    ad->last_calc_bytes = ad->total_bytes;

    web_state_save_json(NULL);
    return true;
}

static bool web_manager_restart_adapter_internal(const char *id) {
    if (!id || !*id) return false;
    web_adapter_t *ad = web_state_find_adapter(id);
    if (!ad) return false;

    web_state_log("INFO", "Restarting Adapter %s (/dev/dvb/adapter%d)...", ad->id, ad->adapter_idx);

    if (i_adapter == ad->adapter_idx) {
        dvb_CloseDevices();
        demux_ResetTables();
    } else {
        web_manager_stop_worker_internal(ad->adapter_idx);
    }

    ad->locked = false;
    ad->signal_percent = 0;
    ad->signal_dbm = -100.0f;
    ad->snr_db = 0.0f;
    ad->snr_raw = 0;
    ad->ber = 0;
    ad->ucb = 0;
    ad->bitrate_bps = 0;
    ad->last_calc_bytes = ad->total_bytes;
    ad->num_channels = 0;
    memset(ad->channels, 0, sizeof(ad->channels));

    if (ad->enabled) {
        if (i_adapter == ad->adapter_idx) {
            web_manager_tune_adapter_internal(ad->id, ad->adapter_idx);
        } else {
            web_manager_sync_worker(ad->adapter_idx);
        }
    }

    web_state_save_json(NULL);
    return true;
}

void web_manager_handle_async_action(void) {
    pthread_mutex_lock(&g_async_action.mutex);
    if (g_async_action.type == WEB_ACTION_NONE) {
        pthread_mutex_unlock(&g_async_action.mutex);
        return;
    }

    bool ok = false;
    switch (g_async_action.type) {
        case WEB_ACTION_TUNE:
            ok = web_manager_tune_adapter_internal(g_async_action.adapter_id, g_async_action.adapter_idx);
            break;
        case WEB_ACTION_DISABLE:
            ok = web_manager_disable_adapter_internal(g_async_action.adapter_id);
            break;
        case WEB_ACTION_RESTART:
            ok = web_manager_restart_adapter_internal(g_async_action.adapter_id);
            break;
        case WEB_ACTION_SCAN:
            demux_TriggerScan();
            ok = true;
            break;
        default:
            break;
    }

    g_async_action.success = ok;
    g_async_action.done = true;
    pthread_cond_broadcast(&g_async_action.cond);
    pthread_mutex_unlock(&g_async_action.mutex);
}

bool web_manager_submit_action(web_action_type_t type, int adapter_idx, const char *adapter_id) {
    if (pthread_equal(pthread_self(), g_main_thread_id)) {
        switch (type) {
            case WEB_ACTION_TUNE:
                return web_manager_tune_adapter_internal(adapter_id, adapter_idx);
            case WEB_ACTION_DISABLE:
                return web_manager_disable_adapter_internal(adapter_id);
            case WEB_ACTION_RESTART:
                return web_manager_restart_adapter_internal(adapter_id);
            case WEB_ACTION_SCAN:
                demux_TriggerScan();
                return true;
            default:
                return false;
        }
    }

    pthread_mutex_lock(&g_async_action.mutex);
    while (g_async_action.type != WEB_ACTION_NONE) {
        pthread_cond_wait(&g_async_action.cond, &g_async_action.mutex);
    }

    g_async_action.type = type;
    g_async_action.adapter_idx = adapter_idx;
    if (adapter_id) snprintf(g_async_action.adapter_id, sizeof(g_async_action.adapter_id), "%s", adapter_id);
    else g_async_action.adapter_id[0] = '\0';
    g_async_action.done = false;
    g_async_action.success = false;

    web_manager_notify_async();

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 4;

    while (!g_async_action.done) {
        int rc = pthread_cond_timedwait(&g_async_action.cond, &g_async_action.mutex, &ts);
        if (rc != 0) {
            break;
        }
    }

    bool res = g_async_action.success;
    g_async_action.type = WEB_ACTION_NONE;
    pthread_mutex_unlock(&g_async_action.mutex);
    return res;
}

bool web_manager_tune_adapter(const char *id, int adapter_idx) {
    return web_manager_submit_action(WEB_ACTION_TUNE, adapter_idx, id);
}

bool web_manager_add_adapter(const web_adapter_t *ad) {
    if (!ad || !ad->id[0]) return false;

    bool ok = web_state_add_or_update_adapter(ad);
    if (ok && ad->enabled) {
        if (ad->adapter_idx == i_adapter) {
            web_manager_tune_adapter(ad->id, ad->adapter_idx);
        } else {
            /* Secondary adapter: sync worker, do NOT touch primary adapter or disrupt existing streams */
            web_manager_sync_worker(ad->adapter_idx);
        }
    }
    web_state_save_json(NULL);
    return ok;
}

bool web_manager_delete_adapter(const char *id) {
    if (!id || !*id) return false;

    web_adapter_t *ad = web_state_find_adapter(id);
    if (!ad) return false;

    if (i_adapter == ad->adapter_idx) {
        web_manager_submit_action(WEB_ACTION_DISABLE, ad->adapter_idx, id);
        i_frequency = 0;
    } else {
        web_manager_stop_worker_internal(ad->adapter_idx);
    }

    bool ok = web_state_delete_adapter(id);
    web_state_save_json(NULL);
    return ok;
}

bool web_manager_toggle_adapter(const char *id, bool enable) {
    if (!id || !*id) return false;
    web_adapter_t *ad = web_state_find_adapter(id);
    if (!ad) return false;

    ad->enabled = enable;
    web_state_save_json(NULL);

    if (ad->adapter_idx == i_adapter) {
        if (enable) {
            return web_manager_submit_action(WEB_ACTION_TUNE, ad->adapter_idx, id);
        } else {
            return web_manager_submit_action(WEB_ACTION_DISABLE, ad->adapter_idx, id);
        }
    } else {
        web_manager_sync_worker(ad->adapter_idx);
        return true;
    }
}

bool web_manager_restart_adapter(const char *id) {
    if (!id || !*id) return false;
    web_adapter_t *ad = web_state_find_adapter(id);
    if (!ad) return false;

    if (ad->adapter_idx == i_adapter) {
        return web_manager_submit_action(WEB_ACTION_RESTART, ad->adapter_idx, id);
    } else {
        web_manager_stop_worker_internal(ad->adapter_idx);
        if (ad->enabled) {
            web_manager_sync_worker(ad->adapter_idx);
        }
        return true;
    }
}

void web_manager_trigger_scan(int adapter_idx) {
    web_state_log("INFO", "Channel scan triggered for Adapter %d", adapter_idx);
    if (adapter_idx == i_adapter) {
        web_manager_submit_action(WEB_ACTION_SCAN, adapter_idx, NULL);
    } else {
        pthread_t scan_th;
        pthread_create(&scan_th, NULL, scan_secondary_adapter_thread, (void *)(intptr_t)adapter_idx);
        pthread_detach(scan_th);
    }
}

/* ========================================================================= */
/*  STREAM MANAGEMENT                                                        */
/* ========================================================================= */

bool web_manager_add_stream(const web_stream_config_t *stream) {
    if (!stream || !stream->ip[0] || stream->port <= 0) return false;

    /* Register SoftCAM BISS key if provided */
    if (stream->biss_key[0] && stream->service_id > 0) {
        softcam_csa_set_key(stream->id, stream->name, stream->service_id, stream->biss_key, true);
    }

    if (stream->adapter_idx == i_adapter) {
        /* Primary adapter: output directly in Master */
        char host_str[256];
        if (stream->is_rtp) {
            snprintf(host_str, sizeof(host_str), "%s:%d", stream->ip, stream->port);
        } else {
            snprintf(host_str, sizeof(host_str), "%s:%d/udp", stream->ip, stream->port);
        }

        output_config_t config;
        config_Defaults(&config);

        if (!config_ParseHost(&config, host_str)) {
            config_Free(&config);
            web_state_log("ERROR", "Failed to parse stream destination %s", host_str);
            return false;
        }

        config.i_sid = stream->service_id;
        if (stream->service_id == 0) {
            config.b_passthrough = true;
        }
        if (stream->ttl > 0) {
            config.i_ttl = stream->ttl;
        }
        config.i_config |= OUTPUT_WATCH | OUTPUT_VALID | OUTPUT_STILL_PRESENT;

        output_t *p_output = output_Find(&config);

        if (stream->enabled) {
            if (!p_output) {
                p_output = output_Create(&config);
            }

            if (p_output) {
                free(p_output->config.psz_displayname);
                p_output->config.psz_displayname = strdup(stream->name[0] ? stream->name : stream->id);
                p_output->b_pacing = stream->pacing;
                snprintf(p_output->sz_stream_id, sizeof(p_output->sz_stream_id), "%s", stream->id);
                output_Change(p_output, &config);
                demux_Change(p_output, &config);
            }
        } else {
            if (p_output) {
                output_config_t blank_config;
                config_Init(&blank_config);
                demux_Change(p_output, &blank_config);
                config_Free(&blank_config);

                output_Close(p_output);
            }
        }

        config_Free(&config);
    } else {
        /* Secondary adapter: sync worker */
        web_state_add_or_update_stream(stream);
        web_state_save_json(NULL);
        web_manager_sync_worker(stream->adapter_idx);
        return true;
    }

    web_state_add_or_update_stream(stream);
    web_state_save_json(NULL);
    return true;
}

bool web_manager_delete_stream(const char *id) {
    if (!id || !*id) return false;
    web_stream_config_t *s = web_state_find_stream(id);
    if (!s) return false;

    int ad_idx = s->adapter_idx;
    if (ad_idx == i_adapter) {
        char host_str[256];
        if (s->is_rtp) {
            snprintf(host_str, sizeof(host_str), "%s:%d", s->ip, s->port);
        } else {
            snprintf(host_str, sizeof(host_str), "%s:%d/udp", s->ip, s->port);
        }

        output_config_t config;
        config_Defaults(&config);
        if (config_ParseHost(&config, host_str)) {
            config.i_sid = s->service_id;
            output_t *p_output = output_Find(&config);
            if (p_output) {
                output_config_t blank_config;
                config_Init(&blank_config);
                demux_Change(p_output, &blank_config);
                config_Free(&blank_config);

                output_Close(p_output);
            }
            config_Free(&config);
        }
    }

    bool ok = web_state_delete_stream(id);
    web_state_save_json(NULL);

    if (ad_idx != i_adapter) {
        web_manager_sync_worker(ad_idx);
    }
    return ok;
}

bool web_manager_toggle_stream(const char *id, bool enabled) {
    if (!id || !*id) return false;
    web_stream_config_t *s = web_state_find_stream(id);
    if (!s) return false;

    s->enabled = enabled;
    return web_manager_add_stream(s);
}

void web_manager_sync_outputs(void) {
    pthread_mutex_lock(&g_gateway.lock);
    int count = g_gateway.num_streams;
    web_stream_config_t streams_copy[MAX_WEB_STREAMS];
    memcpy(streams_copy, g_gateway.streams, count * sizeof(web_stream_config_t));

    int ad_count = g_gateway.num_adapters;
    web_adapter_t adapters_copy[MAX_WEB_ADAPTERS];
    memcpy(adapters_copy, g_gateway.adapters, ad_count * sizeof(web_adapter_t));
    pthread_mutex_unlock(&g_gateway.lock);

    /* Tune primary adapter if any */
    for (int i = 0; i < ad_count; i++) {
        if (adapters_copy[i].enabled && adapters_copy[i].adapter_idx == i_adapter) {
            web_manager_tune_adapter(adapters_copy[i].id, adapters_copy[i].adapter_idx);
            break;
        }
    }

    /* Start active streams on primary adapter */
    for (int i = 0; i < count; i++) {
        if (streams_copy[i].enabled && streams_copy[i].adapter_idx == i_adapter) {
            web_manager_add_stream(&streams_copy[i]);
        }
    }

    /* Sync workers for secondary adapters */
    web_manager_sync_workers();
}

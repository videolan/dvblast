#include "web_state.h"
#include "softcam_csa.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <math.h>

#define DVBLAST_SHM_PATH "/dev/shm/dvblast_stats.dat"

typedef struct {
    uint64_t input_bytes;
    uint64_t input_packets;
} dvb_shm_adapter_t;

typedef struct {
    char id[64];
    uint64_t bytes_sent;
    uint64_t packets_sent;
} dvb_shm_stream_t;

typedef struct {
    dvb_shm_adapter_t adapters[MAX_WEB_ADAPTERS];
    dvb_shm_stream_t streams[MAX_WEB_STREAMS];
} dvb_shm_stats_t;

static dvb_shm_stats_t *g_shm_stats = NULL;

typedef int64_t mtime_t;
mtime_t mdate(void);

gateway_state_t g_gateway;

/* CPU calculation state */
static unsigned long long prev_user = 0, prev_nice = 0, prev_system = 0, prev_idle = 0, prev_iowait = 0;
static float g_cpu_usage = 0.0f;

static void read_system_cpu(void) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return;

    char line[256];
    if (fgets(line, sizeof(line), fp)) {
        unsigned long long user, nice, system, idle, iowait;
        if (sscanf(line, "cpu %llu %llu %llu %llu %llu", &user, &nice, &system, &idle, &iowait) == 5) {
            unsigned long long total = (user - prev_user) + (nice - prev_nice) +
                                      (system - prev_system) + (idle - prev_idle) +
                                      (iowait - prev_iowait);
            unsigned long long work = (user - prev_user) + (nice - prev_nice) +
                                     (system - prev_system);
            if (total > 0) {
                g_cpu_usage = ((float)work / (float)total) * 100.0f;
            }
            prev_user = user;
            prev_nice = nice;
            prev_system = system;
            prev_idle = idle;
            prev_iowait = iowait;
        }
    }
    fclose(fp);
}

static void read_system_ram(uint64_t *out_total_kb, uint64_t *out_available_kb) {
    *out_total_kb = 0;
    *out_available_kb = 0;
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return;

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "MemTotal:", 9) == 0) {
            sscanf(line + 9, "%lu", out_total_kb);
        } else if (strncmp(line, "MemAvailable:", 13) == 0) {
            sscanf(line + 13, "%lu", out_available_kb);
        }
    }
    fclose(fp);
}

void web_state_init(int http_port, const char *http_host, const char *config_file) {
    memset(&g_gateway, 0, sizeof(g_gateway));
    pthread_mutex_init(&g_gateway.lock, NULL);

    snprintf(g_gateway.server_name, sizeof(g_gateway.server_name), "DVBlast Web Gateway");
    g_gateway.http_port = (http_port > 0) ? http_port : 8088;
    snprintf(g_gateway.http_host, sizeof(g_gateway.http_host), "%s", (http_host && *http_host) ? http_host : "0.0.0.0");
    snprintf(g_gateway.config_file, sizeof(g_gateway.config_file), "%s", (config_file && *config_file) ? config_file : "dvblast.json");
    snprintf(g_gateway.access_log, sizeof(g_gateway.access_log), "/var/log/dvblast_web.log");
    g_gateway.start_time = time(NULL);
    g_gateway.last_bitrate_calc_us = 0;

    /* Clean initial state: 0 adapters, 0 streams */
    g_gateway.num_adapters = 0;
    g_gateway.num_streams = 0;

    read_system_cpu();
    web_state_log("INFO", "DVBlast Gateway initialized on port %d (Clean state: 0 adapters, 0 streams)", g_gateway.http_port);
}

void web_state_cleanup(void) {
    if (g_shm_stats) {
        munmap(g_shm_stats, sizeof(dvb_shm_stats_t));
        g_shm_stats = NULL;
        unlink(DVBLAST_SHM_PATH);
    }
    pthread_mutex_destroy(&g_gateway.lock);
}

void web_state_log(const char *level, const char *fmt, ...) {
    char buf[LOG_MSG_MAX];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    time_t now = time(NULL);

    pthread_mutex_lock(&g_gateway.lock);
    int idx = (g_gateway.log_head + g_gateway.log_count) % MAX_LOG_ENTRIES;
    if (g_gateway.log_count >= MAX_LOG_ENTRIES) {
        g_gateway.log_head = (g_gateway.log_head + 1) % MAX_LOG_ENTRIES;
    } else {
        g_gateway.log_count++;
    }

    web_log_entry_t *entry = &g_gateway.logs[idx];
    entry->id = ++g_gateway.next_log_id;
    entry->timestamp = now;
    snprintf(entry->level, sizeof(entry->level), "%s", level ? level : "INFO");
    snprintf(entry->message, sizeof(entry->message), "%s", buf);
    pthread_mutex_unlock(&g_gateway.lock);

    char timestr[32];
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", &tm_buf);
    fprintf(stderr, "[%s] [%s] %s\n", timestr, level ? level : "INFO", buf);
}

/* ========================================================================= */
/*  ADAPTERS MANAGEMENT                                                      */
/* ========================================================================= */

bool web_state_add_or_update_adapter(const web_adapter_t *ad) {
    if (!ad || !ad->id[0]) return false;
    web_adapter_t copy = *ad;
    pthread_mutex_lock(&g_gateway.lock);

    web_adapter_t *target = NULL;
    for (int i = 0; i < g_gateway.num_adapters; i++) {
        if (strcmp(g_gateway.adapters[i].id, copy.id) == 0) {
            target = &g_gateway.adapters[i];
            break;
        }
    }

    if (!target) {
        if (g_gateway.num_adapters >= MAX_WEB_ADAPTERS) {
            pthread_mutex_unlock(&g_gateway.lock);
            return false;
        }
        target = &g_gateway.adapters[g_gateway.num_adapters++];
        memset(target, 0, sizeof(*target));
    }

    snprintf(target->id, sizeof(target->id), "%s", copy.id);
    snprintf(target->name, sizeof(target->name), "%s", copy.name[0] ? copy.name : copy.id);
    target->enabled = copy.enabled;
    target->adapter_idx = copy.adapter_idx;
    target->frontend_idx = copy.frontend_idx;
    snprintf(target->delsys, sizeof(target->delsys), "%s", copy.delsys[0] ? copy.delsys : "DVBT2");
    target->frequency = copy.frequency;
    target->symbol_rate = copy.symbol_rate;
    target->bandwidth = (copy.bandwidth > 0) ? copy.bandwidth : 8;
    snprintf(target->polarization, sizeof(target->polarization), "%s", copy.polarization[0] ? copy.polarization : "H");
    target->voltage = copy.voltage;
    target->tone = copy.tone;
    target->diseqc = copy.diseqc;
    snprintf(target->lnb_type, sizeof(target->lnb_type), "%s", copy.lnb_type);
    snprintf(target->modulation, sizeof(target->modulation), "%s", copy.modulation);
    target->multistream_id = copy.multistream_id;

    pthread_mutex_unlock(&g_gateway.lock);
    web_state_log("INFO", "Adapter %s (%s, Dev: /dev/dvb/adapter%d, Freq: %llu Hz) configured",
                  target->id, target->name, target->adapter_idx, (unsigned long long)target->frequency);
    return true;
}

bool web_state_delete_adapter(const char *id) {
    if (!id || !*id) return false;
    pthread_mutex_lock(&g_gateway.lock);

    int idx = -1;
    for (int i = 0; i < g_gateway.num_adapters; i++) {
        if (strcmp(g_gateway.adapters[i].id, id) == 0) {
            idx = i;
            break;
        }
    }

    if (idx >= 0) {
        char name[128];
        snprintf(name, sizeof(name), "%s", g_gateway.adapters[idx].name);
        for (int i = idx; i < g_gateway.num_adapters - 1; i++) {
            g_gateway.adapters[i] = g_gateway.adapters[i + 1];
        }
        g_gateway.num_adapters--;
        memset(&g_gateway.adapters[g_gateway.num_adapters], 0, sizeof(web_adapter_t));
        pthread_mutex_unlock(&g_gateway.lock);
        web_state_log("INFO", "Adapter %s (%s) deleted", id, name);
        return true;
    }

    pthread_mutex_unlock(&g_gateway.lock);
    return false;
}

web_adapter_t *web_state_find_adapter(const char *id) {
    if (!id) return NULL;
    for (int i = 0; i < g_gateway.num_adapters; i++) {
        if (strcmp(g_gateway.adapters[i].id, id) == 0) return &g_gateway.adapters[i];
    }
    return NULL;
}

web_adapter_t *web_state_find_adapter_by_idx(int adapter_idx) {
    /* Prefer enabled adapter on this index */
    for (int i = 0; i < g_gateway.num_adapters; i++) {
        if (g_gateway.adapters[i].adapter_idx == adapter_idx && g_gateway.adapters[i].enabled) {
            return &g_gateway.adapters[i];
        }
    }
    /* Fallback to any adapter with this index */
    for (int i = 0; i < g_gateway.num_adapters; i++) {
        if (g_gateway.adapters[i].adapter_idx == adapter_idx) {
            return &g_gateway.adapters[i];
        }
    }
    return NULL;
}

void web_state_update_adapter_signal_ext(int adapter_idx, bool locked, uint16_t strength, float snr_db, float signal_dbm, uint32_t ber, uint32_t ucb) {
    pthread_mutex_lock(&g_gateway.lock);
    for (int i = 0; i < g_gateway.num_adapters; i++) {
        if (g_gateway.adapters[i].adapter_idx == adapter_idx && g_gateway.adapters[i].enabled) {
            g_gateway.adapters[i].locked = locked;
            g_gateway.adapters[i].signal_percent = strength;
            g_gateway.adapters[i].snr_db = snr_db;
            g_gateway.adapters[i].signal_dbm = signal_dbm;
            g_gateway.adapters[i].snr_raw = (uint16_t)(snr_db * 10.0f);
            g_gateway.adapters[i].ber = ber;
            g_gateway.adapters[i].ucb = ucb;
            g_gateway.adapters[i].last_status_time = time(NULL);
        } else if (!g_gateway.adapters[i].enabled) {
            g_gateway.adapters[i].locked = false;
            g_gateway.adapters[i].signal_percent = 0;
            g_gateway.adapters[i].snr_db = 0.0f;
            g_gateway.adapters[i].signal_dbm = -100.0f;
            g_gateway.adapters[i].snr_raw = 0;
            g_gateway.adapters[i].bitrate_bps = 0;
        }
    }
    pthread_mutex_unlock(&g_gateway.lock);
}

void web_state_update_adapter_signal(int adapter_idx, bool locked, uint16_t strength, uint16_t snr, uint32_t ber, uint32_t ucb) {
    float snr_db = (float)snr / 655.35f;
    float signal_dbm = -100.0f + (float)strength;
    web_state_update_adapter_signal_ext(adapter_idx, locked, strength, snr_db, signal_dbm, ber, ucb);
}

void web_state_clear_adapter_channels(int adapter_idx) {
    pthread_mutex_lock(&g_gateway.lock);
    for (int i = 0; i < g_gateway.num_adapters; i++) {
        if (g_gateway.adapters[i].adapter_idx == adapter_idx) {
            g_gateway.adapters[i].num_channels = 0;
            memset(g_gateway.adapters[i].channels, 0, sizeof(g_gateway.adapters[i].channels));
            break;
        }
    }
    pthread_mutex_unlock(&g_gateway.lock);
}

void web_state_add_adapter_channel(int adapter_idx, const channel_info_t *ch) {
    if (!ch || ch->service_id == 0) return;
    pthread_mutex_lock(&g_gateway.lock);

    web_adapter_t *ad = NULL;
    for (int i = 0; i < g_gateway.num_adapters; i++) {
        if (g_gateway.adapters[i].adapter_idx == adapter_idx && g_gateway.adapters[i].enabled) {
            ad = &g_gateway.adapters[i];
            break;
        }
    }

    if (!ad) {
        pthread_mutex_unlock(&g_gateway.lock);
        return;
    }

    channel_info_t *target = NULL;
    for (int c = 0; c < ad->num_channels; c++) {
        if (ad->channels[c].service_id == ch->service_id) {
            target = &ad->channels[c];
            break;
        }
    }

    if (!target) {
        if (ad->num_channels >= MAX_DISCOVERED_CHANNELS) {
            pthread_mutex_unlock(&g_gateway.lock);
            return;
        }
        target = &ad->channels[ad->num_channels++];
    }

    target->service_id = ch->service_id;
    if (ch->name[0]) snprintf(target->name, sizeof(target->name), "%s", ch->name);
    else if (!target->name[0]) snprintf(target->name, sizeof(target->name), "Program %u", ch->service_id);
    if (ch->provider[0]) snprintf(target->provider, sizeof(target->provider), "%s", ch->provider);
    if (ch->pmt_pid > 0) target->pmt_pid = ch->pmt_pid;
    if (ch->video_pid > 0) target->video_pid = ch->video_pid;
    if (ch->video_type > 0) target->video_type = ch->video_type;
    if (ch->num_audio_pids > 0) {
        target->num_audio_pids = ch->num_audio_pids;
        memcpy(target->audio_pids, ch->audio_pids, sizeof(target->audio_pids));
        memcpy(target->audio_types, ch->audio_types, sizeof(target->audio_types));
    }
    target->scrambled = ch->scrambled;
    target->last_seen = time(NULL);

    pthread_mutex_unlock(&g_gateway.lock);
}

void web_state_add_channel(const channel_info_t *ch) {
    extern int i_adapter;
    web_state_add_adapter_channel(i_adapter, ch);
}

/* ========================================================================= */
/*  STREAMS MANAGEMENT                                                       */
/* ========================================================================= */

bool web_state_add_or_update_stream(const web_stream_config_t *stream) {
    if (!stream || !stream->id[0] || !stream->ip[0] || stream->port <= 0) return false;
    web_stream_config_t copy = *stream;
    pthread_mutex_lock(&g_gateway.lock);

    web_stream_config_t *target = NULL;
    for (int i = 0; i < g_gateway.num_streams; i++) {
        if (strcmp(g_gateway.streams[i].id, copy.id) == 0) {
            target = &g_gateway.streams[i];
            break;
        }
    }

    if (!target) {
        if (g_gateway.num_streams >= MAX_WEB_STREAMS) {
            pthread_mutex_unlock(&g_gateway.lock);
            return false;
        }
        target = &g_gateway.streams[g_gateway.num_streams++];
        memset(target, 0, sizeof(*target));
        target->created_at = time(NULL);
    }

    snprintf(target->id, sizeof(target->id), "%s", copy.id);
    snprintf(target->name, sizeof(target->name), "%s", copy.name[0] ? copy.name : copy.id);
    target->enabled = copy.enabled;
    snprintf(target->type, sizeof(target->type), "%s", copy.type[0] ? copy.type : (copy.service_id == 0 ? "mpts" : "spts"));
    snprintf(target->input_url, sizeof(target->input_url), "%s", copy.input_url);
    snprintf(target->output_url, sizeof(target->output_url), "%s", copy.output_url);
    target->adapter_idx = copy.adapter_idx;
    target->service_id = copy.service_id;
    snprintf(target->ip, sizeof(target->ip), "%s", copy.ip);
    target->port = copy.port;
    target->ttl = (copy.ttl > 0) ? copy.ttl : 16;
    target->is_rtp = copy.is_rtp;
    target->pacing = copy.pacing;
    target->bitrate_bps = copy.bitrate_bps;
    if (copy.biss_key[0]) snprintf(target->biss_key, sizeof(target->biss_key), "%s", copy.biss_key);

    pthread_mutex_unlock(&g_gateway.lock);
    web_state_log("INFO", "Stream %s (%s, Dest: %s:%d, SID: %u) saved",
                  target->id, target->name, target->ip, target->port, target->service_id);
    return true;
}

bool web_state_delete_stream(const char *id) {
    if (!id || !*id) return false;
    pthread_mutex_lock(&g_gateway.lock);

    int idx = -1;
    for (int i = 0; i < g_gateway.num_streams; i++) {
        if (strcmp(g_gateway.streams[i].id, id) == 0) {
            idx = i;
            break;
        }
    }

    if (idx >= 0) {
        char name[128];
        snprintf(name, sizeof(name), "%s", g_gateway.streams[idx].name);
        for (int i = idx; i < g_gateway.num_streams - 1; i++) {
            g_gateway.streams[i] = g_gateway.streams[i + 1];
        }
        g_gateway.num_streams--;
        memset(&g_gateway.streams[g_gateway.num_streams], 0, sizeof(web_stream_config_t));
        pthread_mutex_unlock(&g_gateway.lock);
        web_state_log("INFO", "Stream %s (%s) deleted", id, name);
        return true;
    }

    pthread_mutex_unlock(&g_gateway.lock);
    return false;
}

web_stream_config_t *web_state_find_stream(const char *id) {
    if (!id || !*id) return NULL;
    for (int i = 0; i < g_gateway.num_streams; i++) {
        if (strcmp(g_gateway.streams[i].id, id) == 0 ||
            (g_gateway.streams[i].name[0] && strcmp(g_gateway.streams[i].name, id) == 0)) {
            return &g_gateway.streams[i];
        }
    }
    return NULL;
}

web_stream_config_t *web_state_find_stream_by_dest(const char *ip, int port) {
    if (!ip || port <= 0) return NULL;
    for (int i = 0; i < g_gateway.num_streams; i++) {
        if (g_gateway.streams[i].port == port && strcmp(g_gateway.streams[i].ip, ip) == 0) {
            return &g_gateway.streams[i];
        }
    }
    return NULL;
}

/* ========================================================================= */
/*  STATS & TELEMETRY (CROSS-PROCESS IPC VIA SHARED MEMORY)                  */
/* ========================================================================= */

void web_state_init_shm(bool is_master) {
    if (g_shm_stats) return;

    int fd = open(DVBLAST_SHM_PATH, O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        fd = open("/tmp/dvblast_stats.dat", O_RDWR | O_CREAT, 0666);
    }
    if (fd < 0) return;

    if (is_master) {
        if (ftruncate(fd, sizeof(dvb_shm_stats_t)) != 0) {
            close(fd);
            return;
        }
    } else {
        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(dvb_shm_stats_t)) {
            if (ftruncate(fd, sizeof(dvb_shm_stats_t)) != 0) {
                close(fd);
                return;
            }
        }
    }

    void *ptr = mmap(NULL, sizeof(dvb_shm_stats_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (ptr == MAP_FAILED) {
        g_shm_stats = NULL;
        return;
    }

    g_shm_stats = (dvb_shm_stats_t *)ptr;
    if (is_master) {
        memset(g_shm_stats, 0, sizeof(dvb_shm_stats_t));
    }
}

void web_state_update_input_stats(int adapter_idx, uint64_t bytes, uint64_t packets) {
    if (g_shm_stats && adapter_idx >= 0 && adapter_idx < MAX_WEB_ADAPTERS) {
        __sync_fetch_and_add(&g_shm_stats->adapters[adapter_idx].input_bytes, bytes);
        __sync_fetch_and_add(&g_shm_stats->adapters[adapter_idx].input_packets, packets);
    }
}

void web_state_update_output_stats(uint64_t bytes, uint64_t packets) {
    (void)bytes;
    (void)packets;
}

void web_state_record_output(const char *stream_id_or_name, uint64_t bytes, uint64_t packets) {
    if (g_shm_stats && stream_id_or_name && *stream_id_or_name) {
        for (int i = 0; i < MAX_WEB_STREAMS; i++) {
            if (strcmp(g_shm_stats->streams[i].id, stream_id_or_name) == 0) {
                __sync_fetch_and_add(&g_shm_stats->streams[i].bytes_sent, bytes);
                __sync_fetch_and_add(&g_shm_stats->streams[i].packets_sent, packets);
                return;
            }
            if (g_shm_stats->streams[i].id[0] == '\0') {
                snprintf(g_shm_stats->streams[i].id, sizeof(g_shm_stats->streams[i].id), "%s", stream_id_or_name);
                __sync_fetch_and_add(&g_shm_stats->streams[i].bytes_sent, bytes);
                __sync_fetch_and_add(&g_shm_stats->streams[i].packets_sent, packets);
                return;
            }
        }
    }
}

void web_state_calc_bitrates(void) {
    read_system_cpu();

    mtime_t now_us = mdate();
    pthread_mutex_lock(&g_gateway.lock);

    /* Sync live statistics across all worker processes and Master from Shared Memory */
    if (g_shm_stats) {
        uint64_t cur_in = 0;
        uint64_t cur_in_pkts = 0;
        uint64_t cur_out = 0;
        uint64_t cur_out_pkts = 0;

        for (int i = 0; i < g_gateway.num_adapters; i++) {
            int a_idx = g_gateway.adapters[i].adapter_idx;
            if (a_idx >= 0 && a_idx < MAX_WEB_ADAPTERS) {
                g_gateway.adapters[i].total_bytes = g_shm_stats->adapters[a_idx].input_bytes;
                g_gateway.adapters[i].total_packets = g_shm_stats->adapters[a_idx].input_packets;
                if (g_gateway.adapters[i].enabled) {
                    cur_in += g_gateway.adapters[i].total_bytes;
                    cur_in_pkts += g_gateway.adapters[i].total_packets;
                }
            }
        }
        g_gateway.total_input_bytes = cur_in;
        g_gateway.total_input_packets = cur_in_pkts;

        for (int i = 0; i < g_gateway.num_streams; i++) {
            web_stream_config_t *ws = &g_gateway.streams[i];
            char dest_str[128];
            snprintf(dest_str, sizeof(dest_str), "%s:%d", ws->ip, ws->port);

            for (int s = 0; s < MAX_WEB_STREAMS; s++) {
                if (g_shm_stats->streams[s].id[0] == '\0') break;
                if (strcmp(g_shm_stats->streams[s].id, ws->id) == 0 ||
                    (ws->name[0] && strcmp(g_shm_stats->streams[s].id, ws->name) == 0) ||
                    strcmp(g_shm_stats->streams[s].id, dest_str) == 0) {
                    ws->bytes_sent = g_shm_stats->streams[s].bytes_sent;
                    ws->packets_sent = g_shm_stats->streams[s].packets_sent;
                    break;
                }
            }
            if (ws->enabled) {
                cur_out += ws->bytes_sent;
                cur_out_pkts += ws->packets_sent;
            }
        }
        g_gateway.total_output_bytes = cur_out;
        g_gateway.total_output_packets = cur_out_pkts;
    }

    if (g_gateway.last_bitrate_calc_us == 0) {
        g_gateway.last_bitrate_calc_us = now_us;
        g_gateway.last_calc_in_bytes = g_gateway.total_input_bytes;
        g_gateway.last_calc_out_bytes = g_gateway.total_output_bytes;
        for (int i = 0; i < g_gateway.num_adapters; i++) {
            g_gateway.adapters[i].last_calc_bytes = g_gateway.adapters[i].total_bytes;
        }
        for (int i = 0; i < g_gateway.num_streams; i++) {
            g_gateway.streams[i].last_calc_bytes = g_gateway.streams[i].bytes_sent;
        }
        pthread_mutex_unlock(&g_gateway.lock);
        return;
    }

    mtime_t elapsed_us = now_us - g_gateway.last_bitrate_calc_us;
    if (elapsed_us < 800000) {
        /* Maintain steady calculation window (>= 800ms) */
        pthread_mutex_unlock(&g_gateway.lock);
        return;
    }

    /* Total Input Bitrate (bps) */
    uint64_t cur_in = g_gateway.total_input_bytes;
    uint64_t delta_in = (cur_in >= g_gateway.last_calc_in_bytes) ? (cur_in - g_gateway.last_calc_in_bytes) : 0;
    g_gateway.total_input_bitrate_bps = (uint32_t)((delta_in * 8ULL * 1000000ULL) / elapsed_us);
    g_gateway.last_calc_in_bytes = cur_in;

    /* Total Output Bitrate (bps) */
    uint64_t cur_out = g_gateway.total_output_bytes;
    uint64_t delta_out = (cur_out >= g_gateway.last_calc_out_bytes) ? (cur_out - g_gateway.last_calc_out_bytes) : 0;
    g_gateway.total_output_bitrate_bps = (uint32_t)((delta_out * 8ULL * 1000000ULL) / elapsed_us);
    g_gateway.last_calc_out_bytes = cur_out;

    /* Per-Adapter Bitrates */
    for (int i = 0; i < g_gateway.num_adapters; i++) {
        web_adapter_t *ad = &g_gateway.adapters[i];
        if (!ad->enabled || !ad->locked) {
            ad->bitrate_bps = 0;
            ad->last_calc_bytes = ad->total_bytes;
            continue;
        }
        uint64_t ad_cur = ad->total_bytes;
        uint64_t ad_delta = (ad_cur >= ad->last_calc_bytes) ? (ad_cur - ad->last_calc_bytes) : 0;
        ad->bitrate_bps = (uint32_t)((ad_delta * 8ULL * 1000000ULL) / elapsed_us);
        ad->last_calc_bytes = ad_cur;
    }

    /* Per-Stream Bitrates */
    for (int i = 0; i < g_gateway.num_streams; i++) {
        web_stream_config_t *st = &g_gateway.streams[i];
        if (!st->enabled) {
            st->bitrate_bps = 0;
            st->last_calc_bytes = st->bytes_sent;
            continue;
        }
        uint64_t st_cur = st->bytes_sent;
        uint64_t st_delta = (st_cur >= st->last_calc_bytes) ? (st_cur - st->last_calc_bytes) : 0;
        st->bitrate_bps = (uint32_t)((st_delta * 8ULL * 1000000ULL) / elapsed_us);
        st->last_calc_bytes = st_cur;
    }

    g_gateway.last_bitrate_calc_us = now_us;
    pthread_mutex_unlock(&g_gateway.lock);
}

/* ========================================================================= */
/*  PERSISTENCE (JSON)                                                       */
/* ========================================================================= */

bool web_state_save_json(const char *path) {
    pthread_mutex_lock(&g_gateway.lock);
    const char *save_file = (path && *path) ? path : g_gateway.config_file;

    json_t *root = json_object();
    json_object_set_new(root, "version", json_string("DVBlast-Web-2.0"));
    json_object_set_new(root, "server_name", json_string(g_gateway.server_name));
    json_object_set_new(root, "http_port", json_integer(g_gateway.http_port));
    json_object_set_new(root, "http_host", json_string(g_gateway.http_host));

    /* Adapters array */
    json_t *adapters_arr = json_array();
    for (int i = 0; i < g_gateway.num_adapters; i++) {
        web_adapter_t *ad = &g_gateway.adapters[i];
        json_t *a_obj = json_object();
        json_object_set_new(a_obj, "id", json_string(ad->id));
        json_object_set_new(a_obj, "name", json_string(ad->name));
        json_object_set_new(a_obj, "enabled", json_boolean(ad->enabled));
        json_object_set_new(a_obj, "adapter_idx", json_integer(ad->adapter_idx));
        json_object_set_new(a_obj, "frontend_idx", json_integer(ad->frontend_idx));
        json_object_set_new(a_obj, "delsys", json_string(ad->delsys));
        json_object_set_new(a_obj, "frequency", json_integer((json_int_t)ad->frequency));
        json_object_set_new(a_obj, "symbol_rate", json_integer(ad->symbol_rate));
        json_object_set_new(a_obj, "bandwidth", json_integer(ad->bandwidth));
        json_object_set_new(a_obj, "polarization", json_string(ad->polarization));
        json_object_set_new(a_obj, "voltage", json_integer(ad->voltage));
        json_object_set_new(a_obj, "tone", json_integer(ad->tone));
        json_object_set_new(a_obj, "diseqc", json_integer(ad->diseqc));
        json_object_set_new(a_obj, "lnb_type", json_string(ad->lnb_type));
        json_object_set_new(a_obj, "modulation", json_string(ad->modulation));
        json_object_set_new(a_obj, "multistream_id", json_integer(ad->multistream_id));
        json_array_append_new(adapters_arr, a_obj);
    }
    json_object_set_new(root, "adapters", adapters_arr);

    /* Streams array */
    json_t *streams_arr = json_array();
    for (int i = 0; i < g_gateway.num_streams; i++) {
        web_stream_config_t *s = &g_gateway.streams[i];
        json_t *s_obj = json_object();
        json_object_set_new(s_obj, "id", json_string(s->id));
        json_object_set_new(s_obj, "name", json_string(s->name));
        json_object_set_new(s_obj, "enabled", json_boolean(s->enabled));
        json_object_set_new(s_obj, "type", json_string(s->type));
        json_object_set_new(s_obj, "input_url", json_string(s->input_url));
        json_object_set_new(s_obj, "output_url", json_string(s->output_url));
        json_object_set_new(s_obj, "adapter_idx", json_integer(s->adapter_idx));
        json_object_set_new(s_obj, "service_id", json_integer(s->service_id));
        json_object_set_new(s_obj, "ip", json_string(s->ip));
        json_object_set_new(s_obj, "port", json_integer(s->port));
        json_object_set_new(s_obj, "ttl", json_integer(s->ttl));
        json_object_set_new(s_obj, "is_rtp", json_boolean(s->is_rtp));
        json_object_set_new(s_obj, "pacing", json_boolean(s->pacing));
        json_object_set_new(s_obj, "biss_key", json_string(s->biss_key));
        json_array_append_new(streams_arr, s_obj);
    }
    json_object_set_new(root, "streams", streams_arr);

    /* SoftCAM Keys */
    json_t *softcam_arr = softcam_csa_to_json();
    json_object_set_new(root, "descramble", softcam_arr);

    char tmp_path[300];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", save_file);
    int res = json_dump_file(root, tmp_path, JSON_INDENT(2) | JSON_PRESERVE_ORDER);
    json_decref(root);

    if (res == 0) {
        rename(tmp_path, save_file);
        pthread_mutex_unlock(&g_gateway.lock);
        web_state_log("INFO", "Configuration successfully saved to %s", save_file);
        return true;
    }

    pthread_mutex_unlock(&g_gateway.lock);
    web_state_log("ERROR", "Failed to save configuration to %s", save_file);
    return false;
}

bool web_state_load_json(const char *path) {
    const char *load_file = (path && *path) ? path : g_gateway.config_file;
    json_error_t err;
    json_t *root = json_load_file(load_file, 0, &err);
    if (!root) {
        web_state_log("WARN", "Config file %s not loaded (%s), keeping clean state", load_file, err.text);
        return false;
    }

    pthread_mutex_lock(&g_gateway.lock);

    const char *s_name = json_string_value(json_object_get(root, "server_name"));
    if (s_name) snprintf(g_gateway.server_name, sizeof(g_gateway.server_name), "%s", s_name);

    /* Adapters list */
    json_t *a_arr = json_object_get(root, "adapters");
    if (json_is_array(a_arr)) {
        g_gateway.num_adapters = 0;
        size_t idx;
        json_t *val;
        json_array_foreach(a_arr, idx, val) {
            if (g_gateway.num_adapters >= MAX_WEB_ADAPTERS) break;
            web_adapter_t *ad = &g_gateway.adapters[g_gateway.num_adapters++];
            memset(ad, 0, sizeof(*ad));

            const char *id = json_string_value(json_object_get(val, "id"));
            const char *nm = json_string_value(json_object_get(val, "name"));
            const char *del = json_string_value(json_object_get(val, "delsys"));
            const char *pol = json_string_value(json_object_get(val, "polarization"));
            const char *lnb = json_string_value(json_object_get(val, "lnb_type"));
            const char *mod = json_string_value(json_object_get(val, "modulation"));

            if (id) snprintf(ad->id, sizeof(ad->id), "%s", id);
            if (nm) snprintf(ad->name, sizeof(ad->name), "%s", nm);
            if (del) snprintf(ad->delsys, sizeof(ad->delsys), "%s", del);
            if (pol) snprintf(ad->polarization, sizeof(ad->polarization), "%s", pol);
            if (lnb) snprintf(ad->lnb_type, sizeof(ad->lnb_type), "%s", lnb);
            if (mod) snprintf(ad->modulation, sizeof(ad->modulation), "%s", mod);

            ad->enabled = !json_is_false(json_object_get(val, "enabled"));
            ad->adapter_idx = (int)json_integer_value(json_object_get(val, "adapter_idx"));
            ad->frontend_idx = (int)json_integer_value(json_object_get(val, "frontend_idx"));
            ad->frequency = (uint64_t)json_integer_value(json_object_get(val, "frequency"));
            ad->symbol_rate = (uint32_t)json_integer_value(json_object_get(val, "symbol_rate"));
            ad->bandwidth = (int)json_integer_value(json_object_get(val, "bandwidth"));
            ad->voltage = (int)json_integer_value(json_object_get(val, "voltage"));
            ad->tone = (int)json_integer_value(json_object_get(val, "tone"));
            ad->diseqc = (int)json_integer_value(json_object_get(val, "diseqc"));
            ad->multistream_id = (int)json_integer_value(json_object_get(val, "multistream_id"));
        }
    }

    /* Streams list */
    json_t *s_arr = json_object_get(root, "streams");
    if (json_is_array(s_arr)) {
        g_gateway.num_streams = 0;
        size_t idx;
        json_t *val;
        json_array_foreach(s_arr, idx, val) {
            if (g_gateway.num_streams >= MAX_WEB_STREAMS) break;
            web_stream_config_t *s = &g_gateway.streams[g_gateway.num_streams++];
            memset(s, 0, sizeof(*s));

            const char *id = json_string_value(json_object_get(val, "id"));
            const char *nm = json_string_value(json_object_get(val, "name"));
            const char *tp = json_string_value(json_object_get(val, "type"));
            const char *in = json_string_value(json_object_get(val, "input_url"));
            const char *out = json_string_value(json_object_get(val, "output_url"));
            const char *ip = json_string_value(json_object_get(val, "ip"));
            const char *bk = json_string_value(json_object_get(val, "biss_key"));

            if (id) snprintf(s->id, sizeof(s->id), "%s", id);
            if (nm) snprintf(s->name, sizeof(s->name), "%s", nm);
            if (tp) snprintf(s->type, sizeof(s->type), "%s", tp);
            if (in) snprintf(s->input_url, sizeof(s->input_url), "%s", in);
            if (out) snprintf(s->output_url, sizeof(s->output_url), "%s", out);
            if (ip) snprintf(s->ip, sizeof(s->ip), "%s", ip);
            if (bk) snprintf(s->biss_key, sizeof(s->biss_key), "%s", bk);

            s->enabled = !json_is_false(json_object_get(val, "enabled"));
            s->adapter_idx = (int)json_integer_value(json_object_get(val, "adapter_idx"));
            s->service_id = (uint16_t)json_integer_value(json_object_get(val, "service_id"));
            s->port = (int)json_integer_value(json_object_get(val, "port"));
            s->ttl = (int)json_integer_value(json_object_get(val, "ttl"));
            if (s->ttl <= 0) s->ttl = 16;
            s->is_rtp = json_is_true(json_object_get(val, "is_rtp"));
            s->pacing = !json_is_false(json_object_get(val, "pacing"));
            s->created_at = time(NULL);
        }
    }

    /* SoftCAM Keys */
    json_t *desc = json_object_get(root, "descramble");
    if (json_is_array(desc)) {
        softcam_csa_from_json(desc);
    }

    pthread_mutex_unlock(&g_gateway.lock);
    json_decref(root);
    web_state_log("INFO", "Configuration loaded from %s (Adapters: %d, Streams: %d)",
                  load_file, g_gateway.num_adapters, g_gateway.num_streams);
    return true;
}

/* ========================================================================= */
/*  JSON SERIALIZERS                                                         */
/* ========================================================================= */

json_t *web_state_telemetry_to_json(void) {
    uint64_t ram_total_kb = 0, ram_avail_kb = 0;
    read_system_ram(&ram_total_kb, &ram_avail_kb);
    uint64_t ram_used_kb = (ram_total_kb > ram_avail_kb) ? (ram_total_kb - ram_avail_kb) : 0;
    float ram_percent = (ram_total_kb > 0) ? (((float)ram_used_kb / (float)ram_total_kb) * 100.0f) : 0.0f;

    pthread_mutex_lock(&g_gateway.lock);

    json_t *root = json_object();
    json_object_set_new(root, "server_name", json_string(g_gateway.server_name));
    json_object_set_new(root, "uptime_seconds", json_integer((json_int_t)(time(NULL) - g_gateway.start_time)));
    json_object_set_new(root, "cpu_usage_percent", json_real(roundf(g_cpu_usage * 10.0f) / 10.0f));
    json_object_set_new(root, "ram_used_mb", json_integer((json_int_t)(ram_used_kb / 1024)));
    json_object_set_new(root, "ram_total_mb", json_integer((json_int_t)(ram_total_kb / 1024)));
    json_object_set_new(root, "ram_percent", json_real(roundf(ram_percent * 10.0f) / 10.0f));

    json_object_set_new(root, "total_input_bytes", json_integer((json_int_t)g_gateway.total_input_bytes));
    json_object_set_new(root, "total_input_packets", json_integer((json_int_t)g_gateway.total_input_packets));
    json_object_set_new(root, "input_bitrate_bps", json_integer(g_gateway.total_input_bitrate_bps));
    json_object_set_new(root, "input_bitrate_kbps", json_integer(g_gateway.total_input_bitrate_bps / 1000));
    json_object_set_new(root, "input_bitrate_mbps", json_real((double)g_gateway.total_input_bitrate_bps / 1000000.0));

    json_object_set_new(root, "total_output_bytes", json_integer((json_int_t)g_gateway.total_output_bytes));
    json_object_set_new(root, "total_output_packets", json_integer((json_int_t)g_gateway.total_output_packets));
    json_object_set_new(root, "output_bitrate_bps", json_integer(g_gateway.total_output_bitrate_bps));
    json_object_set_new(root, "output_bitrate_kbps", json_integer(g_gateway.total_output_bitrate_bps / 1000));
    json_object_set_new(root, "output_bitrate_mbps", json_real((double)g_gateway.total_output_bitrate_bps / 1000000.0));

    json_object_set_new(root, "adapters_count", json_integer(g_gateway.num_adapters));
    json_object_set_new(root, "active_streams_count", json_integer(g_gateway.num_streams));

    int total_ch = 0;
    for (int i = 0; i < g_gateway.num_adapters; i++) {
        total_ch += g_gateway.adapters[i].num_channels;
    }
    json_object_set_new(root, "discovered_channels_count", json_integer(total_ch));

    pthread_mutex_unlock(&g_gateway.lock);
    return root;
}

json_t *web_state_adapters_to_json(void) {
    pthread_mutex_lock(&g_gateway.lock);
    json_t *arr = json_array();

    for (int i = 0; i < g_gateway.num_adapters; i++) {
        web_adapter_t *ad = &g_gateway.adapters[i];
        json_t *obj = json_object();
        json_object_set_new(obj, "id", json_string(ad->id));
        json_object_set_new(obj, "name", json_string(ad->name));
        json_object_set_new(obj, "enabled", json_boolean(ad->enabled));
        json_object_set_new(obj, "adapter_idx", json_integer(ad->adapter_idx));
        json_object_set_new(obj, "frontend_idx", json_integer(ad->frontend_idx));
        json_object_set_new(obj, "delsys", json_string(ad->delsys));
        json_object_set_new(obj, "frequency_hz", json_integer((json_int_t)ad->frequency));
        json_object_set_new(obj, "frequency_mhz", json_real((double)ad->frequency / 1000000.0));
        json_object_set_new(obj, "symbol_rate", json_integer(ad->symbol_rate));
        json_object_set_new(obj, "bandwidth_mhz", json_integer(ad->bandwidth));
        json_object_set_new(obj, "polarization", json_string(ad->polarization));
        json_object_set_new(obj, "voltage", json_integer(ad->voltage));
        json_object_set_new(obj, "tone", json_integer(ad->tone));
        json_object_set_new(obj, "diseqc", json_integer(ad->diseqc));
        json_object_set_new(obj, "lnb_type", json_string(ad->lnb_type));
        json_object_set_new(obj, "modulation", json_string(ad->modulation));
        json_object_set_new(obj, "multistream_id", json_integer(ad->multistream_id));

        /* Telemetry */
        json_object_set_new(obj, "locked", json_boolean(ad->locked));
        json_object_set_new(obj, "snr_raw", json_integer(ad->snr_raw));
        json_object_set_new(obj, "snr_db", json_real((double)ad->snr_db));
        json_object_set_new(obj, "signal_percent", json_integer(ad->signal_percent));
        json_object_set_new(obj, "signal_dbm", json_real((double)ad->signal_dbm));
        json_object_set_new(obj, "ber", json_integer(ad->ber));
        json_object_set_new(obj, "ucb", json_integer(ad->ucb));
        json_object_set_new(obj, "bitrate_bps", json_integer(ad->bitrate_bps));
        json_object_set_new(obj, "bitrate_kbps", json_integer(ad->bitrate_bps / 1000));
        json_object_set_new(obj, "bitrate_mbps", json_real((double)ad->bitrate_bps / 1000000.0));
        json_object_set_new(obj, "total_packets", json_integer((json_int_t)ad->total_packets));
        json_object_set_new(obj, "total_bytes", json_integer((json_int_t)ad->total_bytes));

        /* Channels list on this adapter */
        json_t *ch_arr = json_array();
        for (int c = 0; c < ad->num_channels; c++) {
            channel_info_t *ch = &ad->channels[c];
            json_t *co = json_object();
            json_object_set_new(co, "service_id", json_integer(ch->service_id));
            json_object_set_new(co, "name", json_string(ch->name));
            json_object_set_new(co, "provider", json_string(ch->provider));
            json_object_set_new(co, "pmt_pid", json_integer(ch->pmt_pid));
            json_object_set_new(co, "video_pid", json_integer(ch->video_pid));
            json_object_set_new(co, "video_type", json_integer(ch->video_type));
            json_object_set_new(co, "scrambled", json_boolean(ch->scrambled));

            json_t *audio_arr = json_array();
            for (int a = 0; a < ch->num_audio_pids; a++) {
                json_t *ao = json_object();
                json_object_set_new(ao, "pid", json_integer(ch->audio_pids[a]));
                json_object_set_new(ao, "type", json_integer(ch->audio_types[a]));
                json_array_append_new(audio_arr, ao);
            }
            json_object_set_new(co, "audio_pids", audio_arr);

            const softcam_entry_t *k = softcam_csa_find(ch->service_id);
            json_object_set_new(co, "has_softcam_key", json_boolean(k != NULL));

            json_array_append_new(ch_arr, co);
        }
        json_object_set_new(obj, "channels", ch_arr);
        json_object_set_new(obj, "channels_count", json_integer(ad->num_channels));

        json_array_append_new(arr, obj);
    }

    pthread_mutex_unlock(&g_gateway.lock);
    return arr;
}

typedef struct {
    int adapter_idx;
    char device_path[256];
    char frontend_path[512];
    bool has_frontend;
    bool has_dvr;
    bool configured;
} hw_adapter_info_t;

static int hw_adapter_cmp(const void *a, const void *b) {
    const hw_adapter_info_t *ha = (const hw_adapter_info_t *)a;
    const hw_adapter_info_t *hb = (const hw_adapter_info_t *)b;
    return ha->adapter_idx - hb->adapter_idx;
}

json_t *web_state_hardware_to_json(void) {
    json_t *arr = json_array();
    hw_adapter_info_t items[32];
    int count = 0;

    DIR *d = opendir("/dev/dvb");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strncmp(de->d_name, "adapter", 7) == 0 && count < 32) {
                int adapter_num = atoi(de->d_name + 7);
                hw_adapter_info_t *item = &items[count++];
                item->adapter_idx = adapter_num;
                snprintf(item->device_path, sizeof(item->device_path), "%s", de->d_name);
                snprintf(item->frontend_path, sizeof(item->frontend_path), "/dev/dvb/%s/frontend0", de->d_name);
                char dvr_path[512];
                snprintf(dvr_path, sizeof(dvr_path), "/dev/dvb/%s/dvr0", de->d_name);

                item->has_frontend = (access(item->frontend_path, F_OK) == 0);
                item->has_dvr = (access(dvr_path, F_OK) == 0);

                /* Check if already configured */
                item->configured = false;
                pthread_mutex_lock(&g_gateway.lock);
                for (int i = 0; i < g_gateway.num_adapters; i++) {
                    if (g_gateway.adapters[i].adapter_idx == adapter_num) {
                        item->configured = true;
                        break;
                    }
                }
                pthread_mutex_unlock(&g_gateway.lock);
            }
        }
        closedir(d);
    }

    if (count > 1) {
        qsort(items, count, sizeof(hw_adapter_info_t), hw_adapter_cmp);
    }

    for (int i = 0; i < count; i++) {
        json_t *obj = json_object();
        json_object_set_new(obj, "adapter_idx", json_integer(items[i].adapter_idx));
        json_object_set_new(obj, "device_path", json_string(items[i].device_path));
        json_object_set_new(obj, "frontend_path", json_string(items[i].frontend_path));
        json_object_set_new(obj, "has_frontend", json_boolean(items[i].has_frontend));
        json_object_set_new(obj, "has_dvr", json_boolean(items[i].has_dvr));
        json_object_set_new(obj, "configured", json_boolean(items[i].configured));
        json_array_append_new(arr, obj);
    }

    return arr;
}

json_t *web_state_streams_to_json(void) {
    pthread_mutex_lock(&g_gateway.lock);
    json_t *arr = json_array();

    for (int i = 0; i < g_gateway.num_streams; i++) {
        web_stream_config_t *s = &g_gateway.streams[i];
        json_t *obj = json_object();
        json_object_set_new(obj, "id", json_string(s->id));
        json_object_set_new(obj, "name", json_string(s->name));
        json_object_set_new(obj, "enabled", json_boolean(s->enabled));
        json_object_set_new(obj, "type", json_string(s->type));
        json_object_set_new(obj, "input_url", json_string(s->input_url));
        json_object_set_new(obj, "output_url", json_string(s->output_url));
        json_object_set_new(obj, "adapter_idx", json_integer(s->adapter_idx));
        json_object_set_new(obj, "service_id", json_integer(s->service_id));
        json_object_set_new(obj, "ip", json_string(s->ip));
        json_object_set_new(obj, "port", json_integer(s->port));
        json_object_set_new(obj, "ttl", json_integer(s->ttl));
        json_object_set_new(obj, "is_rtp", json_boolean(s->is_rtp));
        json_object_set_new(obj, "pacing", json_boolean(s->pacing));
        json_object_set_new(obj, "bitrate_bps", json_integer(s->bitrate_bps));
        json_object_set_new(obj, "bitrate_kbps", json_integer(s->bitrate_bps / 1000));
        json_object_set_new(obj, "bitrate_mbps", json_real((double)s->bitrate_bps / 1000000.0));
        json_object_set_new(obj, "packets_sent", json_integer((json_int_t)s->packets_sent));
        json_object_set_new(obj, "bytes_sent", json_integer((json_int_t)s->bytes_sent));
        json_object_set_new(obj, "cc_errors", json_integer(s->cc_errors));
        json_object_set_new(obj, "biss_key", json_string(s->biss_key));

        /* Channel name lookup */
        char channel_name[128] = "";
        for (int a = 0; a < g_gateway.num_adapters; a++) {
            for (int c = 0; c < g_gateway.adapters[a].num_channels; c++) {
                if (g_gateway.adapters[a].channels[c].service_id == s->service_id) {
                    snprintf(channel_name, sizeof(channel_name), "%s", g_gateway.adapters[a].channels[c].name);
                    break;
                }
            }
            if (channel_name[0]) break;
        }

        if (channel_name[0]) {
            json_object_set_new(obj, "channel_name", json_string(channel_name));
        } else {
            json_object_set_new(obj, "channel_name", json_string(s->service_id == 0 ? "Full Transponder (MPTS)" : ""));
        }

        json_array_append_new(arr, obj);
    }

    pthread_mutex_unlock(&g_gateway.lock);
    return arr;
}

json_t *web_state_config_to_json(void) {
    pthread_mutex_lock(&g_gateway.lock);
    json_t *root = json_object();
    json_object_set_new(root, "version", json_string("DVBlast-Web-2.0"));
    json_object_set_new(root, "server_name", json_string(g_gateway.server_name));
    json_object_set_new(root, "http_port", json_integer(g_gateway.http_port));
    json_object_set_new(root, "http_host", json_string(g_gateway.http_host));
    json_object_set_new(root, "config_file", json_string(g_gateway.config_file));

    /* Adapters */
    json_t *a_arr = json_array();
    for (int i = 0; i < g_gateway.num_adapters; i++) {
        web_adapter_t *ad = &g_gateway.adapters[i];
        json_t *ao = json_object();
        json_object_set_new(ao, "id", json_string(ad->id));
        json_object_set_new(ao, "name", json_string(ad->name));
        json_object_set_new(ao, "enabled", json_boolean(ad->enabled));
        json_object_set_new(ao, "adapter_idx", json_integer(ad->adapter_idx));
        json_object_set_new(ao, "frontend_idx", json_integer(ad->frontend_idx));
        json_object_set_new(ao, "delsys", json_string(ad->delsys));
        json_object_set_new(ao, "frequency", json_integer((json_int_t)ad->frequency));
        json_object_set_new(ao, "symbol_rate", json_integer(ad->symbol_rate));
        json_object_set_new(ao, "bandwidth", json_integer(ad->bandwidth));
        json_object_set_new(ao, "polarization", json_string(ad->polarization));
        json_object_set_new(ao, "voltage", json_integer(ad->voltage));
        json_object_set_new(ao, "tone", json_integer(ad->tone));
        json_object_set_new(ao, "diseqc", json_integer(ad->diseqc));
        json_object_set_new(ao, "lnb_type", json_string(ad->lnb_type));
        json_object_set_new(ao, "modulation", json_string(ad->modulation));
        json_object_set_new(ao, "multistream_id", json_integer(ad->multistream_id));
        json_array_append_new(a_arr, ao);
    }
    json_object_set_new(root, "adapters", a_arr);

    /* Streams */
    json_t *s_arr = json_array();
    for (int i = 0; i < g_gateway.num_streams; i++) {
        web_stream_config_t *s = &g_gateway.streams[i];
        json_t *so = json_object();
        json_object_set_new(so, "id", json_string(s->id));
        json_object_set_new(so, "name", json_string(s->name));
        json_object_set_new(so, "enabled", json_boolean(s->enabled));
        json_object_set_new(so, "type", json_string(s->type));
        json_object_set_new(so, "input_url", json_string(s->input_url));
        json_object_set_new(so, "output_url", json_string(s->output_url));
        json_object_set_new(so, "adapter_idx", json_integer(s->adapter_idx));
        json_object_set_new(so, "service_id", json_integer(s->service_id));
        json_object_set_new(so, "ip", json_string(s->ip));
        json_object_set_new(so, "port", json_integer(s->port));
        json_object_set_new(so, "ttl", json_integer(s->ttl));
        json_object_set_new(so, "is_rtp", json_boolean(s->is_rtp));
        json_object_set_new(so, "pacing", json_boolean(s->pacing));
        json_object_set_new(so, "biss_key", json_string(s->biss_key));
        json_array_append_new(s_arr, so);
    }
    json_object_set_new(root, "streams", s_arr);
    json_object_set_new(root, "descramble", softcam_csa_to_json());

    pthread_mutex_unlock(&g_gateway.lock);
    return root;
}

json_t *web_state_logs_to_json(int max_entries) {
    pthread_mutex_lock(&g_gateway.lock);
    json_t *arr = json_array();
    int count = (max_entries > 0 && max_entries < g_gateway.log_count) ? max_entries : g_gateway.log_count;
    int start_idx = (g_gateway.log_head + g_gateway.log_count - count + MAX_LOG_ENTRIES) % MAX_LOG_ENTRIES;

    for (int i = 0; i < count; i++) {
        int idx = (start_idx + i) % MAX_LOG_ENTRIES;
        web_log_entry_t *l = &g_gateway.logs[idx];
        json_t *obj = json_object();
        json_object_set_new(obj, "id", json_integer((json_int_t)l->id));
        json_object_set_new(obj, "timestamp", json_integer((json_int_t)l->timestamp));
        json_object_set_new(obj, "level", json_string(l->level));
        json_object_set_new(obj, "message", json_string(l->message));
        json_array_append_new(arr, obj);
    }
    pthread_mutex_unlock(&g_gateway.lock);
    return arr;
}

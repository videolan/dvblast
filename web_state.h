#ifndef DVBLAST_WEB_STATE_H
#define DVBLAST_WEB_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <jansson.h>
#include <pthread.h>

#define MAX_WEB_ADAPTERS        16
#define MAX_WEB_STREAMS         128
#define MAX_DISCOVERED_CHANNELS 128
#define MAX_AUDIO_PIDS          16
#define MAX_LOG_ENTRIES         256
#define LOG_MSG_MAX             256

/* Log entry structure */
typedef struct {
    uint64_t id;
    time_t   timestamp;
    char     level[12];     /* "INFO", "WARN", "ERROR", "DEBUG" */
    char     message[LOG_MSG_MAX];
} web_log_entry_t;

/* Discovered channel structure */
typedef struct {
    uint16_t service_id;
    char     name[128];
    char     provider[128];
    uint16_t pmt_pid;
    uint16_t video_pid;
    uint8_t  video_type;
    uint16_t audio_pids[MAX_AUDIO_PIDS];
    uint8_t  audio_types[MAX_AUDIO_PIDS];
    int      num_audio_pids;
    bool     scrambled;
    uint32_t bitrate_bps;
    time_t   last_seen;
} channel_info_t;

/* Configured Adapter State */
typedef struct {
    char     id[64];            /* Unique adapter ID e.g. "adapter_0" */
    char     name[128];         /* Friendly name */
    bool     enabled;
    int      adapter_idx;       /* Linux DVB adapter number (0, 1, 2...) */
    int      frontend_idx;      /* Frontend number (0) */
    char     delsys[16];        /* "DVBT2", "DVBT", "DVBS2", "DVBS", "DVBC" */
    uint64_t frequency;         /* Hz */
    uint32_t symbol_rate;       /* S/s */
    int      bandwidth;         /* MHz (6, 7, 8) */
    char     polarization[4];   /* "H", "V", "18", "13" */
    int      voltage;           /* 13, 18, 0 */
    int      tone;              /* 0, 1 */
    int      diseqc;            /* 0..4 */
    char     lnb_type[32];      /* "universal", etc. */
    char     modulation[16];    /* "qpsk", "psk_8", "16qam", etc. */
    int      multistream_id;    /* MIS ID */

    /* Live Telemetry */
    bool     locked;
    uint16_t snr_raw;
    float    snr_db;
    uint16_t signal_percent;
    float    signal_dbm;
    uint32_t ber;
    uint32_t ucb;
    uint32_t bitrate_bps;
    uint64_t total_packets;
    uint64_t total_bytes;
    uint64_t last_calc_bytes;
    time_t   last_status_time;

    /* Channels discovered on this adapter's transponder */
    channel_info_t channels[MAX_DISCOVERED_CHANNELS];
    int      num_channels;
} web_adapter_t;

/* Stream Output Configuration */
typedef struct {
    char     id[64];            /* Unique stream ID e.g. "stream_1" */
    char     name[128];         /* Stream / Channel Name */
    bool     enabled;
    char     type[16];          /* "spts" or "mpts" */
    char     input_url[256];    /* "dvb://#adapter=0&pnr=101" or "udp://@..." */
    char     output_url[256];   /* "udp://239.255.1.1:5000" or "rtp://..." */
    int      adapter_idx;       /* Source adapter if DVB */
    uint16_t service_id;        /* SID / PNR (0 for all/MPTS) */
    char     ip[64];            /* Parsed output IP */
    int      port;              /* Parsed output Port */
    int      ttl;               /* Multicast TTL */
    bool     is_rtp;            /* false = raw UDP, true = RTP */
    bool     pacing;            /* Packet pacing enabled */
    uint32_t bitrate_bps;       /* Current stream bitrate */
    uint64_t packets_sent;
    uint64_t bytes_sent;
    uint64_t last_calc_bytes;
    uint32_t cc_errors;
    bool     scrambled;
    char     biss_key[64];
    time_t   created_at;
} web_stream_config_t;

/* Global Gateway State */
typedef struct {
    pthread_mutex_t lock;

    /* Server Preferences */
    char     server_name[128];
    int      http_port;
    char     http_host[64];
    char     config_file[256];
    char     access_log[256];
    time_t   start_time;

    /* Adapters (Starts completely empty: num_adapters = 0) */
    web_adapter_t adapters[MAX_WEB_ADAPTERS];
    int      num_adapters;

    /* Streams (Starts completely empty: num_streams = 0) */
    web_stream_config_t streams[MAX_WEB_STREAMS];
    int      num_streams;

    /* System Stats */
    uint64_t total_input_bytes;
    uint64_t total_input_packets;
    uint32_t total_input_bitrate_bps;
    uint64_t total_output_bytes;
    uint64_t total_output_packets;
    uint32_t total_output_bitrate_bps;
    uint64_t last_bitrate_calc_us;
    uint64_t last_calc_in_bytes;
    uint64_t last_calc_out_bytes;

    /* Log Ring Buffer */
    web_log_entry_t logs[MAX_LOG_ENTRIES];
    int      log_head;
    int      log_count;
    uint64_t next_log_id;
} gateway_state_t;

extern gateway_state_t g_gateway;

void web_state_init(int http_port, const char *http_host, const char *config_file);
void web_state_cleanup(void);

void web_state_log(const char *level, const char *fmt, ...);

/* Adapters Management */
bool web_state_add_or_update_adapter(const web_adapter_t *ad);
bool web_state_delete_adapter(const char *id);
web_adapter_t *web_state_find_adapter(const char *id);
web_adapter_t *web_state_find_adapter_by_idx(int adapter_idx);
void web_state_update_adapter_signal(int adapter_idx, bool locked, uint16_t strength, uint16_t snr, uint32_t ber, uint32_t ucb);
void web_state_update_adapter_signal_ext(int adapter_idx, bool locked, uint16_t strength, float snr_db, float signal_dbm, uint32_t ber, uint32_t ucb);
void web_state_add_adapter_channel(int adapter_idx, const channel_info_t *ch);
void web_state_clear_adapter_channels(int adapter_idx);

/* Streams Management */
bool web_state_add_or_update_stream(const web_stream_config_t *stream);
bool web_state_delete_stream(const char *id);
web_stream_config_t *web_state_find_stream(const char *id);
web_stream_config_t *web_state_find_stream_by_dest(const char *ip, int port);

/* Stats & System */
void web_state_init_shm(bool is_master);
void web_state_update_input_stats(int adapter_idx, uint64_t bytes, uint64_t packets);
void web_state_update_output_stats(uint64_t bytes, uint64_t packets);
void web_state_record_output(const char *stream_id_or_name, uint64_t bytes, uint64_t packets);
void web_state_calc_bitrates(void);

/* Persistence */
bool web_state_save_json(const char *path);
bool web_state_load_json(const char *path);

/* Discovered Channels helper for demuxer */
void web_state_add_channel(const channel_info_t *ch);

/* JSON Serializers for REST API */
json_t *web_state_telemetry_to_json(void);
json_t *web_state_adapters_to_json(void);
json_t *web_state_hardware_to_json(void);
json_t *web_state_streams_to_json(void);
json_t *web_state_config_to_json(void);
json_t *web_state_logs_to_json(int max_entries);

#endif /* DVBLAST_WEB_STATE_H */

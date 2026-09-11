#ifndef RT_FFI_H
#define RT_FFI_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define RT_OK          0
#define RT_E_ARG       1   /* null pointer, index out of range, rejected value */
#define RT_E_STATE     2   /* open twice, device query before open */
#define RT_E_DEVICE    3   /* device open/start failed */
#define RT_E_INTERNAL  4   /* unexpected exception */

#define RT_MAX_CHANNELS  8
#define RT_MAX_STRIPS   16
#define RT_MAX_PARAMS    8
#define RT_HIST_BUCKETS 322

#define RT_TAPER_LINEAR 0
#define RT_TAPER_LOG    1
#define RT_FLAG_READ_ONLY 1
#define RT_FLAG_TOGGLE    2

typedef struct rt_session rt_session;

typedef struct {
    const char* backend;      /* "wasapi" | "alsa" | "null"; NULL = platform default */
    const char* input_id;     /* NULL = system default */
    const char* output_id;
    double      sample_rate;
    int32_t     block_frames; /* 0 = driver minimum */
    uint8_t     exclusive;
} rt_open_config;

typedef struct {
    const char* id;           /* static storage: valid for the process lifetime */
    const char* name;
    const char* unit;
    double      min, max, def;   /* def = instance default */
    uint8_t     taper, flags;
} rt_param_desc;

typedef struct {
    const char* name;         /* static storage */
    int32_t     param_count;
    int32_t     latency_frames;
} rt_strip_desc;

typedef struct {
    char    backend[64], input[128], output[128];   /* copied; truncated on a UTF-8 boundary */
    double  sample_rate, claimed_rtt_ms;
    int32_t block_frames, channels;
} rt_device_desc;

typedef struct {
    uint64_t callbacks, engine_xruns, device_xruns, capture_overruns;  /* monotonic */
    uint64_t in_clips, out_clips;                                      /* monotonic */
    uint64_t deadline_ns;
    uint64_t hist_window[RT_HIST_BUCKETS];      /* drained since the previous snapshot */
    float    in_peak[RT_MAX_CHANNELS];          /* max |x| since the previous snapshot */
    float    out_peak[RT_MAX_CHANNELS];
    double   params[RT_MAX_STRIPS][RT_MAX_PARAMS]; /* engine-held values, post-clamp */
    int32_t  channels;
    uint8_t  running;
    char     device_error[256];                 /* empty unless the backend reported one */
} rt_snapshot;

rt_session* rt_session_create(void);
void        rt_session_destroy(rt_session* s);
int32_t     rt_session_open(rt_session* s, const rt_open_config* cfg);
int32_t     rt_session_stop(rt_session* s);
size_t      rt_session_last_error(const rt_session* s, char* buf, size_t cap);

int32_t     rt_session_device(const rt_session* s, rt_device_desc* out);
int32_t     rt_session_strip_count(const rt_session* s);   /* count, or -1 for a NULL handle */
int32_t     rt_session_strip(const rt_session* s, int32_t strip, rt_strip_desc* out);
int32_t     rt_session_param(const rt_session* s, int32_t strip, int32_t param, rt_param_desc* out);
int32_t     rt_session_set_param(rt_session* s, int32_t strip, int32_t param, double value);
int32_t     rt_session_snapshot(rt_session* s, rt_snapshot* out);

uint64_t    rt_hist_percentile_ns(const uint64_t counts[RT_HIST_BUCKETS], double p);
uint64_t    rt_hist_bucket_upper_ns(int32_t bucket);

#ifdef __cplusplus
}
#endif
#endif

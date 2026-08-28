#ifndef MINIIPTV_NETWORK_H
#define MINIIPTV_NETWORK_H

#include <stddef.h>

#define MINIIPTV_MANIFEST_LIMIT (128 * 1024)
#define MINIIPTV_NETWORK_TOO_LARGE (-2001)

typedef struct {
    unsigned int http_status;
    size_t size;
    char final_url[1024];
    char *data;
} NetworkTextResponse;

typedef int (*MiniIptvCancelFunction)(void *userdata);
typedef size_t (*MiniIptvStreamWriteFunction)(const unsigned char *data,
                                              size_t size, void *userdata);
typedef void (*MiniIptvStreamProgressFunction)(size_t received_size,
                                               size_t reported_size,
                                               void *userdata);

typedef struct {
    size_t received_size;
    /* Content-Length reported by the server, or zero when it is unknown. */
    size_t reported_size;
} NetworkStreamMetrics;

typedef struct {
    unsigned int connect_timeout_seconds;
    unsigned int total_timeout_seconds;
} NetworkRequestOptions;

int network_init(void);
void network_exit(void);
int network_get_text(const char *url, const char *user_agent, const char *referrer,
                     NetworkTextResponse *response);
int network_get_data(const char *url, const char *user_agent, const char *referrer,
                     size_t maximum_size, NetworkTextResponse *response);
int network_get_data_cancelable(const char *url, const char *user_agent,
                                const char *referrer, size_t maximum_size,
                                MiniIptvCancelFunction should_cancel,
                                void *cancel_userdata,
                                NetworkTextResponse *response);
int network_get_data_cancelable_with_options(
    const char *url, const char *user_agent, const char *referrer,
    size_t maximum_size, MiniIptvCancelFunction should_cancel,
    void *cancel_userdata, const NetworkRequestOptions *options,
    NetworkTextResponse *response);
int network_stream_data(const char *url, const char *user_agent,
                        const char *referrer, size_t maximum_size,
                        MiniIptvStreamWriteFunction write_data,
                        void *write_userdata,
                        MiniIptvStreamProgressFunction report_progress,
                        void *progress_userdata,
                        MiniIptvCancelFunction should_cancel,
                        void *cancel_userdata, NetworkStreamMetrics *metrics);
void network_response_free(NetworkTextResponse *response);

#endif

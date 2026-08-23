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

int network_init(void);
void network_exit(void);
int network_get_text(const char *url, const char *user_agent, const char *referrer,
                     NetworkTextResponse *response);
int network_get_data(const char *url, const char *user_agent, const char *referrer,
                     size_t maximum_size, NetworkTextResponse *response);
int network_download_file(const char *url, const char *user_agent,
                          const char *referrer, size_t maximum_size,
                          const char *output_path,
                          MiniIptvCancelFunction should_cancel,
                          void *cancel_userdata, size_t *downloaded_size);
int network_stream_data(const char *url, const char *user_agent,
                        const char *referrer, size_t maximum_size,
                        MiniIptvStreamWriteFunction write_data,
                        void *write_userdata,
                        MiniIptvCancelFunction should_cancel,
                        void *cancel_userdata, size_t *downloaded_size);
void network_response_free(NetworkTextResponse *response);

#endif

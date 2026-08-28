#include "miniiptv/network.h"
#include "miniiptv/version.h"

#include <curl/curl.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int initialized;
static CURL *persistent_curl;

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
    MiniIptvCancelFunction should_cancel;
    void *cancel_userdata;
    int cancelled;
} CurlBuffer;

typedef struct {
    MiniIptvStreamWriteFunction write_data;
    void *write_userdata;
    MiniIptvStreamProgressFunction report_progress;
    void *progress_userdata;
    MiniIptvCancelFunction should_cancel;
    void *cancel_userdata;
    size_t size;
    size_t observed_size;
    size_t maximum_size;
    size_t reported_size;
    int too_large;
} CurlStream;

static size_t write_callback(char *incoming, size_t size, size_t count, void *userdata) {
    CurlBuffer *buffer = userdata;
    size_t bytes;
    if (size && count > SIZE_MAX / size) return 0;
    bytes = size * count;
    if (!buffer || bytes > buffer->capacity - buffer->size) return 0;
    memcpy(buffer->data + buffer->size, incoming, bytes);
    buffer->size += bytes;
    return bytes;
}

static int buffer_progress_callback(void *userdata, curl_off_t download_total,
                                    curl_off_t download_now,
                                    curl_off_t upload_total,
                                    curl_off_t upload_now) {
    CurlBuffer *buffer = userdata;
    (void)download_total;
    (void)download_now;
    (void)upload_total;
    (void)upload_now;
    if (buffer && buffer->should_cancel &&
        buffer->should_cancel(buffer->cancel_userdata)) {
        buffer->cancelled = 1;
        return 1;
    }
    return 0;
}

static size_t stream_write_callback(char *incoming, size_t size, size_t count,
                                    void *userdata) {
    CurlStream *output = userdata;
    size_t bytes;
    if (size && count > SIZE_MAX / size) return 0;
    bytes = size * count;
    if (!output || !output->write_data) return 0;
    output->observed_size = bytes > SIZE_MAX - output->size
        ? SIZE_MAX : output->size + bytes;
    if (bytes > output->maximum_size - output->size) {
        output->too_large = 1;
        return 0;
    }
    if (output->write_data((const unsigned char *)incoming, bytes,
                           output->write_userdata) != bytes)
        return 0;
    output->size += bytes;
    output->observed_size = output->size;
    if (output->report_progress)
        output->report_progress(output->size, output->reported_size,
                                output->progress_userdata);
    return bytes;
}

static int stream_progress_callback(void *userdata, curl_off_t download_total,
                                    curl_off_t download_now,
                                    curl_off_t upload_total,
                                    curl_off_t upload_now) {
    CurlStream *output = userdata;
    (void)download_now;
    (void)upload_total;
    (void)upload_now;
    if (!output) return 0;
    if (download_total > 0) {
        uint64_t total = (uint64_t)download_total;
        output->reported_size = total > SIZE_MAX ? SIZE_MAX : (size_t)total;
    }
    if (output->report_progress)
        output->report_progress(output->size, output->reported_size,
                                output->progress_userdata);
    if (download_total > 0 &&
        (uint64_t)download_total > (uint64_t)output->maximum_size) {
        output->too_large = 1;
        return 1;
    }
    return output->should_cancel &&
           output->should_cancel(output->cancel_userdata);
}

int network_init(void) {
    if (initialized) return 0;
    // SOC and the platform-specific curl runtime belong to the reference
    // application. Its proven curl path uses curl_easy_init directly; calling
    // curl_global_init here after the fake-pthread layer starts can deadlock.
    persistent_curl = curl_easy_init();
    if (!persistent_curl) return -1;
    initialized = 1;
    return 0;
}

void network_exit(void) {
    if (persistent_curl) curl_easy_cleanup(persistent_curl);
    persistent_curl = NULL;
    initialized = 0;
}

void network_response_free(NetworkTextResponse *response) {
    if (!response) return;
    free(response->data);
    memset(response, 0, sizeof(*response));
}

int network_get_data(const char *url, const char *user_agent, const char *referrer,
                     size_t maximum_size, NetworkTextResponse *response) {
    return network_get_data_cancelable(url, user_agent, referrer, maximum_size,
                                       NULL, NULL, response);
}

int network_get_data_cancelable(const char *url, const char *user_agent,
                                const char *referrer, size_t maximum_size,
                                MiniIptvCancelFunction should_cancel,
                                void *cancel_userdata,
                                NetworkTextResponse *response) {
    return network_get_data_cancelable_with_options(
        url, user_agent, referrer, maximum_size, should_cancel,
        cancel_userdata, NULL, response);
}

int network_get_data_cancelable_with_options(
    const char *url, const char *user_agent, const char *referrer,
    size_t maximum_size, MiniIptvCancelFunction should_cancel,
    void *cancel_userdata, const NetworkRequestOptions *options,
    NetworkTextResponse *response) {
    CURL *curl;
    CURLcode result;
    CurlBuffer buffer;
    long status = 0;
    char *effective_url = NULL;

    if (!initialized || !url || !response || maximum_size == 0 || maximum_size == SIZE_MAX) return -1;
    memset(response, 0, sizeof(*response));
    response->data = malloc(maximum_size + 1);
    if (!response->data) return -2;
    buffer.data = response->data;
    buffer.size = 0;
    buffer.capacity = maximum_size;
    buffer.should_cancel = should_cancel;
    buffer.cancel_userdata = cancel_userdata;
    buffer.cancelled = 0;

    curl = persistent_curl;
    if (!curl) { network_response_free(response); return -3; }
    /* curl_easy_reset keeps the handle's connection cache. Reusing this easy
     * handle avoids a fresh DNS/TCP/TLS setup for every six-second segment. */
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,
                     (long)(options && options->connect_timeout_seconds
                         ? options->connect_timeout_seconds : 15u));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                     (long)(options && options->total_timeout_seconds
                         ? options->total_timeout_seconds
                         : (maximum_size > MINIIPTV_MANIFEST_LIMIT
                            ? 60u : 20u)));
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
        user_agent && *user_agent ? user_agent : "RetroTuner3DS/" RETROTUNER_VERSION);
    if (referrer && *referrer) curl_easy_setopt(curl, CURLOPT_REFERER, referrer);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, buffer_progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 128L * 1024L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, "romfs:/gfx/cert/cacert.pem");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);
    response->http_status = (unsigned int)status;
    if (effective_url) snprintf(response->final_url, sizeof(response->final_url), "%s", effective_url);
    if (result == CURLE_ABORTED_BY_CALLBACK && buffer.cancelled) {
        network_response_free(response);
        return -5;
    }
    if (result != CURLE_OK) {
        network_response_free(response);
        return -(1000 + (int)result);
    }
    if (status != 200) {
        network_response_free(response);
        response->http_status = (unsigned int)status;
        return -4;
    }
    response->size = buffer.size;
    response->data[buffer.size] = '\0';
    return 0;
}

int network_get_text(const char *url, const char *user_agent, const char *referrer,
                     NetworkTextResponse *response) {
    return network_get_data(url, user_agent, referrer, MINIIPTV_MANIFEST_LIMIT, response);
}

int network_stream_data(const char *url, const char *user_agent,
                        const char *referrer, size_t maximum_size,
                        MiniIptvStreamWriteFunction write_data,
                        void *write_userdata,
                        MiniIptvStreamProgressFunction report_progress,
                        void *progress_userdata,
                        MiniIptvCancelFunction should_cancel,
                        void *cancel_userdata, NetworkStreamMetrics *metrics) {
    CURL *curl = persistent_curl;
    CURLcode result;
    CurlStream output;
    long status = 0;

    if (metrics) memset(metrics, 0, sizeof(*metrics));
    if (!initialized || !curl || !url || !write_data || maximum_size == 0)
        return -1;
    memset(&output, 0, sizeof(output));
    output.write_data = write_data;
    output.write_userdata = write_userdata;
    output.report_progress = report_progress;
    output.progress_userdata = progress_userdata;
    output.should_cancel = should_cancel;
    output.cancel_userdata = cancel_userdata;
    output.maximum_size = maximum_size;

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
        user_agent && *user_agent ? user_agent : "RetroTuner3DS/" RETROTUNER_VERSION);
    if (referrer && *referrer) curl_easy_setopt(curl, CURLOPT_REFERER, referrer);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &output);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 128L * 1024L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, stream_progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &output);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, "romfs:/gfx/cert/cacert.pem");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (metrics) {
        metrics->received_size = output.observed_size;
        metrics->reported_size = output.reported_size;
    }
    if (output.too_large || result == CURLE_FILESIZE_EXCEEDED)
        return MINIIPTV_NETWORK_TOO_LARGE;
    if (result == CURLE_ABORTED_BY_CALLBACK && should_cancel &&
        should_cancel(cancel_userdata))
        return -5;
    if (result != CURLE_OK) return -(1000 + (int)result);
    if (status != 200) return -4;
    return 0;
}

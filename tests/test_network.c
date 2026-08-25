#include "miniiptv/network.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    unsigned int calls;
} CancelState;

static int cancel_immediately(void *userdata) {
    CancelState *state = userdata;
    state->calls++;
    /* Cancellation is an event, not necessarily a permanently sticky flag.
     * The network layer must remember that this callback caused the abort. */
    return state->calls == 1;
}

static void test_cancelable_get_aborts_and_cleans_response(void) {
    char cwd[2048];
    char url[4096];
    NetworkTextResponse response;
    const NetworkRequestOptions options = {1u, 2u};
    CancelState state = {0};
    int result;

    assert(getcwd(cwd, sizeof(cwd)) != NULL);
    result = snprintf(url, sizeof(url),
                      "file://%s/source/miniiptv/network.c", cwd);
    assert(result > 0 && (size_t)result < sizeof(url));
    memset(&response, 0, sizeof(response));

    result = network_get_data_cancelable_with_options(
        url, NULL, NULL, 1024 * 1024, cancel_immediately, &state,
        &options, &response);
    assert(result == -5);
    assert(state.calls > 0);
    assert(response.data == NULL);
    assert(response.size == 0);
    assert(response.final_url[0] == '\0');

    /* curl_easy_reset must remove the previous request's callback state. A
     * local file has no HTTP 200 status, so this uncancelled request reaches
     * the ordinary HTTP-status path instead of being reported as cancelled. */
    result = network_get_data(url, NULL, NULL, 1024 * 1024, &response);
    assert(result == -4);
    assert(response.data == NULL);
    assert(response.size == 0);
}

int main(void) {
    assert(network_init() == 0);
    test_cancelable_get_aborts_and_cleans_response();
    network_exit();
    puts("network tests passed");
    return 0;
}

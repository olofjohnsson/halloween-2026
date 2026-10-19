#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "report.h"

LOG_MODULE_REGISTER(report, LOG_LEVEL_INF);

#if defined(CONFIG_WIFI)

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/random/random.h>

#define REPORT_ENABLED (sizeof(CONFIG_HALLOWEEN_WIFI_SSID) > 1 && \
                        sizeof(CONFIG_HALLOWEEN_REPORT_HOST) > 1)

#define WIFI_CONNECT_TIMEOUT_S 30
#define RETRY_DELAY_S 10
#define HTTP_TIMEOUT_MS 5000
#define EVENT_QUEUE_LEN 16

struct dispense_event {
    uint32_t seq;
    uint16_t session;
};

K_MSGQ_DEFINE(event_q, sizeof(struct dispense_event), EVENT_QUEUE_LEN, 4);
K_SEM_DEFINE(ip_sem, 0, 1);

/* The server uses boot_id + seq to ignore an event that is retried after the
 * reply to the first attempt was lost. */
static uint32_t boot_id;
static uint32_t next_seq;
static volatile bool online;

static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;

static void wifi_event_handler(struct net_mgmt_event_callback *cb,
                               uint64_t mgmt_event, struct net_if *iface)
{
    ARG_UNUSED(iface);

    switch (mgmt_event) {
    case NET_EVENT_WIFI_CONNECT_RESULT: {
        const struct wifi_status *status = cb->info;

        if (status->status) {
            LOG_WRN("Wi-Fi connection failed (%d)", status->status);
        } else {
            LOG_INF("Wi-Fi connected");
        }
        break;
    }
    case NET_EVENT_WIFI_DISCONNECT_RESULT:
        online = false;
        LOG_WRN("Wi-Fi disconnected");
        break;
    default:
        break;
    }
}

static void ipv4_event_handler(struct net_mgmt_event_callback *cb,
                               uint64_t mgmt_event, struct net_if *iface)
{
    ARG_UNUSED(cb);

    if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
        struct net_if_config *cfg = net_if_get_config(iface);

        if (cfg != NULL && cfg->ip.ipv4 != NULL) {
            for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
                struct net_if_addr *a = &cfg->ip.ipv4->unicast[i].ipv4;
                char buf[NET_IPV4_ADDR_LEN];

                if (a->is_used &&
                    net_addr_ntop(AF_INET, &a->address.in_addr, buf, sizeof(buf))) {
                    LOG_INF("Got IPv4 address %s", buf);
                }
            }
        }
        online = true;
        k_sem_give(&ip_sem);
    }
}

static int wifi_connect(void)
{
    struct net_if *iface = net_if_get_first_wifi();
    struct wifi_connect_req_params params = {
        .ssid = (const uint8_t *)CONFIG_HALLOWEEN_WIFI_SSID,
        .ssid_length = sizeof(CONFIG_HALLOWEEN_WIFI_SSID) - 1,
        .psk = (const uint8_t *)CONFIG_HALLOWEEN_WIFI_PASSWORD,
        .psk_length = sizeof(CONFIG_HALLOWEEN_WIFI_PASSWORD) - 1,
        .security = sizeof(CONFIG_HALLOWEEN_WIFI_PASSWORD) > 1 ?
                    WIFI_SECURITY_TYPE_PSK : WIFI_SECURITY_TYPE_NONE,
        .band = WIFI_FREQ_BAND_UNKNOWN,
        .channel = WIFI_CHANNEL_ANY,
        .mfp = WIFI_MFP_OPTIONAL,
        .timeout = SYS_FOREVER_MS,
    };

    if (iface == NULL) {
        return -ENODEV;
    }

    LOG_INF("Connecting to Wi-Fi SSID \"%s\" (%s)", CONFIG_HALLOWEEN_WIFI_SSID,
            params.security == WIFI_SECURITY_TYPE_PSK ? "WPA-PSK" : "open");
    k_sem_reset(&ip_sem);
    int rc = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));

    /* -ENOTSUP/-ENETDOWN: the supplicant has not registered the interface yet after boot */
    for (int i = 0; i < 20 && (rc == -ENOTSUP || rc == -ENETDOWN); i++) {
        k_sleep(K_MSEC(500));
        rc = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
    }

    if (rc) {
        LOG_ERR("Wi-Fi connect request rejected: %d", rc);
        return rc;
    }

    if (k_sem_take(&ip_sem, K_SECONDS(WIFI_CONNECT_TIMEOUT_S)) != 0) {
        LOG_ERR("No IP address within %d s (wrong password, out of range or no DHCP?)",
                WIFI_CONNECT_TIMEOUT_S);
        (void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
        return -ETIMEDOUT;
    }
    return 0;
}

static int response_cb(struct http_response *rsp, enum http_final_call final_data,
                        void *user_data)
{
    ARG_UNUSED(final_data);

    if (rsp->http_status_code != 0) {
        *(uint16_t *)user_data = rsp->http_status_code;
    }
    return 0;
}

/* Returns 0 when the event was accepted or can never be accepted (so it is dropped),
 * and a negative value when it should be retried later. */
static int post_event(const struct dispense_event *ev)
{
    struct zsock_addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct zsock_addrinfo *res = NULL;
    char port[8];
    char body[80];
    char api_key_header[sizeof(CONFIG_HALLOWEEN_REPORT_API_KEY) + 16];
    uint8_t recv_buf[256];
    uint16_t status = 0;
    int rc;

    snprintf(port, sizeof(port), "%d", CONFIG_HALLOWEEN_REPORT_PORT);
    LOG_INF("POST seq=%u to %s:%s%s", (unsigned int)ev->seq, CONFIG_HALLOWEEN_REPORT_HOST,
            port, CONFIG_HALLOWEEN_REPORT_PATH);
    rc = zsock_getaddrinfo(CONFIG_HALLOWEEN_REPORT_HOST, port, &hints, &res);
    if (rc != 0) {
        LOG_ERR("Resolving %s failed: %d", CONFIG_HALLOWEEN_REPORT_HOST, rc);
        return -EHOSTUNREACH;
    }

    int sock = zsock_socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP);

    if (sock < 0) {
        LOG_ERR("Creating socket failed: %d", errno);
        zsock_freeaddrinfo(res);
        return -errno;
    }

    rc = zsock_connect(sock, res->ai_addr, res->ai_addrlen);
    zsock_freeaddrinfo(res);
    if (rc < 0) {
        LOG_ERR("Connecting to server failed: %d", errno);
        zsock_close(sock);
        return -ECONNREFUSED;
    }

    int body_len = snprintf(body, sizeof(body),
                            "{\"boot\":%u,\"seq\":%u,\"session\":%u}",
                            (unsigned int)boot_id, (unsigned int)ev->seq,
                            (unsigned int)ev->session);
    snprintf(api_key_header, sizeof(api_key_header), "X-API-Key: %s\r\n",
             CONFIG_HALLOWEEN_REPORT_API_KEY);

    const char *const headers[] = { api_key_header, NULL };
    struct http_request req = {
        .method = HTTP_POST,
        .url = CONFIG_HALLOWEEN_REPORT_PATH,
        .host = CONFIG_HALLOWEEN_REPORT_HOST,
        .protocol = "HTTP/1.1",
        .header_fields = headers,
        .content_type_value = "application/json",
        .payload = body,
        .payload_len = body_len,
        .response = response_cb,
        .recv_buf = recv_buf,
        .recv_buf_len = sizeof(recv_buf),
    };

    rc = http_client_req(sock, &req, HTTP_TIMEOUT_MS, &status);
    zsock_close(sock);
    LOG_INF("HTTP request returned rc=%d status=%u", rc, status);

    if (rc < 0) {
        LOG_ERR("HTTP request failed: %d", rc);
        return -EIO;
    }
    if (status >= 200 && status < 300) {
        LOG_INF("Reported dispense seq=%u session=%u", (unsigned int)ev->seq,
                (unsigned int)ev->session);
        return 0;
    }
    if (status >= 400 && status < 500) {
        LOG_ERR("Server rejected dispense report (HTTP %u); check the API key and path",
                status);
        return 0;
    }
    LOG_WRN("Server error (HTTP %u); will retry", status);
    return -EIO;
}

static void report_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    struct dispense_event pending;
    bool have_pending = false;

    while (1) {
        if (!online) {
            int rc = wifi_connect();

            if (rc) {
                LOG_WRN("Wi-Fi connect failed: %d; retrying", rc);
                k_sleep(K_SECONDS(RETRY_DELAY_S));
                continue;
            }
        }

        if (!have_pending) {
            have_pending = k_msgq_get(&event_q, &pending, K_SECONDS(1)) == 0;
        }

        if (have_pending) {
            if (post_event(&pending) == 0) {
                have_pending = false;
            } else {
                k_sleep(K_SECONDS(RETRY_DELAY_S));
            }
        }
    }
}

#if defined(CONFIG_HALLOWEEN_REPORT_TEST)
static void test_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(test_work, test_work_handler);

static void test_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    report_dispense(0);
    k_work_schedule(&test_work, K_SECONDS(1));
}
#endif

K_THREAD_STACK_DEFINE(report_stack, 6144);
static struct k_thread report_thread_data;

void report_init(void)
{
    if (!REPORT_ENABLED) {
        LOG_WRN("Wi-Fi/server not configured (see wifi_secrets.conf.example); not reporting");
        return;
    }

    boot_id = sys_rand32_get();

    net_mgmt_init_event_callback(&wifi_cb, wifi_event_handler,
                                 NET_EVENT_WIFI_CONNECT_RESULT |
                                 NET_EVENT_WIFI_DISCONNECT_RESULT);
    net_mgmt_add_event_callback(&wifi_cb);
    net_mgmt_init_event_callback(&ipv4_cb, ipv4_event_handler, NET_EVENT_IPV4_ADDR_ADD);
    net_mgmt_add_event_callback(&ipv4_cb);

    k_thread_create(&report_thread_data, report_stack, K_THREAD_STACK_SIZEOF(report_stack),
                    report_thread, NULL, NULL, NULL, 8, 0, K_NO_WAIT);
    k_thread_name_set(&report_thread_data, "report");
#if defined(CONFIG_HALLOWEEN_REPORT_TEST)
    LOG_WRN("Report test mode: sending a fake dispense every second");
    k_work_schedule(&test_work, K_SECONDS(1));
#endif
    LOG_INF("Reporting dispenses to http://%s:%d%s", CONFIG_HALLOWEEN_REPORT_HOST,
            CONFIG_HALLOWEEN_REPORT_PORT, CONFIG_HALLOWEEN_REPORT_PATH);
}

void report_dispense(uint16_t session)
{
    if (!REPORT_ENABLED) {
        return;
    }

    struct dispense_event ev = { .seq = next_seq++, .session = session };

    LOG_INF("Queueing dispense seq=%u session=%u (online=%d)", (unsigned int)ev.seq,
            (unsigned int)session, online);

    if (k_msgq_put(&event_q, &ev, K_NO_WAIT) != 0) {
        LOG_ERR("Report queue full; dispense seq=%u not reported", (unsigned int)ev.seq);
    }
}

#else

void report_init(void)
{
}

void report_dispense(uint16_t session)
{
    ARG_UNUSED(session);
}

#endif

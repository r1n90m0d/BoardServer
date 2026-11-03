#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <stdbool.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <curl/curl.h>

#define MAC_TEXT_LEN 12
#define DEFAULT_HEARTBEAT_URL "http://10.0.0.1:3000/v1/devices/heartbeat"
#define DEFAULT_HEARTBEAT_IFACE "wg1"
#define RESPONSE_BODY_MAX 1024
#define HEARTBEAT_PERIOD_SEC 5
#define MAC_RETRY_SEC 1

static volatile sig_atomic_t g_stop;

static void handle_stop_signal(int signum)
{
    (void)signum;
    g_stop = 1;
}

static int normalize_mac(const char *raw, char out[MAC_TEXT_LEN + 1])
{
    int count = 0;

    for (const char *p = raw; *p != '\0' && *p != '\n'; p++) {
        unsigned char c = (unsigned char)*p;

        if (c == ':')
            continue;

        if (!isxdigit(c) || count == MAC_TEXT_LEN)
            return -1;

        out[count++] = (char)toupper(c);
    }

    if (count != MAC_TEXT_LEN)
        return -1;

    out[count] = '\0';
    return 0;
}

static int read_mac(const char *ifname, char out[MAC_TEXT_LEN + 1])
{
    char path[64];
    char raw[32];
    FILE *fp;
    int written;

    written = snprintf(path, sizeof(path), "/sys/class/net/%s/address", ifname);
    if (written < 0 || (size_t)written >= sizeof(path))
        return -1;

    fp = fopen(path, "r");
    if (fp == NULL)
        return -1;

    if (fgets(raw, sizeof(raw), fp) == NULL) {
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return normalize_mac(raw, out);
}

/*
 * read_mac이 실패해도 프로그램을 종료하지 않고 1초 뒤 재시도한다.
 * SIGTERM/SIGINT가 오면 mac을 못 구해도 즉시 포기하고 false를 반환한다.
 */
static bool read_mac_retrying(const char *ifname, char out[MAC_TEXT_LEN + 1])
{
    while (!g_stop) {
        if (read_mac(ifname, out) == 0)
            return true;

        fprintf(stderr, "heartbeat: failed to read %s MAC, retry in %ds\n", ifname, MAC_RETRY_SEC);

        struct timespec ts = { .tv_sec = MAC_RETRY_SEC, .tv_nsec = 0 };
        while (clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, &ts) == EINTR) {
            if (g_stop)
                break;
        }
    }

    return false;
}

static bool is_process_running(const char *name)
{
    DIR *dir = opendir("/proc");
    struct dirent *entry;
    bool found = false;

    if (dir == NULL)
        return false;

    while (!found && (entry = readdir(dir)) != NULL) {
        char path[272];
        char comm[32];
        FILE *fp;

        if (!isdigit((unsigned char)entry->d_name[0]))
            continue;

        snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);

        fp = fopen(path, "r");
        if (fp == NULL)
            continue;

        if (fgets(comm, sizeof(comm), fp) != NULL) {
            comm[strcspn(comm, "\n")] = '\0';
            found = strcmp(comm, name) == 0;
        }

        fclose(fp);
    }

    closedir(dir);
    return found;
}

/*
 * ifname의 IFF_UP 플래그만 확인한다. peer 존재 여부나 handshake 성공 여부는
 * 알려주지 않으며, operstate는 wg1에서 "unknown"으로 나와 신뢰할 수 없으므로
 * 사용하지 않는다(/sys/class/net/<ifname>/flags의 0x1 비트로 판단).
 */
static bool is_interface_up(const char *ifname)
{
    char path[64];
    unsigned long flags;
    FILE *fp;
    int written;

    written = snprintf(path, sizeof(path), "/sys/class/net/%s/flags", ifname);
    if (written < 0 || (size_t)written >= sizeof(path))
        return false;

    fp = fopen(path, "r");
    if (fp == NULL)
        return false;

    if (fscanf(fp, "%lx", &flags) != 1) {
        fclose(fp);
        return false;
    }

    fclose(fp);
    return (flags & 0x1) != 0;
}

/*
 * mac, rtsp_ok, wg_ok, config_version으로 I1 heartbeat JSON 본문을 만든다.
 * 선택 필드는 넣지 않는다. snprintf가 잘리면(buf_size 부족) 실패로 처리한다.
 */
static bool build_payload(char *buf, size_t buf_size, const char *mac,
                           bool rtsp_ok, bool wg_ok, int config_version)
{
    int written = snprintf(buf, buf_size,
        "{\"mac\":\"%s\",\"status\":\"online\",\"rtsp_ok\":%s,\"wg_ok\":%s,\"config_version\":%d}",
        mac,
        rtsp_ok ? "true" : "false",
        wg_ok ? "true" : "false",
        config_version);

    if (written < 0 || (size_t)written >= buf_size)
        return false;

    return true;
}

struct response_buffer {
    char data[RESPONSE_BODY_MAX];
    size_t len;
};

/*
 * 고정 크기 resp->data에 수신 데이터를 채운다. 버퍼보다 큰 응답이 와도
 * 넘치는 부분은 버리고 저장분만 남기며, curl에는 항상 받은 만큼(chunk_size)을
 * 처리했다고 알려 전송 자체를 에러로 중단시키지 않는다.
 */
static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct response_buffer *resp = (struct response_buffer *)userdata;
    size_t chunk_size = size * nmemb;
    size_t space = sizeof(resp->data) - 1 - resp->len;
    size_t copy_len = chunk_size < space ? chunk_size : space;

    if (copy_len > 0) {
        memcpy(resp->data + resp->len, ptr, copy_len);
        resp->len += copy_len;
        resp->data[resp->len] = '\0';
    }

    return chunk_size;
}

static const char *get_heartbeat_url(void)
{
    const char *env_url = getenv("HEARTBEAT_URL");

    return (env_url != NULL && env_url[0] != '\0') ? env_url : DEFAULT_HEARTBEAT_URL;
}

/*
 * HEARTBEAT_IFACE가 설정 안 됐으면 기본값 wg1로 고정한다.
 * 빈 문자열이면 NULL을 돌려줘 인터페이스 고정을 해제한다(로컬 테스트용).
 */
static const char *get_heartbeat_iface(void)
{
    const char *env_iface = getenv("HEARTBEAT_IFACE");

    if (env_iface == NULL)
        return DEFAULT_HEARTBEAT_IFACE;

    if (env_iface[0] == '\0')
        return NULL;

    return env_iface;
}

/*
 * 아주 단순한 탐색 기반 파서: 응답 바디에서 "config_version" 키의 정수값만
 * 찾아낸다. 엄격한 JSON 검증은 하지 않으며, 못 찾거나 숫자가 아니면 실패.
 */
static bool parse_config_version(const char *json, long *out)
{
    const char *key = "\"config_version\"";
    const char *p = strstr(json, key);
    char *end;
    long val;

    if (p == NULL)
        return false;

    p += strlen(key);
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != ':')
        return false;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;

    val = strtol(p, &end, 10);
    if (end == p)
        return false;

    *out = val;
    return true;
}

/*
 * POST를 1회만 수행한다. 전송 오류는 반환값(CURLcode)으로, HTTP 상태 코드는
 * *http_status로 분리해서 돌려준다. transport가 CURLE_OK가 아니면
 * *http_status는 의미가 없다(0으로 둠). iface가 NULL이 아니면
 * CURLOPT_INTERFACE를 "if!<iface>" 형식으로 적용해 그 인터페이스로만 나가게 한다.
 */
static CURLcode send_heartbeat(const char *url, const char *iface, const char *payload,
                                long *http_status, struct response_buffer *resp)
{
    CURL *curl;
    CURLcode res;
    struct curl_slist *headers = NULL;
    char iface_opt[64];

    resp->len = 0;
    resp->data[0] = '\0';
    *http_status = 0;

    curl = curl_easy_init();
    if (curl == NULL)
        return CURLE_FAILED_INIT;

    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (headers == NULL) {
        curl_easy_cleanup(curl);
        return CURLE_FAILED_INIT;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);

    if (iface != NULL) {
        snprintf(iface_opt, sizeof(iface_opt), "if!%s", iface);
        curl_easy_setopt(curl, CURLOPT_INTERFACE, iface_opt);
    }

    res = curl_easy_perform(curl);
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return res;
}

/*
 * 한 주기치 작업: mac/rtsp_ok/wg_ok 수집 -> payload 생성 -> 전송 1회 -> 응답 처리.
 * 어떤 단계가 실패해도(payload 생성 실패, 전송 실패, HTTP 오류, JSON 파싱 실패)
 * 여기서 로그만 남기고 반환한다 - 호출자가 그대로 다음 주기로 넘어간다.
 */
static void run_heartbeat_cycle(const char *url, const char *iface)
{
    char mac[MAC_TEXT_LEN + 1];
    char payload[256];
    struct response_buffer resp;
    bool rtsp_ok;
    bool wg_ok;
    CURLcode transport;
    long http_status;

    if (!read_mac_retrying("wlan0", mac))
        return; /* g_stop이 설정된 경우 */

    rtsp_ok = is_process_running("rkipc");
    wg_ok = is_interface_up("wg1");

    if (!build_payload(payload, sizeof(payload), mac, rtsp_ok, wg_ok, 0)) {
        fprintf(stderr, "heartbeat: failed to build payload, skip this cycle\n");
        return;
    }

    transport = send_heartbeat(url, iface, payload, &http_status, &resp);

    if (transport != CURLE_OK) {
        fprintf(stderr, "heartbeat: transport error: %s, retry next cycle\n",
                curl_easy_strerror(transport));
        return;
    }

    if (http_status == 200) {
        long config_version;

        if (parse_config_version(resp.data, &config_version))
            printf("heartbeat: 200 OK, config_version=%ld\n", config_version);
        else
            fprintf(stderr, "heartbeat: 200 OK but failed to parse config_version, body=%s\n", resp.data);
    } else if (http_status == 403) {
        fprintf(stderr, "heartbeat: 403 Forbidden, retry next cycle\n");
    } else {
        fprintf(stderr, "heartbeat: unexpected HTTP status=%ld, retry next cycle\n", http_status);
    }
}

static bool timespec_is_past_or_equal(const struct timespec *a, const struct timespec *b)
{
    if (a->tv_sec != b->tv_sec)
        return a->tv_sec < b->tv_sec;
    return a->tv_nsec <= b->tv_nsec;
}

int main(void)
{
    struct sigaction sa;
    struct timespec next;
    const char *url;
    const char *iface;

    setvbuf(stdout, NULL, _IOLBF, 0);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_stop_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    curl_global_init(CURL_GLOBAL_DEFAULT);

    url = get_heartbeat_url();
    iface = get_heartbeat_iface();

    printf("heartbeat: url=%s iface=%s period=%ds\n", url, iface != NULL ? iface : "(해제됨)", HEARTBEAT_PERIOD_SEC);

    clock_gettime(CLOCK_MONOTONIC, &next);

    while (!g_stop) {
        run_heartbeat_cycle(url, iface);

        if (g_stop)
            break;

        next.tv_sec += HEARTBEAT_PERIOD_SEC;

        /*
         * 전송(및 재시도)이 길어져 다음 주기 시작 시각을 이미 지났으면,
         * 밀린 만큼 몰아서 돌리지 않고 지금 이후의 가장 가까운 주기로 재정렬한다.
         */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        while (timespec_is_past_or_equal(&next, &now))
            next.tv_sec += HEARTBEAT_PERIOD_SEC;

        int rc;
        do {
            rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        } while (rc == EINTR && !g_stop);
    }

    curl_global_cleanup();
    fprintf(stderr, "heartbeat: signal received, terminated\n");

    return 0;
}

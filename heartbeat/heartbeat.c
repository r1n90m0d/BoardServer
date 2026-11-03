#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <stdbool.h>
#include <curl/curl.h>

#define MAC_TEXT_LEN 12
#define DEFAULT_HEARTBEAT_URL "http://10.0.0.1:3000/v1/devices/heartbeat"
#define RESPONSE_BODY_MAX 1024

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
 * POST를 1회만 수행한다. 전송 오류는 반환값(CURLcode)으로, HTTP 상태 코드는
 * *http_status로 분리해서 돌려준다. transport가 CURLE_OK가 아니면
 * *http_status는 의미가 없다(0으로 둠).
 */
static CURLcode send_heartbeat(const char *url, const char *payload,
                                long *http_status, struct response_buffer *resp)
{
    CURL *curl;
    CURLcode res;
    struct curl_slist *headers = NULL;

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

    res = curl_easy_perform(curl);
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return res;
}

int main(void)
{
    char mac[MAC_TEXT_LEN + 1];
    char payload[256];
    struct response_buffer resp;
    bool rtsp_ok;
    bool wg_ok;
    const char *url;
    CURLcode transport;
    long http_status;

    if (read_mac("wlan0", mac) != 0) {
        fprintf(stderr, "failed to read wlan0 MAC\n");
        return 1;
    }

    rtsp_ok = is_process_running("rkipc");
    wg_ok = is_interface_up("wg1");

    printf("mac=%s\n", mac);
    printf("rtsp_ok=%s\n", rtsp_ok ? "true" : "false");
    printf("wg_ok=%s\n", wg_ok ? "true" : "false");

    if (!build_payload(payload, sizeof(payload), mac, rtsp_ok, wg_ok, 0)) {
        fprintf(stderr, "failed to build payload\n");
        return 1;
    }

    url = get_heartbeat_url();

    curl_global_init(CURL_GLOBAL_DEFAULT);
    transport = send_heartbeat(url, payload, &http_status, &resp);
    curl_global_cleanup();

    printf("transport=%s\n", curl_easy_strerror(transport));
    printf("http=%ld\n", http_status);
    printf("body=%s\n", resp.data);

    return 0;
}
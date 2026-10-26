#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <stdarg.h>
#include <math.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <cjson/cJSON.h>

#define PORT 8888
#define HTTP_PORT 8080
#define BUFFER_SIZE 1024
#define MAX_CLIENTS 32
#define LOG_DIR "logs"
#define LOG_FILE "logs/server.log"

#define CONFIG_FILE "config/monitor.conf"

typedef struct
{
    double cpu_threshold;
    double memory_threshold;
    double disk_threshold;
} MonitorConfig;

static MonitorConfig config;

typedef struct
{
    int fd;
    pthread_t thread;
    int occupied;
    int finished;
    int cpu_alert;
    int memory_alert;
    int disk_alert;
} Client;

typedef struct
{
    double cpu;
    double memory;
    double disk;
    int processes;
    int sequence;
} MonitorData;

static Client clients[MAX_CLIENTS];

static pthread_mutex_t clients_mutex =
    PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t log_mutex =
    PTHREAD_MUTEX_INITIALIZER;

static volatile sig_atomic_t running = 1;
static int active_clients = 0;
static FILE *log_fp = NULL;

// The latest accepted sample is shared between TCP workers and the HTTP worker.
static pthread_mutex_t snapshot_mutex = PTHREAD_MUTEX_INITIALIZER;
static MonitorData latest_data = {0};
static int has_latest_data = 0;
static time_t latest_timestamp = 0;
static pthread_t http_thread;
static int http_thread_started = 0;


// 去除配置行两端的空白字符
static char *trim(char *text)
{
    while (isspace((unsigned char)*text))
        text++;

    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
        --end;
    *end = '\0';
    return text;
}

// 严格读取配置：三项阈值必须齐全、唯一且位于 (0, 100]。
// 配置在服务端启动前加载，运行期间只读，无须加锁。
static int load_config(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
    {
        fprintf(stderr, "Cannot open config %s: %s\n",
                path, strerror(errno));
        return -1;
    }

    MonitorConfig loaded = {0};
    unsigned int seen = 0;
    char line[512];
    unsigned int line_no = 0;
    int ok = 1;

    while (fgets(line, sizeof(line), fp))
    {
        line_no++;
        size_t length = strlen(line);
        if (length && line[length - 1] != '\n' && !feof(fp))
        {
            fprintf(stderr, "%s:%u: line too long\n", path, line_no);
            ok = 0;
            break;
        }

        char *entry = trim(line);
        if (*entry == '\0' || *entry == '#')
            continue;

        char *equal = strchr(entry, '=');
        if (!equal)
        {
            fprintf(stderr, "%s:%u: expected KEY=VALUE\n", path, line_no);
            ok = 0;
            break;
        }
        *equal = '\0';
        char *key = trim(entry);
        char *value = trim(equal + 1);

        double *target = NULL;
        unsigned int flag = 0;
        if (strcmp(key, "CPU_THRESHOLD") == 0)
        {
            target = &loaded.cpu_threshold;
            flag = 1;
        }
        else if (strcmp(key, "MEMORY_THRESHOLD") == 0)
        {
            target = &loaded.memory_threshold;
            flag = 2;
        }
        else if (strcmp(key, "DISK_THRESHOLD") == 0)
        {
            target = &loaded.disk_threshold;
            flag = 4;
        }
        else
        {
            fprintf(stderr, "%s:%u: unknown key '%s'\n",
                    path, line_no, key);
            ok = 0;
            break;
        }

        if (seen & flag)
        {
            fprintf(stderr, "%s:%u: duplicate key '%s'\n",
                    path, line_no, key);
            ok = 0;
            break;
        }

        errno = 0;
        char *endptr;
        double number = strtod(value, &endptr);
        if (value == endptr || *endptr != '\0' || errno == ERANGE ||
            !isfinite(number) || number <= 0.0 || number > 100.0)
        {
            fprintf(stderr, "%s:%u: invalid value for %s (must be >0 and <=100)\n",
                    path, line_no, key);
            ok = 0;
            break;
        }
        *target = number;
        seen |= flag;
    }

    if (ferror(fp))
    {
        fprintf(stderr, "Error reading %s\n", path);
        ok = 0;
    }
    if (fclose(fp) != 0)
    {
        fprintf(stderr, "Error closing %s\n", path);
        ok = 0;
    }
    if (ok && seen != 7)
    {
        fprintf(stderr, "%s: missing required threshold(s)\n", path);
        ok = 0;
    }
    if (!ok)
        return -1;

    config = loaded;
    return 0;
}

// 初始化日志
static int init_logger(void)
{
    if (mkdir(LOG_DIR, 0755) == -1 &&
        errno != EEXIST)
    {
        perror("mkdir");
        return -1;
    }

    struct stat st;

    if (stat(LOG_DIR, &st) != 0 ||
        !S_ISDIR(st.st_mode))
    {
        fprintf(stderr, "Invalid log directory\n");
        return -1;
    }

    log_fp = fopen(LOG_FILE, "a");

    if (!log_fp)
    {
        perror("fopen");
        return -1;
    }

    setvbuf(log_fp, NULL, _IOLBF, 0);
    return 0;
}

// 线程安全日志
static void write_log(const char *level,
                      const char *fmt, ...)
{
    char message[2048];
    char timestamp[64];

    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    struct tm tm_info;
    localtime_r(&ts.tv_sec, &tm_info);

    strftime(timestamp, sizeof(timestamp),
             "%Y-%m-%d %H:%M:%S", &tm_info);

    pthread_mutex_lock(&log_mutex);

    FILE *out = strcmp(level, "ERROR") == 0
                    ? stderr : stdout;

    fprintf(out, "[%s.%03ld] [%s] %s\n",
            timestamp,
            ts.tv_nsec / 1000000,
            level, message);
    fflush(out);

    if (log_fp)
    {
        fprintf(log_fp, "[%s.%03ld] [%s] %s\n",
                timestamp,
                ts.tv_nsec / 1000000,
                level, message);
        fflush(log_fp);
    }

    pthread_mutex_unlock(&log_mutex);
}

static void close_logger(void)
{
    pthread_mutex_lock(&log_mutex);

    if (log_fp)
    {
        fclose(log_fp);
        log_fp = NULL;
    }

    pthread_mutex_unlock(&log_mutex);
}

static void handle_signal(int sig)
{
    (void)sig;
    running = 0;
}

static int setup_signals(void)
{
    struct sigaction sa = {0};

    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1)
    {
        perror("sigaction");
        return -1;
    }

    struct sigaction ignore = {0};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);

    if (sigaction(SIGPIPE, &ignore, NULL) == -1)
    {
        perror("sigaction SIGPIPE");
        return -1;
    }

    return 0;
}

// 检查百分比字段
static int read_percentage(const cJSON *root,
                           const char *name,
                           double *value)
{
    const cJSON *item =
        cJSON_GetObjectItemCaseSensitive(root, name);

    if (!cJSON_IsNumber(item))
        return -1;

    double number = item->valuedouble;

    if (!isfinite(number) ||
        number < 0.0 || number > 100.0)
        return -1;

    *value = number;
    return 0;
}

// 检查非负整数
static int read_integer(const cJSON *root,
                        const char *name,
                        int *value,
                        int minimum)
{
    const cJSON *item =
        cJSON_GetObjectItemCaseSensitive(root, name);

    if (!cJSON_IsNumber(item))
        return -1;

    double number = item->valuedouble;

    if (!isfinite(number) ||
        number < minimum ||
        number > 2147483647.0 ||
        floor(number) != number)
        return -1;

    *value = (int)number;
    return 0;
}

// 使用 cJSON 解析并校验数据
static int parse_monitor_data(const char *json,
                              MonitorData *data)
{
    cJSON *root = cJSON_Parse(json);

    if (!root)
        return -1;

    int valid = cJSON_IsObject(root);

    if (valid)
    {
        valid =
            read_percentage(root, "cpu_usage",
                            &data->cpu) == 0 &&
            read_percentage(root, "memory_usage",
                            &data->memory) == 0 &&
            read_percentage(root, "disk_usage",
                            &data->disk) == 0 &&
            read_integer(root, "process_count",
                         &data->processes, 0) == 0 &&
            read_integer(root, "sequence",
                         &data->sequence, 1) == 0;
    }

    cJSON_Delete(root);

    return valid ? 0 : -1;
}

// 检查单个指标的告警和恢复
static void check_metric(const char *name,
                         double value,
                         double threshold,
                         int *alert_state,
                         int slot)
{
    if (value >= threshold && !*alert_state)
    {
        *alert_state = 1;

        write_log("WARN",
                  "Client %d %s ALERT: %.2f%% >= %.2f%%",
                  slot, name, value, threshold);
    }
    else if (value < threshold && *alert_state)
    {
        *alert_state = 0;

        write_log("INFO",
                  "Client %d %s RECOVERED: %.2f%%",
                  slot, name, value);
    }
}

// 处理监控数据
static void process_monitor_data(Client *client,
                                 int slot,
                                 const char *json)
{
    MonitorData data = {0};

    if (parse_monitor_data(json, &data) != 0)
    {
        write_log("ERROR",
                  "Client %d invalid monitoring JSON",
                  slot);
        return;
    }

    pthread_mutex_lock(&snapshot_mutex);
    latest_data = data;
    latest_timestamp = time(NULL);
    has_latest_data = 1;
    pthread_mutex_unlock(&snapshot_mutex);

    write_log("INFO",
              "Client %d seq=%d CPU=%.2f%% MEM=%.2f%% DISK=%.2f%% PROC=%d",
              slot, data.sequence,
              data.cpu, data.memory,
              data.disk, data.processes);

    check_metric("CPU", data.cpu, config.cpu_threshold,
                 &client->cpu_alert, slot);

    check_metric("MEMORY", data.memory, config.memory_threshold,
                 &client->memory_alert, slot);

    check_metric("DISK", data.disk, config.disk_threshold,
                 &client->disk_alert, slot);
}

// 回收结束线程
static void reap_finished_clients(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        pthread_mutex_lock(&clients_mutex);

        int finished = clients[i].occupied &&
                       clients[i].finished;

        pthread_mutex_unlock(&clients_mutex);

        if (!finished)
            continue;

        int err = pthread_join(clients[i].thread, NULL);

        if (err != 0)
        {
            write_log("ERROR",
                      "pthread_join: %s",
                      strerror(err));
            continue;
        }

        pthread_mutex_lock(&clients_mutex);

        clients[i].occupied = 0;
        clients[i].finished = 0;
        clients[i].fd = -1;

        pthread_mutex_unlock(&clients_mutex);

        write_log("INFO",
                  "Reclaimed thread slot %d", i);
    }
}

// 客户端工作线程
static void *client_handler(void *arg)
{
    Client *client = (Client *)arg;
    int fd = client->fd;
    int slot = (int)(client - clients);

    pthread_mutex_lock(&clients_mutex);

    active_clients++;
    int count = active_clients;

    pthread_mutex_unlock(&clients_mutex);

    write_log("INFO",
              "Client %d connected. Active: %d",
              slot, count);

    char buffer[BUFFER_SIZE];
    size_t used = 0;

    while (running)
    {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);

        struct timeval timeout = {0, 200000};

        int ready = select(fd + 1, &readfds,
                           NULL, NULL, &timeout);

        if (ready < 0)
        {
            if (errno == EINTR)
                continue;

            write_log("ERROR",
                      "Client %d select: %s",
                      slot, strerror(errno));
            break;
        }

        if (ready == 0 || !running)
            continue;

        ssize_t n = recv(fd, buffer + used,
                         sizeof(buffer) - used, 0);

        if (n < 0)
        {
            if (errno == EINTR)
                continue;

            write_log("ERROR",
                      "Client %d recv: %s",
                      slot, strerror(errno));
            break;
        }

        if (n == 0)
            break;

        used += (size_t)n;
        size_t start = 0;

        for (size_t i = 0; i < used; i++)
        {
            if (buffer[i] == '\n')
            {
                buffer[i] = '\0';

                process_monitor_data(
                    client, slot, buffer + start);

                start = i + 1;
            }
        }

        if (start > 0)
        {
            memmove(buffer, buffer + start,
                    used - start);
            used -= start;
        }

        if (used == sizeof(buffer))
        {
            write_log("WARN",
                      "Client %d message too large",
                      slot);
            break;
        }
    }

    close(fd);

    pthread_mutex_lock(&clients_mutex);

    active_clients--;
    count = active_clients;
    client->finished = 1;

    pthread_mutex_unlock(&clients_mutex);

    write_log("INFO",
              "Client %d disconnected. Active: %d",
              slot, count);

    return NULL;
}

// 退出时等待所有客户端线程
static int cleanup_all_clients(void)
{
    int failed = 0;

    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (!clients[i].occupied)
            continue;

        int err = pthread_join(clients[i].thread, NULL);

        if (err != 0)
        {
            write_log("ERROR",
                      "Cleanup thread %d: %s",
                      i, strerror(err));
            failed = 1;
            continue;
        }

        clients[i].occupied = 0;
        clients[i].finished = 0;
        clients[i].fd = -1;
    }

    return failed ? -1 : 0;
}


// HTTP response helper: send all bytes, including on partial writes.
static int send_all(int fd, const char *buffer, size_t length)
{
    size_t sent = 0;
    while (sent < length)
    {
        ssize_t n = send(fd, buffer + sent, length - sent, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        sent += (size_t)n;
    }
    return 0;
}

static void respond_http(int fd, const char *status, const char *body)
{
    char header[512];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: application/json; charset=utf-8\r\n"
                     "Access-Control-Allow-Origin: http://localhost:5173\r\n"
                     "Cache-Control: no-store\r\n"
                     "X-Content-Type-Options: nosniff\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n\r\n",
                     status, strlen(body));
    if (n < 0 || (size_t)n >= sizeof(header))
        return;
    if (send_all(fd, header, (size_t)n) == 0)
        (void)send_all(fd, body, strlen(body));
}

static void handle_http_request(int fd)
{
    char request[2048];
    ssize_t n = recv(fd, request, sizeof(request) - 1, 0);
    if (n <= 0)
        return;
    request[n] = '\0';

    if (strncmp(request, "GET /api/monitor HTTP/1.", 24) != 0)
    {
        respond_http(fd, "404 Not Found", "{\"error\":\"not found\"}");
        return;
    }

    MonitorData sample;
    int available;
    time_t timestamp;
    pthread_mutex_lock(&snapshot_mutex);
    sample = latest_data;
    available = has_latest_data;
    timestamp = latest_timestamp;
    pthread_mutex_unlock(&snapshot_mutex);

    char body[512];
    int size;
    if (available)
    {
        size = snprintf(body, sizeof(body),
            "{\"connected\":true,\"cpu_usage\":%.2f,"
            "\"memory_usage\":%.2f,\"disk_usage\":%.2f,"
            "\"process_count\":%d,\"sequence\":%d,"
            "\"timestamp\":%lld,"
            "\"thresholds\":{\"cpu\":%.2f,\"memory\":%.2f,\"disk\":%.2f}}",
            sample.cpu, sample.memory, sample.disk,
            sample.processes, sample.sequence, (long long)timestamp,
            config.cpu_threshold, config.memory_threshold, config.disk_threshold);
    }
    else
    {
        size = snprintf(body, sizeof(body),
            "{\"connected\":false,\"message\":\"waiting for TCP monitoring data\","
            "\"thresholds\":{\"cpu\":%.2f,\"memory\":%.2f,\"disk\":%.2f}}",
            config.cpu_threshold, config.memory_threshold, config.disk_threshold);
    }
    if (size < 0 || (size_t)size >= sizeof(body))
        return;
    respond_http(fd, "200 OK", body);
}

// Dedicated HTTP worker, isolated from the existing TCP accept loop.
static void *http_handler(void *unused)
{
    (void)unused;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        write_log("ERROR", "HTTP socket: %s", strerror(errno));
        return NULL;
    }

    int opt = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(HTTP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 8) < 0)
    {
        write_log("ERROR", "HTTP bind/listen on %d: %s", HTTP_PORT, strerror(errno));
        close(fd);
        return NULL;
    }
    write_log("INFO", "HTTP API listening on http://127.0.0.1:%d/api/monitor", HTTP_PORT);

    while (running)
    {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        struct timeval timeout = {0, 200000};
        int ready = select(fd + 1, &set, NULL, NULL, &timeout);
        if (ready < 0)
        {
            if (errno == EINTR)
                continue;
            write_log("ERROR", "HTTP select: %s", strerror(errno));
            break;
        }
        if (ready == 0 || !running)
            continue;
        int client_fd = accept(fd, NULL, NULL);
        if (client_fd < 0)
        {
            if (errno != EINTR)
                write_log("ERROR", "HTTP accept: %s", strerror(errno));
            continue;
        }
        // Bound time spent on each HTTP request to avoid blocking shutdown.
        struct timeval io_timeout = {1, 0};
        (void)setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO,
                         &io_timeout, sizeof(io_timeout));
        (void)setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO,
                         &io_timeout, sizeof(io_timeout));
        handle_http_request(client_fd);
        close(client_fd);
    }
    close(fd);
    return NULL;
}

int main(void)
{
    if (load_config(CONFIG_FILE) != 0)
        return 1;

    if (setup_signals() != 0)
        return 1;

    if (init_logger() != 0)
        return 1;

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0)
    {
        write_log("ERROR", "socket: %s",
                  strerror(errno));
        close_logger();
        return 1;
    }

    int opt = 1;

    if (setsockopt(server_fd, SOL_SOCKET,
                   SO_REUSEADDR, &opt,
                   sizeof(opt)) < 0)
    {
        write_log("ERROR", "setsockopt: %s",
                  strerror(errno));
        close(server_fd);
        close_logger();
        return 1;
    }

    struct sockaddr_in addr = {0};

    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(server_fd,
             (struct sockaddr *)&addr,
             sizeof(addr)) < 0)
    {
        write_log("ERROR", "bind: %s",
                  strerror(errno));
        close(server_fd);
        close_logger();
        return 1;
    }

    if (listen(server_fd, 10) < 0)
    {
        write_log("ERROR", "listen: %s",
                  strerror(errno));
        close(server_fd);
        close_logger();
        return 1;
    }

    write_log("INFO",
              "Linux Monitor Server v3.0 started");

    write_log("INFO",
              "Listening on 127.0.0.1:%d", PORT);

    write_log("INFO",
              "Maximum concurrent clients: %d",
              MAX_CLIENTS);

    write_log("INFO",
              "CPU threshold: %.2f%%",
              config.cpu_threshold);

    write_log("INFO",
              "Memory threshold: %.2f%%",
              config.memory_threshold);

    write_log("INFO",
              "Disk threshold: %.2f%%",
              config.disk_threshold);

    int http_err = pthread_create(&http_thread, NULL, http_handler, NULL);
    if (http_err != 0)
    {
        write_log("ERROR", "HTTP pthread_create: %s", strerror(http_err));
        close(server_fd);
        close_logger();
        return 1;
    }
    http_thread_started = 1;

    write_log("INFO",
              "Press Ctrl+C to stop");

    int result = 0;

    while (running)
    {
        reap_finished_clients();

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);

        struct timeval timeout = {0, 200000};

        int ready = select(server_fd + 1,
                           &readfds, NULL, NULL,
                           &timeout);

        if (ready < 0)
        {
            if (errno == EINTR)
                continue;

            write_log("ERROR",
                      "Server select: %s",
                      strerror(errno));
            result = 1;
            break;
        }

        if (ready == 0 || !running)
            continue;

        int fd = accept(server_fd, NULL, NULL);

        if (fd < 0)
        {
            if (errno == EINTR)
                continue;

            write_log("ERROR",
                      "accept: %s",
                      strerror(errno));
            continue;
        }

        reap_finished_clients();

        int slot = -1;

        for (int i = 0; i < MAX_CLIENTS; i++)
        {
            if (!clients[i].occupied)
            {
                slot = i;
                break;
            }
        }

        if (slot == -1)
        {
            write_log("WARN",
                      "Maximum clients reached");
            close(fd);
            continue;
        }

        clients[slot].fd = fd;
        clients[slot].occupied = 1;
        clients[slot].finished = 0;

        clients[slot].cpu_alert = 0;
        clients[slot].memory_alert = 0;
        clients[slot].disk_alert = 0;

        int err = pthread_create(
            &clients[slot].thread,
            NULL,
            client_handler,
            &clients[slot]
        );

        if (err != 0)
        {
            write_log("ERROR",
                      "pthread_create: %s",
                      strerror(err));

            close(fd);

            clients[slot].fd = -1;
            clients[slot].occupied = 0;
            clients[slot].finished = 0;
        }
    }

    write_log("INFO", "Stopping server...");

    close(server_fd);

    if (http_thread_started)
    {
        int err = pthread_join(http_thread, NULL);
        if (err != 0)
        {
            write_log("ERROR", "HTTP pthread_join: %s", strerror(err));
            result = 1;
        }
    }

    if (cleanup_all_clients() != 0)
        result = 1;

    if (result == 0)
    {
        write_log("INFO",
                  "All client threads stopped");
        write_log("INFO",
                  "Server stopped safely");
    }
    else
    {
        write_log("WARN",
                  "Server stopped with errors");
    }

    close_logger();

    if (result == 0)
    {
        pthread_mutex_destroy(&clients_mutex);
        pthread_mutex_destroy(&log_mutex);
        pthread_mutex_destroy(&snapshot_mutex);
    }

    return result;
}
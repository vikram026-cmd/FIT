#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <termios.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ctype.h>
#include <pthread.h>
#include <stdint.h>

#include "../pass/secrets.h"

/* ======================== CONFIGURATION ======================== */

#define UART_DEVICE "/dev/ser1"

#define APP_DIR "/data/home/qnxuser/apps"
#define CSV_FILE APP_DIR "/patient_monitor.csv"
#define PENDING_FILE APP_DIR "/pending_upload.csv"
#define TEMP_PENDING APP_DIR "/pending_upload.tmp"

#define JSON_FILE "/tmp/patient_upload.json"
#define RESPONSE_FILE "/tmp/patient_upload_response.json"

#define UART_BAUD B115200
#define BUFFER_SIZE 256
#define QUEUE_CAPACITY 128

#define RELAY_GPIO 17
#define HEART_RATE_RELAY_THRESHOLD 130

#define TIMEZONE "IST-5:30"

#define PRIORITY_RELAY 65
#define PRIORITY_UART 60
#define PRIORITY_PROCESS 55
#define PRIORITY_CSV 30
#define PRIORITY_UPLOAD 20

/* ======================== DATA TYPES =========================== */

typedef struct
{
    char timestamp[32];
    int heart_rate;
    float temperature;
    float humidity;
    int ntc;
} PatientSample;

typedef struct
{
    char items[QUEUE_CAPACITY][BUFFER_SIZE];
    unsigned head;
    unsigned tail;
    unsigned count;
    int closed;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} LineQueue;

typedef struct
{
    PatientSample items[QUEUE_CAPACITY];
    unsigned head;
    unsigned tail;
    unsigned count;
    int closed;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} SampleQueue;

/* ======================== GLOBAL VARIABLES ===================== */

static volatile sig_atomic_t running = 1;

static int relay_state = -1;
static int uart_fd = -1;

static LineQueue uart_queue;

static SampleQueue relay_queue;
static SampleQueue csv_queue;
static SampleQueue upload_queue;

/* ======================== SIGNAL HANDLER ======================= */

static void signal_handler(int sig)
{
    (void)sig;
    running = 0;
}

/* ======================== DELAY ================================ */

static void delay_ms(long milliseconds)
{
    struct timespec req;

    req.tv_sec = milliseconds / 1000;
    req.tv_nsec = (milliseconds % 1000) * 1000000L;

    nanosleep(&req, NULL);
}

/* ======================== TIME INITIALIZATION ================== */

static int initialize_system_time(void)
{
    if (setenv("TZ", TIMEZONE, 1) != 0)
    {
        perror("[TIME] setenv");
        return -1;
    }

    tzset();

    time_t now = time(NULL);
    struct tm tm_now;
    char stamp[32];

    if (localtime_r(&now, &tm_now) == NULL)
        return -1;

    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_now);

    printf("[TIME] Local time: %s IST\n", stamp);

    return 0;
}

/* ======================== TIMESTAMP ============================= */

static int get_timestamp(char *out, size_t size)
{
    time_t now = time(NULL);
    struct tm tm_now;

    if (localtime_r(&now, &tm_now) == NULL)
        return -1;

    if (tm_now.tm_year + 1900 < 2020)
    {
        fprintf(stderr, "[TIME] System clock is invalid.\n");
        return -1;
    }

    if (strftime(out, size, "%Y-%m-%d %H:%M:%S", &tm_now) == 0)
        return -1;

    return 0;
}

/* ======================== QUEUE INITIALIZATION ================== */

static int line_queue_init(LineQueue *q)
{
    memset(q, 0, sizeof(*q));

    if (pthread_mutex_init(&q->mutex, NULL) != 0)
        return -1;

    if (pthread_cond_init(&q->not_empty, NULL) != 0)
        return -1;

    if (pthread_cond_init(&q->not_full, NULL) != 0)
        return -1;

    return 0;
}

static int sample_queue_init(SampleQueue *q)
{
    memset(q, 0, sizeof(*q));

    if (pthread_mutex_init(&q->mutex, NULL) != 0)
        return -1;

    if (pthread_cond_init(&q->not_empty, NULL) != 0)
        return -1;

    if (pthread_cond_init(&q->not_full, NULL) != 0)
        return -1;

    return 0;
}

/* ======================== LINE QUEUE ============================ */

static int line_queue_push(LineQueue *q, const char *line)
{
    pthread_mutex_lock(&q->mutex);

    while (q->count == QUEUE_CAPACITY && !q->closed && running)
        pthread_cond_wait(&q->not_full, &q->mutex);

    if (q->closed || !running)
    {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }

    snprintf(q->items[q->tail], BUFFER_SIZE, "%s", line);

    q->tail = (q->tail + 1U) % QUEUE_CAPACITY;
    q->count++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);

    return 0;
}

static int line_queue_pop(LineQueue *q, char *out)
{
    pthread_mutex_lock(&q->mutex);

    while (q->count == 0 && !q->closed)
        pthread_cond_wait(&q->not_empty, &q->mutex);

    if (q->count == 0)
    {
        pthread_mutex_unlock(&q->mutex);
        return 0;
    }

    snprintf(out, BUFFER_SIZE, "%s", q->items[q->head]);

    q->head = (q->head + 1U) % QUEUE_CAPACITY;
    q->count--;

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);

    return 1;
}

/* ======================== SAMPLE QUEUE ========================== */

static int sample_queue_push(SampleQueue *q, const PatientSample *sample)
{
    pthread_mutex_lock(&q->mutex);

    while (q->count == QUEUE_CAPACITY && !q->closed && running)
        pthread_cond_wait(&q->not_full, &q->mutex);

    if (q->closed || !running)
    {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }

    q->items[q->tail] = *sample;

    q->tail = (q->tail + 1U) % QUEUE_CAPACITY;
    q->count++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);

    return 0;
}

static int sample_queue_pop(SampleQueue *q, PatientSample *out)
{
    pthread_mutex_lock(&q->mutex);

    while (q->count == 0 && !q->closed)
        pthread_cond_wait(&q->not_empty, &q->mutex);

    if (q->count == 0)
    {
        pthread_mutex_unlock(&q->mutex);
        return 0;
    }

    *out = q->items[q->head];

    q->head = (q->head + 1U) % QUEUE_CAPACITY;
    q->count--;

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);

    return 1;
}

/* ======================== QUEUE CLOSE =========================== */

static void close_line_queue(LineQueue *q)
{
    pthread_mutex_lock(&q->mutex);

    q->closed = 1;

    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);

    pthread_mutex_unlock(&q->mutex);
}

static void close_sample_queue(SampleQueue *q)
{
    pthread_mutex_lock(&q->mutex);

    q->closed = 1;

    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);

    pthread_mutex_unlock(&q->mutex);
}

/* ======================== RELAY CONTROL ========================= */

static int run_gpio_command(const char *state)
{
    char command[96];

    snprintf(command, sizeof(command),
             "gpio-rp1 set %d %s", RELAY_GPIO, state);

    return system(command);
}

static int initialize_relay(void)
{
    if (system("gpio-rp1 set 17 op pn dl") != 0)
    {
        fprintf(stderr, "[RELAY] GPIO17 initialization failed.\n");
        relay_state = -1;
        return -1;
    }

    relay_state = 0;

    printf("[RELAY] GPIO17 initialized LOW (OFF).\n");

    return 0;
}

/* ======================== REVERSED NTC RELAY LOGIC ============== */

static void set_relay_for_sample(const PatientSample *s)
{
    /*
       REVERSED NTC LOGIC:

       NTC = 0 -> Relay OFF
       NTC = 1 -> Relay ON

       Heart rate > 130 BPM -> Relay ON

       Relay ON if:
       (NTC == 1) OR (Heart rate > 130)
    */

    int required =
        (s->ntc == 1 ||
         s->heart_rate > HEART_RATE_RELAY_THRESHOLD) ? 1 : 0;

    if (required == relay_state)
        return;

    if (run_gpio_command(required ? "dh" : "dl") == 0)
    {
        relay_state = required;

        printf("[RELAY] %s | NTC=%d | Heart rate=%d BPM\n",
               relay_state ? "ON" : "OFF",
               s->ntc,
               s->heart_rate);
    }
    else
    {
        fprintf(stderr, "[RELAY] GPIO update failed.\n");
    }
}

/* ======================== RELAY THREAD ========================== */

static void *relay_thread(void *arg)
{
    (void)arg;

    PatientSample s;

    printf("[THREAD] Relay controller started.\n");

    while (sample_queue_pop(&relay_queue, &s))
        set_relay_for_sample(&s);

    run_gpio_command("dl");
    relay_state = 0;

    printf("[THREAD] Relay controller stopped; relay OFF.\n");

    return NULL;
}

/* ======================== UART DRIVER =========================== */

static int start_uart_driver(void)
{
    int fd = open(UART_DEVICE, O_RDWR | O_NOCTTY);

    if (fd >= 0)
        return fd;

    printf("[UART] Starting UART driver...\n");

    if (system("msix-rp1 -m set 25l") != 0)
    {
        fprintf(stderr, "[UART] Pinmux configuration failed.\n");
        return -1;
    }

    if (system("gpio-rp1 set 14,15 a4") != 0)
    {
        fprintf(stderr, "[UART] GPIO14/15 setup failed.\n");
        return -1;
    }

    if (system("devc-serpl011-rpi5 -b115200 -v -c50000000 -E -F -u1 "
               "0x1f00030000,185 >/dev/null 2>&1 &") != 0)
    {
        fprintf(stderr, "[UART] Driver launch failed.\n");
        return -1;
    }

    for (int i = 0; i < 50 && running; i++)
    {
        delay_ms(100);

        fd = open(UART_DEVICE, O_RDWR | O_NOCTTY);

        if (fd >= 0)
            return fd;
    }

    perror("[UART] Cannot open serial device");

    return -1;
}

/* ======================== UART INITIALIZATION =================== */

static int initialize_uart(void)
{
    int fd = start_uart_driver();

    if (fd < 0)
        return -1;

    struct termios t;

    if (tcgetattr(fd, &t) != 0)
    {
        perror("[UART] tcgetattr");
        close(fd);
        return -1;
    }

    t.c_iflag = 0;
    t.c_oflag = 0;
    t.c_lflag = 0;

    t.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
    t.c_cflag |= CS8 | CLOCAL | CREAD;

#ifdef CRTSCTS
    t.c_cflag &= ~CRTSCTS;
#endif

    cfsetispeed(&t, UART_BAUD);
    cfsetospeed(&t, UART_BAUD);

    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &t) != 0)
    {
        perror("[UART] tcsetattr");
        close(fd);
        return -1;
    }

    tcflush(fd, TCIFLUSH);

    printf("[UART] %s ready: 115200, 8-N-1, RAW.\n", UART_DEVICE);

    return fd;
}

/* ======================== CSV STORAGE =========================== */

static int initialize_storage(void)
{
    if (mkdir(APP_DIR, 0755) != 0 && errno != EEXIST)
    {
        perror("[CSV] Cannot create application directory");
        return -1;
    }

    FILE *f = fopen(CSV_FILE, "a+");

    if (!f)
    {
        perror("[CSV] Cannot open file");
        return -1;
    }

    struct stat st;

    if (fstat(fileno(f), &st) == 0 && st.st_size == 0)
    {
        fprintf(f, "Timestamp,HeartRate,Temperature,Humidity,NTC\n");
        fflush(f);
        fsync(fileno(f));
    }

    fclose(f);

    printf("[CSV] Storage ready: %s\n", CSV_FILE);

    return 0;
}

static int save_to_csv(const PatientSample *s)
{
    FILE *f = fopen(CSV_FILE, "a");

    if (!f)
    {
        perror("[CSV] fopen");
        return -1;
    }

    int ok = fprintf(f, "%s,%d,%.2f,%.2f,%d\n",
                     s->timestamp,
                     s->heart_rate,
                     s->temperature,
                     s->humidity,
                     s->ntc);

    if (ok < 0 || fflush(f) != 0 || fsync(fileno(f)) != 0)
    {
        perror("[CSV] write");
        fclose(f);
        return -1;
    }

    fclose(f);

    printf("[CSV] Saved: %s\n", s->timestamp);

    return 0;
}

/* ======================== CSV THREAD ============================ */

static void *csv_thread(void *arg)
{
    (void)arg;

    PatientSample s;

    printf("[THREAD] CSV storage started.\n");

    while (sample_queue_pop(&csv_queue, &s))
    {
        if (save_to_csv(&s) != 0)
            fprintf(stderr, "[CSV] Record could not be saved.\n");
    }

    printf("[THREAD] CSV storage stopped.\n");

    return NULL;
}

/* ======================== OFFLINE QUEUE ========================= */

static int queue_record(const PatientSample *s)
{
    FILE *f = fopen(PENDING_FILE, "a");

    if (!f)
    {
        perror("[QUEUE] fopen pending file");
        return -1;
    }

    int ok = fprintf(f, "%s,%d,%.2f,%.2f,%d\n",
                     s->timestamp,
                     s->heart_rate,
                     s->temperature,
                     s->humidity,
                     s->ntc);

    if (ok < 0 || fflush(f) != 0 || fsync(fileno(f)) != 0)
    {
        perror("[QUEUE] write");
        fclose(f);
        return -1;
    }

    fclose(f);

    printf("[QUEUE] Offline record stored.\n");

    return 0;
}

/* ======================== GOOGLE SHEETS UPLOAD ================== */

static int upload_to_google(const PatientSample *s, const char *status)
{
    FILE *json = fopen(JSON_FILE, "w");

    if (!json)
    {
        perror("[GOOGLE] JSON file");
        return -1;
    }

    fprintf(json,
            "{\"token\":\"%s\",\"rows\":[[\"%s\",%d,%.2f,%.2f,%d,\"%s\"]]}",
            GOOGLE_SCRIPT_TOKEN,
            s->timestamp,
            s->heart_rate,
            s->temperature,
            s->humidity,
            s->ntc,
            status);

    if (fclose(json) != 0)
        return -1;

    char command[2048];

    int n = snprintf(command, sizeof(command),
                     "curl -L -sS --connect-timeout 5 --max-time 20 "
                     "-H 'Content-Type: application/json' "
                     "--data-binary @%s '%s' -o %s",
                     JSON_FILE,
                     GOOGLE_SCRIPT_URL,
                     RESPONSE_FILE);

    if (n < 0 || (size_t)n >= sizeof(command))
    {
        fprintf(stderr, "[GOOGLE] Upload command too long.\n");
        return -1;
    }

    printf("[GOOGLE] Uploading %s record...\n", status);

    if (system(command) != 0)
    {
        fprintf(stderr, "[GOOGLE] curl failed.\n");
        return -1;
    }

    FILE *response = fopen(RESPONSE_FILE, "r");

    if (!response)
        return -1;

    char body[1024];

    size_t count = fread(body, 1, sizeof(body) - 1, response);

    body[count] = '\0';

    fclose(response);

    if (strstr(body, "\"success\":true") != NULL)
    {
        printf("[GOOGLE] Upload successful.\n");
        return 0;
    }

    printf("[GOOGLE] Server response: %s\n", body);

    return -1;
}

/* ======================== RETRY OFFLINE RECORDS ================= */

static int retry_pending_uploads(void)
{
    FILE *in = fopen(PENDING_FILE, "r");

    if (!in)
        return 0;

    FILE *out = fopen(TEMP_PENDING, "w");

    if (!out)
    {
        perror("[QUEUE] Cannot create temporary queue");
        fclose(in);
        return -1;
    }

    char line[512];

    int uploaded = 0;
    int keep_rest = 0;

    printf("[QUEUE] Retrying pending records in FIFO order...\n");

    while (fgets(line, sizeof(line), in))
    {
        if (keep_rest)
        {
            fputs(line, out);
            continue;
        }

        PatientSample s;
        memset(&s, 0, sizeof(s));

        int used = 0;

        if (sscanf(line, "%31[^,],%d,%f,%f,%d%n",
                   s.timestamp,
                   &s.heart_rate,
                   &s.temperature,
                   &s.humidity,
                   &s.ntc,
                   &used) != 5 ||
            (line[used] != '\n' &&
             line[used] != '\r' &&
             line[used] != '\0'))
        {
            fprintf(stderr, "[QUEUE] Invalid line retained.\n");
            fputs(line, out);
            keep_rest = 1;
            continue;
        }

        if (upload_to_google(&s, "local") == 0)
        {
            uploaded++;
        }
        else
        {
            fputs(line, out);
            keep_rest = 1;
        }
    }

    fclose(in);

    fflush(out);
    fsync(fileno(out));
    fclose(out);

    if (rename(TEMP_PENDING, PENDING_FILE) != 0)
    {
        perror("[QUEUE] Replacing pending file");
        return -1;
    }

    printf("[QUEUE] Uploaded pending records: %d\n", uploaded);

    return keep_rest ? -1 : 0;
}

/* ======================== GOOGLE UPLOAD THREAD ================== */

static void *upload_thread(void *arg)
{
    (void)arg;

    PatientSample s;

    printf("[THREAD] Google Sheets uploader started.\n");

    retry_pending_uploads();

    while (sample_queue_pop(&upload_queue, &s))
    {
        struct stat st;

        if (stat(PENDING_FILE, &st) == 0 && st.st_size > 0)
        {
            if (queue_record(&s) != 0)
                fprintf(stderr, "[QUEUE] Could not preserve new record.\n");

            retry_pending_uploads();
            continue;
        }

        if (upload_to_google(&s, "live") != 0)
        {
            fprintf(stderr, "[GOOGLE] Live upload failed; queueing record.\n");
            queue_record(&s);
        }
    }

    printf("[THREAD] Google Sheets uploader stopped.\n");

    return NULL;
}

/* ======================== DATA PARSING ========================== */

static int parse_patient_line(const char *line, PatientSample *s)
{
    int consumed = 0;

    if (sscanf(line, "HR=%d,TEMP=%f,HUM=%f,NTC=%d%n",
               &s->heart_rate,
               &s->temperature,
               &s->humidity,
               &s->ntc,
               &consumed) != 4)
    {
        return -1;
    }

    while (line[consumed] != '\0' &&
           isspace((unsigned char)line[consumed]))
    {
        consumed++;
    }

    if (line[consumed] != '\0')
        return -1;

    /*
       Range comparisons reject NaN and infinity.
       No isfinite() function is used.
    */

    if (s->heart_rate < 0 ||
        s->heart_rate > 250 ||
        !(s->temperature >= -50.0f &&
          s->temperature <= 100.0f) ||
        !(s->humidity >= 0.0f &&
          s->humidity <= 100.0f) ||
        (s->ntc != 0 && s->ntc != 1))
    {
        return -1;
    }

    return get_timestamp(s->timestamp, sizeof(s->timestamp));
}

/* ======================== DATA PROCESSOR THREAD ================= */

static void *processing_thread(void *arg)
{
    (void)arg;

    char line[BUFFER_SIZE];

    PatientSample s;

    printf("[THREAD] Data processor started.\n");

    while (line_queue_pop(&uart_queue, line))
    {
        memset(&s, 0, sizeof(s));

        if (parse_patient_line(line, &s) != 0)
        {
            printf("[INVALID DATA] %s\n", line);
            continue;
        }

        printf("\n[DATA] %s | HR=%d BPM | TEMP=%.2f C | "
               "HUM=%.2f %% | NTC=%d\n",
               s.timestamp,
               s.heart_rate,
               s.temperature,
               s.humidity,
               s.ntc);

        int relay_rc = sample_queue_push(&relay_queue, &s);
        int csv_rc = sample_queue_push(&csv_queue, &s);
        int upload_rc = sample_queue_push(&upload_queue, &s);

        if (relay_rc != 0)
            fprintf(stderr, "[PROCESS] Relay queue rejected sample.\n");

        if (csv_rc != 0)
            fprintf(stderr, "[PROCESS] CSV queue rejected sample.\n");

        if (upload_rc != 0)
            fprintf(stderr, "[PROCESS] Upload queue rejected sample.\n");

        if (!running)
            break;
    }

    close_sample_queue(&relay_queue);
    close_sample_queue(&csv_queue);
    close_sample_queue(&upload_queue);

    printf("[THREAD] Data processor stopped.\n");

    return NULL;
}

/* ======================== UART RECEIVER THREAD ================== */

static void *uart_thread(void *arg)
{
    int fd = *(int *)arg;

    char line[BUFFER_SIZE];

    size_t index = 0;

    int invalid = 0;
    int previous_cr = 0;

    unsigned char ch;

    printf("[THREAD] UART receiver started.\n");

    printf("[UART] Expected format: HR=...,TEMP=...,HUM=...,NTC=...\n");

    while (running)
    {
        fd_set readfds;

        struct timeval timeout;

        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);

        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;

        int ready = select(fd + 1, &readfds, NULL, NULL, &timeout);

        if (ready == 0)
            continue;

        if (ready < 0)
        {
            if (errno == EINTR)
                continue;

            perror("[UART] select");
            break;
        }

        ssize_t n = read(fd, &ch, 1);

        if (n < 0)
        {
            if (errno == EINTR || errno == EAGAIN)
                continue;

            perror("[UART] read");
            break;
        }

        if (n == 0)
            continue;

        if (ch == '\r' || ch == '\n')
        {
            if (ch == '\n' && previous_cr)
            {
                previous_cr = 0;
                continue;
            }

            if (!invalid && index > 0)
            {
                line[index] = '\0';

                printf("[UART RX] %s\n", line);

                if (line_queue_push(&uart_queue, line) != 0)
                    break;
            }
            else if (invalid)
            {
                fprintf(stderr, "[UART] Oversized/invalid line discarded.\n");
            }

            index = 0;
            invalid = 0;
            previous_cr = (ch == '\r');

            continue;
        }

        previous_cr = 0;

        if (ch < 32 || ch > 126)
        {
            invalid = 1;
            continue;
        }

        if (!invalid && index < sizeof(line) - 1)
        {
            line[index++] = (char)ch;
        }
        else
        {
            invalid = 1;
        }
    }

    close_line_queue(&uart_queue);

    printf("[THREAD] UART receiver stopped.\n");

    return NULL;
}

/* ======================== THREAD CREATION ======================= */

static int create_qnx_thread(pthread_t *thread,
                             void *(*entry)(void *),
                             void *arg,
                             int priority,
                             int policy)
{
    pthread_attr_t attr;

    struct sched_param sp;

    int rc = pthread_attr_init(&attr);

    if (rc != 0)
    {
        errno = rc;
        perror("pthread_attr_init");
        return -1;
    }

    memset(&sp, 0, sizeof(sp));

    sp.sched_priority = priority;

    rc = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

    if (rc == 0)
        rc = pthread_attr_setschedpolicy(&attr, policy);

    if (rc == 0)
        rc = pthread_attr_setschedparam(&attr, &sp);

    if (rc != 0)
    {
        errno = rc;
        perror("pthread scheduling attributes");
        pthread_attr_destroy(&attr);
        return -1;
    }

    rc = pthread_create(thread, &attr, entry, arg);

    pthread_attr_destroy(&attr);

    if (rc != 0)
    {
        errno = rc;
        perror("pthread_create");
        return -1;
    }

    return 0;
}

/* ======================== MAIN ================================== */

int main(void)
{
    pthread_t th_uart;
    pthread_t th_process;
    pthread_t th_relay;
    pthread_t th_csv;
    pthread_t th_upload;

    int uart_created = 0;
    int process_created = 0;
    int relay_created = 0;
    int csv_created = 0;
    int upload_created = 0;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("\n");
    printf("============================================\n");
    printf("       QNX PATIENT MONITORING SYSTEM        \n");
    printf("       CSV + GOOGLE SHEETS + OFFLINE        \n");
    printf("============================================\n");

    if (initialize_system_time() != 0)
    {
        fprintf(stderr, "[FATAL] Time initialization failed.\n");
        return EXIT_FAILURE;
    }

    if (initialize_storage() != 0)
    {
        fprintf(stderr, "[FATAL] CSV storage initialization failed.\n");
        return EXIT_FAILURE;
    }

    if (initialize_relay() != 0)
    {
        fprintf(stderr, "[FATAL] Relay initialization failed.\n");
        return EXIT_FAILURE;
    }

    uart_fd = initialize_uart();

    if (uart_fd < 0)
    {
        run_gpio_command("dl");
        return EXIT_FAILURE;
    }

    if (line_queue_init(&uart_queue) != 0 ||
        sample_queue_init(&relay_queue) != 0 ||
        sample_queue_init(&csv_queue) != 0 ||
        sample_queue_init(&upload_queue) != 0)
    {
        fprintf(stderr, "[FATAL] Queue initialization failed.\n");

        close(uart_fd);
        run_gpio_command("dl");

        return EXIT_FAILURE;
    }

    printf("\n[THREAD CONFIGURATION]\n");
    printf("UART receiver       : Priority %d\n", PRIORITY_UART);
    printf("Data processor      : Priority %d\n", PRIORITY_PROCESS);
    printf("Relay controller    : Priority %d\n", PRIORITY_RELAY);
    printf("CSV storage         : Priority %d\n", PRIORITY_CSV);
    printf("Google uploader     : Priority %d\n", PRIORITY_UPLOAD);

    if (create_qnx_thread(&th_relay, relay_thread, NULL,
                          PRIORITY_RELAY, SCHED_FIFO) != 0)
        goto startup_fail;

    relay_created = 1;

    if (create_qnx_thread(&th_csv, csv_thread, NULL,
                          PRIORITY_CSV, SCHED_FIFO) != 0)
        goto startup_fail;

    csv_created = 1;

    if (create_qnx_thread(&th_upload, upload_thread, NULL,
                          PRIORITY_UPLOAD, SCHED_FIFO) != 0)
        goto startup_fail;

    upload_created = 1;

    if (create_qnx_thread(&th_process, processing_thread, NULL,
                          PRIORITY_PROCESS, SCHED_FIFO) != 0)
        goto startup_fail;

    process_created = 1;

    if (create_qnx_thread(&th_uart, uart_thread, &uart_fd,
                          PRIORITY_UART, SCHED_FIFO) != 0)
        goto startup_fail;

    uart_created = 1;

    printf("\n[SYSTEM] Monitoring active.\n");
    printf("[SYSTEM] Press Ctrl+C to stop.\n");

    pthread_join(th_uart, NULL);
    uart_created = 0;

    pthread_join(th_process, NULL);
    process_created = 0;

    pthread_join(th_relay, NULL);
    relay_created = 0;

    pthread_join(th_csv, NULL);
    csv_created = 0;

    pthread_join(th_upload, NULL);
    upload_created = 0;

    close(uart_fd);
    uart_fd = -1;

    printf("\n[SYSTEM] All threads stopped.\n");
    printf("[SYSTEM] CSV file: %s\n", CSV_FILE);
    printf("[SYSTEM] Offline queue: %s\n", PENDING_FILE);

    return EXIT_SUCCESS;

startup_fail:

    fprintf(stderr, "[FATAL] Thread startup failed. Shutting down.\n");

    running = 0;

    close_line_queue(&uart_queue);
    close_sample_queue(&relay_queue);
    close_sample_queue(&csv_queue);
    close_sample_queue(&upload_queue);

    if (uart_fd >= 0)
    {
        close(uart_fd);
        uart_fd = -1;
    }

    if (uart_created)
        pthread_join(th_uart, NULL);

    if (process_created)
        pthread_join(th_process, NULL);

    if (relay_created)
        pthread_join(th_relay, NULL);

    if (csv_created)
        pthread_join(th_csv, NULL);

    if (upload_created)
        pthread_join(th_upload, NULL);

    run_gpio_command("dl");

    return EXIT_FAILURE;
}

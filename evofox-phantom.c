*
 * evofox-phantom – CLI LED control for Evofox / gaming mice
 * Compile: cc -O2 -o evofox-phantom evofox-phantom.c
 * Usage:   ./evofox-phantom [OPTIONS] [led <off|static|slow|fast>]
 *
 * Requires a POSIX system (Linux). Uses <termios.h> for raw mode.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <termios.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#define PROGRAM_NAME     "evofox-phantom"
#define PROGRAM_VERSION  "1.7.0-beta"
#define HIDRAW_DIR       "/sys/class/hidraw"
#define EVOFOX_VID       0x18f8

/* ANSI Color Codes */
#define COLOR_RED     "\x1b[31m"
#define COLOR_GREEN   "\x1b[32m"
#define COLOR_YELLOW  "\x1b[33m"
#define COLOR_CYAN    "\x1b[36m"
#define COLOR_RESET   "\x1b[0m"

/* ---------------------------------------------------------------------------
 * Data types
 * ------------------------------------------------------------------------- */

typedef struct {
    const char *name;
    const unsigned char *data;
    size_t data_len;
} LedMode;

static const unsigned char DATA_SLOW[]   = {0x07, 0x13, 0x7f, 0x10, 0x0f, 0x00, 0x00, 0x00};
static const unsigned char DATA_FAST[]   = {0x07, 0x13, 0x7f, 0x13, 0x0f, 0x00, 0x00, 0x00};
static const unsigned char DATA_STATIC[] = {0x07, 0x13, 0x7f, 0x16, 0x0f, 0x00, 0x00, 0x00};
static const unsigned char DATA_OFF[]    = {0x07, 0x13, 0x7f, 0x17, 0x0f, 0x00, 0x00, 0x00};

static const LedMode MODES[] = {
    {"slow",   DATA_SLOW,   sizeof(DATA_SLOW)},
    {"fast",   DATA_FAST,   sizeof(DATA_FAST)},
    {"static", DATA_STATIC, sizeof(DATA_STATIC)},
    {"off",    DATA_OFF,    sizeof(DATA_OFF)},
};
#define MODES_COUNT (sizeof(MODES) / sizeof(MODES[0]))

typedef struct {
    char *name;
    char *phys_key;
    uint16_t vid;
    uint16_t pid;
    char **nodes;
    size_t node_count;
} PhysicalDevice;

typedef struct {
    bool permission_denied;
    bool device_rejected;
    int  last_errno;
    char last_errmsg[256];
    char **tried_nodes;
    size_t tried_count;
} SendFailure;

typedef struct {
    bool debug;
    bool quiet;
    bool dry_run;
    bool list_only;
    bool beta;
    bool sudo_child;
    bool vid_filter_set;
    uint16_t vid_filter;
    bool pid_filter_set;
    uint16_t pid_filter;
    char *name_filter;
    unsigned char *raw_data;
    size_t raw_data_len;
    const LedMode *led_mode_opt;
} AppConfig;

/* ---------------------------------------------------------------------------
 * Utility functions
 * ------------------------------------------------------------------------- */

static bool use_colors = false;

static void print_err(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    if (use_colors) fprintf(stderr, COLOR_RED "Error: " COLOR_RESET);
    else fprintf(stderr, "Error: ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

static void print_warn(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    if (use_colors) fprintf(stderr, COLOR_YELLOW "Warning: " COLOR_RESET);
    else fprintf(stderr, "Warning: ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

static char *read_file_lossy(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    size_t cap = 256, len = 0;
    char *buf = malloc(cap);
    if (!buf) { fclose(fp); return NULL; }

    size_t nread;
    while ((nread = fread(buf + len, 1, cap - len - 1, fp)) > 0) {
        len += nread;
        if (len == cap - 1) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); fclose(fp); return NULL; }
            buf = nb;
        }
    }
    fclose(fp);
    buf[len] = '\0';
    return buf;
}

static char *uevent_get(const char *uevent, const char *key) {
    size_t klen = strlen(key);
    const char *p = uevent;
    while (*p) {
        if (strncmp(p, key, klen) == 0) {
            const char *v = p + klen;
            const char *eol = strchr(v, '\n');
            size_t vlen = eol ? (size_t)(eol - v) : strlen(v);
            char *out = malloc(vlen + 1);
            if (!out) return NULL;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return out;
        }
        p = strchr(p, '\n');
        if (!p) break;
        p++;
    }
    return NULL;
}

static bool parse_hex_u16(const char *s, uint16_t *out) {
    if (!s) return false;
    if (strncasecmp(s, "0x", 2) == 0) s += 2;
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 16);
    if (errno != 0 || end == s || *end != '\0') return false;
    if (v < 0 || v > 0xFFFF) return false;
    *out = (uint16_t)v;
    return true;
}

static bool parse_hex_bytes(const char *s, unsigned char **out_buf, size_t *out_len) {
    if (!s || !*s) return false;
    const char *p = s;
    if (strncasecmp(p, "0x", 2) == 0) p += 2;

    size_t cap = 16, len = 0;
    unsigned char *buf = malloc(cap);
    if (!buf) return false;

    while (*p) {
        while (*p && (*p == ':' || *p == ',' || isspace((unsigned char)*p))) p++;
        if (!*p) break;

        if (strncasecmp(p, "0x", 2) == 0) p += 2;

        if (!isxdigit((unsigned char)*p)) { free(buf); return false; }

        char tmp[3] = {0};
        tmp[0] = *p++;
        if (isxdigit((unsigned char)*p)) {
            tmp[1] = *p++;
        }

        if (len == cap) {
            cap *= 2;
            unsigned char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return false; }
            buf = nb;
        }
        buf[len++] = (unsigned char)strtoul(tmp, NULL, 16);

        if (*p && *p != ':' && *p != ',' && !isspace((unsigned char)*p) && strncasecmp(p, "0x", 2) != 0) {
            free(buf); return false; 
        }
    }

    *out_buf = buf;
    *out_len = len;
    return len > 0;
}

static char *read_line_trimmed(const char *prompt) {
    if (isatty(STDOUT_FILENO)) {
        fputs(prompt, stdout);
        fflush(stdout);
    }
    char buf[1024];
    if (!fgets(buf, sizeof(buf), stdin)) return NULL;
    
    if (strlen(buf) == sizeof(buf) - 1 && buf[sizeof(buf) - 2] != '\n') {
        int c;
        while ((c = getchar()) != '\n' && c != EOF);
    }

    char *p = buf;
    while (*p && isspace((unsigned char)*p)) p++;
    char *end = p + strlen(p);
    while (end > p && isspace((unsigned char)end[-1])) {
        end--;
        *end = '\0';
    }
    return strdup(p);
}

static char *format_hex_alloc(const unsigned char *data, size_t len) {
    static const char hex[] = "0123456789abcdef";
    char *out = malloc(len * 3 + 1);
    if (!out) return NULL;
    size_t pos = 0;
    for (size_t i = 0; i < len; i++) {
        if (i > 0) out[pos++] = ':';
        out[pos++] = hex[data[i] >> 4];
        out[pos++] = hex[data[i] & 0x0F];
    }
    out[pos] = '\0';
    return out;
}

/* ---------------------------------------------------------------------------
 * Device enumeration
 * ------------------------------------------------------------------------- */

static bool str_contains_ci(const char *hay, const char *needle) {
    if (!needle || !*needle) return true;
    if (!hay) return false;
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (needle[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (needle[i] == '\0') return true;
    }
    return false;
}

static int dev_sort_rank(const char *name) {
    return str_contains_ci(name, "evofox") ? 0 : 1;
}

static int compare_nodes(const void *a, const void *b) {
    const char *na = *(const char *const *)a;
    const char *nb = *(const char *const *)b;
    int ai = -1, bi = -1;
    if (sscanf(na, "/dev/hidraw%d", &ai) != 1) ai = -1;
    if (sscanf(nb, "/dev/hidraw%d", &bi) != 1) bi = -1;
    if (ai != -1 && bi != -1) return ai - bi;
    return strcmp(na, nb);
}

static void sort_nodes(char **nodes, size_t n) {
    qsort(nodes, n, sizeof(char *), compare_nodes);
}

static int compare_physdev(const void *a, const void *b) {
    const PhysicalDevice *da = a;
    const PhysicalDevice *db = b;
    int ra = dev_sort_rank(da->name);
    int rb = dev_sort_rank(db->name);
    if (ra != rb) return ra - rb;
    int c = strcasecmp(da->name, db->name);
    if (c != 0) return c;
    
    size_t n = da->node_count < db->node_count ? da->node_count : db->node_count;
    for (size_t i = 0; i < n; i++) {
        int k = strcmp(da->nodes[i], db->nodes[i]);
        if (k != 0) return k;
    }
    return (da->node_count < db->node_count) ? -1 : (da->node_count > db->node_count) ? 1 : 0;
}

static PhysicalDevice *list_physical_devices(
        bool vid_filter_set, uint16_t vid_filter,
        bool pid_filter_set, uint16_t pid_filter,
        const char *name_filter,
        size_t *out_count)
{
    DIR *dir = opendir(HIDRAW_DIR);
    if (!dir) { *out_count = 0; return NULL; }

    bool filtering = vid_filter_set || pid_filter_set || (name_filter != NULL);

    PhysicalDevice *list = NULL;
    size_t list_count = 0, list_cap = 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "hidraw", 6) != 0) continue;

        char uevent_path[1024];
        snprintf(uevent_path, sizeof(uevent_path), "%s/%s/device/uevent", HIDRAW_DIR, ent->d_name);
        char *uevent = read_file_lossy(uevent_path);
        if (!uevent) continue;

        char *name = uevent_get(uevent, "HID_NAME=");
        if (!name) { free(uevent); continue; }

        char *hid_id = uevent_get(uevent, "HID_ID=");
        if (!hid_id) { free(name); free(uevent); continue; }

        char *save = NULL;
        char *bus = strtok_r(hid_id, ":", &save);
        char *vid_s = strtok_r(NULL, ":", &save);
        char *pid_s = strtok_r(NULL, ":", &save);
        if (!bus || !vid_s || !pid_s) { free(name); free(hid_id); free(uevent); continue; }

        char *end = NULL;
        errno = 0;
        long vid_l = strtol(vid_s, &end, 16);
        if (errno || *end != '\0') { free(name); free(hid_id); free(uevent); continue; }
        errno = 0;
        long pid_l = strtol(pid_s, &end, 16);
        if (errno || *end != '\0') { free(name); free(hid_id); free(uevent); continue; }

        uint16_t vid = (uint16_t)vid_l;
        uint16_t pid = (uint16_t)pid_l;

        if (vid_filter_set && vid != vid_filter) { free(name); free(hid_id); free(uevent); continue; }
        if (pid_filter_set && pid != pid_filter) { free(name); free(hid_id); free(uevent); continue; }
        if (name_filter && !str_contains_ci(name, name_filter)) {
            free(name); free(hid_id); free(uevent); continue;
        }

        if (!filtering) {
            bool name_ok = str_contains_ci(name, "evofox") || str_contains_ci(name, "mouse");
            bool vid_ok = (vid == EVOFOX_VID);
            if (!name_ok && !vid_ok) {
                free(name); free(hid_id); free(uevent); continue;
            }
        }

        char *phys_full = uevent_get(uevent, "HID_PHYS=");
        if (!phys_full || !*phys_full) {
            free(phys_full);
            char fallback[64];
            snprintf(fallback, sizeof(fallback), "%s:%04x:%04x", bus, vid, pid);
            phys_full = strdup(fallback);
            if (!phys_full) { free(name); free(hid_id); free(uevent); continue; }
        }
        
        char *phys_key;
        char *slash_input = strstr(phys_full, "/input");
        if (slash_input) {
            size_t klen = (size_t)(slash_input - phys_full);
            phys_key = malloc(klen + 1);
            if (!phys_key) { free(phys_full); free(name); free(hid_id); free(uevent); continue; }
            memcpy(phys_key, phys_full, klen);
            phys_key[klen] = '\0';
        } else {
            phys_key = strdup(phys_full);
            if (!phys_key) { free(phys_full); free(name); free(hid_id); free(uevent); continue; }
        }
        free(phys_full);

        char dev_node[512];
        snprintf(dev_node, sizeof(dev_node), "/dev/%s", ent->d_name);

        PhysicalDevice *dev = NULL;
        for (size_t i = 0; i < list_count; i++) {
            if (strcmp(list[i].phys_key, phys_key) == 0) {
                dev = &list[i];
                break;
            }
        }

        if (!dev) {
            if (list_count == list_cap) {
                size_t new_cap = list_cap ? list_cap * 2 : 8;
                PhysicalDevice *nl = realloc(list, new_cap * sizeof(PhysicalDevice));
                if (!nl) { free(phys_key); free(name); free(hid_id); free(uevent); continue; }
                list = nl;
                list_cap = new_cap;
            }
            dev = &list[list_count];
            dev->name = strdup(name);
            dev->phys_key = strdup(phys_key);
            if (!dev->name || !dev->phys_key) {
                free(dev->name);
                free(dev->phys_key);
                free(phys_key); free(name); free(hid_id); free(uevent); continue;
            }
            dev->vid = vid;
            dev->pid = pid;
            dev->nodes = NULL;
            dev->node_count = 0;
            list_count++;
        } else {
            if (!str_contains_ci(dev->name, "evofox") && str_contains_ci(name, "evofox")) {
                char *better = strdup(name);
                if (better) { free(dev->name); dev->name = better; }
            }
        }

        char *new_node = strdup(dev_node);
        if (!new_node) { free(phys_key); free(name); free(hid_id); free(uevent); continue; }
        
        char **new_nodes = realloc(dev->nodes, (dev->node_count + 1) * sizeof(char *));
        if (!new_nodes) { 
            free(new_node); free(phys_key); free(name); free(hid_id); free(uevent); 
            continue; 
        }
        dev->nodes = new_nodes;
        dev->nodes[dev->node_count++] = new_node;

        free(name);
        free(hid_id);
        free(phys_key);
        free(uevent);
    }
    closedir(dir);

    for (size_t i = 0; i < list_count; i++) {
        sort_nodes(list[i].nodes, list[i].node_count);
    }

    if (list_count > 1) {
        qsort(list, list_count, sizeof(PhysicalDevice), compare_physdev);
    }

    *out_count = list_count;
    return list;
}

static void free_physical_devices(PhysicalDevice *list, size_t count) {
    if (!list) return;
    for (size_t i = 0; i < count; i++) {
        free(list[i].name);
        free(list[i].phys_key);
        for (size_t j = 0; j < list[i].node_count; j++) free(list[i].nodes[j]);
        free(list[i].nodes);
    }
    free(list);
}

/* ---------------------------------------------------------------------------
 * Interactive TUI selector
 * ------------------------------------------------------------------------- */

static struct termios saved_termios;
static void tty_sighandler(int s) {
    tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
    _exit(s == SIGINT ? 130 : 1);
}

static void print_menu(const char *title, char *const *items, size_t n, size_t selected) {
    fputs(title, stdout);
    fputs("\r\n", stdout);
    for (size_t i = 0; i < n; i++) {
        fputs(i == selected ? "> " : "  ", stdout);
        fputs(items[i], stdout);
        fputs("\x1b[K\r\n", stdout);
    }
    fflush(stdout);
}

static ssize_t interactive_select(const char *title, char *const *items, size_t n) {
    if (n == 0) return -1;

    bool interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);

    if (!interactive) {
        for (;;) {
            printf("%s\n", title);
            for (size_t i = 0; i < n; i++) {
                printf("  %zu) %s\n", i + 1, items[i]);
            }
            printf("  0) Quit\n");
            char *line = read_line_trimmed("Enter number: ");
            if (!line) return -1;
            if (strcmp(line, "0") == 0 || strcasecmp(line, "q") == 0) { free(line); return -1; }
            
            char *end = NULL;
            errno = 0;
            long v = strtol(line, &end, 10);
            if (errno == 0 && end != line && *end == '\0' && v >= 1 && (size_t)v <= n) {
                free(line);
                return v - 1;
            }
            print_err("Invalid selection, please try again.");
            free(line);
        }
    }

    struct termios orig, raw;
    if (tcgetattr(STDIN_FILENO, &orig) != 0) {
        print_err("could not enable raw terminal mode.");
        return -1;
    }
    raw = orig;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0; 
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        print_err("could not enable raw terminal mode.");
        return -1;
    }

    saved_termios = orig;
    signal(SIGINT,  tty_sighandler);
    signal(SIGTERM, tty_sighandler);
    signal(SIGQUIT, tty_sighandler);

    size_t selected = 0;
    print_menu(title, items, n, selected);

    ssize_t result = -1;

    for (;;) {
        unsigned char buf[8];
        ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue; 

        if (r == 1 && buf[0] == 0x1b) { result = -1; break; }

        if (r >= 3 && buf[0] == 0x1b && buf[1] == 0x5b) {
            if (buf[2] == 0x41 && selected > 0) {
                selected--;
            } else if (buf[2] == 0x42 && selected < n - 1) {
                selected++;
            }
        } else if (r >= 1) {
            unsigned char c = buf[0];
            if (c == '\r' || c == '\n') { result = (ssize_t)selected; break; }
            else if (c == 'k' && selected > 0) selected--;
            else if (c == 'j' && selected < n - 1) selected++;
            else if (c == 'q' || c == 0x03) { result = -1; break; }
        }

        printf("\x1b[%zuA\x1b[J", n + 1);
        print_menu(title, items, n, selected);
    }

    printf("\x1b[%zuA\x1b[J", n + 1);
    fflush(stdout);

    signal(SIGINT,  SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);

    tcsetattr(STDIN_FILENO, TCSANOW, &orig);
    return result;
}

static ssize_t ask_user_to_pick(PhysicalDevice *devices, size_t count, bool debug) {
    char **items = malloc(count * sizeof(char *));
    if (!items) return -1;
    for (size_t i = 0; i < count; i++) {
        char buf[512];
        if (debug) {
            snprintf(buf, sizeof(buf), "%s (VID=%04x PID=%04x)",
                     devices[i].name, devices[i].vid, devices[i].pid);
        } else {
            snprintf(buf, sizeof(buf), "%s", devices[i].name);
        }
        items[i] = strdup(buf);
        if (!items[i]) {
            for (size_t j = 0; j < i; j++) free(items[j]);
            free(items);
            return -1;
        }
    }
    ssize_t r = interactive_select("Select device:", items, count);
    for (size_t i = 0; i < count; i++) free(items[i]);
    free(items);
    return r;
}

static const LedMode *ask_led_mode(void) {
    char *items[MODES_COUNT];
    for (size_t i = 0; i < MODES_COUNT; i++) items[i] = (char *)MODES[i].name;
    ssize_t r = interactive_select("Select LED mode:", items, MODES_COUNT);
    if (r < 0) return NULL;
    return &MODES[r];
}

/* ---------------------------------------------------------------------------
 * Writing to the device
 * ------------------------------------------------------------------------- */

static bool send_to_device(char *const *nodes, size_t node_count,
                           const unsigned char *data, size_t data_len,
                           SendFailure *out_fail, char **used_node)
{
    if (used_node) *used_node = NULL;
    
    if (node_count == 0) {
        out_fail->permission_denied = false;
        out_fail->device_rejected = false;
        out_fail->last_errno = ENOENT;
        snprintf(out_fail->last_errmsg, sizeof(out_fail->last_errmsg), "%s", "No hidraw nodes available");
        out_fail->tried_nodes = NULL;
        out_fail->tried_count = 0;
        return false;
    }

    bool permission_denied = false;
    bool device_rejected = false;
    int last_errno = 0;
    char last_errmsg[256] = {0};
    char **tried = NULL;
    size_t tried_count = 0;

    for (size_t i = 0; i < node_count; i++) {
        int fd = open(nodes[i], O_WRONLY | O_CLOEXEC);
        if (fd < 0) {
            if (errno == EACCES || errno == EPERM) permission_denied = true;
            last_errno = errno;
            snprintf(last_errmsg, sizeof(last_errmsg), "%s", strerror(errno));
            
            char **new_tried = realloc(tried, (tried_count + 1) * sizeof(char *));
            if (new_tried) {
                tried = new_tried;
                tried[tried_count] = strdup(nodes[i]);
                if (tried[tried_count]) tried_count++;
            }
            continue;
        }
        ssize_t w = write(fd, data, data_len);
        int wr_err = errno;
        close(fd);
        if (w < 0) {
            if (wr_err == EACCES || wr_err == EPERM) permission_denied = true;
            if (wr_err == EPIPE || wr_err == EINVAL) device_rejected = true;
            last_errno = wr_err;
            snprintf(last_errmsg, sizeof(last_errmsg), "%s", strerror(wr_err));
            
            char **new_tried = realloc(tried, (tried_count + 1) * sizeof(char *));
            if (new_tried) {
                tried = new_tried;
                tried[tried_count] = strdup(nodes[i]);
                if (tried[tried_count]) tried_count++;
            }
            continue;
        }
        
        for (size_t j = 0; j < tried_count; j++) free(tried[j]);
        free(tried);
        
        out_fail->permission_denied = false;
        out_fail->device_rejected = false;
        out_fail->last_errno = 0;
        out_fail->last_errmsg[0] = '\0';
        out_fail->tried_nodes = NULL;
        out_fail->tried_count = 0;
        if (used_node) *used_node = strdup(nodes[i]);
        return true;
    }

    out_fail->permission_denied = permission_denied;
    out_fail->device_rejected = device_rejected;
    out_fail->last_errno = last_errno;
    snprintf(out_fail->last_errmsg, sizeof(out_fail->last_errmsg), "%s", last_errmsg);
    out_fail->tried_nodes = tried;
    out_fail->tried_count = tried_count;
    return false;
}

static void free_send_failure(SendFailure *f) {
    for (size_t i = 0; i < f->tried_count; i++) free(f->tried_nodes[i]);
    free(f->tried_nodes);
    f->tried_nodes = NULL;
    f->tried_count = 0;
}

/* ---------------------------------------------------------------------------
 * CLI helpers & Auto-Sudo
 * ------------------------------------------------------------------------- */

static bool is_root(void) { return geteuid() == 0; }

static bool auto_sudo(const SendFailure *fail, int argc, char **argv, const char *led_cmd, uint16_t vid, uint16_t pid) {
    if (is_root() || !fail->permission_denied || !isatty(STDIN_FILENO)) return false;

    fprintf(stderr, COLOR_YELLOW "Permission denied. Attempting to re-run with sudo..." COLOR_RESET "\n");

    char exe_path[4096] = {0};
    ssize_t l = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (l < 0 || l == sizeof(exe_path) - 1) {
        strncpy(exe_path, argv[0], sizeof(exe_path) - 1);
        exe_path[sizeof(exe_path) - 1] = '\0';
    } else {
        exe_path[l] = '\0';
    }

    char **new_argv = malloc((argc + 8) * sizeof(char *));
    if (!new_argv) return false;
    
    new_argv[0] = "sudo";
    new_argv[1] = exe_path;
    for (int i = 1; i < argc; i++) new_argv[1 + i] = argv[i];
    
    int idx = argc + 1;
    char vid_buf[8], pid_buf[8];
    snprintf(vid_buf, sizeof(vid_buf), "%04x", vid);
    snprintf(pid_buf, sizeof(pid_buf), "%04x", pid);
    new_argv[idx++] = "--vid";
    new_argv[idx++] = vid_buf;
    new_argv[idx++] = "--pid";
    new_argv[idx++] = pid_buf;
    new_argv[idx++] = "--sudo-child";
    if (led_cmd) {
        new_argv[idx++] = "led";
        new_argv[idx++] = (char *)led_cmd;
    }
    new_argv[idx] = NULL;

    execvp("sudo", new_argv);
    perror("Failed to execute sudo");
    free(new_argv);
    return false;
}

static void print_help(void) {
    printf("%s %s\n", PROGRAM_NAME, PROGRAM_VERSION);
    printf("Control LED modes for Evofox / gaming mice.\n\n");
    printf("Usage:\n");
    printf("  %s [OPTIONS] [led <off|static|slow|fast>]\n\n", PROGRAM_NAME);
    printf("Commands:\n");
    printf("  led <mode>                Set LED mode (off, static, slow, fast)\n\n");
    printf("Options:\n");
    printf("  -h, --help                Show this help\n");
    printf("  -V, --version             Show version\n");
    printf("  -d, --debug               Verbose output\n");
    printf("  -q, --quiet               Suppress success messages\n");
    printf("      --dry-run             Show what would be sent without writing\n");
    printf("      --list                List detected devices and exit\n");
    printf("      --vid <hex>           Filter by vendor ID\n");
    printf("      --pid <hex>           Filter by product ID\n");
    printf("      --name <substring>    Filter by device name\n\n");
    printf("Experimental:\n");
    printf("      --beta                Acknowledge experimental status\n");
    printf("      --raw <hex>           Send a raw HID report (requires --beta)\n\n");
    printf("Examples:\n");
    printf("  # Set LED off on first detected mouse\n");
    printf("  %s led off\n\n", PROGRAM_NAME);
    printf("  # List devices with debug info\n");
    printf("  %s --list --debug\n\n", PROGRAM_NAME);
    printf("  # Preview a raw report (Experimental)\n");
    printf("  %s --dry-run --beta --raw 07:13:7f:17:0f:00:00:00\n", PROGRAM_NAME);
}

static void print_version(void) {
    printf("%s %s\n", PROGRAM_NAME, PROGRAM_VERSION);
}

static void print_device_list(PhysicalDevice *devices, size_t count) {
    if (count == 0) {
        printf("No compatible devices found.\n");
        return;
    }
    printf("Detected devices (%zu):\n", count);
    for (size_t i = 0; i < count; i++) {
        printf("  %zu) %s (VID=%04x PID=%04x)\n", i + 1, devices[i].name, devices[i].vid, devices[i].pid);
        printf("     Nodes: ");
        for (size_t j = 0; j < devices[i].node_count; j++) {
            printf("%s%s", j ? ", " : "", devices[i].nodes[j]);
        }
        printf("\n");
    }
}

/* ---------------------------------------------------------------------------
 * Argument Parsing & Execution
 * ------------------------------------------------------------------------- */

static bool parse_args(int argc, char **argv, AppConfig *config) {
    int arg_idx = 1;
    bool subcommand_parsed = false;

    while (arg_idx < argc) {
        const char *arg = argv[arg_idx];

        if (!subcommand_parsed && strcmp(arg, "led") == 0) {
            if (arg_idx + 1 >= argc) {
                print_err("'led' requires a mode.");
                return false;
            }
            const char *mode_name = argv[arg_idx + 1];
            const LedMode *mode = NULL;
            for (size_t i = 0; i < MODES_COUNT; i++) {
                if (strcmp(MODES[i].name, mode_name) == 0) { mode = &MODES[i]; break; }
            }
            if (!mode) {
                print_err("Invalid LED mode: %s. Choices: off, static, slow, fast", mode_name);
                return false;
            }
            config->led_mode_opt = mode;
            arg_idx += 2;
            subcommand_parsed = true;
            continue;
        }

        if (!subcommand_parsed && strcmp(arg, "--raw") == 0) {
            if (arg_idx + 1 >= argc) {
                print_err("--raw requires a hex string, e.g. --raw 07:13:7f:17:0f:00:00:00");
                return false;
            }
            if (!parse_hex_bytes(argv[arg_idx + 1], &config->raw_data, &config->raw_data_len)) {
                print_err("Invalid hex byte(s): %s", argv[arg_idx + 1]);
                return false;
            }
            arg_idx += 2;
            subcommand_parsed = true;
            continue;
        }

        if (arg[0] == '-') {
            const char *eq = strchr(arg, '=');
            char *key = NULL;
            char *val = NULL;
            
            if (eq) {
                size_t klen = eq - arg;
                key = malloc(klen + 1);
                if (!key) { perror("OOM"); return false; }
                memcpy(key, arg, klen);
                key[klen] = '\0';
                val = strdup(eq + 1);
                if (!val) { free(key); perror("OOM"); return false; }
            } else {
                key = strdup(arg);
                if (!key) { perror("OOM"); return false; }
            }

            if (strcmp(key, "-h") == 0 || strcmp(key, "--help") == 0) { 
                print_help(); free(key); free(val); return false; 
            }
            if (strcmp(key, "-V") == 0 || strcmp(key, "--version") == 0) { 
                print_version(); free(key); free(val); return false; 
            }
            
            if (strcmp(key, "-d") == 0 || strcmp(key, "--debug") == 0) { 
                config->debug = true; free(key); free(val); arg_idx++; continue; 
            }
            if (strcmp(key, "-q") == 0 || strcmp(key, "--quiet") == 0) { 
                config->quiet = true; free(key); free(val); arg_idx++; continue; 
            }
            if (strcmp(key, "--dry-run") == 0) { 
                config->dry_run = true; free(key); free(val); arg_idx++; continue; 
            }
            if (strcmp(key, "--list") == 0) { 
                config->list_only = true; free(key); free(val); arg_idx++; continue; 
            }
            if (strcmp(key, "--sudo-child") == 0) {
                config->sudo_child = true; free(key); free(val); arg_idx++; continue;
            }
            if (strcmp(key, "--beta") == 0) { 
                config->beta = true; free(key); free(val); arg_idx++; continue; 
            }
            
            if (strcmp(key, "--vid") == 0) {
                const char *actual_val = val;
                int next_idx = 1;
                if (!actual_val) {
                    if (arg_idx + 1 >= argc) {
                        print_err("--vid requires a hex value, e.g. --vid 18f8");
                        free(key); free(val); return false;
                    }
                    actual_val = argv[arg_idx + 1];
                    next_idx = 2;
                }
                if (!parse_hex_u16(actual_val, &config->vid_filter)) {
                    print_err("Invalid hex value: %s", actual_val);
                    free(key); free(val); return false;
                }
                config->vid_filter_set = true;
                free(key); free(val); arg_idx += next_idx; continue;
            }
            
            if (strcmp(key, "--pid") == 0) {
                const char *actual_val = val;
                int next_idx = 1;
                if (!actual_val) {
                    if (arg_idx + 1 >= argc) {
                        print_err("--pid requires a hex value, e.g. --pid 1286");
                        free(key); free(val); return false;
                    }
                    actual_val = argv[arg_idx + 1];
                    next_idx = 2;
                }
                if (!parse_hex_u16(actual_val, &config->pid_filter)) {
                    print_err("Invalid hex value: %s", actual_val);
                    free(key); free(val); return false;
                }
                config->pid_filter_set = true;
                free(key); free(val); arg_idx += next_idx; continue;
            }
            
            if (strcmp(key, "--name") == 0) {
                const char *actual_val = val;
                int next_idx = 1;
                if (!actual_val) {
                    if (arg_idx + 1 >= argc) {
                        print_err("--name requires a substring, e.g. --name Evofox");
                        free(key); free(val); return false;
                    }
                    actual_val = argv[arg_idx + 1];
                    next_idx = 2;
                }
                free(config->name_filter);
                config->name_filter = strdup(actual_val);
                if (!config->name_filter) { 
                    free(key); free(val); perror("OOM"); return false; 
                }
                free(key); free(val); arg_idx += next_idx; continue;
            }

            print_err("Unknown option: %s", key);
            free(key); free(val);
            return false;
        }

        print_err("Unexpected argument: %s", arg);
        return false;
    }

    if (config->raw_data && config->led_mode_opt) {
        print_err("cannot combine 'led' subcommand with --raw.");
        return false;
    }

    if (config->raw_data && !config->beta) {
        print_err("--raw is an experimental feature. Use --beta to acknowledge and enable it.");
        return false;
    }

    if (config->raw_data && config->raw_data_len == 0) {
        print_err("--raw requires a non-empty hex string.");
        return false;
    }

    if (config->raw_data && config->raw_data_len % 8 != 0) {
        print_warn("--raw payload length (%zu bytes) is not a multiple of 8 bytes. Some devices may reject this.", config->raw_data_len);
    }

    return true;
}

static int execute_command(AppConfig *config, int argc, char **argv, PhysicalDevice *devices, size_t dev_count) {
    size_t idx;
    if (dev_count == 1) {
        idx = 0;
    } else {
        ssize_t picked = ask_user_to_pick(devices, dev_count, config->debug);
        if (picked < 0) {
            fprintf(stderr, "Quit.\n");
            return 0;
        }
        idx = (size_t)picked;
    }

    PhysicalDevice *target = &devices[idx];
    if (target->vid != EVOFOX_VID && !config->sudo_child) {
        print_warn("%s is not an Evofox device. Compatibility is not guaranteed.", target->name);
    }

    if (config->debug) {
        char nodes_buf[1024] = {0};
        size_t pos = 0;
        for (size_t i = 0; i < target->node_count && pos < sizeof(nodes_buf) - 1; i++) {
            int n = snprintf(nodes_buf + pos, sizeof(nodes_buf) - pos, "%s%s", i ? ", " : "", target->nodes[i]);
            if (n < 0) break;
            pos += (size_t)n;
        }
        fprintf(stderr, "Selected: %s (VID=0x%04x PID=0x%04x)  nodes: %s\n",
                target->name, target->vid, target->pid, nodes_buf);
    }

    char mode_name[64];
    unsigned char *mode_data = NULL;
    size_t mode_data_len = 0;
    bool had_failure = false;

    if (config->raw_data) {
        snprintf(mode_name, sizeof(mode_name), "raw (%zu bytes)", config->raw_data_len);
        mode_data = config->raw_data;
        mode_data_len = config->raw_data_len;
    } else if (config->led_mode_opt) {
        snprintf(mode_name, sizeof(mode_name), "%s", config->led_mode_opt->name);
        mode_data = malloc(config->led_mode_opt->data_len);
        if (!mode_data) { perror("OOM"); return 1; }
        memcpy(mode_data, config->led_mode_opt->data, config->led_mode_opt->data_len);
        mode_data_len = config->led_mode_opt->data_len;
    } else {
        const LedMode *mode = ask_led_mode();
        if (!mode) {
            fprintf(stderr, "Quit.\n");
            return 0;
        }
        snprintf(mode_name, sizeof(mode_name), "%s", mode->name);
        mode_data = malloc(mode->data_len);
        if (!mode_data) { perror("OOM"); return 1; }
        memcpy(mode_data, mode->data, mode->data_len);
        mode_data_len = mode->data_len;
    }

    if (config->dry_run) {
        fprintf(stderr, "Dry run: would send to 1 device(s)\n");
        char *hexbuf = format_hex_alloc(mode_data, mode_data_len);
        if (hexbuf) {
            for (size_t i = 0; i < target->node_count; i++) {
                printf("Would send %zu bytes to %s: %s\n", mode_data_len, target->nodes[i], hexbuf);
            }
            free(hexbuf);
        } else {
            perror("OOM");
        }
    } else {
        SendFailure fail = {0};
        char *used_node = NULL;
        if (send_to_device((char *const *)target->nodes, target->node_count,
                           mode_data, mode_data_len, &fail, &used_node))
        {
            if (config->debug) {
                fprintf(stderr, "Report '%s' (%zu bytes) accepted via %s\n", 
                        mode_name, mode_data_len, used_node ? used_node : "?");
            }
            if (!config->quiet) {
                if (use_colors) printf(COLOR_GREEN "LED mode set:" COLOR_RESET " %s\n", mode_name);
                else printf("LED mode set: %s\n", mode_name);
            }
            free(used_node);
        } else {
            had_failure = true;
            if (fail.permission_denied) {
                const char *led_cmd = (!config->led_mode_opt && !config->raw_data) ? mode_name : NULL;
                if (auto_sudo(&fail, argc, argv, led_cmd, target->vid, target->pid)) {
                    /* Unreachable */
                } else {
                    print_err("Permission denied: cannot write to HID device.");
                    fprintf(stderr, "  Tried nodes: ");
                    for (size_t i = 0; i < fail.tried_count; i++) {
                        fprintf(stderr, "%s%s", i ? ", " : "", fail.tried_nodes[i]);
                    }
                    fprintf(stderr, "\n");
                    fprintf(stderr, "To run with sudo, use:\n  sudo %s", PROGRAM_NAME);
                    for (int i = 1; i < argc; i++) fprintf(stderr, " %s", argv[i]);
                    fprintf(stderr, "\n");
                }
            } else if (fail.device_rejected) {
                print_err("The device actively rejected the HID report (%s).", fail.last_errmsg);
                fprintf(stderr, "This typically happens if the device is not an Evofox Phantom.\n");
                fprintf(stderr, "  Tried nodes: ");
                for (size_t i = 0; i < fail.tried_count; i++) {
                    fprintf(stderr, "%s%s", i ? ", " : "", fail.tried_nodes[i]);
                }
                fprintf(stderr, "\n");
            } else {
                print_err("Failed to send LED report to any interface of: %s", target->name);
                fprintf(stderr, "  Tried nodes: ");
                for (size_t i = 0; i < fail.tried_count; i++) {
                    fprintf(stderr, "%s%s", i ? ", " : "", fail.tried_nodes[i]);
                }
                fprintf(stderr, "\n");
                fprintf(stderr, "  Last error: %s\n", fail.last_errmsg);
            }
            free_send_failure(&fail);
        }
    }

    if (mode_data != config->raw_data) {
        free(mode_data);
    }
    return had_failure ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

int main(int argc, char **argv) {
    use_colors = isatty(STDERR_FILENO);

    AppConfig config = {0};
    if (!parse_args(argc, argv, &config)) {
        free(config.raw_data);
        free(config.name_filter);
        return 1;
    }

    if (config.list_only) {
        size_t count = 0;
        PhysicalDevice *devs = list_physical_devices(
            config.vid_filter_set, config.vid_filter,
            config.pid_filter_set, config.pid_filter,
            config.name_filter, &count);
        print_device_list(devs, count);
        free_physical_devices(devs, count);
        free(config.raw_data);
        free(config.name_filter);
        return 0;
    }

    size_t dev_count = 0;
    PhysicalDevice *devices = list_physical_devices(
        config.vid_filter_set, config.vid_filter,
        config.pid_filter_set, config.pid_filter,
        config.name_filter, &dev_count);

    if (dev_count == 0) {
        print_err("No compatible device found. Is your mouse plugged in?");
        if (config.vid_filter_set || config.pid_filter_set || config.name_filter) {
            fprintf(stderr, "Tip: try without filters, or use --list.\n");
        }
        free_physical_devices(devices, dev_count);
        free(config.raw_data);
        free(config.name_filter);
        return 1;
    }

    int ret = execute_command(&config, argc, argv, devices, dev_count);

    free_physical_devices(devices, dev_count);
    free(config.raw_data);
    free(config.name_filter);

    return ret;
}

// RomM Vita - connection test client.
// Enter your RomM URL (scheme optional - http/https is auto-detected) and a pairing
// code like XXXX-XXXX, or scan the pairing QR with the camera. The code is exchanged
// for a client API token which is saved and verified.

// newlib heap (quirc + OpenSSL need more than the default)
int _newlib_heap_size_user = 48 * 1024 * 1024;

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/camera.h>
#include <psp2/appmgr.h>
#include <psp2/kernel/sysmem.h>
#include <ctype.h>
#include <openssl/md5.h>
#include "quirc/quirc.h"
#include "cjson/cJSON.h"
#include <vita2d.h>
#include <curl/curl.h>

#define DATA_DIR    "ux0:data/RomMVita"
#define CONFIG_PATH DATA_DIR "/config.txt"

// Freegosy palette (sampled from its icon): navy, dark navy, periwinkle, peach, salmon
#define COL_BG     RGBA8(0x14, 0x2b, 0x42, 255)
#define COL_PANEL  RGBA8(0x24, 0x40, 0x60, 255)
#define COL_SEL    RGBA8(0x5a, 0x67, 0x93, 255)
#define COL_TEXT   RGBA8(0xf2, 0xf3, 0xf8, 255)
#define COL_DIM    RGBA8(0x9d, 0xa4, 0xc6, 255)
#define COL_ACCENT RGBA8(0xef, 0xc9, 0xa0, 255)
#define COL_GRAY   RGBA8(0x4a, 0x57, 0x76, 255)
#define COL_YELLOW RGBA8(0xf0, 0xc8, 0x5a, 255)
#define COL_GREEN  RGBA8(0x4d, 0xd0, 0x8a, 255)
#define COL_RED    RGBA8(0xed, 0x71, 0x6b, 255)

typedef enum { ST_IDLE, ST_WORKING, ST_OK, ST_FAIL } Status;

static char g_url[256]   = "";
static char g_code[16]   = "";
static char g_token[128] = "";
static char g_user[64]   = "";

static volatile Status g_status = ST_IDLE;
static volatile int g_enter_library = 0;
static char g_msg[256] = "Not connected";
static SceCtrlData g_pad, g_old;

// ---------------------------------------------------------------- config

static void ensure_dir(void) {
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(DATA_DIR, 0777);
}

static void config_save(void) {
    ensure_dir();
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) return;
    fprintf(f, "url=%s\ntoken=%s\n", g_url, g_token);
    fclose(f);
}

static void config_load(void) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "url=", 4))        snprintf(g_url, sizeof g_url, "%s", line + 4);
        else if (!strncmp(line, "token=", 6)) snprintf(g_token, sizeof g_token, "%s", line + 6);
    }
    fclose(f);
}

// ---------------------------------------------------------------- http

typedef struct { char *data; size_t len; size_t cap; } Buf;

static size_t write_cb(void *p, size_t sz, size_t n, void *ud) {
    Buf *b = ud;
    size_t add = sz * n;
    size_t cap = b->cap ? b->cap : 64 * 1024;
    if (b->len + add > cap) return 0;
    char *nd = realloc(b->data, b->len + add + 1);
    if (!nd) return 0;
    b->data = nd;
    memcpy(b->data + b->len, p, add);
    b->len += add;
    b->data[b->len] = 0;
    return add;
}

// Pull a string value out of a flat JSON object: "key": "value"
static int json_str(const char *json, const char *key, char *out, size_t outsz) {
    if (!json) return 0;
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n') p++;
    if (*p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outsz) {
        if (*p == '\\' && p[1]) p++;
        out[i++] = *p++;
    }
    out[i] = 0;
    return i > 0;
}

// Returns HTTP status (>0), or -1 on transport error (msg filled in err).
// If eff is non-NULL it receives the final URL after redirects.
static long http_request(const char *url, const char *bearer, const char *post_body,
                         Buf *resp, char *eff, size_t effsz, long connect_timeout,
                         char *err, size_t errsz) {
    CURL *c = curl_easy_init();
    if (!c) { snprintf(err, errsz, "curl init failed"); return -1; }

    char curl_err[CURL_ERROR_SIZE] = "";
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    if (post_body) hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    char auth[256];
    if (bearer) {
        snprintf(auth, sizeof auth, "Authorization: Bearer %s", bearer);
        hdrs = curl_slist_append(hdrs, auth);
    }

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "RomMVita/0.1");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, resp);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, curl_err);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, connect_timeout);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 3L);
    // The Vita has no usable CA store, so certificates are not verified.
    // Traffic is still encrypted; it just isn't authenticated.
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    if (post_body) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, post_body);
    }

    CURLcode rc = curl_easy_perform(c);
    long code = -1;
    if (rc == CURLE_OK) {
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        if (eff && effsz) {
            char *e = NULL;
            curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &e);
            snprintf(eff, effsz, "%s", e ? e : "");
        }
    } else {
        snprintf(err, errsz, "%s", curl_err[0] ? curl_err : curl_easy_strerror(rc));
    }
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return code;
}

#include <stdarg.h>
static void set_status(Status s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_status(Status s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_msg, sizeof g_msg, fmt, ap);
    va_end(ap);
    g_status = s;
}

// ---------------------------------------------------------------- url / code helpers

static int has_scheme(const char *s) {
    return !strncasecmp(s, "http://", 7) || !strncasecmp(s, "https://", 8);
}

// Copies "scheme://authority" out of a URL (drops any path/query).
static void origin_of(const char *url, char *out, size_t outsz) {
    const char *p = strstr(url, "://");
    size_t n = p ? (size_t)(strcspn(p + 3, "/?#") + (p + 3 - url)) : strlen(url);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, url, n);
    out[n] = 0;
}

// Private-looking hosts (IPs, bare names, .local etc.) are tried over http first.
static int host_is_local(const char *input) {
    const char *h = strstr(input, "://");
    h = h ? h + 3 : input;
    char host[128];
    size_t n = strcspn(h, "/?#");
    if (n >= sizeof host) n = sizeof host - 1;
    memcpy(host, h, n);
    host[n] = 0;
    if (host[0] == '[') return 1;                       // IPv6 literal
    char *colon = strchr(host, ':');
    if (colon) *colon = 0;
    int all_ip = host[0] != 0;
    for (char *q = host; *q; q++)
        if (!isdigit((unsigned char)*q) && *q != '.') { all_ip = 0; break; }
    if (all_ip) return 1;
    if (!strchr(host, '.')) return 1;                   // bare hostname
    size_t l = strlen(host);
    static const char *suffix[] = { ".local", ".lan", ".home", ".internal", ".localdomain" };
    for (unsigned i = 0; i < sizeof suffix / sizeof *suffix; i++) {
        size_t sl = strlen(suffix[i]);
        if (l > sl && !strcasecmp(host + l - sl, suffix[i])) return 1;
    }
    return 0;
}

// Works out the working "scheme://host[:port]" for a user-entered server address.
// If the scheme is missing, tries both http and https and keeps whichever answers.
static int probe_base(const char *input, char *base, size_t basesz, char *err, size_t errsz) {
    char host[300];
    while (*input == ' ') input++;
    snprintf(host, sizeof host, "%s", input);
    size_t n = strlen(host);
    while (n > 0 && (host[n - 1] == '/' || host[n - 1] == ' ')) host[--n] = 0;
    if (!host[0]) { snprintf(err, errsz, "Enter your RomM URL first"); return 0; }

    char cand[2][320];
    int count = 0;
    if (has_scheme(host)) {
        origin_of(host, cand[count++], sizeof cand[0]);
    } else {
        const char *bare = host;
        char first[320], second[320];
        snprintf(first,  sizeof first,  "%s://%s", host_is_local(bare) ? "http" : "https", bare);
        snprintf(second, sizeof second, "%s://%s", host_is_local(bare) ? "https" : "http", bare);
        origin_of(first,  cand[count++], sizeof cand[0]);
        origin_of(second, cand[count++], sizeof cand[0]);
    }

    char last_err[200] = "";
    for (int i = 0; i < count; i++) {
        set_status(ST_WORKING, "Trying %s ...", cand[i]);
        char url[360], eff[360] = "";
        snprintf(url, sizeof url, "%s/api/heartbeat", cand[i]);
        Buf b = {0};
        char e[200] = "";
        long code = http_request(url, NULL, NULL, &b, eff, sizeof eff, 5L, e, sizeof e);
        int is_json = b.data && b.data[0] == '{';
        free(b.data);
        if (code == 200 && is_json) {
            // honour http->https redirects etc.
            origin_of(eff[0] ? eff : cand[i], base, basesz);
            return 1;
        }
        if (code < 0) snprintf(last_err, sizeof last_err, "%s", e);
        else          snprintf(last_err, sizeof last_err, "no RomM at %s (HTTP %ld)", cand[i], code);
    }
    snprintf(err, errsz, "Can't reach server: %s", last_err);
    return 0;
}

// Normalises a pairing code (any case, with/without dash) to 8 uppercase
// alphanumerics. Fills `dashed` as XXXX-XXXX. Returns 1 if valid.
static int code_normalise(const char *in, char dashed[10], char plain[9]) {
    int n = 0;
    for (; *in; in++) {
        if (isalnum((unsigned char)*in)) {
            if (n >= 8) return 0;
            plain[n++] = (char)toupper((unsigned char)*in);
        } else if (*in != '-' && *in != ' ') {
            return 0;
        }
    }
    if (n != 8) return 0;
    plain[8] = 0;
    snprintf(dashed, 10, "%.4s-%.4s", plain, plain + 4);
    return 1;
}

static int verify_token(const char *base, const char *token, char *err, size_t errsz) {
    char url[320];
    snprintf(url, sizeof url, "%s/api/users/me", base);
    Buf b = {0};
    long code = http_request(url, token, NULL, &b, NULL, 0, 10L, err, errsz);
    int ok = 0;
    if (code == 200) {
        if (!json_str(b.data, "username", g_user, sizeof g_user))
            snprintf(g_user, sizeof g_user, "(unknown)");
        ok = 1;
    } else if (code == 401 || code == 403) {
        snprintf(err, errsz, "Token rejected (HTTP %ld). Pair again.", code);
    } else if (code > 0) {
        snprintf(err, errsz, "Server replied HTTP %ld - is this a RomM URL?", code);
    }
    free(b.data);
    return ok;
}

// POSTs the code; returns HTTP status (or -1) and fills token on 200.
static long exchange_code(const char *base, const char *code, char *token, size_t toksz,
                          char *err, size_t errsz) {
    char url[320], body[64];
    snprintf(url, sizeof url, "%s/api/client-tokens/exchange", base);
    snprintf(body, sizeof body, "{\"code\":\"%s\"}", code);
    Buf b = {0};
    long rc = http_request(url, NULL, body, &b, NULL, 0, 10L, err, errsz);
    if (rc == 200 && !json_str(b.data, "raw_token", token, toksz)) {
        snprintf(err, errsz, "Unexpected reply from server (no token)");
        rc = -1;
    }
    free(b.data);
    return rc;
}

static int connect_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    char base[300], err[200] = "";

    if (!probe_base(g_url, base, sizeof base, err, sizeof err)) {
        set_status(ST_FAIL, "%s", err);
        return sceKernelExitDeleteThread(0);
    }

    // Step 1: if a pairing code was entered, exchange it for a token.
    if (g_code[0]) {
        char dashed[10], plain[9];
        if (!code_normalise(g_code, dashed, plain)) {
            set_status(ST_FAIL, "Pairing code should look like XXXX-XXXX");
            return sceKernelExitDeleteThread(0);
        }
        set_status(ST_WORKING, "Exchanging pairing code...");
        char tok[128] = "";
        long rc = exchange_code(base, dashed, tok, sizeof tok, err, sizeof err);
        // Be lenient about which form the server wants.
        if (rc == 400 || rc == 404 || rc == 422)
            rc = exchange_code(base, plain, tok, sizeof tok, err, sizeof err);

        if (rc != 200) {
            if (rc < 0)                         set_status(ST_FAIL, "%s", err[0] ? err : "Connection failed");
            else if (rc == 404 || rc == 410)    set_status(ST_FAIL, "Pairing code expired or already used");
            else if (rc == 429)                 set_status(ST_FAIL, "Too many attempts - wait a minute");
            else if (rc >= 400 && rc < 500)     set_status(ST_FAIL, "Pairing code invalid (HTTP %ld)", rc);
            else                                set_status(ST_FAIL, "Server error (HTTP %ld)", rc);
            return sceKernelExitDeleteThread(0);
        }
        snprintf(g_token, sizeof g_token, "%s", tok);
        g_code[0] = 0;
    }

    if (!g_token[0]) {
        set_status(ST_FAIL, "Enter a pairing code or scan the QR");
        return sceKernelExitDeleteThread(0);
    }

    // Step 2: verify the token actually works.
    set_status(ST_WORKING, "Verifying token...");
    if (verify_token(base, g_token, err, sizeof err)) {
        snprintf(g_url, sizeof g_url, "%s", base);
        config_save();
        set_status(ST_OK, "Connected as %s  (%s)", g_user, base);
        g_enter_library = 1;
    } else {
        set_status(ST_FAIL, "%s", err[0] ? err : "Verification failed");
    }
    return sceKernelExitDeleteThread(0);
}

static void start_connect(void) {
    if (g_status == ST_WORKING) return;
    g_status = ST_WORKING;
    snprintf(g_msg, sizeof g_msg, "Connecting...");
    SceUID t = sceKernelCreateThread("connect", connect_thread, 0x40, 0x40000, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
    else set_status(ST_FAIL, "Could not start network thread");
}

// ---------------------------------------------------------------- ime

static void draw_frame_begin(void);
static void draw_frame_end(void);

// Shows the system keyboard. Returns 1 if the user confirmed.
static int ime_prompt(const char *title, char *buf, size_t bufsz, int max_len) {
    static uint16_t t16[64], init16[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1],
                    out16[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
    memset(t16, 0, sizeof t16);
    memset(init16, 0, sizeof init16);
    memset(out16, 0, sizeof out16);
    for (int i = 0; title[i] && i < 62; i++) t16[i] = (uint8_t)title[i];
    for (int i = 0; buf[i] && i < SCE_IME_DIALOG_MAX_TEXT_LENGTH; i++) init16[i] = (uint8_t)buf[i];

    SceImeDialogParam p;
    sceImeDialogParamInit(&p);
    p.supportedLanguages = 0x0001FFFF;
    p.languagesForced = SCE_FALSE;
    p.type = SCE_IME_TYPE_BASIC_LATIN;
    p.option = 0;
    p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    p.maxTextLength = max_len;
    p.title = t16;
    p.initialText = init16;
    p.inputTextBuffer = out16;
    if (sceImeDialogInit(&p) < 0) return 0;

    while (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
        draw_frame_begin();
        draw_frame_end();
        vita2d_common_dialog_update();
    }
    SceImeDialogResult res;
    memset(&res, 0, sizeof res);
    sceImeDialogGetResult(&res);
    int ok = (res.button == SCE_IME_DIALOG_BUTTON_ENTER);
    if (ok) {
        size_t n = 0;
        for (int i = 0; out16[i] && n + 1 < bufsz; i++)
            if (out16[i] < 128) buf[n++] = (char)out16[i];  // ASCII only
        buf[n] = 0;
    }
    sceImeDialogTerm();
    return ok;
}

// ---------------------------------------------------------------- ui

static vita2d_pgf *g_font;
static vita2d_texture *g_logo;
static int g_sel = 0;  // 0 url, 1 code, 2 scan, 3 connect, 4 forget

static void text(int x, int y, unsigned col, float s, const char *t) {
    vita2d_pgf_draw_text(g_font, x, y, col, s, t);
}

static void draw_frame_begin(void) {
    vita2d_start_drawing();
    vita2d_clear_screen();
}
static void draw_frame_end(void) {
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

// ---------------------------------------------------------------- qr scan

#define CAM_W 640
#define CAM_H 480

static SceUID g_cam_mem = -1;
static uint8_t *g_cam_base = NULL;

static int cam_alloc(void) {
    if (g_cam_mem >= 0) return 0;
    // Camera buffers must live in physically contiguous memory. Which memory type
    // the firmware accepts varies, so try a few (GPU memory is the usual choice).
    static const struct { unsigned type; unsigned size; } tries[] = {
        { 0x09408060, 0x100000 },  // USER_CDRAM_RW
        { 0x0F208060, 0x100000 },  // USER_MAIN_PHYCONT_NC_RW
        { 0x0F20D060, 0x100000 },  // USER_MAIN_PHYCONT_RW
        { 0x0C208060, 0x100000 },  // USER_RW_UNCACHE
    };
    int first_err = 0;
    for (unsigned i = 0; i < sizeof tries / sizeof *tries; i++) {
        SceUID m = sceKernelAllocMemBlock("cam", tries[i].type, tries[i].size, NULL);
        if (m < 0) { if (!first_err) first_err = m; continue; }
        void *base = NULL;
        if (sceKernelGetMemBlockBase(m, &base) < 0 || !base) {
            sceKernelFreeMemBlock(m);
            continue;
        }
        g_cam_mem = m;
        g_cam_base = base;
        return 0;
    }
    return first_err ? first_err : -1;
}

static int cam_open(int dev) {
    int a = cam_alloc();
    if (a < 0) return a;
    SceCameraInfo info;
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    info.priority = SCE_CAMERA_PRIORITY_SHARE;
    info.format = SCE_CAMERA_FORMAT_YUV420_PLANE;
    info.resolution = SCE_CAMERA_RESOLUTION_640_480;
    info.framerate = SCE_CAMERA_FRAMERATE_30_FPS;
    info.sizeIBase = CAM_W * CAM_H;
    info.sizeUBase = CAM_W * CAM_H / 4;
    info.sizeVBase = CAM_W * CAM_H / 4;
    info.pIBase = g_cam_base;
    info.pUBase = g_cam_base + CAM_W * CAM_H;
    info.pVBase = g_cam_base + CAM_W * CAM_H * 5 / 4;
    info.pitch = 0;
    info.buffer = 0;
    int r = sceCameraOpen(dev, &info);
    if (r < 0) return r;
    r = sceCameraStart(dev);
    if (r < 0) sceCameraClose(dev);
    return r;
}

static void cam_close(int dev) {
    sceCameraStop(dev);
    sceCameraClose(dev);
}

// Parses a RomM pairing payload: <scheme>://host[:port]/pair?code=XXXX-XXXX
// (a bare code is accepted too). Returns 1 on success.
static int parse_pair_payload(const char *s, char *url, size_t urlsz, char *code, size_t codesz) {
    char d[10], p[9];
    url[0] = 0;
    if (has_scheme(s)) {
        const char *c = strstr(s, "code=");
        if (!c) return 0;
        c += 5;
        char tmp[32];
        size_t n = strcspn(c, "&# ");
        if (n >= sizeof tmp) return 0;
        memcpy(tmp, c, n);
        tmp[n] = 0;
        if (!code_normalise(tmp, d, p)) return 0;
        origin_of(s, url, urlsz);
        snprintf(code, codesz, "%s", d);
        return 1;
    }
    if (code_normalise(s, d, p)) {  // bare code
        snprintf(code, codesz, "%s", d);
        return 1;
    }
    return 0;
}

// Full-screen camera scanner. Returns 1 if a pairing QR was read.
static int qr_scan(char *out_url, size_t urlsz, char *out_code, size_t codesz) {
    int dev = SCE_CAMERA_DEVICE_FRONT;
    int r = cam_open(dev);
    if (r < 0) {
        set_status(ST_FAIL, "Camera setup failed: 0x%08X", (unsigned)r);
        return 0;
    }

    struct quirc *q = quirc_new();
    if (!q || quirc_resize(q, CAM_W, CAM_H) < 0) {
        if (q) quirc_destroy(q);
        cam_close(dev);
        set_status(ST_FAIL, "Not enough memory for QR decoder");
        return 0;
    }
    vita2d_texture *tex = vita2d_create_empty_texture(CAM_W / 2, CAM_H / 2);

    int found = 0, frame = 0;
    char hint[96] = "Point the camera at the pairing QR code";
    SceCtrlData pad, old;
    memset(&pad, 0, sizeof pad);
    memset(&old, 0, sizeof old);

    while (!found) {
        old = pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if ((pad.buttons & SCE_CTRL_CIRCLE) && !(old.buttons & SCE_CTRL_CIRCLE)) break;
        if ((pad.buttons & SCE_CTRL_TRIANGLE) && !(old.buttons & SCE_CTRL_TRIANGLE)) {
            cam_close(dev);
            dev = !dev;
            if (cam_open(dev) < 0) { dev = !dev; cam_open(dev); }
        }

        SceCameraRead rd;
        memset(&rd, 0, sizeof rd);
        rd.size = sizeof rd;
        rd.mode = 0;
        int got = sceCameraRead(dev, &rd);
        const uint8_t *y = g_cam_base;

        if (got >= 0 && tex) {
            // half-size greyscale preview
            uint8_t *dst = vita2d_texture_get_datap(tex);
            int stride = vita2d_texture_get_stride(tex);
            for (int row = 0; row < CAM_H / 2; row++) {
                uint32_t *d = (uint32_t *)(dst + row * stride);
                const uint8_t *srow = y + (row * 2) * CAM_W;
                for (int col = 0; col < CAM_W / 2; col++) {
                    uint8_t g = srow[col * 2];
                    d[col] = RGBA8(g, g, g, 255);
                }
            }

            if ((++frame % 3) == 0) {  // decoding is heavy; don't do it every frame
                int w, h;
                uint8_t *img = quirc_begin(q, &w, &h);
                memcpy(img, y, (size_t)w * h);
                quirc_end(q);
                int n = quirc_count(q);
                for (int i = 0; i < n && !found; i++) {
                    struct quirc_code code;
                    struct quirc_data data;
                    quirc_extract(q, i, &code);
                    if (quirc_decode(&code, &data) != QUIRC_SUCCESS) continue;
                    if (parse_pair_payload((const char *)data.payload, out_url, urlsz,
                                           out_code, codesz))
                        found = 1;
                    else
                        snprintf(hint, sizeof hint, "That QR isn't a RomM pairing code");
                }
            }
        }

        draw_frame_begin();
        vita2d_draw_rectangle(0, 0, 960, 544, COL_BG);
        if (tex) vita2d_draw_texture_scale(tex, 160, 16, 2.0f, 2.0f);
        text(60, 520, COL_TEXT, 1.0f, hint);
        text(560, 520, COL_DIM, 1.0f, "Triangle: switch camera   Circle: cancel");
        draw_frame_end();
    }

    if (tex) { vita2d_wait_rendering_done(); vita2d_free_texture(tex); }
    quirc_destroy(q);
    cam_close(dev);
    return found;
}

static int pressed(unsigned btn) { return (g_pad.buttons & btn) && !(g_old.buttons & btn); }

// ---------------------------------------------------------------- library

#define MAX_GAMES 5000
#define PAGE_SIZE 100
#define ROM_DIR   DATA_DIR "/roms/snes"
#define LIB_ROWS  11

typedef struct {
    int      id;
    uint32_t size;
    uint8_t  installed;
    char     name[96];
    char     fs_name[128];
} Game;

static Game *g_games;
static volatile int g_game_count = 0;
static volatile int g_game_total = 0;
static volatile int g_lib_state = 0;      // 0 idle, 1 loading, 2 done, 3 error
static volatile int g_lib_busy = 0;
static volatile int g_lib_restart = 0;
static char g_lib_msg[160] = "";
static char g_lib_toast_pending[200] = "";
static int  g_view_dirty;               // defined with the view below
static void ensure_dirs(void);
static int  g_platform_id = -1;
static char g_search[64] = "";
static int  g_cursor = 0, g_scroll = 0;

static void url_encode(const char *in, char *out, size_t outsz) {
    static const char *hex = "0123456789ABCDEF";
    size_t n = 0;
    for (; *in && n + 4 < outsz; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[n++] = c;
        else { out[n++] = '%'; out[n++] = hex[c >> 4]; out[n++] = hex[c & 15]; }
    }
    out[n] = 0;
}

static void sanitize_filename(const char *in, char *out, size_t outsz) {
    size_t n = 0;
    for (; *in && n + 1 < outsz; in++)
        out[n++] = (isalnum((unsigned char)*in) || *in == '.' || *in == '-' || *in == '_') ? *in : '_';
    out[n] = 0;
}

static void local_rom_path(const Game *g, char *out, size_t outsz) {
    char fn[160];
    sanitize_filename(g->fs_name, fn, sizeof fn);
    snprintf(out, outsz, ROM_DIR "/%s", fn);
}

static int file_exists(const char *path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

static int find_snes_platform(char *err, size_t errsz) {
    char url[320];
    snprintf(url, sizeof url, "%s/api/platforms", g_url);
    Buf b = { .cap = 4 * 1024 * 1024 };
    long code = http_request(url, g_token, NULL, &b, NULL, 0, 10L, err, errsz);
    int id = -1;
    if (code == 200) {
        cJSON *arr = cJSON_Parse(b.data);
        cJSON *p;
        cJSON_ArrayForEach(p, arr) {
            cJSON *slug = cJSON_GetObjectItem(p, "slug");
            cJSON *pid = cJSON_GetObjectItem(p, "id");
            if (cJSON_IsString(slug) && !strcmp(slug->valuestring, "snes") && cJSON_IsNumber(pid)) {
                id = pid->valueint;
                break;
            }
        }
        cJSON_Delete(arr);
        if (id < 0) snprintf(err, errsz, "No SNES platform found in RomM");
    } else if (code > 0) {
        snprintf(err, errsz, "Platforms request failed (HTTP %ld)", code);
    }
    free(b.data);
    return id;
}

// ---- installed index: one directory scan instead of a stat per game

#define MAX_INSTALLED 2048
static char (*g_inst)[160] = NULL;
static int  g_inst_n = 0;

static int cmp_name(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static void scan_installed(void) {
    if (!g_inst) g_inst = calloc(MAX_INSTALLED, sizeof *g_inst);
    if (!g_inst) return;
    int n = 0;
    SceUID d = sceIoDopen(ROM_DIR);
    if (d >= 0) {
        SceIoDirent e;
        memset(&e, 0, sizeof e);
        while (sceIoDread(d, &e) > 0 && n < MAX_INSTALLED) {
            size_t l = strlen(e.d_name);
            if (!SCE_S_ISDIR(e.d_stat.st_mode) && !(l > 5 && !strcmp(e.d_name + l - 5, ".part")))
                snprintf(g_inst[n++], sizeof *g_inst, "%s", e.d_name);
            memset(&e, 0, sizeof e);
        }
        sceIoDclose(d);
    }
    qsort(g_inst, n, sizeof *g_inst, cmp_name);
    g_inst_n = n;
}

static int is_installed(const Game *g) {
    if (!g_inst || !g_inst_n) return 0;
    char fn[160];
    sanitize_filename(g->fs_name, fn, sizeof fn);
    return bsearch(fn, g_inst, g_inst_n, sizeof *g_inst, cmp_name) != NULL;
}

// ---- library cache: the list opens instantly, a background refresh updates it

#define CACHE_PATH DATA_DIR "/library.cache"
static Game *g_stage;
static volatile int g_from_cache = 0;
static volatile int g_follow = 0;               // keep cursor on the same game after a swap
static unsigned long long g_last_refresh = 0;   // microseconds (process time)

static void clean_field(char *s) { for (; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' '; }

static int cache_load(void) {
    FILE *f = fopen(CACHE_PATH, "r");
    if (!f) return 0;
    char line[512], url[256];
    int pid = -1, n = 0;
    if (!fgets(line, sizeof line, f) || sscanf(line, "RV1\t%255[^\t]\t%d", url, &pid) != 2 ||
        strcmp(url, g_url) || pid < 0) { fclose(f); return 0; }
    g_platform_id = pid;
    while (n < MAX_GAMES && fgets(line, sizeof line, f)) {
        Game *g = &g_games[n];
        memset(g, 0, sizeof *g);
        unsigned sz = 0;
        if (sscanf(line, "%d\t%u\t%127[^\t]\t%95[^\r\n]", &g->id, &sz, g->fs_name, g->name) < 3) continue;
        if (!g->name[0]) snprintf(g->name, sizeof g->name, "%s", g->fs_name);
        g->size = sz;
        g->installed = is_installed(g);
        n++;
    }
    fclose(f);
    return n;
}

static void cache_save(const Game *list, int n) {
    ensure_dirs();
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s.tmp", CACHE_PATH);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "RV1\t%s\t%d\n", g_url, g_platform_id);
    for (int i = 0; i < n; i++) {
        char a[128], b[96];
        snprintf(a, sizeof a, "%s", list[i].fs_name); clean_field(a);
        snprintf(b, sizeof b, "%s", list[i].name);    clean_field(b);
        fprintf(f, "%d\t%u\t%s\t%s\n", list[i].id, list[i].size, a, b);
    }
    int bad = fflush(f);
    fclose(f);
    if (!bad) { sceIoRemove(CACHE_PATH); sceIoRename(tmp, CACHE_PATH); }
    else sceIoRemove(tmp);
}

static int library_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    char err[160] = "";
    g_lib_state = 1;
    snprintf(g_lib_msg, sizeof g_lib_msg, "Loading...");

    scan_installed();
    int had_cache = g_game_count > 0;
    if (!had_cache) {
        int n = cache_load();
        if (n > 0) { g_game_count = n; g_game_total = n; g_from_cache = 1; g_view_dirty = 1; had_cache = 1; }
    }

    if (g_platform_id < 0) g_platform_id = find_snes_platform(err, sizeof err);
    if (g_platform_id < 0) {
        snprintf(g_lib_msg, sizeof g_lib_msg, "%s", had_cache ? "Offline - showing cached list"
                                                              : (err[0] ? err : "Could not find SNES platform"));
        g_lib_state = had_cache ? 2 : 3;
        goto done;
    }

    Game *dst = had_cache ? g_stage : g_games;   // refresh behind the visible list if we have one
    int direct = !had_cache, count = 0, offset = 0, total = 1, failed = 0;
    while (offset < total && count < MAX_GAMES) {
        char url[512];
        snprintf(url, sizeof url,
                 "%s/api/roms?platform_ids=%d&limit=%d&offset=%d&order_by=name&order_dir=asc",
                 g_url, g_platform_id, PAGE_SIZE, offset);
        Buf b = { .cap = 16 * 1024 * 1024 };
        long code = http_request(url, g_token, NULL, &b, NULL, 0, 10L, err, sizeof err);
        if (code != 200) {
            if (code > 0) snprintf(g_lib_msg, sizeof g_lib_msg, "Library request failed (HTTP %ld)", code);
            else          snprintf(g_lib_msg, sizeof g_lib_msg, "%s", err);
            free(b.data);
            failed = 1;
            break;
        }
        cJSON *root = cJSON_Parse(b.data);
        free(b.data);
        if (!root) { snprintf(g_lib_msg, sizeof g_lib_msg, "Bad reply from server"); failed = 1; break; }
        cJSON *tot = cJSON_GetObjectItem(root, "total");
        total = cJSON_IsNumber(tot) ? tot->valueint : 0;
        cJSON *items = cJSON_GetObjectItem(root, "items");
        int got = 0;
        cJSON *it;
        cJSON_ArrayForEach(it, items) {
            if (count >= MAX_GAMES) break;
            cJSON *id = cJSON_GetObjectItem(it, "id");
            cJSON *name = cJSON_GetObjectItem(it, "name");
            cJSON *fs = cJSON_GetObjectItem(it, "fs_name");
            cJSON *sz = cJSON_GetObjectItem(it, "fs_size_bytes");
            cJSON *noext = cJSON_GetObjectItem(it, "fs_name_no_ext");
            if (!cJSON_IsNumber(id) || !cJSON_IsString(fs)) continue;
            Game *g = &dst[count];
            memset(g, 0, sizeof *g);
            g->id = id->valueint;
            g->size = cJSON_IsNumber(sz) ? (uint32_t)sz->valuedouble : 0;
            snprintf(g->fs_name, sizeof g->fs_name, "%s", fs->valuestring);
            snprintf(g->name, sizeof g->name, "%s",
                     cJSON_IsString(name) && name->valuestring[0] ? name->valuestring
                     : cJSON_IsString(noext) ? noext->valuestring : fs->valuestring);
            g->installed = is_installed(g);
            count++;
            got++;
        }
        cJSON_Delete(root);
        if (direct) { g_game_count = count; g_game_total = count; }
        if (got == 0) break;
        offset += PAGE_SIZE;
    }

    if (failed) {
        g_lib_state = had_cache ? 2 : 3;              // keep showing what we have
        if (had_cache) snprintf(g_lib_toast_pending, sizeof g_lib_toast_pending, "Refresh failed: %s", g_lib_msg);
    } else {
        if (!direct) {
            // refresh finished: swap it in (installed flags re-read from disk)
            scan_installed();
            for (int i = 0; i < count; i++) g_stage[i].installed = is_installed(&g_stage[i]);
            memcpy(g_games, g_stage, (size_t)count * sizeof(Game));
            g_game_count = count;
            g_game_total = count;
            g_view_dirty = 1;
            g_follow = 1;
        }
        cache_save(g_games, g_game_count);
        g_lib_msg[0] = 0;
        if (g_game_count == 0) snprintf(g_lib_msg, sizeof g_lib_msg, "No SNES games found");
        g_lib_state = 2;
    }
done:
    g_last_refresh = sceKernelGetProcessTimeWide();
    g_lib_busy = 0;
    return sceKernelExitDeleteThread(0);
}

static void library_load(void) {
    if (!g_games) g_games = calloc(MAX_GAMES, sizeof(Game));
    if (!g_stage) g_stage = calloc(MAX_GAMES, sizeof(Game));
    if (!g_games || !g_stage) { g_lib_state = 3; snprintf(g_lib_msg, sizeof g_lib_msg, "Out of memory"); return; }
    if (g_lib_busy) return;
    g_lib_busy = 1;
    g_lib_state = 1;
    SceUID t = sceKernelCreateThread("library", library_thread, 0x40, 0x40000, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
    else { g_lib_busy = 0; g_lib_state = 3; snprintf(g_lib_msg, sizeof g_lib_msg, "Could not start thread"); }
}

// ---- file transfer helpers

static volatile double g_job_now = 0, g_job_total = 0;
static volatile int g_job_cancel = 0;

static int dl_progress(void *u, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un) {
    (void)u; (void)ut; (void)un;
    g_job_total = (double)dt;
    g_job_now = (double)dn;
    return g_job_cancel ? 1 : 0;
}

static size_t dl_write(void *p, size_t sz, size_t n, void *ud) {
    return fwrite(p, sz, n, (FILE *)ud);
}

static void ensure_dirs(void) {
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(DATA_DIR, 0777);
    sceIoMkdir(DATA_DIR "/roms", 0777);
    sceIoMkdir(ROM_DIR, 0777);
}

// Downloads url (authenticated) to dest via a .part file. Returns 0 on success.
static int fetch_file(const char *url, const char *dest, char *err, size_t errsz) {
    char part[300], auth[256];
    snprintf(part, sizeof part, "%s.part", dest);
    FILE *f = fopen(part, "wb");
    if (!f) { snprintf(err, errsz, "Cannot write %s", dest); return -1; }
    static char fbuf[64 * 1024];
    setvbuf(f, fbuf, _IOFBF, sizeof fbuf);

    CURL *c = curl_easy_init();
    char cerr[CURL_ERROR_SIZE] = "";
    struct curl_slist *h = NULL;
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", g_token);
    h = curl_slist_append(h, auth);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "RomMVita/0.3");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, dl_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, cerr);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 512L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, dl_progress);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    int werr = fflush(f);
    fclose(f);

    if (rc == CURLE_OK && werr == 0) {
        sceIoRemove(dest);
        if (sceIoRename(part, dest) < 0) { snprintf(err, errsz, "Could not save file"); return -1; }
        return 0;
    }
    sceIoRemove(part);
    if (g_job_cancel)                       snprintf(err, errsz, "Cancelled");
    else if (rc == CURLE_HTTP_RETURNED_ERROR) snprintf(err, errsz, "Download failed (HTTP %ld)", code);
    else if (werr)                          snprintf(err, errsz, "Write error - is the card full?");
    else                                    snprintf(err, errsz, "%s", cerr[0] ? cerr : curl_easy_strerror(rc));
    return -1;
}

// ---- save sync (RetroArch .srm <-> RomM saves)

enum { K_SAVE = 0, K_STATE = 1 };
typedef struct { int kind; int rom_id; char md5[33]; int save_id; char updated[48]; } SyncRec;
typedef struct { int found; int id; char updated[48]; char hash[64]; } RemoteSave;

static SyncRec g_recs[1024];
static int g_nrecs = 0;
#define SYNC_PATH DATA_DIR "/sync.txt"

static void sync_load(void) {
    FILE *f = fopen(SYNC_PATH, "r");
    if (!f) return;
    char line[200];
    while (g_nrecs < 1024 && fgets(line, sizeof line, f)) {
        SyncRec r;
        memset(&r, 0, sizeof r);
        if (sscanf(line, "%d %d %32s %d %47s", &r.kind, &r.rom_id, r.md5, &r.save_id, r.updated) == 5)
            g_recs[g_nrecs++] = r;
    }
    fclose(f);
}

static void sync_store(void) {
    ensure_dirs();
    FILE *f = fopen(SYNC_PATH, "w");
    if (!f) return;
    for (int i = 0; i < g_nrecs; i++)
        fprintf(f, "%d %d %s %d %s\n", g_recs[i].kind, g_recs[i].rom_id, g_recs[i].md5, g_recs[i].save_id, g_recs[i].updated);
    fclose(f);
}

static SyncRec *rec_find(int kind, int rom_id) {
    for (int i = 0; i < g_nrecs; i++)
        if (g_recs[i].kind == kind && g_recs[i].rom_id == rom_id) return &g_recs[i];
    return NULL;
}

static void rec_set(int kind, int rom_id, const char *md5, int save_id, const char *updated) {
    SyncRec *r = rec_find(kind, rom_id);
    if (!r) {
        if (g_nrecs >= 1024) return;
        r = &g_recs[g_nrecs++];
    }
    r->kind = kind;
    r->rom_id = rom_id;
    snprintf(r->md5, sizeof r->md5, "%s", md5);
    r->save_id = save_id;
    snprintf(r->updated, sizeof r->updated, "%s", updated[0] ? updated : "-");
    sync_store();
}

static int file_md5(const char *path, char out[33]) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    MD5_CTX ctx;
    MD5_Init(&ctx);
    unsigned char buf[4096], dig[16];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) MD5_Update(&ctx, buf, n);
    fclose(f);
    MD5_Final(dig, &ctx);
    for (int i = 0; i < 16; i++) snprintf(out + i * 2, 3, "%02x", dig[i]);
    return 0;
}

// ---- core <-> save-state compatibility
// A state only loads in the core that made it. The state's uncompressed size
// (retro_serialize_size) identifies the core, so we keep a table of known sizes:
// one seeded from real data, the rest learned from states this Vita creates.

typedef struct { char core[48]; long size; } CoreSize;
static CoreSize g_cs[24];
static int g_ncs = 0;
static int  g_last_rom = -1;           // last game we launched, and with which core
static char g_last_core[48] = "";
#define CORES_PATH  DATA_DIR "/cores.txt"
#define LAUNCH_PATH DATA_DIR "/lastlaunch.txt"

static void cores_save(void) {
    ensure_dirs();
    FILE *f = fopen(CORES_PATH, "w");
    if (!f) return;
    for (int i = 0; i < g_ncs; i++) fprintf(f, "%s %ld\n", g_cs[i].core, g_cs[i].size);
    fclose(f);
}

static void cores_load(void) {
    g_ncs = 0;
    snprintf(g_cs[g_ncs].core, sizeof g_cs[0].core, "snes9x_libretro");   // seen in RomM: EmulatorJS Snes9x states
    g_cs[g_ncs++].size = 823432;
    FILE *f = fopen(CORES_PATH, "r");
    if (f) {
        char line[120];
        while (g_ncs < 24 && fgets(line, sizeof line, f)) {
            CoreSize c;
            memset(&c, 0, sizeof c);
            if (sscanf(line, "%47s %ld", c.core, &c.size) == 2) {
                int dup = 0;
                for (int i = 0; i < g_ncs; i++) if (!strcmp(g_cs[i].core, c.core)) { g_cs[i].size = c.size; dup = 1; }
                if (!dup) g_cs[g_ncs++] = c;
            }
        }
        fclose(f);
    }
    f = fopen(LAUNCH_PATH, "r");
    if (f) { if (fscanf(f, "%d %47s", &g_last_rom, g_last_core) != 2) g_last_rom = -1; fclose(f); }
}

static void core_learn(const char *core, long size) {
    for (int i = 0; i < g_ncs; i++)
        if (!strcmp(g_cs[i].core, core)) {
            if (g_cs[i].size != size) { g_cs[i].size = size; cores_save(); }
            return;
        }
    if (g_ncs >= 24) return;
    snprintf(g_cs[g_ncs].core, sizeof g_cs[0].core, "%s", core);
    g_cs[g_ncs++].size = size;
    cores_save();
}

// Known core whose state size matches, or NULL if we've never seen this size.
static const char *core_for_size(long size) {
    for (int i = 0; i < g_ncs; i++)
        if (labs(g_cs[i].size - size) <= 1024) return g_cs[i].core;
    return NULL;
}

static int core_installed(const char *core) {
    char probe[128];
    snprintf(probe, sizeof probe, "ux0:app/RETROVITA/%s.self", core);
    return file_exists(probe);
}

// Uncompressed size of a RetroArch state: RZIP files record it in their header.
static long state_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char h[20];
    size_t n = fread(h, 1, sizeof h, f);
    long sz = -1;
    if (n == sizeof h && !memcmp(h, "#RZIPv", 6)) {
        sz = (long)h[12] | ((long)h[13] << 8) | ((long)h[14] << 16) | ((long)h[15] << 24);
    } else {
        fseek(f, 0, SEEK_END);
        sz = ftell(f);
    }
    fclose(f);
    return sz;
}

static char g_popup_pending[360] = "";   // set by sync threads, shown by the UI

static void popup_add(const char *text) {
    if (!g_popup_pending[0]) snprintf(g_popup_pending, sizeof g_popup_pending, "%s", text);
    else if (!strstr(g_popup_pending, "(and more)")) {
        size_t l = strlen(g_popup_pending);
        if (l + 14 < sizeof g_popup_pending) strcat(g_popup_pending, "\n(and more)");
    }
}

static char g_cfg_savedir[160] = "";
static char g_cfg_statedir[160] = "";
static int  g_cfg_sort_saves = 0, g_cfg_sort_states = 0;   // RetroArch "sort into folders by core"
static int  g_cfg_loaded = 0;

// RetroArch may keep saves in a configured folder; read it from its config.
static void load_retroarch_cfg(void) {
    if (g_cfg_loaded) return;
    g_cfg_loaded = 1;
    FILE *f = fopen("ux0:data/retroarch/retroarch.cfg", "r");
    if (!f) return;
    char line[300];
    while (fgets(line, sizeof line, f)) {
        char *dst = NULL;
        if (!strncmp(line, "sort_savefiles_enable", 21))  { g_cfg_sort_saves  = strstr(line, "true") != NULL; continue; }
        if (!strncmp(line, "sort_savestates_enable", 22)) { g_cfg_sort_states = strstr(line, "true") != NULL; continue; }
        if (!strncmp(line, "savefile_directory", 18))       dst = g_cfg_savedir;
        else if (!strncmp(line, "savestate_directory", 19)) dst = g_cfg_statedir;
        if (!dst) continue;
        char *q = strchr(line, '"');
        char *e = q ? strchr(q + 1, '"') : NULL;
        if (!e) continue;
        *e = 0;
        if (strncmp(q + 1, "ux0:", 4)) continue;
        snprintf(dst, 160, "%s", q + 1);
        size_t n = strlen(dst);
        while (n > 0 && (dst[n - 1] == '/' || dst[n - 1] == '\\')) dst[--n] = 0;
    }
    fclose(f);
}

static void strip_ext(const char *name, char *out, size_t sz) {
    snprintf(out, sz, "%s", name);
    char *dot = strrchr(out, '.');
    if (dot && strlen(dot) <= 6) *dot = 0;
}

// SNES cores in preference order; the first one RetroArch has installed wins.
// If RetroArch's folder can't be read (sandbox) we fall back to the first.
static const char *pick_core(void) {
    static const char *cores[] = {
        "snes9x2010_libretro", "snes9x2005_plus_libretro",
        "snes9x2005_libretro", "snes9x2002_libretro", "snes9x_libretro",
    };
    char probe[128];
    for (unsigned i = 0; i < sizeof cores / sizeof *cores; i++) {
        snprintf(probe, sizeof probe, "ux0:app/RETROVITA/%s.self", cores[i]);
        if (file_exists(probe)) return cores[i];
    }
    return cores[0];
}

// Folder name RetroArch uses when saves are sorted by core: the core's library name.
static void core_sort_dir(const char *core, char *out, size_t sz) {
    static const struct { const char *core, *dir; } map[] = {
        { "snes9x2010_libretro",      "Snes9x 2010" },
        { "snes9x2005_plus_libretro", "Snes9x 2005 Plus" },
        { "snes9x2005_libretro",      "Snes9x 2005" },
        { "snes9x2002_libretro",      "Snes9x 2002" },
        { "snes9x_libretro",          "Snes9x" },
    };
    for (unsigned i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(core, map[i].core)) { snprintf(out, sz, "%s", map[i].dir); return; }
    snprintf(out, sz, "%s", core);
}

// Finds the local .srm / .state for a game. Returns 1 if it exists; out always
// receives the path a newly pulled file should be written to.
static int local_file_path(const Game *g, int kind, char *out, size_t outsz) {
    load_retroarch_cfg();
    char lp[256], base[160];
    local_rom_path(g, lp, sizeof lp);
    const char *fn = strrchr(lp, '/');
    strip_ext(fn ? fn + 1 : lp, base, sizeof base);
    const char *ext = kind == K_STATE ? ".state" : ".srm";
    const char *cfg = kind == K_STATE ? g_cfg_statedir : g_cfg_savedir;

    char dirs[4][160];
    int n = 0;
    if (cfg[0]) snprintf(dirs[n++], sizeof dirs[0], "%s", cfg);
    snprintf(dirs[n++], sizeof dirs[0], kind == K_STATE ? "ux0:data/retroarch/states" : "ux0:data/retroarch/saves");
    snprintf(dirs[n++], sizeof dirs[0], kind == K_STATE ? "ux0:data/retroarch/savestates" : "ux0:data/retroarch/savefiles");
    snprintf(dirs[n++], sizeof dirs[0], "%s", ROM_DIR);  // RetroArch default: next to content

    int sorted = kind == K_STATE ? g_cfg_sort_states : g_cfg_sort_saves;
    for (int i = 0; i < n; i++) {
        if (sorted) {
            // RetroArch keeps files in a per-core subfolder; look through them all
            SceUID d = sceIoDopen(dirs[i]);
            if (d >= 0) {
                SceIoDirent e;
                memset(&e, 0, sizeof e);
                while (sceIoDread(d, &e) > 0) {
                    if (SCE_S_ISDIR(e.d_stat.st_mode)) {
                        snprintf(out, outsz, "%s/%s/%s%s", dirs[i], e.d_name, base, ext);
                        if (file_exists(out)) { sceIoDclose(d); return 1; }
                    }
                    memset(&e, 0, sizeof e);
                }
                sceIoDclose(d);
            }
        } else {
            snprintf(out, outsz, "%s/%s%s", dirs[i], base, ext);
            if (file_exists(out)) return 1;
        }
    }
    if (sorted) {
        char sub[64];
        core_sort_dir(pick_core(), sub, sizeof sub);
        snprintf(out, outsz, "%s/%s/%s%s", dirs[0], sub, base, ext);
    } else {
        snprintf(out, outsz, "%s/%s%s", dirs[0], base, ext);
    }
    return 0;
}

static void mkdirs_for(const char *path) {
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 5; *p; p++)
        if (*p == '/') { *p = 0; sceIoMkdir(tmp, 0777); *p = '/'; }
}

static int remote_latest(int kind, int rom_id, RemoteSave *out, char *err, size_t errsz) {
    char url[300];
    snprintf(url, sizeof url, "%s/api/%s?rom_id=%d", g_url, kind == K_STATE ? "states" : "saves", rom_id);
    Buf b = { .cap = 4 * 1024 * 1024 };
    long code = http_request(url, g_token, NULL, &b, NULL, 0, 10L, err, errsz);
    memset(out, 0, sizeof *out);
    if (code != 200) {
        if (code == 401 || code == 403) snprintf(err, errsz, "Token has no save/state access (HTTP %ld)", code);
        else if (code > 0)              snprintf(err, errsz, "Sync list request failed (HTTP %ld)", code);
        free(b.data);
        return -1;
    }
    cJSON *root = cJSON_Parse(b.data);
    free(b.data);
    if (!root) { snprintf(err, errsz, "Bad saves reply"); return -1; }
    cJSON *arr = cJSON_IsArray(root) ? root : cJSON_GetObjectItem(root, "items");
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        cJSON *id = cJSON_GetObjectItem(it, "id");
        cJSON *up = cJSON_GetObjectItem(it, "updated_at");
        cJSON *hs = cJSON_GetObjectItem(it, "content_hash");
        if (!cJSON_IsNumber(id)) continue;
        if (kind == K_SAVE) {    // battery saves only; RomM also stores states/auto-saves here
            cJSON *fe = cJSON_GetObjectItem(it, "file_extension");
            cJSON *fn = cJSON_GetObjectItem(it, "file_name");
            const char *ext = cJSON_IsString(fe) ? fe->valuestring : "";
            int is_srm = !strcasecmp(ext, "srm");
            if (!is_srm && cJSON_IsString(fn)) {
                size_t l = strlen(fn->valuestring);
                is_srm = l > 4 && !strcasecmp(fn->valuestring + l - 4, ".srm");
            }
            if (!is_srm) continue;
        }
        if (kind == K_STATE) {   // only trust states made by RetroArch (others are incompatible)
            cJSON *em = cJSON_GetObjectItem(it, "emulator");
            if (!cJSON_IsString(em) || strcmp(em->valuestring, "retroarch")) continue;
        }
        const char *u = cJSON_IsString(up) ? up->valuestring : "";
        if (!out->found || strcmp(u, out->updated) > 0 ||
            (!strcmp(u, out->updated) && id->valueint > out->id)) {
            out->found = 1;
            out->id = id->valueint;
            snprintf(out->updated, sizeof out->updated, "%s", u);
            snprintf(out->hash, sizeof out->hash, "%s", cJSON_IsString(hs) ? hs->valuestring : "");
        }
    }
    cJSON_Delete(root);
    return 0;
}

static int push_file(int kind, int rom_id, const char *path, const char *upname, int overwrite,
                     RemoteSave *out, char *err, size_t errsz) {
    char url[300], auth[256];
    if (kind == K_STATE)
        snprintf(url, sizeof url, "%s/api/states?rom_id=%d&emulator=retroarch", g_url, rom_id);
    else
        snprintf(url, sizeof url, "%s/api/saves?rom_id=%d&overwrite=%s", g_url, rom_id,
                 overwrite ? "true" : "false");
    CURL *c = curl_easy_init();
    if (!c) { snprintf(err, errsz, "curl init failed"); return -1; }
    curl_mime *mime = curl_mime_init(c);
    curl_mimepart *part = curl_mime_addpart(mime);
    curl_mime_name(part, kind == K_STATE ? "stateFile" : "saveFile");
    curl_mime_filedata(part, path);
    curl_mime_filename(part, upname);
    curl_mime_type(part, "application/octet-stream");

    struct curl_slist *h = NULL;
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", g_token);
    h = curl_slist_append(h, auth);
    h = curl_slist_append(h, "Accept: application/json");
    Buf b = {0};
    char cerr[CURL_ERROR_SIZE] = "";
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "RomMVita/0.3");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, cerr);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(h);
    curl_mime_free(mime);
    curl_easy_cleanup(c);

    int ret = -1;
    if (rc != CURLE_OK) {
        snprintf(err, errsz, "%s", cerr[0] ? cerr : curl_easy_strerror(rc));
    } else if (code == 200 || code == 201) {
        memset(out, 0, sizeof *out);
        cJSON *r = cJSON_Parse(b.data);
        cJSON *id = r ? cJSON_GetObjectItem(r, "id") : NULL;
        cJSON *up = r ? cJSON_GetObjectItem(r, "updated_at") : NULL;
        if (cJSON_IsNumber(id)) {
            out->found = 1;
            out->id = id->valueint;
            snprintf(out->updated, sizeof out->updated, "%s", cJSON_IsString(up) ? up->valuestring : "");
            ret = 0;
        } else {
            snprintf(err, errsz, "Upload reply had no save id");
        }
        cJSON_Delete(r);
    } else if (code == 401 || code == 403) {
        snprintf(err, errsz, "Token has no save/state access (HTTP %ld)", code);
    } else {
        snprintf(err, errsz, "Upload failed (HTTP %ld)", code);
    }
    free(b.data);
    return ret;
}

enum { SY_NONE, SY_PUSHED, SY_PULLED, SY_ERR, SY_INCOMPAT };

// Two-way sync of one game's save. Never destroys data: a pull first backs the
// local file up to .bak, and a push on conflict keeps the older remote copy.
static int sync_one(const Game *g, int kind, char *msg, size_t msgsz) {
    char path[300], md5[33] = "", err[160] = "";
    const char *what = kind == K_STATE ? "state" : "save";
    int has_local = local_file_path(g, kind, path, sizeof path);
    if (has_local && file_md5(path, md5) < 0) has_local = 0;

    RemoteSave rs;
    if (remote_latest(kind, g->id, &rs, err, sizeof err) < 0) {
        snprintf(msg, msgsz, "%s", err);
        return SY_ERR;
    }
    SyncRec *rec = rec_find(kind, g->id);
    int local_changed  = has_local && (!rec || strcmp(rec->md5, md5));
    int remote_changed = rs.found && (!rec || rec->save_id != rs.id || strcmp(rec->updated, rs.updated));

    if (has_local && rs.found && !strcasecmp(rs.hash, md5)) {   // already identical
        rec_set(kind, g->id, md5, rs.id, rs.updated);
        return SY_NONE;
    }

    // learn which core made the local state: it changed since last sync and we launched this game
    if (kind == K_STATE && has_local && local_changed && g_last_rom == g->id && g_last_core[0]) {
        long sz = state_size(path);
        if (sz > 0) core_learn(g_last_core, sz);
    }
    // a remote state we already judged incompatible: skip unless its core became available
    if (kind == K_STATE && !has_local && rs.found && rec && rec->md5[0] == 'i' &&
        rec->save_id == rs.id && !strcmp(rec->updated, rs.updated)) {
        const char *c = core_for_size(atol(rec->md5 + 1));
        if (!c || !core_installed(c)) { snprintf(msg, msgsz, "state skipped (incompatible core)"); return SY_NONE; }
    }

    int action = SY_NONE, conflict = 0;
    if (!has_local && !rs.found)                 action = SY_NONE;
    else if (has_local && !rs.found)             action = SY_PUSHED;
    else if (!has_local && rs.found)             action = SY_PULLED;
    else if (local_changed && !remote_changed)   action = SY_PUSHED;
    else if (!local_changed && remote_changed)   action = SY_PULLED;
    else if (local_changed && remote_changed)  { action = SY_PUSHED; conflict = 1; }

    if (action == SY_PULLED) {
        char url[300], bak[320], incoming[330];
        mkdirs_for(path);
        snprintf(url, sizeof url, "%s/api/%s/%d/content", g_url, kind == K_STATE ? "states" : "saves", rs.id);
        snprintf(incoming, sizeof incoming, "%s.incoming", path);
        if (fetch_file(url, incoming, err, sizeof err) < 0) {
            snprintf(msg, msgsz, "%s download: %s", what, err);
            return SY_ERR;
        }
        int unverified = 0;
        if (kind == K_STATE) {
            long sz = state_size(incoming);
            const char *core = core_for_size(sz);
            if (core && !core_installed(core)) {
                sceIoRemove(incoming);
                char ib[40];
                snprintf(ib, sizeof ib, "i%ld", sz);
                rec_set(kind, g->id, ib, rs.id, rs.updated);
                char pm[360];
                snprintf(pm, sizeof pm,
                         "Incompatible cores\n%.60s\nThis save state was made with %s, which RetroArch on this Vita doesn't have.\nSave states won't work for this game here (battery saves still sync).",
                         g->name, core);
                popup_add(pm);
                snprintf(msg, msgsz, "state needs %s (not installed)", core);
                return SY_INCOMPAT;
            }
            unverified = (core == NULL);
        }
        if (has_local) {
            snprintf(bak, sizeof bak, "%s.bak", path);
            sceIoRemove(bak);
            sceIoRename(path, bak);
        }
        sceIoRemove(path);
        if (sceIoRename(incoming, path) < 0) {
            if (has_local) sceIoRename(bak, path);
            sceIoRemove(incoming);
            snprintf(msg, msgsz, "%s: could not save file", what);
            return SY_ERR;
        }
        char nm[33] = "";
        file_md5(path, nm);
        rec_set(kind, g->id, nm, rs.id, rs.updated);
        snprintf(msg, msgsz, unverified ? "Pulled %s (core unverified - may not load)" : "Pulled %s from RomM", what);
        return SY_PULLED;
    }
    if (action == SY_PUSHED) {
        char noext[160], up[200];
        strip_ext(g->fs_name, noext, sizeof noext);
        snprintf(up, sizeof up, "%s%s", noext, kind == K_STATE ? ".state" : ".srm");
        RemoteSave res;
        if (push_file(kind, g->id, path, up, !conflict, &res, err, sizeof err) < 0) {
            snprintf(msg, msgsz, "%s", err);
            return SY_ERR;
        }
        rec_set(kind, g->id, md5, res.id, res.updated);
        snprintf(msg, msgsz, conflict ? "Pushed %s (RomM had a newer one too)" : "Pushed %s to RomM", what);
        return SY_PUSHED;
    }
    if (has_local || rs.found) rec_set(kind, g->id, has_local ? md5 : "-", rs.found ? rs.id : 0,
                                        rs.found ? rs.updated : "-");
    snprintf(msg, msgsz, "%ss up to date", what);
    return SY_NONE;
}

// Syncs both the battery save (.srm) and the save state (.state) of a game.
static int sync_game(const Game *g, char *msg, size_t msgsz, int *npush, int *npull) {
    char m1[160] = "", m2[160] = "";
    int r1 = sync_one(g, K_SAVE, m1, sizeof m1);
    int r2 = sync_one(g, K_STATE, m2, sizeof m2);
    if (npush) *npush += (r1 == SY_PUSHED) + (r2 == SY_PUSHED);
    if (npull) *npull += (r1 == SY_PULLED) + (r2 == SY_PULLED);
    if (r1 == SY_ERR)      snprintf(msg, msgsz, "%s", m1);
    else if (r2 == SY_ERR) snprintf(msg, msgsz, "%s", m2);
    else                   snprintf(msg, msgsz, "%s; %s", m1, m2);
    return (r1 == SY_ERR || r2 == SY_ERR) ? SY_ERR
         : ((r1 != SY_NONE && r1 != SY_INCOMPAT) || (r2 != SY_NONE && r2 != SY_INCOMPAT)) ? SY_PUSHED : SY_NONE;
}

// ---- background jobs (download / sync) — one at a time, overlay shown while running

enum { JOB_DOWNLOAD = 1, JOB_SYNC_LAUNCH, JOB_SYNC_ALL };

static volatile int g_job_kind = 0;
static volatile int g_job_state = 0;      // 0 idle, 1 running, 2 done, 3 failed
static int  g_job_index = 0;              // index into g_games
static char g_job_title[140] = "";
static char g_job_detail[200] = "";
static char g_job_msg[200] = "";
static char g_job_rom[256] = "";
static int  g_skip_sync_id = -1;          // game whose sync failed; next play skips it

static int job_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    char err[200] = "", msg[200] = "";
    Game *g = &g_games[g_job_index];

    if (g_job_kind == JOB_DOWNLOAD) {
        ensure_dirs();
        char url[700], enc[400];
        url_encode(g->fs_name, enc, sizeof enc);
        snprintf(url, sizeof url, "%s/api/roms/%d/content/%s", g_url, g->id, enc);
        local_rom_path(g, g_job_rom, sizeof g_job_rom);
        if (fetch_file(url, g_job_rom, err, sizeof err) < 0) {
            snprintf(g_job_msg, sizeof g_job_msg, "%s", err);
            g_job_state = 3;
            return sceKernelExitDeleteThread(0);
        }
        g->installed = 1;
        scan_installed();
        // pull an existing RomM save for this game (failure here must not block play)
        snprintf(g_job_title, sizeof g_job_title, "Checking saves  %.60s", g->name);
        g_job_total = 0;
        sync_game(g, msg, sizeof msg, NULL, NULL);
        g_job_state = 2;
    } else if (g_job_kind == JOB_SYNC_LAUNCH) {
        local_rom_path(g, g_job_rom, sizeof g_job_rom);
        int r = sync_game(g, msg, sizeof msg, NULL, NULL);
        if (r == SY_ERR) {
            snprintf(g_job_msg, sizeof g_job_msg, "Sync failed: %s - press Cross again to play anyway", msg);
            g_skip_sync_id = g->id;
            g_job_state = 3;
        } else {
            g_job_state = 2;
        }
    } else {  // JOB_SYNC_ALL
        int total = 0, pushed = 0, pulled = 0, errors = 0, done = 0;
        for (int i = 0; i < g_game_count; i++) if (g_games[i].installed) total++;
        char first_err[160] = "";
        for (int i = 0; i < g_game_count && !g_job_cancel; i++) {
            const Game *x = &g_games[i];
            if (!x->installed) continue;
            snprintf(g_job_detail, sizeof g_job_detail, "%d / %d   %.70s", ++done, total, x->name);
            int r = sync_game(x, msg, sizeof msg, &pushed, &pulled);
            if (r == SY_ERR) { errors++; if (!first_err[0]) snprintf(first_err, sizeof first_err, "%s", msg); }
            if (r == SY_ERR && (strstr(msg, "access") || strstr(msg, "Can't") || strstr(msg, "resolve"))) break;
        }
        if (errors) snprintf(g_job_msg, sizeof g_job_msg, "Sync: %d pushed, %d pulled, %d error(s): %s",
                             pushed, pulled, errors, first_err);
        else        snprintf(g_job_msg, sizeof g_job_msg, "Saves/states synced: %d pushed, %d pulled (%d game(s))",
                             pushed, pulled, total);
        g_job_state = errors ? 3 : 2;
    }
    return sceKernelExitDeleteThread(0);
}

static void job_start(int kind, int game_index) {
    g_job_kind = kind;
    g_job_index = game_index;
    g_job_now = g_job_total = 0;
    g_job_cancel = 0;
    g_job_msg[0] = g_job_detail[0] = 0;
    const char *name = (kind != JOB_SYNC_ALL && game_index >= 0) ? g_games[game_index].name : "";
    if (kind == JOB_DOWNLOAD)         snprintf(g_job_title, sizeof g_job_title, "Downloading  %.80s", name);
    else if (kind == JOB_SYNC_LAUNCH) snprintf(g_job_title, sizeof g_job_title, "Syncing save  %.80s", name);
    else                              snprintf(g_job_title, sizeof g_job_title, "Syncing all saves");
    g_job_state = 1;
    SceUID t = sceKernelCreateThread("job", job_thread, 0x40, 0x40000, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
    else { snprintf(g_job_msg, sizeof g_job_msg, "Could not start worker thread"); g_job_state = 3; }
}

// Launches the ROM in RetroArch (must already be installed on the Vita).
static int retroarch_launch(const char *rom_path, const char *core, char *err, size_t errsz) {
    char uri[512];
    snprintf(uri, sizeof uri, "psgm:play?titleid=RETROVITA&param=app0:/%s.self&param2=%s",
             core, rom_path);
    sceAppMgrDestroyOtherApp();
    int r = sceAppMgrLaunchAppByUri(0xFFFFF, uri);
    if (r < 0) {
        snprintf(err, errsz, "Launch failed: 0x%08X", (unsigned)r);
        return 0;
    }
    sceKernelDelayThread(500 * 1000);
    sceKernelExitProcess(0);
    return 1;
}

// ---- library ui

static char g_lib_toast[260] = "";
static char g_popup[360] = "";
static int  g_popup_launch = 0;   // launch the game once the popup is dismissed
static int  g_installed_only = 0;
static int  g_view[MAX_GAMES];
static int  g_view_count = 0, g_view_built = -1, g_view_dirty = 1;
static int  g_auto_synced = 0;

// every space-separated word in `q` must appear in `name` (case-insensitive)
static int name_matches(const char *name, const char *q) {
    if (!q[0]) return 1;
    char low[100], word[64];
    size_t i = 0;
    for (; name[i] && i < sizeof low - 1; i++) low[i] = (char)tolower((unsigned char)name[i]);
    low[i] = 0;
    const char *p = q;
    while (*p) {
        while (*p == ' ') p++;
        size_t w = 0;
        while (*p && *p != ' ' && w < sizeof word - 1) word[w++] = (char)tolower((unsigned char)*p++);
        word[w] = 0;
        if (w && !strstr(low, word)) return 0;
    }
    return 1;
}

static int g_cursor_id = -1;   // rom id under the cursor, so a refresh doesn't move it

static void view_rebuild(void) {
    int n = 0, cnt = g_game_count;
    for (int i = 0; i < cnt; i++)
        if ((!g_installed_only || g_games[i].installed) && name_matches(g_games[i].name, g_search))
            g_view[n++] = i;
    g_view_count = n;
    g_view_built = cnt;
    g_view_dirty = 0;
    if (g_follow) {
        g_follow = 0;
        for (int i = 0; i < n; i++)
            if (g_games[g_view[i]].id == g_cursor_id) { g_cursor = i; break; }
    }
    if (g_cursor >= n) g_cursor = n ? n - 1 : 0;
    if (g_scroll > g_cursor) g_scroll = g_cursor;
    if (g_cursor >= g_scroll + LIB_ROWS) g_scroll = g_cursor - LIB_ROWS + 1;
}

static void fmt_size(uint32_t b, char *out, size_t sz) {
    if (b >= 1024 * 1024) snprintf(out, sz, "%.1f MB", b / 1048576.0);
    else                  snprintf(out, sz, "%u KB", (unsigned)(b / 1024));
}

static void draw_library(void) {
    if (g_view_dirty || g_view_built != g_game_count) view_rebuild();
    draw_frame_begin();
    vita2d_draw_rectangle(0, 0, 960, 544, COL_BG);
    if (g_logo) vita2d_draw_texture_scale(g_logo, 30, 8, 0.0859f, 0.0859f);
    text(84, 50, COL_ACCENT, 1.6f, "SNES Library");
    if (g_installed_only) text(340, 50, COL_GREEN, 1.2f, "[Installed only]");

    char head[160];
    if (g_search[0]) snprintf(head, sizeof head, "Search: \"%s\"   %d shown", g_search, g_view_count);
    else             snprintf(head, sizeof head, "%d shown of %d%s", g_view_count, g_game_count, g_from_cache && g_lib_state == 1 ? "  (cached)" : "");
    text(40, 82, COL_DIM, 1.0f, head);
    if (g_lib_state == 1) text(700, 82, COL_YELLOW, 1.0f, g_from_cache ? "Updating..." : "Loading...");

    for (int i = 0; i < LIB_ROWS; i++) {
        int vi = g_scroll + i;
        if (vi >= g_view_count) break;
        const Game *g = &g_games[g_view[vi]];
        int y = 96 + i * 36;
        int sel = (vi == g_cursor);
        vita2d_draw_rectangle(30, y, 900, 34, sel ? COL_SEL : COL_PANEL);
        char shown[80];
        snprintf(shown, sizeof shown, "%.76s", g->name);
        text(44, y + 24, COL_TEXT, 1.1f, shown);
        char sz[24];
        fmt_size(g->size, sz, sizeof sz);
        text(800, y + 24, g->installed ? COL_GREEN : COL_DIM, 1.0f, sz);
    }
    if (g_view_count == 0 && g_lib_state != 1) {
        const char *m = g_lib_msg[0] ? g_lib_msg
                      : (g_installed_only ? "No installed games yet" : "No games");
        text(44, 140, g_lib_state == 3 ? COL_RED : COL_DIM, 1.2f, m);
    }
    if (g_lib_state == 3 && g_view_count > 0) text(40, 500, COL_RED, 1.0f, g_lib_msg);
    else if (g_lib_toast[0])                  text(40, 500, COL_YELLOW, 1.0f, g_lib_toast);

    text(40, 528, COL_DIM, 1.0f,
         "Cross: play   Square: search   Select: installed only   Triangle: sync saves   Circle: back");

    if (g_popup[0]) {
        vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(0, 0, 0, 170));
        vita2d_draw_rectangle(110, 130, 740, 250, COL_PANEL);
        vita2d_draw_rectangle(110, 130, 740, 6, COL_YELLOW);
        char buf[360], *line = buf;
        snprintf(buf, sizeof buf, "%s", g_popup);
        int y = 175, first = 1;
        while (line && *line && y < 340) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            // word-wrap long lines at ~70 chars
            while (strlen(line) > 70) {
                int cut = 70;
                while (cut > 40 && line[cut] != ' ') cut--;
                char tmp = line[cut];
                line[cut] = 0;
                text(135, y, first ? COL_YELLOW : COL_TEXT, first ? 1.3f : 1.0f, line);
                y += 26; first = 0;
                line[cut] = tmp;
                line += cut + (tmp == ' ' ? 1 : 0);
            }
            text(135, y, first ? COL_YELLOW : COL_TEXT, first ? 1.3f : 1.0f, line);
            y += first ? 36 : 26; first = 0;
            line = nl ? nl + 1 : NULL;
        }
        text(690, 365, COL_DIM, 1.0f, "Cross: OK");
    }
    if (g_job_state == 1) {
        vita2d_draw_rectangle(130, 190, 700, 150, RGBA8(10, 11, 15, 245));
        text(150, 232, COL_TEXT, 1.1f, g_job_title);
        if (g_job_kind == JOB_DOWNLOAD && g_job_total > 0) {
            vita2d_draw_rectangle(150, 258, 660, 22, COL_PANEL);
            double frac = g_job_now / g_job_total;
            if (frac > 1) frac = 1;
            vita2d_draw_rectangle(150, 258, (int)(660 * frac), 22, COL_GREEN);
            char pr[80];
            snprintf(pr, sizeof pr, "%.1f / %.1f MB", g_job_now / 1048576.0, g_job_total / 1048576.0);
            text(150, 308, COL_DIM, 1.0f, pr);
        } else if (g_job_detail[0]) {
            text(150, 270, COL_DIM, 1.0f, g_job_detail);
        }
        if (g_job_kind != JOB_SYNC_LAUNCH) text(660, 320, COL_DIM, 1.0f, "Circle: cancel");
    }
    draw_frame_end();
}

static int g_hold_up = 0, g_hold_down = 0;

static void cursor_move(int delta) {
    int count = g_view_count;
    if (count <= 0) return;
    g_cursor += delta;
    if (g_cursor < 0) g_cursor = 0;
    if (g_cursor >= count) g_cursor = count - 1;
    if (g_cursor < g_scroll) g_scroll = g_cursor;
    if (g_cursor >= g_scroll + LIB_ROWS) g_scroll = g_cursor - LIB_ROWS + 1;
}

// Core for a game: the one matching its local state if we can tell, else our default.
static const char *core_for_game(const Game *g) {
    char p[300];
    if (local_file_path(g, K_STATE, p, sizeof p)) {
        const char *c = core_for_size(state_size(p));
        if (c && core_installed(c)) return c;
    }
    return pick_core();
}

static void do_launch(void) {
    char err[160] = "";
    const Game *g = &g_games[g_job_index];
    const char *core = core_for_game(g);
    g_last_rom = g->id;
    snprintf(g_last_core, sizeof g_last_core, "%s", core);
    FILE *lf = fopen(LAUNCH_PATH, "w");
    if (lf) { fprintf(lf, "%d %s\n", g_last_rom, g_last_core); fclose(lf); }
    if (!retroarch_launch(g_job_rom, core, err, sizeof err))
        snprintf(g_lib_toast, sizeof g_lib_toast, "%s", err);
}

// returns 1 when the user wants to go back to the connect screen
static int library_input(void) {
    if (g_job_state == 1) {
        if (pressed(SCE_CTRL_CIRCLE) && g_job_kind != JOB_SYNC_LAUNCH) g_job_cancel = 1;
        return 0;
    }
    if (g_popup[0]) {                       // modal message: Cross to dismiss
        if (pressed(SCE_CTRL_CROSS) || pressed(SCE_CTRL_CIRCLE)) {
            g_popup[0] = 0;
            if (g_popup_launch) { g_popup_launch = 0; do_launch(); }
        }
        return 0;
    }
    if (g_job_state == 2) {
        g_job_state = 0;
        g_view_dirty = 1;
        int launch = (g_job_kind != JOB_SYNC_ALL);
        if (g_job_kind == JOB_SYNC_ALL) snprintf(g_lib_toast, sizeof g_lib_toast, "%s", g_job_msg);
        if (g_popup_pending[0]) {
            snprintf(g_popup, sizeof g_popup, "%s", g_popup_pending);
            g_popup_pending[0] = 0;
            g_popup_launch = launch;
        } else if (launch) {
            do_launch();
        }
        return 0;
    }
    if (g_job_state == 3) {
        g_job_state = 0;
        g_view_dirty = 1;
        snprintf(g_lib_toast, sizeof g_lib_toast, "%s", g_job_msg);
        return 0;
    }

    g_cursor_id = (g_cursor < g_view_count) ? g_games[g_view[g_cursor]].id : -1;
    if (g_lib_toast_pending[0]) {
        snprintf(g_lib_toast, sizeof g_lib_toast, "%s", g_lib_toast_pending);
        g_lib_toast_pending[0] = 0;
    }
    // refresh the list in the background every 10 minutes
    if (!g_lib_busy && g_lib_state != 1 &&
        sceKernelGetProcessTimeWide() - g_last_refresh > 600ULL * 1000000ULL)
        library_load();

    // once the list is on screen (cache or live), push any saves changed since last session
    if (!g_auto_synced && (g_lib_state == 2 || g_from_cache)) {
        g_auto_synced = 1;
        int any = 0;
        for (int i = 0; i < g_game_count; i++) if (g_games[i].installed) { any = 1; break; }
        if (any) { job_start(JOB_SYNC_ALL, -1); return 0; }
    }

    if (pressed(SCE_CTRL_CIRCLE)) return 1;

    g_hold_up   = (g_pad.buttons & SCE_CTRL_UP)   ? g_hold_up + 1   : 0;
    g_hold_down = (g_pad.buttons & SCE_CTRL_DOWN) ? g_hold_down + 1 : 0;
    if (g_hold_up == 1   || (g_hold_up > 15   && g_hold_up % 3 == 0))   cursor_move(-1);
    if (g_hold_down == 1 || (g_hold_down > 15 && g_hold_down % 3 == 0)) cursor_move(1);
    if (pressed(SCE_CTRL_LEFT)  || pressed(SCE_CTRL_LTRIGGER)) cursor_move(-LIB_ROWS);
    if (pressed(SCE_CTRL_RIGHT) || pressed(SCE_CTRL_RTRIGGER)) cursor_move(LIB_ROWS);

    if (pressed(SCE_CTRL_SELECT)) {
        g_installed_only = !g_installed_only;
        g_cursor = g_scroll = 0;
        g_view_dirty = 1;
        g_lib_toast[0] = 0;
    }
    if (pressed(SCE_CTRL_SQUARE)) {
        char tmp[64];
        snprintf(tmp, sizeof tmp, "%s", g_search);
        if (ime_prompt("Search SNES games (empty = all)", tmp, sizeof tmp, 60)) {
            snprintf(g_search, sizeof g_search, "%s", tmp);
            g_cursor = g_scroll = 0;
            g_view_dirty = 1;
        }
    }
    if (pressed(SCE_CTRL_TRIANGLE)) {
        g_lib_toast[0] = 0;
        job_start(JOB_SYNC_ALL, -1);
        return 0;
    }
    if (pressed(SCE_CTRL_CROSS) && g_view_count > 0 && g_cursor < g_view_count) {
        g_lib_toast[0] = 0;
        int gi = g_view[g_cursor];
        Game *g = &g_games[gi];
        if (!g->installed) {
            job_start(JOB_DOWNLOAD, gi);
        } else if (g_skip_sync_id == g->id) {
            g_skip_sync_id = -1;
            g_job_index = gi;
            local_rom_path(g, g_job_rom, sizeof g_job_rom);
            do_launch();
        } else {
            job_start(JOB_SYNC_LAUNCH, gi);
        }
    }
    return 0;
}

// ---------------------------------------------------------------- main ui

static void draw_row(int idx, int y, const char *label, const char *value) {
    int sel = (g_sel == idx);
    vita2d_draw_rectangle(60, y, 840, 70, sel ? COL_SEL : COL_PANEL);
    text(80, y + 26, sel ? COL_TEXT : COL_DIM, 1.0f, label);
    text(80, y + 56, COL_TEXT, 1.3f, value[0] ? value : "(empty)");
}

static void draw_button(int idx, int x, const char *label) {
    vita2d_draw_rectangle(x, 270, 270, 56, g_sel == idx ? COL_SEL : COL_PANEL);
    text(x + 20, 306, COL_TEXT, 1.3f, label);
}

static void draw_ui(void) {
    draw_frame_begin();
    vita2d_draw_rectangle(0, 0, 960, 544, COL_BG);

    if (g_logo) vita2d_draw_texture_scale(g_logo, 56, 14, 0.125f, 0.125f);
    text(136, 62, COL_ACCENT, 1.8f, "RomM Vita");

    unsigned dot = COL_GRAY;
    switch (g_status) {
        case ST_WORKING: dot = COL_YELLOW; break;
        case ST_OK:      dot = COL_GREEN;  break;
        case ST_FAIL:    dot = COL_RED;    break;
        default: break;
    }
    vita2d_draw_fill_circle(900, 44, 22, dot);

    draw_row(0, 100, "Server URL  (http/https detected automatically)", g_url);
    draw_row(1, 180, "Pairing code  (like XXXX-XXXX, from RomM web UI)", g_code);

    draw_button(2, 60,  "Scan QR");
    draw_button(3, 345, g_status == ST_WORKING ? "Connecting..." : "Connect");
    draw_button(4, 630, "Forget token");

    vita2d_draw_rectangle(60, 350, 840, 90, COL_PANEL);
    text(80, 385, dot, 1.3f, g_msg);
    text(80, 420, COL_DIM, 1.0f, g_token[0] ? "Saved token present" : "No token saved yet");

    text(60, 510, COL_DIM, 1.0f,
         "D-pad: select    Cross: edit / confirm    Start: exit");
    draw_frame_end();
}

// ---------------------------------------------------------------- main

static void net_init(void) {
    static char net_mem[1024 * 1024];
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    SceNetInitParam p = { net_mem, sizeof net_mem, 0 };
    sceNetInit(&p);
    sceNetCtlInit();
    curl_global_init(CURL_GLOBAL_DEFAULT);
}


static void do_scan(void) {
    char url[256] = "", code[16] = "";
    if (qr_scan(url, sizeof url, code, sizeof code)) {
        if (url[0]) { snprintf(g_url, sizeof g_url, "%s", url); g_token[0] = 0; }
        snprintf(g_code, sizeof g_code, "%s", code);
        start_connect();
    }
}

int main(void) {
    vita2d_init();
    vita2d_set_clear_color(COL_BG);
    g_font = vita2d_load_default_pgf();
    g_logo = vita2d_load_PNG_file("app0:/logo.png");
    net_init();
    config_load();
    sync_load();
    cores_load();

    // Auto-verify a previously saved token on launch.
    if (g_url[0] && g_token[0]) start_connect();

    int screen = 0;  // 0 connect, 1 library
    for (;;) {
        g_old = g_pad;
        sceCtrlPeekBufferPositive(0, &g_pad, 1);

        if (g_enter_library) {
            g_enter_library = 0;
            screen = 1;
            if (g_lib_state == 0 || g_lib_state == 3) g_auto_synced = 0;
            library_load();
        }

        if (screen == 1) {
            if (pressed(SCE_CTRL_START) && g_job_state != 1) break;
            if (library_input()) screen = 0;
            draw_library();
            continue;
        }

        if (pressed(SCE_CTRL_START)) break;
        if (pressed(SCE_CTRL_UP))
            g_sel = (g_sel >= 2) ? 1 : (g_sel == 1 ? 0 : 3);
        if (pressed(SCE_CTRL_DOWN))
            g_sel = (g_sel == 0) ? 1 : (g_sel == 1 ? 3 : 0);
        if (g_sel >= 2) {
            if (pressed(SCE_CTRL_LEFT))  g_sel = g_sel > 2 ? g_sel - 1 : 4;
            if (pressed(SCE_CTRL_RIGHT)) g_sel = g_sel < 4 ? g_sel + 1 : 2;
        }

        if (pressed(SCE_CTRL_CROSS)) {
            switch (g_sel) {
                case 0:
                    if (ime_prompt("RomM server URL", g_url, sizeof g_url, 200)) {
                        g_token[0] = 0;  // new server => old token is meaningless
                        g_platform_id = -1;
                        g_lib_state = 0;
                        g_status = ST_IDLE; snprintf(g_msg, sizeof g_msg, "Not connected");
                    }
                    break;
                case 1:
                    if (ime_prompt("Pairing code (XXXX-XXXX)", g_code, sizeof g_code, 12)) {
                        char d[10], p[9];
                        if (code_normalise(g_code, d, p)) snprintf(g_code, sizeof g_code, "%s", d);
                    }
                    break;
                case 2: do_scan(); break;
                case 3: start_connect(); break;
                case 4:
                    g_token[0] = 0;
                    config_save();
                    g_lib_state = 0;
                    g_status = ST_IDLE;
                    snprintf(g_msg, sizeof g_msg, "Token forgotten");
                    break;
            }
        }
        draw_ui();
    }

    vita2d_fini();
    if (g_logo) vita2d_free_texture(g_logo);
    vita2d_free_pgf(g_font);
    sceKernelExitProcess(0);
    return 0;
}

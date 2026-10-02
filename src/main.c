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
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/camera.h>
#include <psp2/kernel/sysmem.h>
#include <ctype.h>
#include "quirc/quirc.h"
#include <vita2d.h>
#include <curl/curl.h>

#define DATA_DIR    "ux0:data/RomMVita"
#define CONFIG_PATH DATA_DIR "/config.txt"

#define COL_BG     RGBA8(24, 26, 33, 255)
#define COL_PANEL  RGBA8(38, 41, 52, 255)
#define COL_SEL    RGBA8(88, 101, 242, 255)
#define COL_TEXT   RGBA8(235, 237, 245, 255)
#define COL_DIM    RGBA8(140, 146, 165, 255)
#define COL_GRAY   RGBA8(120, 124, 138, 255)
#define COL_YELLOW RGBA8(240, 190, 50, 255)
#define COL_GREEN  RGBA8(60, 200, 100, 255)
#define COL_RED    RGBA8(230, 70, 70, 255)

typedef enum { ST_IDLE, ST_WORKING, ST_OK, ST_FAIL } Status;

static char g_url[256]   = "";
static char g_code[16]   = "";
static char g_token[128] = "";
static char g_user[64]   = "";

static volatile Status g_status = ST_IDLE;
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

typedef struct { char *data; size_t len; } Buf;

static size_t write_cb(void *p, size_t sz, size_t n, void *ud) {
    Buf *b = ud;
    size_t add = sz * n;
    if (b->len + add > 64 * 1024) return 0;  // responses here are tiny
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

    text(60, 60, COL_TEXT, 1.8f, "RomM Vita");

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

static int pressed(unsigned btn) { return (g_pad.buttons & btn) && !(g_old.buttons & btn); }

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
    net_init();
    config_load();

    // Auto-verify a previously saved token on launch.
    if (g_url[0] && g_token[0]) start_connect();

    for (;;) {
        g_old = g_pad;
        sceCtrlPeekBufferPositive(0, &g_pad, 1);

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
                    g_status = ST_IDLE;
                    snprintf(g_msg, sizeof g_msg, "Token forgotten");
                    break;
            }
        }
        draw_ui();
    }

    vita2d_fini();
    vita2d_free_pgf(g_font);
    sceKernelExitProcess(0);
    return 0;
}

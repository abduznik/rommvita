// Pure, SDK-free logic shared by main.c and the host-side tests in tests/.
#ifndef ROMMVITA_CORE_H
#define ROMMVITA_CORE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdarg.h>
#include <ctype.h>

#define VITA_TAG       "rommvita"   // RomM slot / emulator value identifying uploads from this app (never in file names)
#define KEEP_VERSIONS  10           // versions kept per game (saves: server-side, states: pruned by us)



enum { K_SAVE = 0, K_STATE = 1 };

typedef struct {
    int      id;
    uint32_t size;
    uint8_t  installed;
    char     name[96];
    char     fs_name[128];
} Game;

// ---- platforms (only the systems we plan to support; `impl` marks the working ones)
typedef struct {
    const char *name, *dir, *slug;   // display name, local folder, RomM slug (NULL = not confirmed yet)
    int impl;                        // 1 = launch and sync implemented
    const char *cores[6];            // RetroArch cores in preference order
    int id, count;                   // RomM platform id and rom_count (-1 = unknown)
} Platform;

static Platform g_plats[] = {
    { "Super Nintendo",            "snes",    "snes", 1, { "snes9x2010_libretro", "snes9x2005_plus_libretro",
                                                           "snes9x2005_libretro", "snes9x2002_libretro", "snes9x_libretro" }, -1, -1 },
    { "Game Boy Advance",          "gba",     "gba",  1, { "gpsp_libretro", "mgba_libretro", "vba_next_libretro" }, -1, -1 },
    { "NES",                       "nes",     "nes",  1, { "fceumm_libretro", "nestopia_libretro" }, -1, -1 },
    { "Game Boy",                  "gb",      "gb",   1, { "gambatte_libretro" }, -1, -1 },
    { "Game Boy Color",            "gbc",     "gbc",  1, { "gambatte_libretro" }, -1, -1 },
    { "Sega Genesis / Mega Drive", "genesis", "genesis", 1,
                                                         { "genesis_plus_gx_libretro", "picodrive_libretro" }, -1, -1 },
    { "PlayStation",               "ps1",     NULL,   0, { 0 }, -1, -1 },
    { "Arcade",                    "arcade",  NULL,   0, { 0 }, -1, -1 },
    { "Neo Geo",                   "neogeo",  NULL,   0, { 0 }, -1, -1 },
    { "Sega Master System",        "sms",     NULL,   0, { 0 }, -1, -1 },
    { "TurboGrafx-16 / PC Engine", "pce",     NULL,   0, { 0 }, -1, -1 },
};
#define NPLATS ((int)(sizeof g_plats / sizeof g_plats[0]))


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

static void strip_ext(const char *name, char *out, size_t sz) {
    snprintf(out, sz, "%s", name);
    char *dot = strrchr(out, '.');
    if (dot && strlen(dot) <= 6) *dot = 0;
}

// Folder name RetroArch uses when saves are sorted by core: the core's library name.
static void core_sort_dir(const char *core, char *out, size_t sz) {
    static const struct { const char *core, *dir; } map[] = {
        { "snes9x2010_libretro",      "Snes9x 2010" },
        { "snes9x2005_plus_libretro", "Snes9x 2005 Plus" },
        { "snes9x2005_libretro",      "Snes9x 2005" },
        { "snes9x2002_libretro",      "Snes9x 2002" },
        { "snes9x_libretro",          "Snes9x" },
        { "gpsp_libretro",            "gpSP" },
        { "mgba_libretro",            "mGBA" },
        { "vba_next_libretro",        "VBA Next" },
        { "fceumm_libretro",          "FCEUmm" },
        { "nestopia_libretro",        "Nestopia" },
        { "gambatte_libretro",        "Gambatte" },
        { "genesis_plus_gx_libretro", "Genesis Plus GX" },
        { "picodrive_libretro",       "PicoDrive" },
    };
    for (unsigned i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(core, map[i].core)) { snprintf(out, sz, "%s", map[i].dir); return; }
    snprintf(out, sz, "%s", core);
}

// Lowercase letters and digits only, so "Snes9x 2010", "snes9x2010_libretro" and "VBA Next" compare sensibly.
static void norm_key(const char *in, char *out, size_t sz) {
    size_t n = 0;
    for (; *in && n + 1 < sz; in++)
        if (isalnum((unsigned char)*in)) out[n++] = (char)tolower((unsigned char)*in);
    out[n] = 0;
    char *u = strstr(out, "libretro");
    if (u && u != out) *u = 0;
}

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

static void fmt_size(uint32_t b, char *out, size_t sz) {
    if (b >= 1024 * 1024) snprintf(out, sz, "%.1f MB", b / 1048576.0);
    else                  snprintf(out, sz, "%u KB", (unsigned)(b / 1024));
}

// ---- RomM API URLs (cross-checked against rommapp/romm backend/endpoints; see tests/test_core.c)
static int api_url(char *out, size_t n, const char *base, const char *path_fmt, ...) __attribute__((format(printf, 4, 5)));
static int api_url(char *out, size_t n, const char *base, const char *path_fmt, ...) {
    int k = snprintf(out, n, "%s", base);
    if (k < 0 || (size_t)k >= n) return -1;
    va_list ap;
    va_start(ap, path_fmt);
    int r = vsnprintf(out + k, n - (size_t)k, path_fmt, ap);
    va_end(ap);
    return (r < 0 || (size_t)r >= n - (size_t)k) ? -1 : 0;
}
static int api_heartbeat(char *o, size_t n, const char *b)          { return api_url(o, n, b, "/api/heartbeat"); }
static int api_users_me(char *o, size_t n, const char *b)           { return api_url(o, n, b, "/api/users/me"); }
static int api_token_exchange(char *o, size_t n, const char *b)     { return api_url(o, n, b, "/api/client-tokens/exchange"); }
static int api_platforms(char *o, size_t n, const char *b)          { return api_url(o, n, b, "/api/platforms"); }
static int api_roms_page(char *o, size_t n, const char *b, int plat_id, int limit, int offset) {
    return api_url(o, n, b, "/api/roms?platform_ids=%d&limit=%d&offset=%d&order_by=name&order_dir=asc", plat_id, limit, offset);
}
static int api_rom_content(char *o, size_t n, const char *b, int id, const char *fs_name) {
    char enc[400];
    url_encode(fs_name, enc, sizeof enc);
    return api_url(o, n, b, "/api/roms/%d/content/%s", id, enc);
}
static int api_assets_list(char *o, size_t n, const char *b, int kind, int rom_id) {
    return api_url(o, n, b, "/api/%s?rom_id=%d", kind == K_STATE ? "states" : "saves", rom_id);
}
static int api_asset_content(char *o, size_t n, const char *b, int kind, int id) {
    return api_url(o, n, b, "/api/%s/%d/content", kind == K_STATE ? "states" : "saves", id);
}
static int api_save_upload(char *o, size_t n, const char *b, int rom_id, int overwrite) {
    return api_url(o, n, b, "/api/saves?rom_id=%d&overwrite=%s&slot=" VITA_TAG "&emulator=" VITA_TAG
                            "&autocleanup=true&autocleanup_limit=%d", rom_id, overwrite ? "true" : "false", KEEP_VERSIONS);
}
static int api_state_upload(char *o, size_t n, const char *b, int rom_id) {
    return api_url(o, n, b, "/api/states?rom_id=%d&emulator=" VITA_TAG, rom_id);
}
static int api_states_delete(char *o, size_t n, const char *b)      { return api_url(o, n, b, "/api/states/delete"); }

#define MAX_QUEUE 32
static Game g_queue[MAX_QUEUE];
static int  g_nqueue = 0;

static int queue_find(int id) {
    for (int i = 0; i < g_nqueue; i++) if (g_queue[i].id == id) return i;
    return -1;
}
static int queue_add(const Game *g) {
    if (queue_find(g->id) >= 0 || g_nqueue >= MAX_QUEUE) return 0;
    g_queue[g_nqueue++] = *g;
    return 1;
}
static void queue_remove(int i) {
    if (i < 0 || i >= g_nqueue) return;
    memmove(&g_queue[i], &g_queue[i + 1], (size_t)(g_nqueue - i - 1) * sizeof g_queue[0]);
    g_nqueue--;
}


#endif

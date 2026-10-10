// Host-side unit and regression tests for src/core.h (no Vita SDK needed): `make -C tests test`
#include "../src/core.h"

static int g_run = 0, g_fail = 0;
#define CHECK(cond) do { g_run++; if (!(cond)) { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(got, want) do { g_run++; const char *g_ = (got), *w_ = (want); \
    if (strcmp(g_, w_)) { g_fail++; printf("FAIL %s:%d\n   got:  %s\n   want: %s\n", __FILE__, __LINE__, g_, w_); } } while (0)

// ---------------------------------------------------------------- unit tests
static void test_json_str(void) {
    char o[32];
    CHECK(json_str("{\"token\": \"abc123\"}", "token", o, sizeof o)); CHECK_STR(o, "abc123");
    CHECK(json_str("{\"a\":\"x\",\"token\":\"t\\\"q\"}", "token", o, sizeof o)); CHECK_STR(o, "t\"q");
    CHECK(!json_str("{\"token\": 5}", "token", o, sizeof o));
    CHECK(!json_str("{}", "token", o, sizeof o));
    CHECK(!json_str(NULL, "token", o, sizeof o));
    CHECK(json_str("{\"k\":\"abcdefghij\"}", "k", o, 4)); CHECK_STR(o, "abc");   // truncates, stays terminated
}

static void test_url_encode(void) {
    char o[64];
    url_encode("Super Mario (USA).sfc", o, sizeof o); CHECK_STR(o, "Super%20Mario%20%28USA%29.sfc");
    url_encode("a-b_c.d~e", o, sizeof o);             CHECK_STR(o, "a-b_c.d~e");
    url_encode("a/b?c&d", o, sizeof o);               CHECK_STR(o, "a%2Fb%3Fc%26d");
    url_encode("\xc3\xa9", o, sizeof o);              CHECK_STR(o, "%C3%A9");
    url_encode("aaaaaaaaaa", o, 8);                   CHECK(strlen(o) < 8);        // never overflows
}

static void test_sanitize_and_ext(void) {
    char o[64];
    sanitize_filename("Zelda: A Link (U) [!].sfc", o, sizeof o); CHECK_STR(o, "Zelda__A_Link__U_____.sfc");
    sanitize_filename("../../etc/passwd", o, sizeof o);          CHECK(!strchr(o, '/'));
    sanitize_filename("abcdef", o, 4);                            CHECK_STR(o, "abc");
    strip_ext("Game.sfc", o, sizeof o);       CHECK_STR(o, "Game");
    strip_ext("Game.v1.2.gba", o, sizeof o);  CHECK_STR(o, "Game.v1.2");
    strip_ext("NoExt", o, sizeof o);          CHECK_STR(o, "NoExt");
    strip_ext("Dotted.name.verylongext", o, sizeof o); CHECK_STR(o, "Dotted.name.verylongext");   // not an extension
}

static void test_norm_key_and_sort_dir(void) {
    char o[64];
    norm_key("Snes9x 2010", o, sizeof o);           CHECK_STR(o, "snes9x2010");
    norm_key("snes9x2010_libretro", o, sizeof o);   CHECK_STR(o, "snes9x2010");
    norm_key("VBA Next", o, sizeof o);              CHECK_STR(o, "vbanext");
    core_sort_dir("fceumm_libretro", o, sizeof o);  CHECK_STR(o, "FCEUmm");
    core_sort_dir("gambatte_libretro", o, sizeof o);CHECK_STR(o, "Gambatte");
    core_sort_dir("mystery_libretro", o, sizeof o); CHECK_STR(o, "mystery_libretro");   // unknown: falls back to the name
}

static void test_pairing(void) {
    char d[10], p[9], url[64], code[16];
    CHECK(code_normalise("abcd-1234", d, p)); CHECK_STR(d, "ABCD-1234"); CHECK_STR(p, "ABCD1234");
    CHECK(code_normalise("abcd 1234", d, p));
    CHECK(code_normalise("ABCD1234", d, p));
    CHECK(!code_normalise("ABCD123", d, p));
    CHECK(!code_normalise("ABCD12345", d, p));
    CHECK(!code_normalise("ABCD_123", d, p));
    CHECK(parse_pair_payload("http://192.168.1.10:8285/pair?code=ABCD-1234", url, sizeof url, code, sizeof code));
    CHECK_STR(url, "http://192.168.1.10:8285"); CHECK_STR(code, "ABCD-1234");
    CHECK(parse_pair_payload("https://romm.example.com/pair?code=abcd1234&x=1", url, sizeof url, code, sizeof code));
    CHECK_STR(url, "https://romm.example.com"); CHECK_STR(code, "ABCD-1234");
    CHECK(parse_pair_payload("abcd-1234", url, sizeof url, code, sizeof code)); CHECK_STR(url, "");
    CHECK(!parse_pair_payload("http://host/pair", url, sizeof url, code, sizeof code));
    CHECK(!parse_pair_payload("hello", url, sizeof url, code, sizeof code));
    CHECK(has_scheme("HTTP://x")); CHECK(!has_scheme("ftp://x")); CHECK(!has_scheme("192.168.1.1"));
    origin_of("https://h:1/a/b?c", url, sizeof url); CHECK_STR(url, "https://h:1");
}

static void test_search_and_size(void) {
    CHECK(name_matches("Super Mario World", ""));
    CHECK(name_matches("Super Mario World", "mario world"));
    CHECK(name_matches("Super Mario World", "WORLD  super"));
    CHECK(!name_matches("Super Mario World", "zelda"));
    CHECK(!name_matches("Super Mario World", "mario zelda"));
    char o[24];
    fmt_size(512 * 1024, o, sizeof o);        CHECK_STR(o, "512 KB");
    fmt_size(3 * 1024 * 1024, o, sizeof o);   CHECK_STR(o, "3.0 MB");
}

static Game mk(int id) { Game g; memset(&g, 0, sizeof g); g.id = id; snprintf(g.name, sizeof g.name, "game%d", id); return g; }

static void test_queue(void) {
    g_nqueue = 0;
    Game a = mk(1), b = mk(2), c = mk(3);
    CHECK(queue_add(&a)); CHECK(queue_add(&b)); CHECK(queue_add(&c));
    CHECK(!queue_add(&b));                       // duplicates are refused
    CHECK(g_nqueue == 3 && queue_find(2) == 1 && queue_find(9) == -1);
    queue_remove(1);                              // middle
    CHECK(g_nqueue == 2 && g_queue[0].id == 1 && g_queue[1].id == 3);
    queue_remove(5); queue_remove(-1);            // out of range is a no-op
    CHECK(g_nqueue == 2);
    queue_remove(1); queue_remove(0);
    CHECK(g_nqueue == 0);
    for (int i = 0; i < MAX_QUEUE; i++) { Game x = mk(100 + i); CHECK(queue_add(&x)); }
    Game extra = mk(999);
    CHECK(!queue_add(&extra) && g_nqueue == MAX_QUEUE);   // full
    g_nqueue = 0;
}

// ---------------------------------------------------------------- API contract tests
// Cross-checked against rommapp/romm backend/endpoints (saves.py, states.py, roms/__init__.py, client_tokens.py,
// heartbeat.py, platform.py, user.py). Each URL we build must hit a real route and use only parameters that route accepts.

static int query_keys_subset(const char *url, const char *const *allowed) {
    const char *q = strchr(url, '?');
    if (!q) return 1;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", q + 1);
    for (char *kv = strtok(buf, "&"); kv; kv = strtok(NULL, "&")) {
        char *eq = strchr(kv, '=');
        if (eq) *eq = 0;
        int ok = 0;
        for (int i = 0; allowed[i]; i++) if (!strcmp(kv, allowed[i])) ok = 1;
        if (!ok) { printf("   unknown query parameter: %s in %s\n", kv, url); return 0; }
    }
    return 1;
}

static void test_api_contract(void) {
    char u[700];
    const char *B = "http://romm.lan:8285";
    static const char *saves_post[]  = { "rom_id", "emulator", "slot", "device_id", "content_hash", "session_id",
                                         "overwrite", "autocleanup", "autocleanup_limit", NULL };
    static const char *states_post[] = { "rom_id", "emulator", NULL };
    static const char *list_q[]      = { "rom_id", "rom_ids", "platform_id", "device_id", "slot", NULL };
    static const char *roms_q[]      = { "platform_ids", "limit", "offset", "order_by", "order_dir", NULL };

    api_heartbeat(u, sizeof u, B);       CHECK_STR(u, "http://romm.lan:8285/api/heartbeat");
    api_users_me(u, sizeof u, B);        CHECK_STR(u, "http://romm.lan:8285/api/users/me");
    api_token_exchange(u, sizeof u, B);  CHECK_STR(u, "http://romm.lan:8285/api/client-tokens/exchange");
    api_platforms(u, sizeof u, B);       CHECK_STR(u, "http://romm.lan:8285/api/platforms");
    api_states_delete(u, sizeof u, B);   CHECK_STR(u, "http://romm.lan:8285/api/states/delete");

    api_roms_page(u, sizeof u, B, 7, 100, 200);
    CHECK_STR(u, "http://romm.lan:8285/api/roms?platform_ids=7&limit=100&offset=200&order_by=name&order_dir=asc");
    CHECK(query_keys_subset(u, roms_q));

    api_rom_content(u, sizeof u, B, 42, "Super Mario (USA).sfc");
    CHECK_STR(u, "http://romm.lan:8285/api/roms/42/content/Super%20Mario%20%28USA%29.sfc");

    api_assets_list(u, sizeof u, B, K_SAVE, 5);   CHECK_STR(u, "http://romm.lan:8285/api/saves?rom_id=5");
    CHECK(query_keys_subset(u, list_q));
    api_assets_list(u, sizeof u, B, K_STATE, 5);  CHECK_STR(u, "http://romm.lan:8285/api/states?rom_id=5");
    CHECK(query_keys_subset(u, list_q));
    api_asset_content(u, sizeof u, B, K_SAVE, 9); CHECK_STR(u, "http://romm.lan:8285/api/saves/9/content");
    api_asset_content(u, sizeof u, B, K_STATE, 9);CHECK_STR(u, "http://romm.lan:8285/api/states/9/content");

    api_save_upload(u, sizeof u, B, 5, 0);
    CHECK_STR(u, "http://romm.lan:8285/api/saves?rom_id=5&overwrite=false&slot=rommvita&emulator=rommvita&autocleanup=true&autocleanup_limit=10");
    CHECK(query_keys_subset(u, saves_post));
    api_save_upload(u, sizeof u, B, 5, 1); CHECK(strstr(u, "overwrite=true") != NULL);

    api_state_upload(u, sizeof u, B, 5);
    CHECK_STR(u, "http://romm.lan:8285/api/states?rom_id=5&emulator=rommvita");
    CHECK(query_keys_subset(u, states_post));   // RomM's state upload has no slot/autocleanup parameters

    CHECK(api_heartbeat(u, 10, B) < 0);         // too small: reported, not overflowed
}

// ---------------------------------------------------------------- platform table regression tests
static void test_platform_table(void) {
    // slugs from RomM backend/utils/platform_slugs.py (UPS enum) for the systems we support
    static const char *ups[] = { "snes", "gba", "nes", "gb", "gbc", "genesis", NULL };
    int impl = 0;
    for (int i = 0; i < NPLATS; i++) {
        const Platform *p = &g_plats[i];
        CHECK(p->name && p->dir);
        for (int j = i + 1; j < NPLATS; j++) { CHECK(strcmp(p->dir, g_plats[j].dir)); }
        if (p->slug) for (int j = i + 1; j < NPLATS; j++) CHECK(!g_plats[j].slug || strcmp(p->slug, g_plats[j].slug));
        if (!p->impl) continue;
        impl++;
        int known = 0;
        for (int k = 0; ups[k]; k++) if (p->slug && !strcmp(p->slug, ups[k])) known = 1;
        if (!known) printf("   %s has slug %s which RomM does not define\n", p->name, p->slug ? p->slug : "(null)");
        CHECK(known);
        CHECK(p->cores[0] != NULL);
        for (int c = 0; c < 6 && p->cores[c]; c++) {
            const char *core = p->cores[c];
            size_t l = strlen(core);
            CHECK(l > 9 && !strcmp(core + l - 9, "_libretro"));
            char dir[64];
            core_sort_dir(core, dir, sizeof dir);
            CHECK(strcmp(dir, core) != 0);      // every shipped core has a sort-folder name
        }
    }
    CHECK(impl == 6);                            // snes, gba, nes, gb, gbc, genesis
    // regression: the Genesis slug was once the legacy "genesis-slash-megadrive", which RomM 5 no longer returns
    for (int i = 0; i < NPLATS; i++) if (!strcmp(g_plats[i].dir, "genesis")) CHECK_STR(g_plats[i].slug, "genesis");
    CHECK_STR(g_plats[0].slug, "snes");          // SNES stays first: it is the default platform
}

int main(void) {
    test_json_str(); test_url_encode(); test_sanitize_and_ext(); test_norm_key_and_sort_dir();
    test_pairing(); test_search_and_size(); test_queue();
    test_api_contract(); test_platform_table();
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}

// PlayLog - per-game play time statistics for Nintendo Switch.
// Reads the console's own play history (pdm service), so it works
// retroactively: everything you played before installing is included.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>
#include <switch.h>
#include <SDL.h>
#include <SDL_ttf.h>
#include <SDL_image.h>

#define APP_VER "1.0.0"

#define SCREEN_W 1280
#define SCREEN_H 720
#define HEADER_H 72
#define FOOTER_H 48

#define MAX_SEG_SEC (24 * 3600) // ignore segments longer than this (clock jumps)
#define MERGE_GAP_SEC 300       // focus gaps shorter than this join one session

#define LIST_Y 124
#define ROW_H 104
#define LIST_VISIBLE ((SCREEN_H - FOOTER_H - LIST_Y) / ROW_H)

#define DETAIL_DAYS 30
#define ACT_DAYS 14

// Colors are 0xRRGGBBAA
#define C_BG       0x14161BFF
#define C_HEADER   0x1B1E25FF
#define C_CARD     0x22262EFF
#define C_CARD_SEL 0x2C3442FF
#define C_ACCENT   0x2FB4FFFF
#define C_ACCENT_D 0x1D5E86FF
#define C_WARM     0xFFB547FF
#define C_TEXT     0xF0F2F5FF
#define C_DIM      0x9AA3B0FF
#define C_FAINT    0x353B47FF

enum { F_SMALL, F_BODY, F_LARGE, F_TITLE, F_HUGE, F_COUNT };
static const int kFontSizes[F_COUNT] = {18, 22, 28, 36, 52};

enum { AL_LEFT, AL_CENTER, AL_RIGHT };
enum { SCR_GAMES, SCR_DETAIL, SCR_ACTIVITY };
enum { SORT_TIME, SORT_RECENT, SORT_LAUNCHES, SORT_FIRST, SORT_NAME, SORT_COUNT };
static const char *kSortNames[SORT_COUNT] = {"Play time", "Recently played", "Launches", "First played", "Title"};

static const char *kMonths[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *kWeekdays[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *kWd3[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

typedef struct {
    u64 start; // POSIX seconds (user clock)
    u32 dur;   // seconds in focus
} Seg;

typedef struct {
    u64 id;
    bool meta;
    char name[0x201];
    char author[0x101];
    SDL_Texture *icon;

    // Official per-user totals from pdm
    bool hasStats;
    u64 statSec;
    u32 launches;
    u64 first, last;

    // Reconstructed from the event log
    Seg *seg;
    int nSeg, capSeg;
    u64 tracked;
} Game;

typedef struct {
    AccountUid uid;
    char nick[0x21];
    SDL_Texture *avatar;
} User;

static SDL_Window *g_win;
static SDL_Renderer *R;
static TTF_Font *g_font[F_COUNT];

static Game **g_games;
static int g_nGames, g_capGames;
static Game **g_view;
static int g_nView;

static User g_users[8];
static int g_nUsers, g_curUser;

static NsApplicationControlData *g_ctrl;
static PdmPlayEvent g_ev[256];

static s64 g_tzoff; // local - UTC, seconds
static s64 g_today; // local day index

// Aggregates for the current user
static s64 g_dayBase;
static int g_nDays;
static u32 *g_daySec;
static u64 g_hourSec[24];
static u64 g_wdSec[7];
static u64 g_totalTracked;

static int g_screen = SCR_GAMES;
static int g_sort = SORT_TIME;
static int g_sel, g_scroll;

// Detail screen state
static Game *g_det;
static u32 *g_detDaily;
static Seg *g_detSess;
static int g_nDetSess;
static u64 g_detLongest;
static int g_detDaysPlayed;
static s64 g_detEnd;

// Activity screen state
static s64 g_actEnd, g_actSel;
static Game *g_dayGames[5];
static u32 g_dayGameSec[5];
static int g_nDayGames;
static s64 g_dayGamesFor = -1;

static char g_toast[160];
static u64 g_toastUntil;
static u32 g_frame;

// ---------------------------------------------------------------------------
// Time helpers

static s64 floorDiv(s64 a, s64 b) {
    s64 q = a / b;
    if ((a % b) != 0 && ((a < 0) != (b < 0))) q--;
    return q;
}

static s64 daysFromCivil(s64 y, int m, int d) {
    y -= m <= 2;
    s64 era = (y >= 0 ? y : y - 399) / 400;
    s64 yoe = y - era * 400;
    s64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    s64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civilFromDays(s64 z, int *y, int *m, int *d) {
    z += 719468;
    s64 era = (z >= 0 ? z : z - 146096) / 146097;
    s64 doe = z - era * 146097;
    s64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    s64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    s64 mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

static int weekday(s64 day) { return (int)(((day % 7) + 7 + 4) % 7); } // 1970-01-01 was a Thursday

static u64 nowPosix(void) {
    u64 t = 0;
    if (R_FAILED(timeGetCurrentTime(TimeType_UserSystemClock, &t))) t = (u64)time(NULL);
    return t;
}

static void initTimezone(void) {
    u64 now = nowPosix();
    TimeCalendarTime ct;
    TimeCalendarAdditionalInfo ai;
    g_tzoff = 0;
    if (R_SUCCEEDED(timeToCalendarTimeWithMyRule(now, &ct, &ai))) {
        s64 local = daysFromCivil(ct.year, ct.month, ct.day) * 86400 + ct.hour * 3600 + ct.minute * 60 + ct.second;
        g_tzoff = local - (s64)now;
    }
    g_today = floorDiv((s64)now + g_tzoff, 86400);
}

static s64 localDay(u64 ts) { return floorDiv((s64)ts + g_tzoff, 86400); }

static void fmtDur(char *b, size_t n, u64 s) {
    unsigned long long m = s / 60, h = m / 60;
    m %= 60;
    if (h) snprintf(b, n, "%lluh %02llum", h, m);
    else if (s > 0 && s < 60) snprintf(b, n, "<1m");
    else snprintf(b, n, "%llum", m);
}

static void fmtDay(char *b, size_t n, s64 day, bool withYear) {
    int y, m, d;
    civilFromDays(day, &y, &m, &d);
    if (withYear) snprintf(b, n, "%s %d, %d", kMonths[m - 1], d, y);
    else snprintf(b, n, "%s %d", kMonths[m - 1], d);
}

static void fmtDate(char *b, size_t n, u64 ts) {
    if (!ts) { snprintf(b, n, "-"); return; }
    fmtDay(b, n, localDay(ts), true);
}

// Calls cb for each local-time bucket (day or hour) the interval overlaps
typedef void (*SplitCb)(s64 bucket, u32 sec, void *ctx);
static void splitLocal(u64 start, u32 dur, s64 bucket, SplitCb cb, void *ctx) {
    s64 t = (s64)start + g_tzoff, end = t + dur;
    while (t < end) {
        s64 b = floorDiv(t, bucket);
        s64 bEnd = (b + 1) * bucket;
        s64 e = end < bEnd ? end : bEnd;
        cb(b, (u32)(e - t), ctx);
        t = e;
    }
}

// ---------------------------------------------------------------------------
// Drawing helpers

static void setColor(u32 c) { SDL_SetRenderDrawColor(R, c >> 24, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF); }

static void fillRect(int x, int y, int w, int h, u32 c) {
    SDL_Rect r = {x, y, w, h};
    setColor(c);
    SDL_RenderFillRect(R, &r);
}

static void outlineRect(int x, int y, int w, int h, int t, u32 c) {
    fillRect(x, y, w, t, c);
    fillRect(x, y + h - t, w, t, c);
    fillRect(x, y, t, h, c);
    fillRect(x + w - t, y, t, h, c);
}

static void drawTex(SDL_Texture *t, int x, int y, int w, int h) {
    SDL_Rect r = {x, y, w, h};
    SDL_RenderCopy(R, t, NULL, &r);
}

typedef struct {
    char key[520];
    int font, maxw;
    u32 color;
    SDL_Texture *tex;
    int w, h;
    u32 used;
} TextEntry;

#define TEXT_CACHE 384
static TextEntry g_tc[TEXT_CACHE];

static void fitText(TTF_Font *f, const char *s, int maxw, char *out, size_t outsz) {
    snprintf(out, outsz, "%s", s);
    int w = 0, h = 0;
    if (maxw <= 0) return;
    TTF_SizeUTF8(f, out, &w, &h);
    if (w <= maxw) return;
    size_t len = strlen(out);
    char tmp[600];
    while (len > 0) {
        do { len--; } while (len > 0 && ((unsigned char)out[len] & 0xC0) == 0x80);
        memcpy(tmp, out, len);
        memcpy(tmp + len, "...", 4);
        TTF_SizeUTF8(f, tmp, &w, &h);
        if (w <= maxw) break;
    }
    snprintf(out, outsz, "%s", len > 0 ? tmp : "...");
}

static TextEntry *textGet(int font, u32 color, int maxw, const char *s) {
    if (!s[0]) return NULL;
    TextEntry *freeE = NULL, *lru = NULL;
    for (int i = 0; i < TEXT_CACHE; i++) {
        TextEntry *e = &g_tc[i];
        if (!e->tex) {
            if (!freeE) freeE = e;
            continue;
        }
        if (e->font == font && e->color == color && e->maxw == maxw && strcmp(e->key, s) == 0) {
            e->used = g_frame;
            return e;
        }
        if (!lru || e->used < lru->used) lru = e;
    }
    TextEntry *e = freeE ? freeE : lru;
    if (e->tex) {
        SDL_DestroyTexture(e->tex);
        e->tex = NULL;
    }
    char buf[600];
    fitText(g_font[font], s, maxw, buf, sizeof(buf));
    SDL_Color c = {color >> 24, (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF};
    SDL_Surface *surf = TTF_RenderUTF8_Blended(g_font[font], buf, c);
    if (!surf) return NULL;
    e->tex = SDL_CreateTextureFromSurface(R, surf);
    e->w = surf->w;
    e->h = surf->h;
    SDL_FreeSurface(surf);
    if (!e->tex) return NULL;
    snprintf(e->key, sizeof(e->key), "%s", s);
    e->font = font;
    e->color = color;
    e->maxw = maxw;
    e->used = g_frame;
    return e;
}

static int text(int font, int x, int y, u32 color, int align, int maxw, const char *fmt, ...) {
    char buf[520];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    TextEntry *e = textGet(font, color, maxw, buf);
    if (!e) return 0;
    int dx = x;
    if (align == AL_CENTER) dx = x - e->w / 2;
    else if (align == AL_RIGHT) dx = x - e->w;
    drawTex(e->tex, dx, y, e->w, e->h);
    return e->w;
}

static void clearTextCache(void) {
    for (int i = 0; i < TEXT_CACHE; i++) {
        if (g_tc[i].tex) SDL_DestroyTexture(g_tc[i].tex);
        g_tc[i].tex = NULL;
    }
}

static SDL_Texture *loadJpeg(const void *data, size_t size) {
    SDL_RWops *rw = SDL_RWFromConstMem(data, (int)size);
    if (!rw) return NULL;
    SDL_Surface *s = IMG_Load_RW(rw, 1);
    if (!s) return NULL;
    SDL_Texture *t = SDL_CreateTextureFromSurface(R, s);
    SDL_FreeSurface(s);
    return t;
}

static void drawLoading(const char *msg) {
    g_frame++;
    setColor(C_BG);
    SDL_RenderClear(R);
    text(F_TITLE, SCREEN_W / 2, 300, C_TEXT, AL_CENTER, 0, "Play Log");
    text(F_BODY, SCREEN_W / 2, 360, C_DIM, AL_CENTER, 0, "%s", msg);
    SDL_RenderPresent(R);
}

// ---------------------------------------------------------------------------
// Data

static Game *findGame(u64 id) {
    static Game *last;
    if (last && last->id == id) return last;
    for (int i = 0; i < g_nGames; i++)
        if (g_games[i]->id == id) return last = g_games[i];
    return NULL;
}

static Game *addGame(u64 id) {
    Game *g = findGame(id);
    if (g) return g;
    if (g_nGames == g_capGames) {
        g_capGames = g_capGames ? g_capGames * 2 : 128;
        g_games = realloc(g_games, sizeof(Game *) * g_capGames);
    }
    g = calloc(1, sizeof(Game));
    g->id = id;
    g_games[g_nGames++] = g;
    return g;
}

static void addSeg(Game *g, u64 start, u32 dur) {
    if (g->nSeg == g->capSeg) {
        g->capSeg = g->capSeg ? g->capSeg * 2 : 32;
        g->seg = realloc(g->seg, sizeof(Seg) * g->capSeg);
    }
    g->seg[g->nSeg].start = start;
    g->seg[g->nSeg].dur = dur;
    g->nSeg++;
    g->tracked += dur;
}

static void ensureMeta(Game *g) {
    if (g->meta) return;
    g->meta = true;
    u64 sz = 0;
    Result rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, g->id, g_ctrl, sizeof(*g_ctrl), &sz);
    if (R_FAILED(rc))
        rc = nsGetApplicationControlData(NsApplicationControlSource_CacheOnly, g->id, g_ctrl, sizeof(*g_ctrl), &sz);
    if (R_SUCCEEDED(rc)) {
        NacpLanguageEntry *le = NULL;
        if (R_SUCCEEDED(nacpGetLanguageEntry(&g_ctrl->nacp, &le)) && le) {
            snprintf(g->name, sizeof(g->name), "%.*s", (int)sizeof(le->name), le->name);
            snprintf(g->author, sizeof(g->author), "%.*s", (int)sizeof(le->author), le->author);
        }
        if (sz > sizeof(g_ctrl->nacp)) g->icon = loadJpeg(g_ctrl->icon, sz - sizeof(g_ctrl->nacp));
    }
    if (!g->name[0]) snprintf(g->name, sizeof(g->name), "Unknown title %016llX", (unsigned long long)g->id);
}

static u64 gameSecs(const Game *g) { return g->hasStats ? g->statSec : g->tracked; }

static u64 gameLast(const Game *g) {
    if (g->hasStats && g->last) return g->last;
    return g->nSeg ? g->seg[g->nSeg - 1].start + g->seg[g->nSeg - 1].dur : 0;
}

static u64 gameFirst(const Game *g) {
    if (g->hasStats && g->first) return g->first;
    return g->nSeg ? g->seg[0].start : 0;
}

static void loadUsers(void) {
    AccountUid uids[8];
    s32 n = 0;
    if (R_FAILED(accountListAllUsers(uids, 8, &n))) n = 0;
    g_nUsers = n;
    for (int i = 0; i < n; i++) {
        User *u = &g_users[i];
        u->uid = uids[i];
        AccountProfile p;
        if (R_SUCCEEDED(accountGetProfile(&p, uids[i]))) {
            AccountUserData ud;
            AccountProfileBase base;
            if (R_SUCCEEDED(accountProfileGet(&p, &ud, &base)))
                snprintf(u->nick, sizeof(u->nick), "%.*s", (int)sizeof(base.nickname), base.nickname);
            u32 isz = 0;
            if (R_SUCCEEDED(accountProfileGetImageSize(&p, &isz)) && isz > 0) {
                void *buf = malloc(isz);
                u32 real = 0;
                if (buf && R_SUCCEEDED(accountProfileLoadImage(&p, buf, isz, &real))) u->avatar = loadJpeg(buf, real);
                free(buf);
            }
            accountProfileClose(&p);
        }
        if (!u->nick[0]) snprintf(u->nick, sizeof(u->nick), "User %d", i + 1);
    }

    g_curUser = 0;
    AccountUid pre;
    if (R_SUCCEEDED(accountGetPreselectedUser(&pre)) && accountUidIsValid(&pre)) {
        for (int i = 0; i < n; i++)
            if (memcmp(&g_users[i].uid, &pre, sizeof(pre)) == 0) g_curUser = i;
    }
}

// pdm stores the uid as four u32 words; accept either word order
static bool uidMatch(const u32 *w, AccountUid id) {
    if (memcmp(w, &id, sizeof(id)) == 0) return true;
    u64 a = ((u64)w[0] << 32) | w[1];
    u64 b = ((u64)w[2] << 32) | w[3];
    return (a == id.uid[0] && b == id.uid[1]) || (a == id.uid[1] && b == id.uid[0]);
}

typedef struct {
    bool active, user;
    u64 app, start;
} Run;

static void endRun(Run *r, u64 ts) {
    if (r->active && r->user && ts > r->start && ts - r->start < MAX_SEG_SEC) addSeg(addGame(r->app), r->start, (u32)(ts - r->start));
    r->active = false;
}

// Rebuilds per-game focus segments for one user from the system play log
static void processEvents(AccountUid uid) {
    s32 total = 0, start = 0, end = 0;
    if (R_FAILED(pdmqryGetAvailablePlayEventRange(&total, &start, &end)) || total <= 0) return;

    bool single = g_nUsers <= 1;
    bool userOpen = false; // selected user is signed in to the running game
    Run run = {0};
    u64 lastTs = 0;

    for (s32 idx = start; idx <= end;) {
        s32 n = 0;
        if (R_FAILED(pdmqryQueryPlayEvent(idx, g_ev, 256, &n)) || n <= 0) break;
        for (int i = 0; i < n; i++) {
            PdmPlayEvent *e = &g_ev[i];
            u64 ts = e->timestamp_user;
            switch (e->play_event_type) {
            case PdmPlayEventType_Applet: {
                if (e->event_data.applet.applet_id != 0x01) break; // AppletId_application
                u64 app = ((u64)e->event_data.applet.program_id[0] << 32) | e->event_data.applet.program_id[1];
                switch (e->event_data.applet.event_type) {
                case PdmAppletEventType_Launch:
                case PdmAppletEventType_InFocus:
                    if (run.active && run.app != app) endRun(&run, ts);
                    if (!run.active) {
                        run.active = true;
                        run.app = app;
                        run.start = ts;
                        run.user = single || userOpen;
                    }
                    break;
                case PdmAppletEventType_OutOfFocus:
                case PdmAppletEventType_OutOfFocus4:
                    if (run.active && run.app == app) endRun(&run, ts);
                    break;
                case PdmAppletEventType_Exit:
                case PdmAppletEventType_Exit5:
                case PdmAppletEventType_Exit6:
                    if (run.active && run.app == app) endRun(&run, ts);
                    userOpen = false;
                    break;
                default:
                    break;
                }
                break;
            }
            case PdmPlayEventType_Account: {
                if (!uidMatch(e->event_data.account.uid, uid)) break;
                u64 app = ((u64)e->event_data.account.application_id[0] << 32) | e->event_data.account.application_id[1];
                // type 0 = open, 1 = close (no ApplicationId), 2 = open with ApplicationId
                u8 type = e->event_data.account.type;
                if (type == 0) {
                    userOpen = true;
                    if (run.active) run.user = true;
                } else if (type == 1) {
                    userOpen = false;
                } else if (type == 2 && run.active && run.app == app) {
                    run.user = true;
                }
                break;
            }
            case PdmPlayEventType_Initialize: // console booted: whatever was running ended before this
                if (run.active) endRun(&run, lastTs);
                userOpen = false;
                break;
            default:
                break;
            }
            if (ts) lastTs = ts;
        }
        idx += n;
    }
}

static void addDayCb(s64 day, u32 sec, void *ctx) {
    (void)ctx;
    s64 i = day - g_dayBase;
    if (i >= 0 && i < g_nDays) g_daySec[i] += sec;
    g_wdSec[weekday(day)] += sec;
}

static void addHourCb(s64 hour, u32 sec, void *ctx) {
    (void)ctx;
    g_hourSec[((hour % 24) + 24) % 24] += sec;
}

static void buildAggregates(void) {
    s64 minDay = g_today;
    for (int i = 0; i < g_nGames; i++)
        if (g_games[i]->nSeg) {
            s64 d = localDay(g_games[i]->seg[0].start);
            if (d < minDay) minDay = d;
        }
    if (g_today - minDay > 3650) minDay = g_today - 3650;
    free(g_daySec);
    g_dayBase = minDay;
    g_nDays = (int)(g_today - minDay + 1);
    g_daySec = calloc(g_nDays, sizeof(u32));
    memset(g_hourSec, 0, sizeof(g_hourSec));
    memset(g_wdSec, 0, sizeof(g_wdSec));
    g_totalTracked = 0;
    for (int i = 0; i < g_nGames; i++) {
        Game *g = g_games[i];
        for (int k = 0; k < g->nSeg; k++) {
            splitLocal(g->seg[k].start, g->seg[k].dur, 86400, addDayCb, NULL);
            splitLocal(g->seg[k].start, g->seg[k].dur, 3600, addHourCb, NULL);
        }
        g_totalTracked += g->tracked;
    }
}

static u32 daySec(s64 day) {
    s64 i = day - g_dayBase;
    return (i >= 0 && i < g_nDays) ? g_daySec[i] : 0;
}

static int cmpGames(const void *pa, const void *pb) {
    const Game *a = *(Game *const *)pa, *b = *(Game *const *)pb;
    u64 va = 0, vb = 0;
    switch (g_sort) {
    case SORT_TIME: va = gameSecs(a); vb = gameSecs(b); break;
    case SORT_RECENT: va = gameLast(a); vb = gameLast(b); break;
    case SORT_LAUNCHES: va = a->launches; vb = b->launches; break;
    case SORT_FIRST: va = gameFirst(b); vb = gameFirst(a); break; // oldest first
    case SORT_NAME: return strcasecmp(a->name, b->name);
    }
    if (va != vb) return va > vb ? -1 : 1;
    return strcasecmp(a->name, b->name);
}

static void sortView(void) { qsort(g_view, g_nView, sizeof(Game *), cmpGames); }

static void loadUser(int ui) {
    AccountUid uid = g_users[ui].uid;
    drawLoading("Reading play history...");

    for (int i = 0; i < g_nGames; i++) {
        Game *g = g_games[i];
        g->hasStats = false;
        g->statSec = 0;
        g->launches = 0;
        g->first = g->last = 0;
        g->nSeg = 0;
        g->tracked = 0;
    }

    u64 ids[256];
    s32 n = 0;
    if (R_SUCCEEDED(pdmqryQueryRecentlyPlayedApplication(uid, false, ids, 256, &n)))
        for (int i = 0; i < n; i++) addGame(ids[i]);

    processEvents(uid);

    for (int i = 0; i < g_nGames; i++) {
        Game *g = g_games[i];
        PdmPlayStatistics st;
        memset(&st, 0, sizeof(st));
        if (R_SUCCEEDED(pdmqryQueryPlayStatisticsByApplicationIdAndUserAccountId(g->id, uid, false, &st)) &&
            (st.playtime || st.total_launches)) {
            g->hasStats = true;
            g->statSec = st.playtime / 1000000000ULL; // nanoseconds
            g->launches = st.total_launches;
            g->first = st.first_timestamp_user;
            g->last = st.last_timestamp_user;
        }
    }

    buildAggregates();

    free(g_view);
    g_view = malloc(sizeof(Game *) * (size_t)(g_nGames > 0 ? g_nGames : 1));
    g_nView = 0;
    for (int i = 0; i < g_nGames; i++)
        if (g_games[i]->hasStats || g_games[i]->nSeg) g_view[g_nView++] = g_games[i];

    for (int i = 0; i < g_nView; i++) {
        if (!g_view[i]->meta && (i % 8) == 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Loading titles... %d / %d", i + 1, g_nView);
            drawLoading(msg);
        }
        ensureMeta(g_view[i]);
    }
    sortView();

    g_sel = g_scroll = 0;
    g_actEnd = g_actSel = g_today;
    g_dayGamesFor = -1;
}

// ---------------------------------------------------------------------------
// Detail screen

static void detDayCb(s64 day, u32 sec, void *ctx) {
    (void)ctx;
    s64 i = day - g_dayBase;
    if (i >= 0 && i < g_nDays) g_detDaily[i] += sec;
}

static void openDetail(Game *g) {
    g_det = g;
    free(g_detDaily);
    free(g_detSess);
    g_detDaily = calloc(g_nDays, sizeof(u32));
    g_detSess = malloc(sizeof(Seg) * (g->nSeg ? g->nSeg : 1));
    g_nDetSess = 0;
    g_detLongest = 0;
    for (int k = 0; k < g->nSeg; k++) {
        Seg s = g->seg[k];
        splitLocal(s.start, s.dur, 86400, detDayCb, NULL);
        // Merge short interruptions (HOME menu, sleep) into one session; dur stays active time
        Seg *prev = g_nDetSess ? &g_detSess[g_nDetSess - 1] : NULL;
        if (prev && s.start <= prev->start + prev->dur + MERGE_GAP_SEC && localDay(s.start) == localDay(prev->start)) {
            prev->dur = (u32)(s.start + s.dur - prev->start);
        } else {
            g_detSess[g_nDetSess++] = s;
        }
    }
    for (int k = 0; k < g_nDetSess; k++)
        if (g_detSess[k].dur > g_detLongest) g_detLongest = g_detSess[k].dur;
    g_detDaysPlayed = 0;
    for (int i = 0; i < g_nDays; i++)
        if (g_detDaily[i] >= 60) g_detDaysPlayed++;
    g_detEnd = g_today;
    g_screen = SCR_DETAIL;
}

// ---------------------------------------------------------------------------
// Activity screen

static void computeDayGames(s64 day) {
    if (g_dayGamesFor == day) return;
    g_dayGamesFor = day;
    g_nDayGames = 0;
    s64 d0 = day * 86400 - g_tzoff, d1 = d0 + 86400;
    for (int i = 0; i < g_nView; i++) {
        Game *g = g_view[i];
        u64 sum = 0;
        for (int k = 0; k < g->nSeg; k++) {
            s64 s = (s64)g->seg[k].start, e = s + g->seg[k].dur;
            if (e <= d0 || s >= d1) continue;
            sum += (u64)((e < d1 ? e : d1) - (s > d0 ? s : d0));
        }
        if (sum < 60) continue;
        int pos = g_nDayGames;
        while (pos > 0 && g_dayGameSec[pos - 1] < sum) pos--;
        if (pos >= 5) continue;
        int last = g_nDayGames < 5 ? g_nDayGames : 4;
        for (int j = last; j > pos; j--) {
            g_dayGames[j] = g_dayGames[j - 1];
            g_dayGameSec[j] = g_dayGameSec[j - 1];
        }
        g_dayGames[pos] = g;
        g_dayGameSec[pos] = (u32)sum;
        if (g_nDayGames < 5) g_nDayGames++;
    }
}

static int streakDays(void) {
    s64 d = g_today;
    if (daySec(d) < 60) d--;
    int n = 0;
    while (d >= g_dayBase && daySec(d) >= 60) {
        n++;
        d--;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Export

static void exportCsv(void) {
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/PlayLog", 0777);
    char path[128];
    snprintf(path, sizeof(path), "sdmc:/switch/PlayLog/playlog_%s.csv", g_users[g_curUser].nick);
    for (char *p = path + strlen("sdmc:/switch/PlayLog/"); *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':' || *p == '"') *p = '_';
    FILE *f = fopen(path, "w");
    if (!f) {
        snprintf(g_toast, sizeof(g_toast), "Could not write %s", path);
    } else {
        fprintf(f, "title_id,name,publisher,play_minutes,launches,first_played,last_played\n");
        for (int i = 0; i < g_nView; i++) {
            Game *g = g_view[i];
            char a[32], b[32];
            fmtDate(a, sizeof(a), gameFirst(g));
            fmtDate(b, sizeof(b), gameLast(g));
            char name[0x201];
            snprintf(name, sizeof(name), "%s", g->name);
            for (char *p = name; *p; p++)
                if (*p == '"') *p = '\'';
            fprintf(f, "%016llX,\"%s\",\"%s\",%llu,%u,\"%s\",\"%s\"\n", (unsigned long long)g->id, name, g->author,
                    (unsigned long long)(gameSecs(g) / 60), g->launches, a, b);
        }
        fprintf(f, "\ndate,minutes\n");
        for (int i = 0; i < g_nDays; i++) {
            if (!g_daySec[i]) continue;
            int y, m, d;
            civilFromDays(g_dayBase + i, &y, &m, &d);
            fprintf(f, "%04d-%02d-%02d,%u\n", y, m, d, g_daySec[i] / 60);
        }
        fclose(f);
        snprintf(g_toast, sizeof(g_toast), "Saved to %s", path + 5);
    }
    g_toastUntil = armGetSystemTick() + armGetSystemTickFreq() * 3;
}

// ---------------------------------------------------------------------------
// Rendering

static void drawIcon(Game *g, int x, int y, int size) {
    if (g->icon) {
        drawTex(g->icon, x, y, size, size);
    } else {
        fillRect(x, y, size, size, C_FAINT);
        text(size >= 80 ? F_TITLE : F_BODY, x + size / 2, y + size / 2 - (size >= 80 ? 22 : 14), C_DIM, AL_CENTER, 0, "?");
    }
}

static void drawHeader(void) {
    fillRect(0, 0, SCREEN_W, HEADER_H, C_HEADER);
    text(F_TITLE, 40, 14, C_TEXT, AL_LEFT, 0, "Play Log");

    bool act = g_screen == SCR_ACTIVITY;
    int w1 = text(F_LARGE, 420, 20, act ? C_DIM : C_TEXT, AL_LEFT, 0, "Games");
    int w2 = text(F_LARGE, 560, 20, act ? C_TEXT : C_DIM, AL_LEFT, 0, "Activity");
    if (act) fillRect(560, 62, w2, 4, C_ACCENT);
    else fillRect(420, 62, w1, 4, C_ACCENT);

    if (g_nUsers > 0) {
        User *u = &g_users[g_curUser];
        if (u->avatar) drawTex(u->avatar, SCREEN_W - 40 - 48, 12, 48, 48);
        else fillRect(SCREEN_W - 40 - 48, 12, 48, 48, C_FAINT);
        text(F_BODY, SCREEN_W - 40 - 48 - 14, 22, C_TEXT, AL_RIGHT, 300, "%s", u->nick);
    }
}

static void drawFooter(const char *hints) {
    fillRect(0, SCREEN_H - FOOTER_H, SCREEN_W, FOOTER_H, C_HEADER);
    text(F_BODY, 40, SCREEN_H - FOOTER_H + 11, C_DIM, AL_LEFT, 1080, "%s", hints);
    text(F_SMALL, SCREEN_W - 40, SCREEN_H - FOOTER_H + 14, C_FAINT, AL_RIGHT, 0, "v" APP_VER);
}

static void drawTile(int x, int y, int w, int h, const char *label, const char *value) {
    fillRect(x, y, w, h, C_CARD);
    text(F_SMALL, x + 16, y + 12, C_DIM, AL_LEFT, w - 32, "%s", label);
    text(F_LARGE, x + 16, y + 38, C_TEXT, AL_LEFT, w - 32, "%s", value);
}

static void drawGames(void) {
    u64 total = 0, maxSecs = 1;
    for (int i = 0; i < g_nView; i++) {
        u64 s = gameSecs(g_view[i]);
        total += s;
        if (s > maxSecs) maxSecs = s;
    }
    char tbuf[32];
    fmtDur(tbuf, sizeof(tbuf), total);
    text(F_BODY, 40, 86, C_DIM, AL_LEFT, 0, "%d games  \xC2\xB7  %s total", g_nView, tbuf);
    text(F_BODY, SCREEN_W - 40, 86, C_DIM, AL_RIGHT, 0, "Sorted by %s", kSortNames[g_sort]);

    if (!g_nView) {
        text(F_LARGE, SCREEN_W / 2, 320, C_DIM, AL_CENTER, 0, "No play data for this user yet.");
        return;
    }

    for (int r = 0; r < LIST_VISIBLE && g_scroll + r < g_nView; r++) {
        int i = g_scroll + r;
        Game *g = g_view[i];
        int y = LIST_Y + r * ROW_H;
        bool sel = i == g_sel;
        fillRect(40, y, SCREEN_W - 80, ROW_H - 8, sel ? C_CARD_SEL : C_CARD);
        if (sel) outlineRect(40, y, SCREEN_W - 80, ROW_H - 8, 3, C_ACCENT);
        drawIcon(g, 48, y + 8, 80);

        text(F_SMALL, 148, y + 14, C_DIM, AL_LEFT, 0, "#%d", i + 1);
        text(F_LARGE, 196, y + 8, C_TEXT, AL_LEFT, 700, "%s", g->name);
        text(F_SMALL, 148, y + 46, C_DIM, AL_LEFT, 700, "%s", g->author);

        u64 s = gameSecs(g);
        int barW = 760;
        fillRect(148, y + 74, barW, 8, C_FAINT);
        fillRect(148, y + 74, (int)(barW * s / maxSecs) > 2 ? (int)(barW * s / maxSecs) : 2, 8, C_ACCENT);

        fmtDur(tbuf, sizeof(tbuf), s);
        text(F_LARGE, SCREEN_W - 56, y + 8, C_TEXT, AL_RIGHT, 0, "%s", tbuf);
        char dbuf[32];
        fmtDate(dbuf, sizeof(dbuf), gameLast(g));
        text(F_SMALL, SCREEN_W - 56, y + 46, C_DIM, AL_RIGHT, 0, "%u launch%s", g->launches, g->launches == 1 ? "" : "es");
        text(F_SMALL, SCREEN_W - 56, y + 68, C_DIM, AL_RIGHT, 0, "Last played %s", dbuf);
    }

    if (g_nView > LIST_VISIBLE) {
        int trackH = LIST_VISIBLE * ROW_H - 8;
        int thumbH = trackH * LIST_VISIBLE / g_nView;
        if (thumbH < 24) thumbH = 24;
        int thumbY = LIST_Y + (trackH - thumbH) * g_scroll / (g_nView - LIST_VISIBLE);
        fillRect(SCREEN_W - 28, LIST_Y, 4, trackH, C_FAINT);
        fillRect(SCREEN_W - 28, thumbY, 4, thumbH, C_DIM);
    }
}

static void drawBarChart(int x, int y, int w, int h, const u32 *vals, int n, int sel, s64 firstDay) {
    u32 maxV = 60;
    for (int i = 0; i < n; i++)
        if (vals[i] > maxV) maxV = vals[i];
    int slot = w / n;
    int barW = slot * 3 / 4;
    int labelEvery = n > 20 ? 5 : 1;
    fillRect(x, y + h, w, 2, C_FAINT);
    for (int i = 0; i < n; i++) {
        int bx = x + i * slot + (slot - barW) / 2;
        int bh = (int)((u64)vals[i] * (h - 30) / maxV);
        if (vals[i] && bh < 3) bh = 3;
        u32 col = i == sel ? C_WARM : (vals[i] ? C_ACCENT : C_FAINT);
        fillRect(bx, y + h - bh, barW, bh, col);
        s64 day = firstDay + i;
        if (n <= 20 || (i % labelEvery) == 0 || i == n - 1 || i == sel) {
            int yy, mm, dd;
            civilFromDays(day, &yy, &mm, &dd);
            if (n <= 20) {
                text(F_SMALL, bx + barW / 2, y + h + 6, i == sel ? C_TEXT : C_DIM, AL_CENTER, 0, "%s", kWd3[weekday(day)]);
                text(F_SMALL, bx + barW / 2, y + h + 28, i == sel ? C_TEXT : C_DIM, AL_CENTER, 0, "%d/%d", mm, dd);
            } else {
                text(F_SMALL, bx + barW / 2, y + h + 6, C_DIM, AL_CENTER, 0, "%d/%d", mm, dd);
            }
        }
    }
    char buf[32];
    fmtDur(buf, sizeof(buf), maxV);
    text(F_SMALL, x + w, y - 4, C_DIM, AL_RIGHT, 0, "max %s", buf);
}

static void drawDetail(void) {
    Game *g = g_det;
    char a[48], b[48];
    drawIcon(g, 40, 92, 200);
    text(F_TITLE, 264, 90, C_TEXT, AL_LEFT, 976, "%s", g->name);
    text(F_BODY, 264, 138, C_DIM, AL_LEFT, 976, "%s", g->author);

    int tw = 236, th = 80, tx = 264, gap = 8;
    u64 secs = gameSecs(g);
    fmtDur(a, sizeof(a), secs);
    drawTile(tx, 184, tw, th, "Total play time", a);
    snprintf(a, sizeof(a), "%u", g->launches);
    drawTile(tx + (tw + gap), 184, tw, th, "Launches", a);
    fmtDur(a, sizeof(a), g->launches ? secs / g->launches : 0);
    drawTile(tx + (tw + gap) * 2, 184, tw, th, "Average per launch", a);
    fmtDur(a, sizeof(a), g_detLongest);
    drawTile(tx + (tw + gap) * 3, 184, tw, th, "Longest session", a);

    snprintf(a, sizeof(a), "%d", g_detDaysPlayed);
    drawTile(tx, 272, tw, th, "Days played", a);
    fmtDur(a, sizeof(a), g_detDaysPlayed ? g->tracked / g_detDaysPlayed : 0);
    drawTile(tx + (tw + gap), 272, tw, th, "Average per day played", a);
    fmtDate(a, sizeof(a), gameFirst(g));
    drawTile(tx + (tw + gap) * 2, 272, tw, th, "First played", a);
    fmtDate(a, sizeof(a), gameLast(g));
    drawTile(tx + (tw + gap) * 3, 272, tw, th, "Last played", a);

    // Daily chart
    int px = 40, py = 372, pw = 800, ph = SCREEN_H - FOOTER_H - 16 - py;
    fillRect(px, py, pw, ph, C_CARD);
    s64 first = g_detEnd - DETAIL_DAYS + 1;
    u32 vals[DETAIL_DAYS];
    u64 sum = 0;
    for (int i = 0; i < DETAIL_DAYS; i++) {
        s64 idx = first + i - g_dayBase;
        vals[i] = (idx >= 0 && idx < g_nDays) ? g_detDaily[idx] : 0;
        sum += vals[i];
    }
    fmtDay(a, sizeof(a), first, false);
    fmtDay(b, sizeof(b), g_detEnd, true);
    text(F_BODY, px + 20, py + 12, C_TEXT, AL_LEFT, 0, "Daily play time");
    char sbuf[32];
    fmtDur(sbuf, sizeof(sbuf), sum);
    text(F_SMALL, px + 20, py + 42, C_DIM, AL_LEFT, 0, "%s \xE2\x80\x93 %s  \xC2\xB7  %s", a, b, sbuf);
    drawBarChart(px + 20, py + 80, pw - 40, ph - 120, vals, DETAIL_DAYS, -1, first);

    // Recent sessions
    int sx = 856, sw = SCREEN_W - 40 - sx;
    fillRect(sx, py, sw, ph, C_CARD);
    text(F_BODY, sx + 20, py + 12, C_TEXT, AL_LEFT, 0, "Recent sessions");
    if (!g_nDetSess) text(F_SMALL, sx + 20, py + 52, C_DIM, AL_LEFT, sw - 40, "No detailed sessions in the log");
    for (int k = 0; k < 6 && k < g_nDetSess; k++) {
        Seg s = g_detSess[g_nDetSess - 1 - k];
        s64 loc = (s64)s.start + g_tzoff;
        s64 day = floorDiv(loc, 86400);
        int sod = (int)(loc - day * 86400);
        fmtDay(a, sizeof(a), day, false);
        fmtDur(b, sizeof(b), s.dur);
        int yy = py + 52 + k * 36;
        text(F_SMALL, sx + 20, yy, C_DIM, AL_LEFT, 0, "%s, %s  %02d:%02d", kWd3[weekday(day)], a, sod / 3600, (sod / 60) % 60);
        text(F_SMALL, sx + sw - 20, yy, C_TEXT, AL_RIGHT, 0, "%s", b);
    }
}

static void drawActivity(void) {
    char a[48], b[48];
    u64 d7 = 0, d30 = 0;
    for (int i = 0; i < 30; i++) {
        u32 v = daySec(g_today - i);
        d30 += v;
        if (i < 7) d7 += v;
    }
    u64 total = 0;
    for (int i = 0; i < g_nView; i++) total += gameSecs(g_view[i]);
    int tw = 191, gap = 10, ty = 88;
    fmtDur(a, sizeof(a), total);
    drawTile(40, ty, tw, 80, "Total played", a);
    fmtDur(a, sizeof(a), daySec(g_today));
    drawTile(40 + (tw + gap), ty, tw, 80, "Today", a);
    fmtDur(a, sizeof(a), d7);
    drawTile(40 + (tw + gap) * 2, ty, tw, 80, "Last 7 days", a);
    fmtDur(a, sizeof(a), d30);
    drawTile(40 + (tw + gap) * 3, ty, tw, 80, "Last 30 days", a);
    fmtDur(a, sizeof(a), d30 / 30);
    drawTile(40 + (tw + gap) * 4, ty, tw, 80, "Daily avg (30 days)", a);
    int st = streakDays();
    snprintf(a, sizeof(a), "%d day%s", st, st == 1 ? "" : "s");
    drawTile(40 + (tw + gap) * 5, ty, tw, 80, "Current streak", a);

    // Two-week chart with a cursor
    int px = 40, py = 182, pw = SCREEN_W - 80, ph = 284;
    fillRect(px, py, pw, ph, C_CARD);
    s64 first = g_actEnd - ACT_DAYS + 1;
    u32 vals[ACT_DAYS];
    for (int i = 0; i < ACT_DAYS; i++) vals[i] = daySec(first + i);
    fmtDay(a, sizeof(a), first, false);
    fmtDay(b, sizeof(b), g_actEnd, true);
    text(F_BODY, px + 20, py + 12, C_TEXT, AL_LEFT, 0, "Daily play time");
    text(F_SMALL, px + 220, py + 16, C_DIM, AL_LEFT, 0, "%s \xE2\x80\x93 %s", a, b);
    drawBarChart(px + 20, py + 52, pw - 40, ph - 110, vals, ACT_DAYS, (int)(g_actSel - first), first);

    // Selected day
    int dy = py + ph + 12, dh = SCREEN_H - FOOTER_H - 16 - dy;
    int dw = 640;
    fillRect(px, dy, dw, dh, C_CARD);
    fmtDay(a, sizeof(a), g_actSel, true);
    fmtDur(b, sizeof(b), daySec(g_actSel));
    text(F_BODY, px + 20, dy + 10, C_TEXT, AL_LEFT, 0, "%s, %s", kWeekdays[weekday(g_actSel)], a);
    text(F_BODY, px + dw - 20, dy + 10, C_WARM, AL_RIGHT, 0, "%s", b);
    computeDayGames(g_actSel);
    if (!g_nDayGames) text(F_SMALL, px + 20, dy + 50, C_DIM, AL_LEFT, 0, "Nothing played this day");
    for (int k = 0; k < g_nDayGames && k < 4; k++) {
        int yy = dy + 46 + k * 34;
        drawIcon(g_dayGames[k], px + 20, yy, 28);
        fmtDur(b, sizeof(b), g_dayGameSec[k]);
        text(F_SMALL, px + 58, yy + 2, C_TEXT, AL_LEFT, dw - 200, "%s", g_dayGames[k]->name);
        text(F_SMALL, px + dw - 20, yy + 2, C_DIM, AL_RIGHT, 0, "%s", b);
    }

    // When you play: hour-of-day histogram
    int hx = px + dw + 12, hw = SCREEN_W - 40 - hx;
    fillRect(hx, dy, hw, dh, C_CARD);
    text(F_BODY, hx + 20, dy + 10, C_TEXT, AL_LEFT, 0, "When you play");
    int bestWd = 0;
    for (int i = 1; i < 7; i++)
        if (g_wdSec[i] > g_wdSec[bestWd]) bestWd = i;
    if (g_totalTracked) text(F_SMALL, hx + hw - 20, dy + 14, C_DIM, AL_RIGHT, 0, "Busiest day: %s", kWeekdays[bestWd]);
    u64 maxH = 1;
    for (int i = 0; i < 24; i++)
        if (g_hourSec[i] > maxH) maxH = g_hourSec[i];
    int cx = hx + 20, cw = hw - 40, cy = dy + 48, ch = dh - 48 - 30;
    int slot = cw / 24;
    for (int i = 0; i < 24; i++) {
        int bh = (int)(g_hourSec[i] * ch / maxH);
        if (g_hourSec[i] && bh < 2) bh = 2;
        fillRect(cx + i * slot + 1, cy + ch - bh, slot - 3, bh, g_hourSec[i] ? C_ACCENT : C_FAINT);
        if (i % 6 == 0) text(F_SMALL, cx + i * slot, cy + ch + 4, C_DIM, AL_LEFT, 0, "%02d:00", i);
    }
}

static void drawToast(void) {
    if (!g_toast[0] || armGetSystemTick() > g_toastUntil) return;
    TextEntry *e = textGet(F_BODY, C_TEXT, 1000, g_toast);
    if (!e) return;
    int w = e->w + 48, x = (SCREEN_W - w) / 2, y = SCREEN_H - FOOTER_H - 72;
    fillRect(x, y, w, 52, C_ACCENT_D);
    drawTex(e->tex, x + 24, y + 12, e->w, e->h);
}

static void render(void) {
    g_frame++;
    setColor(C_BG);
    SDL_RenderClear(R);
    drawHeader();
    const char *users = g_nUsers > 1 ? "    ZL/ZR User" : "";
    char hints[160];
    switch (g_screen) {
    case SCR_GAMES:
        drawGames();
        snprintf(hints, sizeof(hints), "(A) Details    (Y) Sort    (X) Export CSV    L/R Tab%s    (+) Exit", users);
        break;
    case SCR_DETAIL:
        drawDetail();
        snprintf(hints, sizeof(hints), "(B) Back    Left/Right Shift 30 days    Up/Down Prev/next game    (+) Exit");
        break;
    case SCR_ACTIVITY:
        drawActivity();
        snprintf(hints, sizeof(hints), "Left/Right Select day    (Y) Today    (X) Export CSV    L/R Tab%s    (+) Exit", users);
        break;
    }
    drawFooter(hints);
    drawToast();
    SDL_RenderPresent(R);
}

// ---------------------------------------------------------------------------
// Input

static int g_rep[4];
static bool repeatKey(u64 down, u64 held, u64 mask, int slot) {
    if (down & mask) {
        g_rep[slot] = 0;
        return true;
    }
    if (held & mask) {
        g_rep[slot]++;
        return g_rep[slot] > 18 && (g_rep[slot] % 4) == 0;
    }
    g_rep[slot] = 0;
    return false;
}

static void clampList(void) {
    if (g_sel >= g_nView) g_sel = g_nView - 1;
    if (g_sel < 0) g_sel = 0;
    if (g_sel < g_scroll) g_scroll = g_sel;
    if (g_sel >= g_scroll + LIST_VISIBLE) g_scroll = g_sel - LIST_VISIBLE + 1;
    int maxScroll = g_nView > LIST_VISIBLE ? g_nView - LIST_VISIBLE : 0;
    if (g_scroll > maxScroll) g_scroll = maxScroll;
    if (g_scroll < 0) g_scroll = 0;
}

static void moveActSel(int delta) {
    s64 s = g_actSel + delta;
    if (s > g_today) s = g_today;
    if (s < g_dayBase) s = g_dayBase;
    g_actSel = s;
    if (g_actSel > g_actEnd) g_actEnd = g_actSel;
    if (g_actSel < g_actEnd - ACT_DAYS + 1) g_actEnd = g_actSel + ACT_DAYS - 1;
}

static void handleTap(int x, int y) {
    if (y < HEADER_H) {
        if (x >= 400 && x < 540) g_screen = SCR_GAMES;
        else if (x >= 540 && x < 720) g_screen = SCR_ACTIVITY;
        else if (g_screen == SCR_DETAIL && x < 300) g_screen = SCR_GAMES;
        return;
    }
    if (g_screen == SCR_GAMES && y >= LIST_Y) {
        int i = g_scroll + (y - LIST_Y) / ROW_H;
        if (i < g_nView) {
            if (i == g_sel) openDetail(g_view[i]);
            else g_sel = i;
        }
    } else if (g_screen == SCR_ACTIVITY && y >= 182 && y < 466) {
        int slot = (SCREEN_W - 120) / ACT_DAYS;
        int i = (x - 60) / slot;
        if (i >= 0 && i < ACT_DAYS) {
            s64 d = g_actEnd - ACT_DAYS + 1 + i;
            if (d <= g_today) g_actSel = d;
        }
    }
}

// ---------------------------------------------------------------------------

static bool initSdl(void) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) return false;
    g_win = SDL_CreateWindow("PlayLog", 0, 0, SCREEN_W, SCREEN_H, 0);
    if (!g_win) return false;
    R = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!R) return false;
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
    IMG_Init(IMG_INIT_JPG);
    if (TTF_Init() < 0) return false;

    PlFontData font;
    if (R_FAILED(plGetSharedFontByType(&font, PlSharedFontType_Standard))) return false;
    for (int i = 0; i < F_COUNT; i++) {
        g_font[i] = TTF_OpenFontRW(SDL_RWFromConstMem(font.address, font.size), 1, kFontSizes[i]);
        if (!g_font[i]) return false;
    }
    return true;
}

static void showFatal(const char *msg, Result rc) {
    PadState pad;
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & (HidNpadButton_Plus | HidNpadButton_B)) break;
        g_frame++;
        setColor(C_BG);
        SDL_RenderClear(R);
        text(F_TITLE, SCREEN_W / 2, 280, C_TEXT, AL_CENTER, 0, "Play Log");
        text(F_BODY, SCREEN_W / 2, 340, C_WARM, AL_CENTER, 1100, "%s (0x%X)", msg, rc);
        text(F_BODY, SCREEN_W / 2, 380, C_DIM, AL_CENTER, 0, "Press + to exit");
        SDL_RenderPresent(R);
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    Result rc;
    Result rcPdm = pdmqryInitialize();
    Result rcNs = nsInitialize();
    Result rcAcc = accountInitialize(AccountServiceType_Application);
    plInitialize(PlServiceType_User);
    hidInitializeTouchScreen();
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);

    if (!initSdl()) {
        // Nothing to draw with; bail out to hbmenu
        printf("SDL init failed: %s\n", SDL_GetError());
        goto cleanup;
    }

    initTimezone();
    g_ctrl = malloc(sizeof(NsApplicationControlData));

    if (R_FAILED(rcPdm)) {
        showFatal("Could not open the play data service (pdm:qry)", rcPdm);
        goto cleanup;
    }
    if (R_FAILED(rcAcc)) {
        showFatal("Could not open the account service", rcAcc);
        goto cleanup;
    }

    loadUsers();
    if (!g_nUsers) {
        showFatal("No user accounts found", 0);
        goto cleanup;
    }

    if (R_SUCCEEDED(rcNs)) {
        // Seed with installed titles so names resolve even for old play records
        NsApplicationRecord recs[64];
        s32 off = 0, cnt = 0;
        while (R_SUCCEEDED(rc = nsListApplicationRecord(recs, 64, off, &cnt)) && cnt > 0) {
            for (int i = 0; i < cnt; i++) addGame(recs[i].application_id);
            off += cnt;
        }
    }

    loadUser(g_curUser);

    PadState pad;
    padInitializeDefault(&pad);
    bool tDown = false, tMoved = false;
    int tx0 = 0, ty0 = 0, tScroll0 = 0, tLastX = 0, tLastY = 0;

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        u64 held = padGetButtons(&pad);
        if (down & HidNpadButton_Plus) break;

        bool up = repeatKey(down, held, HidNpadButton_AnyUp, 0);
        bool dn = repeatKey(down, held, HidNpadButton_AnyDown, 1);
        bool lf = repeatKey(down, held, HidNpadButton_AnyLeft, 2);
        bool rt = repeatKey(down, held, HidNpadButton_AnyRight, 3);

        if ((down & (HidNpadButton_L | HidNpadButton_R)) && g_screen != SCR_DETAIL)
            g_screen = g_screen == SCR_GAMES ? SCR_ACTIVITY : SCR_GAMES;

        if ((down & (HidNpadButton_ZL | HidNpadButton_ZR)) && g_nUsers > 1 && g_screen != SCR_DETAIL) {
            g_curUser = (g_curUser + ((down & HidNpadButton_ZR) ? 1 : g_nUsers - 1)) % g_nUsers;
            loadUser(g_curUser);
        }

        switch (g_screen) {
        case SCR_GAMES:
            if (up) g_sel--;
            if (dn) g_sel++;
            if (lf) g_sel -= LIST_VISIBLE;
            if (rt) g_sel += LIST_VISIBLE;
            clampList();
            if ((down & HidNpadButton_A) && g_nView) openDetail(g_view[g_sel]);
            if (down & HidNpadButton_Y) {
                Game *cur = g_nView ? g_view[g_sel] : NULL;
                g_sort = (g_sort + 1) % SORT_COUNT;
                sortView();
                for (int i = 0; i < g_nView; i++)
                    if (g_view[i] == cur) g_sel = i;
                clampList();
            }
            if (down & HidNpadButton_X) exportCsv();
            break;
        case SCR_DETAIL:
            if (down & HidNpadButton_B) g_screen = SCR_GAMES;
            if (lf && g_detEnd - DETAIL_DAYS >= g_dayBase) g_detEnd -= DETAIL_DAYS;
            if (rt && g_detEnd < g_today) g_detEnd = g_detEnd + DETAIL_DAYS > g_today ? g_today : g_detEnd + DETAIL_DAYS;
            if ((up && g_sel > 0) || (dn && g_sel < g_nView - 1)) {
                g_sel += up ? -1 : 1;
                clampList();
                openDetail(g_view[g_sel]);
            }
            break;
        case SCR_ACTIVITY:
            if (lf) moveActSel(-1);
            if (rt) moveActSel(1);
            if (up) moveActSel(-7);
            if (dn) moveActSel(7);
            if (down & HidNpadButton_Y) g_actSel = g_actEnd = g_today;
            if (down & HidNpadButton_X) exportCsv();
            if (down & HidNpadButton_B) g_screen = SCR_GAMES;
            break;
        }

        // Touch: tap to select/open, drag to scroll the game list
        HidTouchScreenState ts = {0};
        if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
            int x = ts.touches[0].x, y = ts.touches[0].y;
            if (!tDown) {
                tDown = true;
                tMoved = false;
                tx0 = x;
                ty0 = y;
                tScroll0 = g_scroll;
            } else if (abs(y - ty0) > 12 || abs(x - tx0) > 12) {
                tMoved = true;
            }
            if (tMoved && g_screen == SCR_GAMES) {
                int maxScroll = g_nView > LIST_VISIBLE ? g_nView - LIST_VISIBLE : 0;
                g_scroll = tScroll0 + (ty0 - y) / (ROW_H / 2);
                if (g_scroll > maxScroll) g_scroll = maxScroll;
                if (g_scroll < 0) g_scroll = 0;
                if (g_sel < g_scroll) g_sel = g_scroll;
                if (g_sel >= g_scroll + LIST_VISIBLE) g_sel = g_scroll + LIST_VISIBLE - 1;
            }
            tLastX = x;
            tLastY = y;
        } else if (tDown) {
            tDown = false;
            if (!tMoved) handleTap(tLastX, tLastY);
        }

        render();
    }

cleanup:
    clearTextCache();
    for (int i = 0; i < g_nGames; i++) {
        if (g_games[i]->icon) SDL_DestroyTexture(g_games[i]->icon);
        free(g_games[i]->seg);
        free(g_games[i]);
    }
    free(g_games);
    free(g_view);
    for (int i = 0; i < g_nUsers; i++)
        if (g_users[i].avatar) SDL_DestroyTexture(g_users[i].avatar);
    free(g_daySec);
    free(g_detDaily);
    free(g_detSess);
    free(g_ctrl);
    for (int i = 0; i < F_COUNT; i++)
        if (g_font[i]) TTF_CloseFont(g_font[i]);
    TTF_Quit();
    IMG_Quit();
    if (R) SDL_DestroyRenderer(R);
    if (g_win) SDL_DestroyWindow(g_win);
    SDL_Quit();
    plExit();
    accountExit();
    nsExit();
    pdmqryExit();
    return 0;
}

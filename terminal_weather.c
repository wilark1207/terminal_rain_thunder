
// terminal_weather.c
// Rain + lightning animation for TTY using ncurses (Arch Linux friendly)
// Controls: 't' toggle thunderstorm, 'q' or ESC to quit
// Build: gcc -O2 -Wall -Wextra terminal_weather.c -lncurses -o terminal_weather

#define _XOPEN_SOURCE 700
#include <ncurses.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <string.h>
#include <strings.h>    // <-- needed for strcasecmp
#include <unistd.h>
#include <signal.h>
#include <getopt.h>

#define UPDATE_INTERVAL 0.015  // seconds (approx 66 FPS)

// Colors
#define CP_RAIN_NORMAL 1
#define CP_LIGHTNING   4

// Lightning config
static const char *LIGHTNING_CHARS = "*+#";  // fades '*' -> '+' -> '#'
static const double LIGHTNING_GROWTH_DELAY = 0.002;
static const int    LIGHTNING_MAX_BRANCHES = 2;
static const double LIGHTNING_BRANCH_CHANCE = 0.3;
static const double FORK_CHANCE = 0.15;
static const int    FORK_HORIZONTAL_SPREAD = 3;
static const double SEGMENT_LIFESPAN = 0.8; // seconds to fade out
static const double LIGHTNING_CHANCE = 0.005;

static int g_rows, g_cols;
static volatile sig_atomic_t g_resized = 0;

typedef struct {
    int x;
    double y;
    double speed;
    char ch;
} Raindrop;

typedef struct {
    int y, x;
    double birth; // seconds (monotonic)
} Segment;

typedef struct {
    int target_len;
    bool growing;
    double last_growth;
    int max_y, max_x;
    Segment *segs;
    int seg_count;
    int seg_cap;
} Bolt;

// --- timing helpers ---
static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sleep_sec(double s) {
    if (s <= 0) return;
    struct timespec req;
    req.tv_sec = (time_t)s;
    req.tv_nsec = (long)((s - req.tv_sec) * 1e9);
    nanosleep(&req, NULL);
}

// --- SIGWINCH handler for resize ---
static void on_winch(int signo) {
    (void)signo;
    g_resized = 1;
}

// --- color parsing ---
typedef struct { const char *name; short code; } ColorMap;

static const ColorMap COLOR_MAP[] = {
    {"black", COLOR_BLACK}, {"red", COLOR_RED}, {"green", COLOR_GREEN},
    {"yellow", COLOR_YELLOW}, {"blue", COLOR_BLUE}, {"magenta", COLOR_MAGENTA},
    {"cyan", COLOR_CYAN}, {"white", COLOR_WHITE},
};

static short color_from_name(const char *s, short defval) {
    if (!s) return defval;
    for (size_t i = 0; i < sizeof(COLOR_MAP)/sizeof(COLOR_MAP[0]); ++i) {
        if (strcasecmp(s, COLOR_MAP[i].name) == 0)
            return COLOR_MAP[i].code;
    }
    return defval;
}

// --- dynamic arrays (minimal) ---
typedef struct {
    Raindrop *v; int n, cap;
} RainVec;

typedef struct {
    Bolt *v; int n, cap;
} BoltVec;

static void rain_push(RainVec *rv, Raindrop d) {
    if (rv->n == rv->cap) {
        rv->cap = rv->cap ? rv->cap * 2 : 256;
        rv->v = (Raindrop*)realloc(rv->v, rv->cap * sizeof(*rv->v));
    }
    rv->v[rv->n++] = d;
}

static void boltvec_push(BoltVec *bv, Bolt b) {
    if (bv->n == bv->cap) {
        bv->cap = bv->cap ? bv->cap * 2 : 64;
        bv->v = (Bolt*)realloc(bv->v, bv->cap * sizeof(*bv->v));
    }
    bv->v[bv->n++] = b;
}

static void seg_push(Bolt *b, Segment s) {
    if (b->seg_count == b->seg_cap) {
        b->seg_cap = b->seg_cap ? b->seg_cap * 2 : 64;
        b->segs = (Segment*)realloc(b->segs, b->seg_cap * sizeof(*b->segs));
    }
    b->segs[b->seg_count++] = s;
}

// --- lightning bolt ---
static Bolt bolt_create(int start_row, int start_col, int max_y, int max_x) {
    Bolt b;
    b.max_y = max_y; b.max_x = max_x;
    int min_len = max_y / 2;
    if (min_len < 2) min_len = 2;
    int max_len = max_y - 2;
    if (max_len < min_len) max_len = min_len + 1;
    b.target_len = (rand() % (max_len - min_len + 1)) + min_len;
    b.growing = true;
    b.last_growth = now_sec();
    b.segs = NULL; b.seg_count = 0; b.seg_cap = 0;
    Segment first = { start_row, start_col, now_sec() };
    seg_push(&b, first);
    return b;
}

static bool bolt_update(Bolt *b) {
    double t = now_sec();

    if (b->growing && (t - b->last_growth) >= LIGHTNING_GROWTH_DELAY) {
        b->last_growth = t;
        bool added = false;

        Segment last = b->segs[b->seg_count - 1];
        if (b->seg_count < b->target_len && last.y < b->max_y - 1) {
            int branches = 1;
            if (((double)rand()/RAND_MAX) < LIGHTNING_BRANCH_CHANCE) {
                branches = (rand() % (LIGHTNING_MAX_BRANCHES + 1)) + 1;
            }

            int current_x = last.x;
            int primary_next_x = current_x;

            for (int i = 0; i < branches; ++i) {
                int offset = (rand() % 5) - 2; // [-2,2]
                int nx = current_x + offset;
                if (nx < 0) nx = 0;
                if (nx >= b->max_x) nx = b->max_x - 1;
                int ny = last.y + 1;
                if (ny >= b->max_y) ny = b->max_y - 1;
                Segment s = { ny, nx, t };
                seg_push(b, s);
                if (i == 0) primary_next_x = nx;
                current_x = nx;
                added = true;
            }

            if (((double)rand()/RAND_MAX) < FORK_CHANCE) {
                int off = (rand() % (2*FORK_HORIZONTAL_SPREAD + 1)) - FORK_HORIZONTAL_SPREAD;
                if (off == 0) off = (rand()%2) ? -1 : 1;
                int fx = last.x + off;
                if (fx < 0) fx = 0;
                if (fx >= b->max_x) fx = b->max_x - 1;
                int fy = last.y + 1;
                if (fy >= b->max_y) fy = b->max_y - 1;
                if (fx != primary_next_x) {
                    Segment s = { fy, fx, t };
                    seg_push(b, s);
                    added = true;
                }
            }
        }

        if (!added || b->seg_count >= b->target_len || last.y >= b->max_y - 1) {
            b->growing = false;
        }
    }

    // prune if all segments expired
    bool any_alive = false;
    for (int i = 0; i < b->seg_count; ++i) {
        if ((now_sec() - b->segs[i].birth) <= SEGMENT_LIFESPAN) { any_alive = true; break; }
    }
    return any_alive;
}

static void bolt_draw(const Bolt *b, chtype l_attr) {
    double t = now_sec();
    int max_idx = 2; // '*','#','+' index via mapping below

    for (int i = 0; i < b->seg_count; ++i) {
        double age = t - b->segs[i].birth;
        if (age > SEGMENT_LIFESPAN) continue;
        double norm = age / SEGMENT_LIFESPAN; // 0..1
        int char_idx;
        if (norm < 0.33) char_idx = 2;      // '#'
        else if (norm < 0.66) char_idx = 1; // '+'
        else char_idx = 0;                  // '*'
        if (char_idx < 0)
            char_idx = 0;
        if (char_idx > max_idx)
            char_idx = max_idx;

        int y = b->segs[i].y, x = b->segs[i].x;
        if (y >= 0 && y < g_rows && x >= 0 && x < g_cols) {
            mvaddch(y, x, LIGHTNING_CHARS[char_idx] | l_attr);
        }
    }
}

// --- main sim ---
static void setup_colors(const char *rain_name, const char *lightning_name, chtype *rain_attr, chtype *light_attr) {
    *rain_attr = A_NORMAL;
    *light_attr = A_BOLD;

    if (!has_colors()) return;

    start_color();
#ifdef NCURSES_VERSION
    use_default_colors();
#endif

    short bg = -1;
    short rain_fg = color_from_name(rain_name, COLOR_CYAN);
    short light_fg = color_from_name(lightning_name, COLOR_YELLOW);

    init_pair(CP_RAIN_NORMAL, rain_fg, bg);
    init_pair(CP_LIGHTNING,   light_fg, bg);

    *rain_attr  = COLOR_PAIR(CP_RAIN_NORMAL);
    *light_attr = COLOR_PAIR(CP_LIGHTNING) | A_BOLD;
}

int main(int argc, char **argv) {
    // Parse options
    const char *rain_color = "cyan";
    const char *light_color = "yellow";

    static struct option long_opts[] = {
        {"rain-color", required_argument, 0, 'r'},
        {"lightning-color", required_argument, 0, 'l'},
        {0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "r:l:", long_opts, NULL)) != -1) {
        if (c == 'r') rain_color = optarg;
        else if (c == 'l') light_color = optarg;
    }

    if (!isatty(STDOUT_FILENO) || getenv("TERM") == NULL || strcmp(getenv("TERM"), "dumb") == 0) {
        fprintf(stderr, "Error: This program requires a real TTY.\n");
        return 1;
    }

    srand((unsigned)time(NULL));

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    timeout(1); // getch timeout (ms)
    curs_set(0);

    // Handle resize
    signal(SIGWINCH, on_winch);

    getmaxyx(stdscr, g_rows, g_cols);

    chtype rain_attr, light_attr;
    setup_colors(rain_color, light_color, &rain_attr, &light_attr);

    RainVec rain = {0};
    BoltVec bolts = {0};

    bool thunder = false;
    double last = now_sec();

    while (1) {
        if (g_resized) {
            g_resized = 0;
#ifdef NCURSES_VERSION
            endwin(); refresh();
#endif
            getmaxyx(stdscr, g_rows, g_cols);
            clear();
            rain.n = 0;
            for (int i = 0; i < bolts.n; ++i) { free(bolts.v[i].segs); }
            bolts.n = 0;
        }

        int ch = getch();
        if (ch == 'q' || ch == 'Q' || ch == 27) break;
        if (ch == 't' || ch == 'T') {
            thunder = !thunder;
            clear();
        }

        double now = now_sec();
        double dt = now - last;
        if (dt < UPDATE_INTERVAL) sleep_sec(UPDATE_INTERVAL - dt);
        last = now_sec();

        if (thunder && bolts.n < 3 && ((double)rand()/RAND_MAX) < LIGHTNING_CHANCE) {
            int start_col = (g_cols/4) + (rand() % (g_cols/2));
            int start_row = rand() % ((g_rows > 5) ? (g_rows/5) : g_rows);
            Bolt b = bolt_create(start_row, start_col, g_rows, g_cols);
            boltvec_push(&bolts, b);
        }

        int w = 0;
        for (int i = 0; i < bolts.n; ++i) {
            if (bolt_update(&bolts.v[i])) {
                bolts.v[w++] = bolts.v[i];
            } else {
                free(bolts.v[i].segs);
            }
        }
        bolts.n = w;

        double gen_chance = thunder ? 0.5 : 0.3;
        int max_new = thunder ? (g_cols/8) : (g_cols/15);
        double min_speed = 0.3;
        double max_speed = thunder ? 1.0 : 0.6;

        if (((double)rand()/RAND_MAX) < gen_chance) {
            int n_new = 1 + (max_new > 1 ? rand() % max_new : 0);
            for (int i = 0; i < n_new; ++i) {
                Raindrop d;
                d.x = rand() % (g_cols > 1 ? g_cols : 1);
                d.y = 0.0;
                d.speed = min_speed + ((double)rand()/RAND_MAX) * (max_speed - min_speed);
                char ra_chars[] = {'|', '.', '`'};
                d.ch = ra_chars[rand()%3];
                rain_push(&rain, d);
            }
        }

        int rw = 0;
        for (int i = 0; i < rain.n; ++i) {
            rain.v[i].y += rain.v[i].speed;
            if ((int)rain.v[i].y < g_rows) {
                rain.v[rw++] = rain.v[i];
            }
        }
        rain.n = rw;

        erase();

        for (int i = 0; i < bolts.n; ++i) bolt_draw(&bolts.v[i], light_attr);

        for (int i = 0; i < rain.n; ++i) {
            int y = (int)rain.v[i].y;
            if (y >= 0 && y < g_rows && rain.v[i].x >= 0 && rain.v[i].x < g_cols) {
                chtype attr = rain_attr;
                if (thunder) attr |= A_BOLD;
                else if (rain.v[i].speed < 0.8) attr |= A_DIM;
                mvaddch(y, rain.v[i].x, rain.v[i].ch | attr);
            }
        }

        doupdate();
        refresh();
    }

    for (int i = 0; i < bolts.n; ++i) free(bolts.v[i].segs);
    free(bolts.v); free(rain.v);

    endwin();
    return 0;
}

// tools/layout/main.c — run src/html.c's layout over a saved page and check it
//
// Usage:
//   layout <file.html> [--width N] [--dump] [--quiet]
//
// Exit status is 0 when every invariant below holds and 1 otherwise, so this
// drops straight into a test script. The invariants are the ones whose
// violation is visible as a rendering bug rather than as a crash:
//
//   overlap    two text runs share pixels -- the "two sentences printed on
//              top of each other" failure
//   backflow   a run sits above one emitted before it. html_paint stops
//              scanning at the first run below the viewport, so a run that
//              goes backwards in y simply never gets drawn once the page is
//              scrolled
//   margin     a run starts left of the measure. List markers hang by
//              design, so the tolerance is the indent step
//   empty      the page produced (almost) no text. Not a bug on its own --
//              some pages really are built by JavaScript -- but the browser
//              has to notice and say so rather than showing a void
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "header/html.h"
#include "header/theme.h"

#define MARGIN_TOLERANCE 40

static char* slurp(const char* path, uint32_t* len_out){
    FILE* f = fopen(path, "rb");
    if (!f){ fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n){ fprintf(stderr, "short read\n"); exit(2); }
    buf[n] = 0;
    fclose(f);
    *len_out = (uint32_t)n;
    return buf;
}

static const char* FACE[] = { "body", "bold", "ital", "h3", "h2", "h1", "mono" };

static int overlaps(const html_run_t* a, const html_run_t* b){
    // Runs are allowed to touch. Anything past that is ink on ink.
    if (a->x + a->w <= b->x || b->x + b->w <= a->x) return 0;
    if (a->y + a->h <= b->y || b->y + b->h <= a->y) return 0;
    return 1;
}

int main(int argc, char** argv){
    if (argc < 2){ fprintf(stderr, "usage: layout <file> [--width N] [--dump] [--quiet]\n"); return 2; }

    const char* path = argv[1];
    int width = 880, dump = 0, quiet = 0, plain = 0, keep_chrome = 0;
    for (int i = 2; i < argc; i++){
        if (!strcmp(argv[i], "--width") && i + 1 < argc) width = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump"))  dump = 1;
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
        else if (!strcmp(argv[i], "--plain")) plain = 1;
        else if (!strcmp(argv[i], "--keep-chrome")) keep_chrome = 1;
    }

    theme_init();               // the layout reads colours out of the palette

    uint32_t len = 0;
    char* src = slurp(path, &len);

    html_page_t* p = html_layout_ex(src, len, width, plain, keep_chrome);
    if (!p){ fprintf(stderr, "layout returned NULL\n"); return 2; }

    if (dump){
        for (uint32_t i = 0; i < p->run_n; i++){
            const html_run_t* r = &p->runs[i];
            printf("%5u  %4d,%-4d %3dx%-3d %-4s %s%.*s\n", i, r->x, r->y, r->w, r->h,
                   FACE[r->face < 7 ? r->face : 0], r->link >= 0 ? "@" : " ",
                   (int)r->len, p->text + r->off);
        }
    }

    // ------------------------------------------------------------- checks

    int fail = 0;
    uint32_t chars = 0;
    for (uint32_t i = 0; i < p->run_n; i++) chars += p->runs[i].len;

    // Overlap. Runs are ordered by y, so the inner loop can stop as soon as
    // it is past the outer run's bottom -- otherwise this is O(n^2) over
    // tens of thousands of runs on a large page.
    int overlap_n = 0;
    for (uint32_t i = 0; i < p->run_n; i++){
        for (uint32_t j = i + 1; j < p->run_n; j++){
            if (p->runs[j].y >= p->runs[i].y + p->runs[i].h) break;
            if (overlaps(&p->runs[i], &p->runs[j])){
                if (overlap_n < 8 && !quiet)
                    printf("OVERLAP  (%d,%d %dx%d) \"%.*s\"  vs  (%d,%d %dx%d) \"%.*s\"\n",
                           p->runs[i].x, p->runs[i].y, p->runs[i].w, p->runs[i].h,
                           (int)p->runs[i].len, p->text + p->runs[i].off,
                           p->runs[j].x, p->runs[j].y, p->runs[j].w, p->runs[j].h,
                           (int)p->runs[j].len, p->text + p->runs[j].off);
                overlap_n++;
            }
        }
    }

    // Runs are allowed to go backwards by up to one line box: baseline
    // alignment moves a short face down inside its own line, so a mono run
    // and the body run beside it end up a few pixels apart. Anything past
    // that is a run html_paint's early-out will drop.
    int back_n = 0;
    int32_t high = -1000000;
    for (uint32_t i = 0; i < p->run_n; i++){
        if (p->runs[i].y + HTML_LINE_SLACK < high){
            if (back_n < 4 && !quiet)
                printf("BACKFLOW y=%d after y=%d  \"%.*s\"\n", p->runs[i].y, high,
                       (int)p->runs[i].len, p->text + p->runs[i].off);
            back_n++;
        }
        if (p->runs[i].y > high) high = p->runs[i].y;
    }

    int margin_n = 0;
    for (uint32_t i = 0; i < p->run_n; i++){
        if (p->runs[i].x < -MARGIN_TOLERANCE){
            if (margin_n < 4 && !quiet)
                printf("MARGIN   x=%d  \"%.*s\"\n", p->runs[i].x,
                       (int)p->runs[i].len, p->text + p->runs[i].off);
            margin_n++;
        }
    }

    printf("%-28s runs=%-6u chars=%-7u links=%-5u height=%-6d title=\"%s\"\n",
           path, p->run_n, chars, p->link_n, p->height, p->title);
    if (overlap_n) { printf("  FAIL %d overlapping runs\n", overlap_n); fail = 1; }
    if (back_n)    { printf("  FAIL %d runs out of vertical order\n", back_n); fail = 1; }
    if (margin_n)  { printf("  FAIL %d runs left of the measure\n", margin_n); fail = 1; }
    if (chars < 40) printf("  NOTE only %u characters of text\n", chars);

    html_free(p);
    free(src);
    return fail;
}

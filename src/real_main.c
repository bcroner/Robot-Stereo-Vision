/* real_main.c -- run real camera imagery through the engine.
 *
 * Everything else in this repo is measured on rzn_scene_render(), a synthetic
 * backdrop with a moving rectangle. That is a fair test of the mechanism but
 * not of a real sensor: real imagery brings noise, imperfect rectification and
 * exposure drift, and the delta path's sparsity is exactly what noise degrades.
 *
 *   rzn_real [-t THRESHOLD] [-x X] [-y Y] [-d TRUTH.pgm] L1.ppm R1.ppm [L2.ppm R2.ppm ...]
 *
 * Each L/R pair is submitted as one frame, so frame 0 builds the I-frame and
 * every pair after it exercises the delta path against real content.
 *
 * -d scores the optional disparity stage against a ground-truth disparity map
 *    (binary PGM). Middlebury stores disparity scaled by 8; -s changes that.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rzn_fovea.h"
#include "rzn_agi_sink.h"
#ifdef RZN_ENABLE_DISPARITY
#include "rzn_disparity.h"
#endif

/* Binary PGM (P5, maxval 255), for ground-truth disparity maps. */
static bool load_pgm(const char *path, int32_t *w, int32_t *h, uint8_t **px)
{
    FILE *f = fopen(path, "rb");
    int c, vals[3], n = 0;
    size_t need;

    if (!f) return false;
    if (fgetc(f) != 'P' || fgetc(f) != '5') { fclose(f); return false; }

    while (n < 3) {
        c = fgetc(f);
        if (c == EOF) { fclose(f); return false; }
        if (c == '#') { while (c != '\n' && c != EOF) c = fgetc(f); continue; }
        if (c >= '0' && c <= '9') {
            int v = 0;
            while (c >= '0' && c <= '9') { v = v * 10 + (c - '0'); c = fgetc(f); }
            vals[n++] = v;
        }
    }
    *w = vals[0]; *h = vals[1];
    need = (size_t)*w * (size_t)*h;
    *px = (uint8_t *)malloc(need);
    if (!*px) { fclose(f); return false; }
    if (fread(*px, 1, need, f) != need) { free(*px); fclose(f); return false; }
    fclose(f);
    return true;
}

static bool sink_count(int32_t word, void *ctx)
{
    (void)word;
    (*(uint64_t *)ctx)++;
    return true;
}

int main(int argc, char **argv)
{
    int32_t threshold = 0, sx = -1, sy = -1, dscale = 8;
    int mapcols = 0;
    const char *truth = NULL;
    const char *paths[64];
    int npaths = 0, i;

    rzn_fovea f;
    rzn_stereo cur;
    uint64_t words = 0;
    int64_t total_pixels = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) threshold = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-x") && i + 1 < argc) sx = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-y") && i + 1 < argc) sy = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) dscale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) truth = argv[++i];
        else if (!strcmp(argv[i], "-map") && i + 1 < argc) mapcols = atoi(argv[++i]);
        else if (npaths < 64) paths[npaths++] = argv[i];
    }

    if (npaths < 2 || (npaths % 2) != 0) {
        printf("usage: rzn_real [-t THR] [-x X] [-y Y] [-map COLS] "
               "[-d TRUTH.pgm [-s SCALE]] "
               "L1.ppm R1.ppm [L2.ppm R2.ppm ...]\n");
        return 2;
    }

    if (!rzn_image_load_ppm(&cur.left, paths[0]) ||
        !rzn_image_load_ppm(&cur.right, paths[1])) {
        printf("error: cannot read %s / %s\n", paths[0], paths[1]);
        return 1;
    }
    if (cur.left.w != cur.right.w || cur.left.h != cur.right.h) {
        printf("error: cameras differ in resolution\n");
        return 1;
    }

    if (sx < 0) sx = cur.left.w / 2;
    if (sy < 0) sy = cur.left.h / 2;

    if (!rzn_fovea_init(&f, cur.left.w, cur.left.h, sx, sy, sink_count, &words)) {
        printf("error: engine init failed\n");
        return 1;
    }
    f.change_threshold = threshold;

    printf("real stereo imagery -- %d x %d, seed (%d, %d), profile %d (%s)\n",
           cur.left.w, cur.left.h, sx, sy, RZN_PACK_PROFILE,
           RZN_PACK_PROFILE == 32 ? "RGB888" : "RGB444");
    printf("threshold %d\n\n", threshold);
    printf("%-4s %-24s %-6s %9s %10s %10s %9s\n",
           "#", "pair", "kind", "pixels", "words", "baseline", "vs dense");
    printf("--------------------------------------------------------------"
           "-------------------\n");

    for (i = 0; i + 1 < npaths; i += 2) {
        const rzn_frame_stats *st;
        double ratio;
        char label[26];

        if (i > 0) {
            rzn_image im_l, im_r;
            if (!rzn_image_load_ppm(&im_l, paths[i]) ||
                !rzn_image_load_ppm(&im_r, paths[i + 1])) {
                printf("error: cannot read pair %d\n", i / 2);
                return 1;
            }
            if (im_l.w != cur.left.w || im_l.h != cur.left.h) {
                printf("error: pair %d differs in resolution\n", i / 2);
                return 1;
            }
            rzn_image_copy(&cur.left, &im_l);
            rzn_image_copy(&cur.right, &im_r);
            rzn_image_free(&im_l);
            rzn_image_free(&im_r);
        }

        words = 0;
        if (!rzn_fovea_submit(&f, &cur)) { printf("error: submit failed\n"); return 1; }

        st = rzn_fovea_last_stats(&f);
        ratio = st->words_emitted
              ? (double)st->baseline_words / (double)st->words_emitted : 0.0;
        total_pixels += st->pixels_emitted;

        {
            const char *base = strrchr(paths[i], '/');
            base = base ? base + 1 : paths[i];
            snprintf(label, sizeof label, "%.23s", base);
        }

        printf("%-4d %-24s %-6s %9lld %10lld %10lld %8.1fx\n",
               i / 2, label,
               st->kind == RZN_FRAME_INTRA ? "I" : "delta",
               (long long)st->pixels_emitted,
               (long long)st->words_emitted,
               (long long)st->baseline_words, ratio);
    }

    {
        const rzn_total_stats *t = rzn_fovea_totals(&f);
        printf("--------------------------------------------------------------"
               "-------------------\n");
        printf("%lld frames, %lld words vs %lld dense -- %.1fx overall, "
               "%lld pixel addresses fed\n",
               (long long)t->frames, (long long)t->words_emitted,
               (long long)t->baseline_words,
               t->words_emitted ? (double)t->baseline_words / (double)t->words_emitted : 0.0,
               (long long)total_pixels);
    }

#ifdef RZN_ENABLE_DISPARITY
    if (mapcols > 0) {
        rzn_disparity_cfg cfg;
        int32_t cw, gx, gy, gcols = mapcols, grows;

        rzn_disparity_default(&cfg);
        cw = cur.left.w / gcols;
        if (cw < 1) cw = 1;
        grows = cur.left.h / cw;

        printf("\ncoarse disparity map -- mean px per cell, '.' = no match\n");
        printf("search %d..%d px, %dx%d window, cell %dx%d px\n\n",
               cfg.min_disparity, cfg.max_disparity,
               cfg.window * 2 + 1, cfg.window * 2 + 1, cw, cw);

        for (gy = 0; gy < grows; gy++) {
            for (gx = 0; gx < gcols; gx++) {
                long sum = 0, cnt = 0;
                int32_t px, py;
                for (py = gy * cw; py < (gy + 1) * cw; py += 4)
                    for (px = gx * cw; px < (gx + 1) * cw; px += 4) {
                        int32_t d;
                        if (px < cfg.window || py < cfg.window ||
                            px >= cur.left.w - cfg.window ||
                            py >= cur.left.h - cfg.window) continue;
                        d = rzn_disparity_at(&cur, &cfg, px, py);
                        if (d == RZN_DISPARITY_NONE) continue;
                        sum += d; cnt++;
                    }
                if (cnt) printf("%3ld", sum / cnt);
                else     printf("  .");
            }
            printf("\n");
        }
    }

    if (truth) {
        int32_t tw, th, x, y;
        uint8_t *tp = 0;

        if (!load_pgm(truth, &tw, &th, &tp)) {
            printf("\nerror: cannot read ground truth %s\n", truth);
        } else if (tw != cur.left.w || th != cur.left.h) {
            printf("\nerror: ground truth is %dx%d, images are %dx%d\n",
                   tw, th, cur.left.w, cur.left.h);
            free(tp);
        } else {
            rzn_disparity_cfg cfg;
            long scored = 0, within1 = 0, within2 = 0;
            double abserr = 0.0;

            rzn_disparity_default(&cfg);

            for (y = cfg.window; y < th - cfg.window; y++)
                for (x = cfg.window; x < tw - cfg.window; x++) {
                    int32_t tv = tp[(size_t)y * tw + x];
                    double truth_px, err;
                    int32_t d;
                    if (tv == 0) continue;              /* occluded / unknown */
                    truth_px = (double)tv / (double)dscale;
                    d = rzn_disparity_at(&cur, &cfg, x, y);
                    if (d == RZN_DISPARITY_NONE) continue;
                    err = (double)d - truth_px;
                    if (err < 0) err = -err;
                    abserr += err;
                    scored++;
                    if (err <= 1.0) within1++;
                    if (err <= 2.0) within2++;
                }

            printf("\ndisparity vs ground truth (%s, scale 1/%d)\n", truth, dscale);
            if (scored) {
                printf("  scored pixels     %ld (non-occluded, inside the SAD window)\n", scored);
                printf("  mean abs error    %.2f px\n", abserr / (double)scored);
                printf("  within 1.0 px     %.1f%%\n", 100.0 * (double)within1 / (double)scored);
                printf("  within 2.0 px     %.1f%%\n", 100.0 * (double)within2 / (double)scored);
            } else {
                printf("  no scorable pixels\n");
            }
            free(tp);
        }
    }
#else
    if (truth)
        printf("\n(-d ignored: build with DISPARITY=1 to score the disparity stage)\n");
#endif

    rzn_fovea_free(&f);
    rzn_stereo_free(&cur);
    return 0;
}

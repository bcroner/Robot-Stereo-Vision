# Real camera imagery

Everything else in this repo is measured on `rzn_scene_render()` — a synthetic
backdrop with a moving rectangle. That is a fair test of the mechanism and a
poor test of a sensor. This is the engine run on real photographs.

**Source:** the [Middlebury 2001 stereo
set](https://vision.middlebury.edu/stereo/data/scenes2001/) — real camera
images of real scenes, rectified, with dense ground-truth disparity. Binary
PPM, which the engine reads natively. Six scenes at ~434×383.

```bash
DISPARITY=1 sh build.sh
./build/rzn_real -d venus/disp2.pgm -s 8 venus/im2.ppm venus/im6.ppm
```

---

## Disparity against ground truth — the strong result

7×7 SAD along the epipolar scanline, no post-processing: no left-right
consistency check, no sub-pixel refinement, no smoothing. Scored against
Middlebury's dense truth (stored scaled by 8), `im2` vs `im6`.

| scene | scored px | mean abs error | within 1 px | within 2 px |
|---|---|---|---|---|
| bull | 160 125 | **0.69** | **90.7%** | 93.5% |
| barn1 | 159 750 | 0.91 | 89.6% | 91.5% |
| sawtooth | 160 072 | 0.92 | 88.8% | 91.3% |
| barn2 | 159 000 | 1.34 | 84.1% | 86.8% |
| poster | 161 733 | 1.60 | 79.6% | 82.8% |
| venus | 161 356 | 1.95 | 79.3% | 82.7% |

Sub-pixel-to-2-pixel mean error on real photographs, from a plain block matcher
with no cleanup stage. That is a credible result and it holds across all six
scenes.

**It also corrects an over-claim.** The synthetic scene reported this stage
recovering ground truth *exactly* — 368/368 and 12/12. That was flattering
rather than impressive: the synthetic scene contains exactly two disparity
planes and blocky high-contrast texture, so a window matcher cannot really go
wrong. Real imagery has continuous depth, low-texture regions and occlusion.
79–91% within a pixel is the honest number.

---

## Sparsity — the weak result, and why

The delta path was measured at **38× on synthetic video**. Real imagery does
not reproduce that, and it is worth being precise about why rather than quoting
the friendlier figure.

Middlebury provides **viewpoints, not time**. There is no temporal sequence, so
the only motion that can be constructed is a rig translating between camera
positions — which is the *worst case* the engine has always documented ("a
full-field pan approaches dense cost"). Every pixel genuinely changes.

Five rig positions on `venus`, one full view-step apart, fixed baseline:

| change threshold | delta pixels | delta words | vs dense |
|---|---|---|---|
| 0 | 166 221 | 332 172 | 1.0× |
| 2 | 164 169 | 306 206 | 1.1× |
| 4 | 146 908 | 253 616 | 1.3× |
| 8 | 97 716 | 160 111 | 2.1× |
| 16 | 59 501 | 90 237 | 3.7× |
| 32 | 35 842 | 53 171 | 6.3× |

At threshold 0 the delta path buys **nothing** — 166 221 of 166 222 pixels
changed. A sensible threshold recovers 2–6×, but nothing close to 38×.

**What this does and does not show.** It shows the worst case honestly: under
full-field motion the engine degrades to dense cost, exactly as designed and
documented. It does *not* test the case the 38× figure came from — a static
camera watching a small moving object, which is the common household-robot
workload and the one Grab A Bot actually lives in. Middlebury cannot test that,
because it has no two frames from the same viewpoint at different times.

**The measurement still missing** is a real temporal sequence from a static
stereo rig: two or more frames of the same scene seconds apart, with genuine
sensor noise. That is the number worth quoting in licensing material, and it
has not been taken. Until it is, the defensible claim is the range — dense cost
under full-field motion, up to 38× on a static camera with a small moving
subject, scene-dependent by construction.

---

## I-frame on real imagery

| | |
|---|---|
| 434×383, seed at centre | 166 222 pixels addressed, exactly W×H |
| words | 332 495 |
| vs dense | 1.0× |

The I-frame is a dense pass by definition — it addresses every pixel once — so
1.0× is correct, not a regression. Index elision works as designed: 332 495
words for 166 222 pixels is 2.0002 words per pixel, meaning only ~50 index
words across the whole frame, at the discontinuities where the spiral leaves
and re-enters a non-square sensor.

---

## A handheld pair — what an uncontrolled rig actually does

Two handheld phone photographs of the same indoor scene, taken seconds apart,
supplied by the author. 2316x3088 downscaled to 480x640. These are not from a
rig: the camera was free in the hand, so there is translation *and* rotation
between the frames.

**I-frame:** 307 200 pixel addresses on a 480x640 sensor — exactly W x H, every
pixel addressed once. The spiral handles real imagery with no special casing.

**Delta, frame to frame:**

| change threshold | delta pixels | share of frame | vs dense |
|---|---|---|---|
| 0 | 306 284 | 99.7% | 1.0x |
| 4 | 264 843 | 86.2% | 1.1x |
| 8 | 205 955 | 67.0% | 1.4x |
| 16 | 116 179 | 37.8% | 2.4x |
| 32 | 49 174 | 16.0% | 5.9x |

Nearly the whole frame changes. The per-pixel difference distribution says why:

| max-channel difference | share |
|---|---|
| 0–3 levels (consistent with sensor noise) | **9.2%** |
| > 3 levels (genuine scene or camera motion) | **90.8%** |

So this is a *moving* camera, not a noise floor. **It still does not measure
the static-rig case**, which is the one the 38x figure came from and the one a
robot actually operates in. That measurement continues to be owed; it needs two
frames from a camera that did not move.

**Disparity on an unrectified pair: incoherent.** The coarse map
(`rzn_real -map 16`) shows no depth structure at all — the near subject does not
separate from the far hallway, and neighbouring cells disagree wildly:

```
  5 16 22 24 18 21 17 20 19 25 26 21  6  3  6  5
 10 35 37 28 23 25 20  6  8 20 34  8  4 17  2  9
  8 24 33 37 42 21  6 13  0  4  1  6  6 27  4 17
  2  4  5  0 12  8 16  1  1  0  0 10  6 20 10 12
```

This is the documented precondition failing, not a defect. The disparity stage
searches **along the scanline**, which is only valid when the two views are
rectified — coplanar sensors, no relative rotation. A handheld pair violates
that, so correspondences leave the row and the block matcher locks onto noise.

It is worth having measured. The "rigid, coplanar mount" in the design is
load-bearing: on properly rectified imagery the same code returns 0.69–1.95 px
mean error (above), and on a handheld pair it returns nothing usable. Anyone
building this rig should treat mechanical rigidity and rectification as a
correctness requirement, not a quality nicety.

---

## Reproducing

```bash
# fetch (any scene from the 2001 set)
curl -O https://vision.middlebury.edu/stereo/data/scenes2001/data/venus/im2.ppm
curl -O https://vision.middlebury.edu/stereo/data/scenes2001/data/venus/im6.ppm
curl -O https://vision.middlebury.edu/stereo/data/scenes2001/data/venus/disp2.pgm

DISPARITY=1 sh build.sh
./build/rzn_real -d disp2.pgm -s 8 im2.ppm im6.ppm          # disparity vs truth
./build/rzn_real -t 8 im0.ppm im2.ppm im1.ppm im3.ppm ...   # delta across rig motion
```

`rzn_real` takes any number of L/R pairs; the first builds the I-frame and each
one after exercises the delta path. `-d` scores disparity against a ground-truth
PGM, `-s` sets its scaling, and `-map COLS` prints a coarse mean-disparity grid
when no ground truth is available.

No imagery is committed to this repo. The Middlebury set is fetched by URL
above; the handheld pair was processed locally and only the numbers kept.

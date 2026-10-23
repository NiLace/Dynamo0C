/* GENERATED from the approved module scene's projected layout. Do not edit. */
#ifndef ZC_UI_PHOTO_LAYOUT_H
#define ZC_UI_PHOTO_LAYOUT_H

#define PH_W 406            /* panel.png */
#define PH_H 1358
#define PH_PX_MM 10
#define PH_BTN_W 69            /* one cell of buttons.png (8 cells: 4 lit, 4 dark) */
#define PH_BTN_H 69

/* knobs, in the plugin's KNOBS[] order: the cap's front centre (where the pointer turns) and its radius */
static const struct { double cx, cy, cap_r, body_r; } PH_KNOB[] = {
  { 131.03, 239.60, 33.60, 42.00 },   /* 0 port 6 */
  { 296.97, 311.93, 33.60, 42.00 },   /* 1 port 7 */
  { 131.03, 434.23, 33.60, 42.00 },   /* 2 port 9 */
  { 296.97, 506.55, 33.60, 42.00 },   /* 3 port 10 */
  { 131.03, 628.86, 33.60, 42.00 },   /* 4 port 11 */
  { 296.97, 701.18, 33.60, 42.00 },   /* 5 port 12 */
  { 131.03, 823.49, 33.60, 42.00 },   /* 6 port 13 */
  { 296.97, 895.81, 33.60, 42.00 },   /* 7 port 14 */
  { 131.03, 1018.12, 33.60, 42.00 },   /* 8 port 16 */
  { 288.97, 1090.44, 33.60, 42.00 },   /* 9 port 18 */
  { 296.97, 117.30, 33.60, 42.00 },   /* 10 port 23 */
};
/* buttons, in SWITCHES[] order: the click box, and where its crop sits in the panel */
static const struct { double x0, y0, x1, y1; int cx0, cy0; } PH_BTN[] = {
  { 65.05, 1245.04, 109.03, 1289.03, 53, 1233 },   /* 0 port 15 */
  { 153.02, 1245.04, 197.00, 1289.03, 141, 1233 },   /* 1 port 8 */
  { 230.99, 1245.04, 274.97, 1289.03, 218, 1233 },   /* 2 port 5 */
  { 318.96, 1245.04, 362.94, 1289.03, 306, 1233 },   /* 3 port 22 */
};
/* curve symbols (lit when their mode is the selected one): switch index, mode (0 bell, 1 shelf) and box */
static const struct { int sw, shelf; double x0, y0, x1, y1; } PH_ICON[] = {
  { 0, 0, 55.00, 1206.50, 87.00, 1228.50 },   /* bell */
  { 0, 1, 87.00, 1206.50, 119.00, 1228.50 },   /* shelf_lo */
  { 1, 0, 143.00, 1206.50, 175.00, 1228.50 },   /* bell */
  { 1, 1, 175.00, 1206.50, 207.00, 1228.50 },   /* shelf_hi */
};
/* the shape of each symbol, for the live highlight: kind 0 bell, 1 low shelf, 2 high shelf; centre/size in px */
static const struct { int kind; double cx, cy, w, h, lw; } PH_ICON_SHAPE[] = {
  { 0, 71.00, 1217.50, 24.00, 14.00, 3.50 },
  { 1, 103.00, 1217.50, 24.00, 14.00, 3.50 },
  { 0, 159.00, 1217.50, 24.00, 14.00, 3.50 },
  { 2, 191.00, 1217.50, 24.00, 14.00, 3.50 },
};
/* labels that light when their section is on: gain port, kind (0 band: on off 0 dB; 1 drive: on above 0), box */
static const struct { int port, kind; double x0, y0, x1, y1; } PH_LABEL[] = {
  { 7, 0, 92.72, 328.85, 169.28, 357.85 },
  { 10, 0, 73.58, 523.55, 188.42, 552.55 },
  { 12, 0, 73.58, 718.25, 188.42, 747.25 },
  { 14, 0, 102.29, 912.95, 159.71, 941.95 },
  { 23, 1, 161.00, 124.00, 225.00, 146.00 },
};
static const double PH_TITLE[4] = { 32.00, 50.00, 214.00, 88.00 };   /* click = bypass */
static const double PH_DRIVE_LABEL[4] = { 161.00, 124.00, 225.00, 146.00 };   /* click = OS card */
/* the panel's metal, inside the backdrop margin (for the bypass dim) */
static const double PH_METAL[4] = { 15.00, 15.00, 391.00, 1343.50 };

#endif

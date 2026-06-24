// Radar scope (M1) + aircraft (M2) + selection (M3) + selectable themes (M4).
// Pure LVGL, portable. Visual reference: assets/plane_radar_2.0_mockup.html
//   THEME_PHOSPHOR : green-on-black radar scope (rings, sweep, altitude glyphs)
//   THEME_ORB   : Orb scope: green gradient, square grid, the 7 nearest
//                    aircraft as yellow balls (emitting waves) + off-range arrows.
#include "radar_view.h"
#include "config.h"
#include "geo.h"
#include "coastline.h"
#include "airports.h"
#include "rare_types.h"
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <deque>
#include <algorithm>
#include <stdlib.h>
#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---- phosphor palette (mockup) ----
#define COL_GREEN  lv_color_hex(0x1DFF86)
#define COL_LEAD   lv_color_hex(0x3DFF9A)
#define COL_INK    lv_color_hex(0xEAFFF3)
#define COL_SOFT   lv_color_hex(0x9AFFC8)
#define COL_EMERG  lv_color_hex(0xFF5A3C)
// coastline outline — steel blue, deliberately off the red/amber/lime/green/cyan
// altitude-trail palette so land never reads as an aircraft track.
#define COAST_COLOR lv_color_hex(0x4E86C6)
// airport markers — a neutral muted grey-blue so they sit quietly under the traffic.
#define AIRPORT_COLOR lv_color_hex(0x8A93A6)
// ---- orb palette (Orb) ----
#define ORB_BLIP   lv_color_hex(0xFFE11A)
#define ORB_EMERG  lv_color_hex(0xFF4D2E)
#define ORB_ACCENT lv_color_hex(0xFF8A1E)
#define ORB_GRID   lv_color_hex(0x3F8B30)
#define ORB_BG_TOP lv_color_hex(0x18540F)
#define ORB_BG_BOT lv_color_hex(0x09250A)
#define ORB_FLOW   lv_color_hex(0xFFC24D)

// ---- sweep config ----
#define SWEEP_PERIOD_MS   8000
#define SWEEP_FRAME_MS    30
#define SWEEP_TRAIL_DEG   38.0f
#define SWEEP_TRAIL_STEPS 20
#define SWEEP_TRAIL_OPA   72

// ---- aircraft / flow / orb config ----
#define TRAIL_MAX         7
#define TAP_RADIUS_PX     40    // generous finger-tap catch radius (picks the nearest glyph within it)
#define FLOW_MAX          700
#define FLOW_REDRAW_EVERY 80
#define FLOW_OPA          55
#define ORB_BLIPS      7
#define ORB_ARROWS     8
#define BALL_R            9
#define WAVE_EXPAND       28.0f

static int        s_theme    = THEME_PHOSPHOR;
static void      (*s_themeCb)(int) = nullptr;
// scope "chrome" palette (rings/sweep/crosshair/labels) — retinted per theme
static lv_color_t s_cRing = COL_GREEN, s_cLead = COL_LEAD, s_cInk = COL_INK, s_cSoft = COL_SOFT;
static lv_obj_t  *s_parent   = nullptr;
static lv_obj_t  *s_gridLayer = nullptr;
static lv_obj_t  *s_sweep     = nullptr;
static lv_obj_t  *s_acLayer   = nullptr;
static lv_obj_t  *s_flowCanvas = nullptr;
static lv_color_t *s_flowBuf  = nullptr;
static lv_obj_t  *s_rose[4]   = {nullptr, nullptr, nullptr, nullptr};
static lv_obj_t  *s_centerDot = nullptr;
static lv_obj_t  *s_pulse     = nullptr;
static lv_obj_t  *s_rangeLbl  = nullptr;
static bool       s_rangeLblVisible = true;
static bool       s_sweepEnabled    = true;
static bool       s_airportsEnabled = true;
static int        s_trailMax        = TRAIL_MAX;   // per-aircraft trail length (0 = off)
static int        s_flowMax         = FLOW_MAX;    // persistent flow-layer segments, count cap (0 = off)
static int        s_flowGenMax      = 14;          // ...and an age cap in polls (~2 s each) so tracks fade out
static lv_timer_t *s_timer    = nullptr;
static float       s_sweepDeg = 0.0f;
static float       s_prevSweepDeg = 0.0f;
static float       s_wavePhase = 0.0f;
static uint32_t    s_lastUpdateMs = 0;       // smooth-motion: cadence + animation clock
static uint32_t    s_animStartMs  = 0;
static uint32_t    s_pollMs       = POLL_INTERVAL_MS;
static int         s_frameCtr     = 0;
static lv_coord_t  s_cx = SCREEN_CX, s_cy = SCREEN_CY;
static std::string s_selHex;

struct FlowSeg { lv_point_t a, b; uint16_t gen; };   // gen = the poll it was laid down on
static std::deque<FlowSeg> s_flow;
static int s_flowRedrawCtr = 0;
static uint16_t s_flowGen = 0;        // ++ each update(); flow segments fade out after s_flowGenMax polls

struct AcDraw {
    lv_point_t pos;            // current (animated) screen position — what gets drawn
    lv_point_t from, to;       // smooth-motion glide endpoints (M4 interpolation)
    float      track;
    lv_color_t color;
    bool       emergency;
    bool       military;
    bool       rare;
    bool       inRange;
    char       hex[8];
    char       call[12];
    char       type[8];
    char       category[4];
    char       altTxt[12];
    float      altFt;
    bool       onGround;
    float      vsFpm, gsKt, distKm, bearingDeg;
    int        squawk;
    std::vector<lv_point_t> trail;
};
static std::vector<AcDraw> s_acs;
static std::map<std::string, std::vector<lv_point_t>> s_trails;

static inline bool orb() { return s_theme == THEME_ORB; }

static void show(lv_obj_t *o, bool v) {
    if (!o) return;
    if (v) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else   lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static lv_color_t alt_color(float altFt, bool onGround) {
    if (onGround)      return lv_color_hex(0x888888);
    if (altFt < 3000)  return lv_color_hex(0x00FF88);  // spring green (low alt)
    if (altFt < 10000) return lv_color_hex(0xFFB23C);  // amber
    if (altFt < 20000) return lv_color_hex(0xC8FF3C);  // lime
    if (altFt < 30000) return lv_color_hex(0x39FF14);  // green
    return lv_color_hex(0x3CE0FF);                     // cyan (high alt)
}

static inline lv_point_t rim_point(float bearingDeg, float r) {
    const float a = bearingDeg * (float)M_PI / 180.0f;
    lv_point_t p;
    p.x = (lv_coord_t)lroundf((float)s_cx + r * sinf(a));
    p.y = (lv_coord_t)lroundf((float)s_cy - r * cosf(a));
    return p;
}

// rotate the local point (px,py) by `deg` (clockwise, screen coords) and offset to (ox,oy)
static inline lv_point_t rot_pt(float px, float py, float deg, lv_coord_t ox, lv_coord_t oy) {
    const float a = deg * (float)M_PI / 180.0f;
    const float c = cosf(a), s = sinf(a);
    lv_point_t p;
    p.x = (lv_coord_t)(ox + (lv_coord_t)lroundf(px * c - py * s));
    p.y = (lv_coord_t)(oy + (lv_coord_t)lroundf(px * s + py * c));
    return p;
}

// =============================== flow map ====================================
static void flow_draw_seg(const FlowSeg &s) {
    if (!s_flowCanvas) return;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = orb() ? ORB_FLOW : s_cRing;
    d.width = 2;
    d.opa = FLOW_OPA;
    lv_point_t pts[2] = { s.a, s.b };
    lv_canvas_draw_line(s_flowCanvas, pts, 2, &d);
}

static void flow_redraw_all(void) {
    if (!s_flowCanvas) return;
    lv_canvas_fill_bg(s_flowCanvas, lv_color_black(), LV_OPA_TRANSP);
    for (const FlowSeg &s : s_flow) flow_draw_seg(s);
}

// =============================== grid ========================================
static void grid_draw_cb(lv_event_t *e) {
    lv_draw_ctx_t *d = lv_event_get_draw_ctx(e);
    const lv_point_t c = { s_cx, s_cy };

    if (orb()) {
        lv_draw_line_dsc_t gl;
        lv_draw_line_dsc_init(&gl);
        gl.color = ORB_GRID;
        gl.width = 1;
        gl.opa = 120;
        const int step = 38;
        for (int x = s_cx % step; x < SCREEN_W; x += step) {
            lv_point_t p1 = { (lv_coord_t)x, 0 }, p2 = { (lv_coord_t)x, SCREEN_H - 1 };
            lv_draw_line(d, &gl, &p1, &p2);
        }
        for (int y = s_cy % step; y < SCREEN_H; y += step) {
            lv_point_t p1 = { 0, (lv_coord_t)y }, p2 = { SCREEN_W - 1, (lv_coord_t)y };
            lv_draw_line(d, &gl, &p1, &p2);
        }
        // center "you are here" triangle (orange, pointing up)
        lv_point_t tri[3] = { rot_pt(0, -11, 0, s_cx, s_cy),
                              rot_pt(10, 8, 0, s_cx, s_cy),
                              rot_pt(-10, 8, 0, s_cx, s_cy) };
        lv_draw_rect_dsc_t td;
        lv_draw_rect_dsc_init(&td);
        td.bg_color = ORB_ACCENT;
        td.bg_opa = LV_OPA_COVER;
        td.border_color = lv_color_hex(0x8A4A00);
        td.border_width = 1;
        td.border_opa = 160;
        coastline_draw(d, COAST_COLOR, 170, 2);    // landmass outline under the triangle
        if (s_airportsEnabled) airports_draw(d, AIRPORT_COLOR, 150);
        lv_draw_polygon(d, &td, tri, 3);
        return;
    }

    // coastline first, so the rings/crosshair sit cleanly on top of it.
    // Steel blue + 2 px so it reads as a map outline, distinct from the green altitude trails.
    coastline_draw(d, COAST_COLOR, 165, 2);
    if (s_airportsEnabled) airports_draw(d, AIRPORT_COLOR, 150);

    // phosphor: concentric rings + crosshair
    lv_draw_arc_dsc_t ad;
    lv_draw_arc_dsc_init(&ad);
    ad.color = s_cRing;
    ad.width = 2;
    const lv_coord_t rr[4] = { 50, 104, 160, RADAR_R_OUTER_PX };
    const lv_opa_t   ro[4] = { 66, 66, 66, 87 };
    for (int i = 0; i < 4; ++i) { ad.opa = ro[i]; lv_draw_arc(d, &ad, &c, rr[i], 0, 360); }

    lv_draw_line_dsc_t ll;
    lv_draw_line_dsc_init(&ll);
    ll.color = s_cRing;
    ll.width = 2;
    ll.opa = 41;
    lv_point_t h1 = { (lv_coord_t)(s_cx - 211), s_cy }, h2 = { (lv_coord_t)(s_cx + 211), s_cy };
    lv_point_t v1 = { s_cx, (lv_coord_t)(s_cy - 211) }, v2 = { s_cx, (lv_coord_t)(s_cy + 211) };
    lv_draw_line(d, &ll, &h1, &h2);
    lv_draw_line(d, &ll, &v1, &v2);
}

// =============================== sweep =======================================
static void sweep_draw_cb(lv_event_t *e) {
    if (orb()) return;
    lv_draw_ctx_t *dctx = lv_event_get_draw_ctx(e);
    const lv_point_t center = { s_cx, s_cy };
    const float R = (float)RADAR_R_OUTER_PX;

    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = s_cRing;
    ld.width = 5;
    ld.round_start = 1;
    ld.round_end = 1;
    for (int i = SWEEP_TRAIL_STEPS; i >= 1; --i) {
        const float frac = 1.0f - (float)i / (float)SWEEP_TRAIL_STEPS;
        const float ang  = s_sweepDeg - (float)i * (SWEEP_TRAIL_DEG / (float)SWEEP_TRAIL_STEPS);
        ld.opa = (lv_opa_t)(frac * frac * (float)SWEEP_TRAIL_OPA);
        if (ld.opa < 2) continue;
        lv_point_t p2 = rim_point(ang, R);
        lv_draw_line(dctx, &ld, &center, &p2);
    }
    lv_draw_line_dsc_t le;
    lv_draw_line_dsc_init(&le);
    le.color = s_cLead;
    le.width = 2;
    le.opa = 217;
    le.round_start = 1;
    le.round_end = 1;
    lv_point_t lead = rim_point(s_sweepDeg, R);
    lv_draw_line(dctx, &le, &center, &lead);
}

static void wedge_bbox(float deg, lv_area_t *out) {
    lv_coord_t minx = s_cx, maxx = s_cx, miny = s_cy, maxy = s_cy;
    const int steps = 10;
    for (int i = 0; i <= steps; ++i) {
        const float a = deg - SWEEP_TRAIL_DEG * (float)i / (float)steps;
        const lv_point_t p = rim_point(a, (float)RADAR_R_OUTER_PX);
        if (p.x < minx) minx = p.x;
        if (p.x > maxx) maxx = p.x;
        if (p.y < miny) miny = p.y;
        if (p.y > maxy) maxy = p.y;
    }
    const lv_coord_t pad = 6;
    out->x1 = minx - pad; out->y1 = miny - pad;
    out->x2 = maxx + pad; out->y2 = maxy + pad;
}

// glyph + label bounding box (for partial invalidation during the glide)
static inline lv_area_t glyph_bbox(lv_point_t p) {
    lv_area_t a;
    if (orb()) { a.x1 = p.x - 30; a.y1 = p.y - 30; a.x2 = p.x + 30;  a.y2 = p.y + 30; }
    else          { a.x1 = p.x - 22; a.y1 = p.y - 22; a.x2 = p.x + 148; a.y2 = p.y + 26; }
    return a;
}
static inline void area_union(lv_area_t &d, const lv_area_t &s) {
    d.x1 = LV_MIN(d.x1, s.x1); d.y1 = LV_MIN(d.y1, s.y1);
    d.x2 = LV_MAX(d.x2, s.x2); d.y2 = LV_MAX(d.y2, s.y2);
}

// Advance each glyph from its previous position toward the new target (ease-out),
// invalidating only the small region each one occupies. Self-limiting: when a plane
// barely moves (far away / slow), nx==pos and it's skipped — near-zero cost.
static void interp_step(void) {
#if MOTION_INTERP
    if (!s_acLayer || s_acs.empty()) return;
    const uint32_t now = lv_tick_get();
    float t = s_pollMs ? (float)(now - s_animStartMs) / (float)s_pollMs : 1.0f;
    if (t > 1.0f) t = 1.0f;
    const float e = t * (2.0f - t);                  // ease-out quad
    for (AcDraw &ac : s_acs) {
        const lv_coord_t nx = ac.from.x + (lv_coord_t)lroundf((float)(ac.to.x - ac.from.x) * e);
        const lv_coord_t ny = ac.from.y + (lv_coord_t)lroundf((float)(ac.to.y - ac.from.y) * e);
        if (nx == ac.pos.x && ny == ac.pos.y) continue;
        lv_point_t np; np.x = nx; np.y = ny;
        lv_area_t inv = glyph_bbox(ac.pos);
        area_union(inv, glyph_bbox(np));
        ac.pos = np;
        lv_obj_invalidate_area(s_acLayer, &inv);
    }
#endif
}

static void sweep_timer_cb(lv_timer_t *t) {
    (void)t;
    if (++s_frameCtr % 3 == 0) interp_step();         // smooth glyph motion (~90 ms cadence)
    if (orb()) {
        // animate the blip waves (invalidate only the ball areas)
        s_wavePhase += 0.05f;
        if (s_wavePhase >= 1.0f) s_wavePhase -= 1.0f;
        if (!s_acLayer) return;
        int balls = 0;
        for (const AcDraw &ac : s_acs) {
            if (!ac.inRange) continue;
            if (balls >= ORB_BLIPS) break;
            balls++;
            lv_area_t a = { (lv_coord_t)(ac.pos.x - 44), (lv_coord_t)(ac.pos.y - 44),
                            (lv_coord_t)(ac.pos.x + 44), (lv_coord_t)(ac.pos.y + 44) };
            lv_obj_invalidate_area(s_acLayer, &a);
        }
        return;
    }
    if (!s_sweepEnabled) return;          // sweep disabled: glyph interpolation above still runs
    s_prevSweepDeg = s_sweepDeg;
    s_sweepDeg += 360.0f * (float)SWEEP_FRAME_MS / (float)SWEEP_PERIOD_MS;
    if (s_sweepDeg >= 360.0f) s_sweepDeg -= 360.0f;
    if (!s_sweep) return;
    lv_area_t a, b, area;
    wedge_bbox(s_prevSweepDeg, &a);
    wedge_bbox(s_sweepDeg, &b);
    area.x1 = LV_MIN(a.x1, b.x1);
    area.y1 = LV_MIN(a.y1, b.y1);
    area.x2 = LV_MAX(a.x2, b.x2);
    area.y2 = LV_MAX(a.y2, b.y2);
    lv_obj_invalidate_area(s_sweep, &area);
}

// =============================== aircraft ====================================
static void draw_trail(lv_draw_ctx_t *d, const AcDraw &ac, lv_color_t col) {
    const int n = (int)ac.trail.size();
    if (n < 2) return;
    lv_draw_line_dsc_t t;
    lv_draw_line_dsc_init(&t);
    t.color = col;
    t.width = 2;
    for (int i = 1; i < n; ++i) {
        t.opa = (lv_opa_t)(10 + 45 * i / n);
        lv_point_t a = ac.trail[i - 1], b = ac.trail[i];
        lv_draw_line(d, &t, &a, &b);
    }
}

static void draw_ball(lv_draw_ctx_t *d, const AcDraw &ac) {
    // emitted waves: several expanding rings (sonar-ping look)
    lv_draw_arc_dsc_t w;
    lv_draw_arc_dsc_init(&w);
    w.color = ORB_ACCENT;
    w.width = 3;
    for (int wv = 0; wv < 3; ++wv) {
        float ph = s_wavePhase + (float)wv * 0.34f;
        if (ph >= 1.0f) ph -= 1.0f;
        w.opa = (lv_opa_t)((1.0f - ph) * 245.0f);
        if (w.opa > 6) lv_draw_arc(d, &w, &ac.pos, (uint16_t)(BALL_R + 3 + ph * WAVE_EXPAND), 0, 360);
    }

    // the ball
    lv_draw_rect_dsc_t b;
    lv_draw_rect_dsc_init(&b);
    b.bg_color = ac.emergency ? ORB_EMERG : ORB_BLIP;
    b.bg_opa = LV_OPA_COVER;
    b.radius = LV_RADIUS_CIRCLE;
    b.border_color = lv_color_hex(0x7A5A00);
    b.border_width = 1;
    b.border_opa = 150;
    lv_area_t r = { (lv_coord_t)(ac.pos.x - BALL_R), (lv_coord_t)(ac.pos.y - BALL_R),
                    (lv_coord_t)(ac.pos.x + BALL_R), (lv_coord_t)(ac.pos.y + BALL_R) };
    lv_draw_rect(d, &b, &r);

    // glossy highlight
    lv_draw_rect_dsc_t hl;
    lv_draw_rect_dsc_init(&hl);
    hl.bg_color = lv_color_hex(0xFFFBCC);
    hl.bg_opa = 170;
    hl.radius = LV_RADIUS_CIRCLE;
    lv_area_t hr = { (lv_coord_t)(ac.pos.x - 5), (lv_coord_t)(ac.pos.y - 6),
                     (lv_coord_t)(ac.pos.x - 1), (lv_coord_t)(ac.pos.y - 2) };
    lv_draw_rect(d, &hl, &hr);
}

static void draw_offrange(lv_draw_ctx_t *d, const AcDraw &ac) {
    // small ball at the rim
    lv_draw_rect_dsc_t b;
    lv_draw_rect_dsc_init(&b);
    b.bg_color = ac.emergency ? ORB_EMERG : ORB_BLIP;
    b.bg_opa = LV_OPA_COVER;
    b.radius = LV_RADIUS_CIRCLE;
    lv_area_t r = { (lv_coord_t)(ac.pos.x - 5), (lv_coord_t)(ac.pos.y - 5),
                    (lv_coord_t)(ac.pos.x + 5), (lv_coord_t)(ac.pos.y + 5) };
    lv_draw_rect(d, &b, &r);

    // small orange triangle just outside it, pointing toward the aircraft's bearing
    const lv_coord_t ox = (lv_coord_t)lroundf(ac.pos.x + 12.0f * sinf(ac.bearingDeg * (float)M_PI / 180.0f));
    const lv_coord_t oy = (lv_coord_t)lroundf(ac.pos.y - 12.0f * cosf(ac.bearingDeg * (float)M_PI / 180.0f));
    lv_point_t tri[3] = { rot_pt(0, -7, ac.bearingDeg, ox, oy),
                          rot_pt(5, 4, ac.bearingDeg, ox, oy),
                          rot_pt(-5, 4, ac.bearingDeg, ox, oy) };
    lv_draw_rect_dsc_t td;
    lv_draw_rect_dsc_init(&td);
    td.bg_color = ORB_ACCENT;
    td.bg_opa = LV_OPA_COVER;
    lv_draw_polygon(d, &td, tri, 3);
}

// Draw a line safely — skip if both endpoints are the same pixel (avoids LVGL 8 degenerate-line hang).
static void safe_line(lv_draw_ctx_t *d, const lv_draw_line_dsc_t *dsc,
                      const lv_point_t *p1, const lv_point_t *p2) {
    if (p1->x == p2->x && p1->y == p2->y) return;
    lv_draw_line(d, dsc, p1, p2);
}

// Draw a filled polygon safely — skip if fewer than 3 distinct points after rounding.
static void safe_poly(lv_draw_ctx_t *d, const lv_draw_rect_dsc_t *dsc,
                      const lv_point_t *pts, uint16_t n) {
    // Check that at least 3 points are distinct (catches degenerate cases).
    int distinct = 1;
    for (uint16_t i = 1; i < n && distinct < 3; ++i) {
        bool dup = false;
        for (uint16_t j = 0; j < i; ++j)
            if (pts[i].x == pts[j].x && pts[i].y == pts[j].y) { dup = true; break; }
        if (!dup) ++distinct;
    }
    if (distinct < 3) return;
    lv_draw_polygon(d, dsc, pts, n);
}

// Classify ICAO type code into a shape index.
// Returns -1 if no type-specific match (fall through to category).
static int type_to_shape(const char *t) {
    if (!t || !t[0]) return -1;
    // Helicopters by type
    static const char* heliTypes[] = {
        "EC","AS","BO","BK","R22","R44","R66","H60","UH","AH","CH","S76","S92",
        "MI","MIL","MD5","MD6","B06","B07","B21","B22","B30","B40","B47","B50",
        "H1","H2","H3","H6","NH9","EH1","W3","PZL", nullptr };
    for (int i = 0; heliTypes[i]; ++i)
        if (strncmp(t, heliTypes[i], strlen(heliTypes[i])) == 0) return 5;
    // Business jets
    static const char* bizTypes[] = {
        "GLF","C56","C52","C55","C750","LJ","BE40","CL6","F900","FA5","FA7",
        "GL5","GLE","GLEX","E50","E55","PC24","HA4","C25","EA5","P180",
        "CL30","CL35","CL60","F2TH","H25","HS25","WW24","C68A","C6","SBR",
        "ASTR","G150","G200","G280","G450","G550","G650","GALX","HDJT", nullptr };
    for (int i = 0; bizTypes[i]; ++i)
        if (strncmp(t, bizTypes[i], strlen(bizTypes[i])) == 0) return 7;
    // Turboprops
    static const char* tpTypes[] = {
        "AT4","AT7","DH8","SF3","C208","PC12","TBM","BE20","BE99","BE9",
        "PA46T","JS3","JS4","C212","DHC6","P68","BN2","GA8","SW4","SW3",
        "E110","F27","F50","AN24","AN26","DHC7","L188","C2","E120", nullptr };
    for (int i = 0; tpTypes[i]; ++i)
        if (strncmp(t, tpTypes[i], strlen(tpTypes[i])) == 0) return 2;
    // Wide-body jets
    static const char* wbTypes[] = {
        "B74","B75","B76","B77","B78","B79","A30","A31","A33","A34","A35",
        "A38","IL9","IL6","DC8","L101","B747","B767","B777","B787","A300",
        "A310","A330","A340","A350","A380","C5","AN12","IL76", nullptr };
    for (int i = 0; wbTypes[i]; ++i)
        if (strncmp(t, wbTypes[i], strlen(wbTypes[i])) == 0) return 4;
    // Narrow-body jets
    static const char* nbTypes[] = {
        "B73","B72","B71","A32","A31","A22","MD8","MD9","MD1","DC9",
        "737","727","717","321","320","319","318","E17","E19","E29","CRJ",
        "RJ1","RJ7","RJ8","RJ9","B38","B39", nullptr };
    for (int i = 0; nbTypes[i]; ++i)
        if (strncmp(t, nbTypes[i], strlen(nbTypes[i])) == 0) return 3;
    return -1;
}

static void draw_aircraft_icon(lv_draw_ctx_t *d, const AcDraw &ac) {
    const float deg = (ac.track != ac.track) ? 0.0f : ac.track;
    const lv_coord_t ox = ac.pos.x, oy = ac.pos.y;

    // Shape indices:
    // 0=generic swept-wing fallback  1=light GA (high-wing)  2=turboprop
    // 3=narrow-body jet  4=wide-body jet  5=helicopter
    // 6=military delta   7=business jet
    int shape = 0;
    lv_color_t col = ac.color;
    const char *cat = ac.category;
    const char *typ = ac.type;   // ICAO type code e.g. "B738", "C172", "GLF5"

    // Resolve shape for all aircraft (military included); military colour is forced red after.
    {
        int ts = type_to_shape(typ);
        if (ts >= 0) {
            shape = ts;
            if      (ts == 5) col = lv_color_hex(0xFFE11A);   // heli: yellow
            else if (ts == 7) col = lv_color_hex(0xD4AAFF);   // bizjet: lavender
            else if (ts == 2) col = lv_color_hex(0xC8FF3C);   // turboprop: lime
            else if (ts == 3) col = lv_color_hex(0xEAFFF3);   // narrow-body: white
            else if (ts == 4) col = lv_color_hex(0xEAFFF3);   // wide-body: white
            else              col = lv_color_hex(0x3CE0FF);   // GA: cyan
        } else if (cat[0] == 'A') {
            const int n = (cat[1] >= '1' && cat[1] <= '9') ? (cat[1]-'0') : 0;
            if      (n == 1 || n == 2) { shape = 1; col = lv_color_hex(0x3CE0FF); }
            else if (n == 3)           { shape = 2; col = lv_color_hex(0xC8FF3C); }
            else if (n == 4 || n == 5) { shape = 3; col = lv_color_hex(0xEAFFF3); }
            else if (n == 6 || n == 7) { shape = 4; col = lv_color_hex(0xEAFFF3); }
        } else if (cat[0] == 'B') {
            if   (cat[1] == '1') { shape = 5; col = lv_color_hex(0xFFE11A); }
            else                  { shape = 0; col = lv_color_hex(0xAAAAAA); }
        }
        // Military with no known type: show delta wing as the generic military fallback
        if (ac.military && ts < 0 && cat[0] != 'A' && cat[0] != 'B') shape = 6;
    }
    // Military aircraft are always red regardless of shape
    if (ac.military) col = lv_color_hex(0xFF5A3C);

    lv_draw_rect_dsc_t g;
    lv_draw_rect_dsc_init(&g);
    g.bg_color = col; g.bg_opa = LV_OPA_COVER;

    lv_draw_line_dsc_t ln;
    lv_draw_line_dsc_init(&ln);
    ln.color = col; ln.opa = LV_OPA_COVER;

    // ── HELICOPTER ──────────────────────────────────────────────────────────
    if (shape == 5) {
        ln.width = 2;
        lv_point_t r1a = rot_pt(-11, 0, deg, ox, oy), r1b = rot_pt(11, 0, deg, ox, oy);
        lv_point_t r2a = rot_pt( -4,-9, deg, ox, oy), r2b = rot_pt( 4, 9, deg, ox, oy);
        safe_line(d, &ln, &r1a, &r1b);
        safe_line(d, &ln, &r2a, &r2b);
        lv_point_t body[6] = {
            rot_pt( 0, -5, deg, ox, oy), rot_pt( 3, -2, deg, ox, oy),
            rot_pt( 3,  3, deg, ox, oy), rot_pt( 0,  6, deg, ox, oy),
            rot_pt(-3,  3, deg, ox, oy), rot_pt(-3, -2, deg, ox, oy),
        };
        safe_poly(d, &g, body, 6);
        lv_point_t tb1 = rot_pt(0, 6, deg, ox, oy), tb2 = rot_pt(0, 14, deg, ox, oy);
        safe_line(d, &ln, &tb1, &tb2);
        ln.width = 1;
        lv_point_t tr1 = rot_pt(-4, 14, deg, ox, oy), tr2 = rot_pt(4, 14, deg, ox, oy);
        safe_line(d, &ln, &tr1, &tr2);
        lv_point_t sk1a = rot_pt(-5, 3, deg, ox, oy), sk1b = rot_pt(-5, 7, deg, ox, oy);
        lv_point_t sk2a = rot_pt( 5, 3, deg, ox, oy), sk2b = rot_pt( 5, 7, deg, ox, oy);
        safe_line(d, &ln, &sk1a, &sk1b);
        safe_line(d, &ln, &sk2a, &sk2b);
        return;
    }

    // ── MILITARY DELTA + WINGTIP MISSILES ───────────────────────────────────
    if (shape == 6) {
        // Delta as two convex triangles (left + right half)
        lv_point_t rdelta[3] = {
            rot_pt(  0, -13, deg, ox, oy),
            rot_pt( 11,   5, deg, ox, oy),
            rot_pt(  0,   7, deg, ox, oy),
        };
        lv_point_t ldelta[3] = {
            rot_pt(  0, -13, deg, ox, oy),
            rot_pt(-11,   5, deg, ox, oy),
            rot_pt(  0,   7, deg, ox, oy),
        };
        safe_poly(d, &g, rdelta, 3);
        safe_poly(d, &g, ldelta, 3);
        lv_draw_rect_dsc_t cp; lv_draw_rect_dsc_init(&cp);
        cp.bg_color = lv_color_darken(col, LV_OPA_40); cp.bg_opa = LV_OPA_COVER; cp.radius = 2;
        lv_point_t cc = rot_pt(0, -5, deg, ox, oy);
        lv_area_t ca = { (lv_coord_t)(cc.x-2),(lv_coord_t)(cc.y-2),
                         (lv_coord_t)(cc.x+2),(lv_coord_t)(cc.y+2) };
        lv_draw_rect(d, &cp, &ca);
        ln.width = 2;
        lv_point_t rw1 = rot_pt(11, 3, deg, ox, oy), rw2 = rot_pt(11, 8, deg, ox, oy);
        lv_point_t lw1 = rot_pt(-11,3, deg, ox, oy), lw2 = rot_pt(-11,8, deg, ox, oy);
        safe_line(d, &ln, &rw1, &rw2);
        safe_line(d, &ln, &lw1, &lw2);
        return;
    }

    // ── BUSINESS JET ────────────────────────────────────────────────────────
    if (shape == 7) {
        lv_point_t fuse[4] = {
            rot_pt( 0,-13, deg, ox, oy), rot_pt( 1,  9, deg, ox, oy),
            rot_pt( 0, 12, deg, ox, oy), rot_pt(-1,  9, deg, ox, oy),
        };
        safe_poly(d, &g, fuse, 4);
        // Two convex wing triangles
        lv_point_t rwing[3] = {
            rot_pt(0,  2, deg, ox, oy), rot_pt(10,  8, deg, ox, oy), rot_pt(0, 6, deg, ox, oy),
        };
        lv_point_t lwing[3] = {
            rot_pt(0,  2, deg, ox, oy), rot_pt(-10, 8, deg, ox, oy), rot_pt(0, 6, deg, ox, oy),
        };
        safe_poly(d, &g, rwing, 3);
        safe_poly(d, &g, lwing, 3);
        ln.width = 2;
        lv_point_t ta = rot_pt(-3,10, deg, ox, oy), tb = rot_pt(3,10, deg, ox, oy);
        safe_line(d, &ln, &ta, &tb);
        return;
    }

    // ── LIGHT GA (HIGH-WING CESSNA) ─────────────────────────────────────────
    if (shape == 1) {
        lv_point_t fuse[4] = {
            rot_pt( 0,-10, deg, ox, oy), rot_pt( 2,  7, deg, ox, oy),
            rot_pt( 0,  9, deg, ox, oy), rot_pt(-2,  7, deg, ox, oy),
        };
        safe_poly(d, &g, fuse, 4);
        // Straight high wing: one convex rectangle
        lv_point_t wing[4] = {
            rot_pt(-11,-3, deg, ox, oy), rot_pt( 11,-3, deg, ox, oy),
            rot_pt( 11, 0, deg, ox, oy), rot_pt(-11, 0, deg, ox, oy),
        };
        safe_poly(d, &g, wing, 4);
        ln.width = 2;
        lv_point_t ta = rot_pt(-5, 7, deg, ox, oy), tb = rot_pt(5, 7, deg, ox, oy);
        lv_point_t pa = rot_pt(-5,-10, deg, ox, oy), pb = rot_pt(5,-10, deg, ox, oy);
        safe_line(d, &ln, &ta, &tb);
        safe_line(d, &ln, &pa, &pb);
        return;
    }

    // ── TURBOPROP ───────────────────────────────────────────────────────────
    if (shape == 2) {
        lv_point_t fuse[4] = {
            rot_pt( 0,-11, deg, ox, oy), rot_pt( 2,  8, deg, ox, oy),
            rot_pt( 0, 10, deg, ox, oy), rot_pt(-2,  8, deg, ox, oy),
        };
        safe_poly(d, &g, fuse, 4);
        // Wide straight wing: convex rectangle
        lv_point_t wing[4] = {
            rot_pt(-14,-2, deg, ox, oy), rot_pt( 14,-2, deg, ox, oy),
            rot_pt( 14, 2, deg, ox, oy), rot_pt(-14, 2, deg, ox, oy),
        };
        safe_poly(d, &g, wing, 4);
        ln.width = 2;
        lv_point_t ta = rot_pt(-6, 8, deg, ox, oy), tb = rot_pt(6, 8, deg, ox, oy);
        lv_point_t pa = rot_pt(-7,-11, deg, ox, oy), pb = rot_pt(7,-11, deg, ox, oy);
        safe_line(d, &ln, &ta, &tb);
        safe_line(d, &ln, &pa, &pb);
        return;
    }

    // ── NARROW-BODY JET (737 / A320) ────────────────────────────────────────
    if (shape == 3) {
        lv_point_t fuse[4] = {
            rot_pt( 0,-12, deg, ox, oy), rot_pt( 2,  9, deg, ox, oy),
            rot_pt( 0, 11, deg, ox, oy), rot_pt(-2,  9, deg, ox, oy),
        };
        safe_poly(d, &g, fuse, 4);
        // Swept wings as two triangles
        lv_point_t rwing[3] = {
            rot_pt(0, -1, deg, ox, oy), rot_pt(12,  7, deg, ox, oy), rot_pt(0, 3, deg, ox, oy),
        };
        lv_point_t lwing[3] = {
            rot_pt(0, -1, deg, ox, oy), rot_pt(-12, 7, deg, ox, oy), rot_pt(0, 3, deg, ox, oy),
        };
        safe_poly(d, &g, rwing, 3);
        safe_poly(d, &g, lwing, 3);
        ln.width = 2;
        lv_point_t ta = rot_pt(-4, 9, deg, ox, oy), tb = rot_pt(4, 9, deg, ox, oy);
        safe_line(d, &ln, &ta, &tb);
        return;
    }

    // ── WIDE-BODY JET (747 / A380) ──────────────────────────────────────────
    if (shape == 4) {
        lv_point_t fuse[4] = {
            rot_pt( 0,-13, deg, ox, oy), rot_pt( 3, 10, deg, ox, oy),
            rot_pt( 0, 13, deg, ox, oy), rot_pt(-3, 10, deg, ox, oy),
        };
        safe_poly(d, &g, fuse, 4);
        // Broad swept wings as two triangles
        lv_point_t rwing[3] = {
            rot_pt(0, -1, deg, ox, oy), rot_pt(16,  8, deg, ox, oy), rot_pt(0, 4, deg, ox, oy),
        };
        lv_point_t lwing[3] = {
            rot_pt(0, -1, deg, ox, oy), rot_pt(-16, 8, deg, ox, oy), rot_pt(0, 4, deg, ox, oy),
        };
        safe_poly(d, &g, rwing, 3);
        safe_poly(d, &g, lwing, 3);
        ln.width = 3;
        lv_point_t ta = rot_pt(-6,10, deg, ox, oy), tb = rot_pt(6,10, deg, ox, oy);
        safe_line(d, &ln, &ta, &tb);
        return;
    }

    // ── GENERIC SWEPT-WING FALLBACK (shape 0) ───────────────────────────────
    {
        lv_point_t fuse[4] = {
            rot_pt( 0,-11, deg, ox, oy), rot_pt( 2,  8, deg, ox, oy),
            rot_pt( 0, 10, deg, ox, oy), rot_pt(-2,  8, deg, ox, oy),
        };
        safe_poly(d, &g, fuse, 4);
        lv_point_t rwing[3] = {
            rot_pt(0, -1, deg, ox, oy), rot_pt(10, 6, deg, ox, oy), rot_pt(0, 3, deg, ox, oy),
        };
        lv_point_t lwing[3] = {
            rot_pt(0, -1, deg, ox, oy), rot_pt(-10,6, deg, ox, oy), rot_pt(0, 3, deg, ox, oy),
        };
        safe_poly(d, &g, rwing, 3);
        safe_poly(d, &g, lwing, 3);
        ln.width = 2;
        lv_point_t ta = rot_pt(-3, 8, deg, ox, oy), tb = rot_pt(3, 8, deg, ox, oy);
        safe_line(d, &ln, &ta, &tb);
    }
}

static void ac_draw_cb(lv_event_t *e) {
    lv_draw_ctx_t *d = lv_event_get_draw_ctx(e);
    const bool drg = orb();
    int balls = 0, arrows = 0;

    for (const AcDraw &ac : s_acs) {
        if (drg) {
            if (ac.inRange) {
                if (balls >= ORB_BLIPS) continue;   // up to 7 in-range balls
                draw_trail(d, ac, ORB_FLOW);
                draw_ball(d, ac);
                balls++;
            } else {
                if (arrows >= ORB_ARROWS) continue;  // up to 8 off-range arrows
                draw_offrange(d, ac);
                arrows++;
            }
        } else {
            if (!ac.inRange) continue;            // phosphor shows in-range traffic only
            draw_trail(d, ac, ac.color);
            // Speed vector: thin line in heading direction for military and rare only.
            // Helps spot fast movers that need the camera out quickly.
            if ((ac.military || ac.rare) && ac.gsKt == ac.gsKt && ac.gsKt > 20.0f) {
                const float deg = (ac.track != ac.track) ? 0.0f : ac.track;
                const float len = fminf(ac.gsKt * 0.10f, 40.0f);  // 0.1 px/kt, cap 40px
                lv_draw_line_dsc_t vl;
                lv_draw_line_dsc_init(&vl);
                vl.color = ac.color; vl.width = 1; vl.opa = 140;
                const float rad = deg * (float)M_PI / 180.0f;
                lv_point_t va = ac.pos;
                lv_point_t vb = {
                    (lv_coord_t)(ac.pos.x + lroundf(sinf(rad) * len)),
                    (lv_coord_t)(ac.pos.y - lroundf(cosf(rad) * len))
                };
                safe_line(d, &vl, &va, &vb);
            }
            draw_aircraft_icon(d, ac);
            if (ac.emergency) {
                lv_draw_arc_dsc_t h;
                lv_draw_arc_dsc_init(&h);
                h.color = COL_EMERG; h.width = 2; h.opa = 200;
                lv_draw_arc(d, &h, &ac.pos, 16, 0, 360);
            }
            if (ac.rare) {
                lv_draw_arc_dsc_t h;
                lv_draw_arc_dsc_init(&h);
                h.color = lv_color_white(); h.width = 2; h.opa = 200;
                lv_draw_arc(d, &h, &ac.pos, 20, 0, 360);
            }
        }

        // selection ring(s)
        if (!s_selHex.empty() && s_selHex == ac.hex) {
            lv_draw_arc_dsc_t sr;
            lv_draw_arc_dsc_init(&sr);
            sr.width = 2;
            sr.opa = 240;
            if (drg) {
                sr.color = ORB_ACCENT;
                lv_draw_arc(d, &sr, &ac.pos, 15, 0, 360);
                lv_draw_arc(d, &sr, &ac.pos, 23, 0, 360);
            } else {
                sr.color = ac.emergency ? COL_EMERG : s_cInk;
                lv_draw_arc(d, &sr, &ac.pos, 19, 0, 360);
            }
        }

        // floating labels (phosphor only; orb keeps clean balls + the tap card)
        if (!drg) {
            lv_draw_label_dsc_t lc;
            lv_draw_label_dsc_init(&lc);
            lc.font = &lv_font_montserrat_14;
            lc.color = s_cInk;
            lv_area_t a1 = { (lv_coord_t)(ac.pos.x + 12), (lv_coord_t)(ac.pos.y - 14),
                             (lv_coord_t)(ac.pos.x + 142), (lv_coord_t)(ac.pos.y + 2) };
            if (ac.call[0]) lv_draw_label(d, &lc, &a1, ac.call, NULL);
            lv_draw_label_dsc_t la;
            lv_draw_label_dsc_init(&la);
            la.font = &lv_font_montserrat_12;
            la.color = ac.color;
            lv_area_t a2 = { a1.x1, (lv_coord_t)(ac.pos.y + 2), a1.x2, (lv_coord_t)(ac.pos.y + 20) };
            if (ac.altTxt[0]) lv_draw_label(d, &la, &a2, ac.altTxt, NULL);
        }
    }
}

// =============================== helpers =====================================
static lv_obj_t *make_label(lv_obj_t *parent, const char *txt, const lv_font_t *font,
                            lv_color_t color, lv_align_t align, lv_coord_t dx, lv_coord_t dy) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_align(l, align, dx, dy);
    return l;
}

static lv_obj_t *make_layer(lv_obj_t *parent, lv_event_cb_t draw_cb) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, SCREEN_W, SCREEN_H);
    lv_obj_center(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    if (draw_cb) lv_obj_add_event_cb(o, draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    return o;
}

static void pulse_anim_cb(void *obj, int32_t v) {
    lv_obj_t *o = (lv_obj_t *)obj;
    const lv_coord_t dia = 10 + (lv_coord_t)((v * 44) / 100);
    lv_obj_set_size(o, dia, dia);
    lv_obj_center(o);
    lv_obj_set_style_border_opa(o, (lv_opa_t)(220 - v * 220 / 100), 0);
}

namespace radar {

void setTheme(int t) {
    s_theme = ((t % THEME_COUNT) + THEME_COUNT) % THEME_COUNT;
    const bool drg = orb();

    switch (s_theme) {                          // pick the scope chrome palette
        case THEME_AMBER:
            s_cRing = lv_color_hex(0xFFB23C); s_cLead = lv_color_hex(0xFFD27A);
            s_cInk  = lv_color_hex(0xFFE9C2); s_cSoft = lv_color_hex(0xFFC98A); break;
        case THEME_MILITARY:
            s_cRing = lv_color_hex(0x49C46B); s_cLead = lv_color_hex(0x76E08C);
            s_cInk  = lv_color_hex(0xE0FFE6); s_cSoft = lv_color_hex(0x9FD7A8); break;
        default:                                // phosphor (orb uses its own colors)
            s_cRing = COL_GREEN; s_cLead = COL_LEAD; s_cInk = COL_INK; s_cSoft = COL_SOFT; break;
    }

    if (s_parent) {
        if (drg) {
            lv_obj_set_style_bg_color(s_parent, ORB_BG_TOP, 0);
            lv_obj_set_style_bg_grad_color(s_parent, ORB_BG_BOT, 0);
            lv_obj_set_style_bg_grad_dir(s_parent, LV_GRAD_DIR_VER, 0);
        } else {
            lv_obj_set_style_bg_color(s_parent, lv_color_black(), 0);
            lv_obj_set_style_bg_grad_dir(s_parent, LV_GRAD_DIR_NONE, 0);
        }
        lv_obj_set_style_bg_opa(s_parent, LV_OPA_COVER, 0);
    }
    for (int i = 0; i < 4; ++i) show(s_rose[i], !drg);   // hide compass in Orb
    show(s_rangeLbl, !drg && s_rangeLblVisible);
    show(s_centerDot, !drg);                             // orb draws an orange triangle instead
    show(s_pulse, !drg);

    // retint the persistent chrome objects for the active palette
    if (s_rose[0]) lv_obj_set_style_text_color(s_rose[0], s_cInk, 0);
    for (int i = 1; i < 4; ++i) if (s_rose[i]) lv_obj_set_style_text_color(s_rose[i], s_cSoft, 0);
    if (s_centerDot) lv_obj_set_style_bg_color(s_centerDot, s_cInk, 0);
    if (s_pulse)     lv_obj_set_style_border_color(s_pulse, s_cInk, 0);
    if (s_rangeLbl)  lv_obj_set_style_text_color(s_rangeLbl, s_cRing, 0);

    flow_redraw_all();
    if (s_parent) lv_obj_invalidate(s_parent);
    if (s_themeCb) s_themeCb(s_theme);
}

int  theme() { return s_theme; }
void cycleTheme() { setTheme(s_theme + 1); }
void setThemeChangedCb(void (*cb)(int)) { s_themeCb = cb; }
void setRangeLabelVisible(bool v) { s_rangeLblVisible = v; if (s_rangeLbl) show(s_rangeLbl, v && !orb()); }

void setSweepEnabled(bool on) {
    s_sweepEnabled = on;
    if (s_sweep) {
        show(s_sweep, on);
        if (!on) lv_obj_invalidate(s_sweep);   // clear any wedge currently painted
    }
}
bool sweepEnabled() { return s_sweepEnabled; }

void setAirportsEnabled(bool on) {
    s_airportsEnabled = on;
    if (s_gridLayer) lv_obj_invalidate(s_gridLayer);   // repaint the chrome with/without markers
}
bool airportsEnabled() { return s_airportsEnabled; }

// 0 = off, 1 = short, 2 = medium (default), 3 = long. Controls both the per-aircraft
// trail and the persistent flow layer (the long-lived "where everything has been" tracks).
void setTrailLength(int level) {
    switch (level) {
        case 0: s_trailMax = 0;  s_flowMax = 0;    s_flowGenMax = 0;  break;
        case 1: s_trailMax = 3;  s_flowMax = 150;  s_flowGenMax = 8;  break;   // ~16 s
        case 3: s_trailMax = 12; s_flowMax = 1500; s_flowGenMax = 30; break;   // ~60 s
        default: s_trailMax = 7; s_flowMax = 700;  s_flowGenMax = 14; break;   // ~28 s
    }
    if (s_flowMax == 0) { s_flow.clear(); s_trails.clear(); }
    else while ((int)s_flow.size() > s_flowMax) s_flow.pop_front();
    flow_redraw_all();                              // repaint the flow canvas at the new length
    if (s_acLayer) lv_obj_invalidate(s_acLayer);
}

void init(void *lv_parent) {
    lv_obj_t *parent = (lv_obj_t *)lv_parent;
    s_parent = parent;
    s_cx = SCREEN_CX;
    s_cy = SCREEN_CY;
    s_acs.clear();
    s_trails.clear();
    s_flow.clear();
    s_selHex.clear();
    s_flowRedrawCtr = 0;

    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    if (!s_flowBuf) {
        const size_t sz = LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(SCREEN_W, SCREEN_H);
#if defined(ESP_PLATFORM)
        s_flowBuf = (lv_color_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
#else
        s_flowBuf = (lv_color_t *)malloc(sz);
#endif
    }
    s_flowCanvas = lv_canvas_create(parent);
    lv_obj_clear_flag(s_flowCanvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    if (s_flowBuf) {
        lv_canvas_set_buffer(s_flowCanvas, s_flowBuf, SCREEN_W, SCREEN_H, LV_IMG_CF_TRUE_COLOR_ALPHA);
        lv_canvas_fill_bg(s_flowCanvas, lv_color_black(), LV_OPA_TRANSP);
    }
    lv_obj_center(s_flowCanvas);

    s_gridLayer = make_layer(parent, grid_draw_cb);
    s_sweep     = make_layer(parent, sweep_draw_cb);
    s_acLayer   = make_layer(parent, ac_draw_cb);

    s_rose[0] = make_label(parent, "N", &lv_font_montserrat_28, COL_INK,  LV_ALIGN_TOP_MID,    0, 12);
    s_rose[1] = make_label(parent, "S", &lv_font_montserrat_16, COL_SOFT, LV_ALIGN_BOTTOM_MID, 0, -12);
    s_rose[2] = make_label(parent, "E", &lv_font_montserrat_16, COL_SOFT, LV_ALIGN_RIGHT_MID, -12, 0);
    s_rose[3] = make_label(parent, "W", &lv_font_montserrat_16, COL_SOFT, LV_ALIGN_LEFT_MID,   12, 0);

    char rng[16];
    snprintf(rng, sizeof(rng), "%.0f km", (double)RANGE_KM_DEFAULT);
    s_rangeLbl = make_label(parent, rng, &lv_font_montserrat_14, COL_GREEN, LV_ALIGN_CENTER, 92, -8);
    lv_obj_set_style_text_opa(s_rangeLbl, 128, 0);

    s_pulse = lv_obj_create(parent);
    lv_obj_remove_style_all(s_pulse);
    lv_obj_set_size(s_pulse, 12, 12);
    lv_obj_center(s_pulse);
    lv_obj_set_style_radius(s_pulse, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_pulse, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_pulse, COL_INK, 0);
    lv_obj_set_style_border_width(s_pulse, 2, 0);
    lv_obj_clear_flag(s_pulse, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_pulse);
    lv_anim_set_exec_cb(&a, pulse_anim_cb);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_time(&a, 2600);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    s_centerDot = lv_obj_create(parent);
    lv_obj_remove_style_all(s_centerDot);
    lv_obj_set_size(s_centerDot, 7, 7);
    lv_obj_center(s_centerDot);
    lv_obj_set_style_radius(s_centerDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_centerDot, COL_INK, 0);
    lv_obj_set_style_bg_opa(s_centerDot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_centerDot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_sweepDeg = 0.0f;
    s_prevSweepDeg = 0.0f;
    if (!s_timer) s_timer = lv_timer_create(sweep_timer_cb, SWEEP_FRAME_MS, nullptr);

    setTheme(s_theme);
}

void update(const std::vector<Aircraft> &aircraft, const RadarSettings &s) {
    std::vector<AcDraw> out;
    out.reserve(aircraft.size());
    std::set<std::string> present;
    const float R = (float)RADAR_R_OUTER_PX;
    ++s_flowGen;                                  // one tick per poll; flow segments age in these units

    // Reproject the coastline only when the scope geometry actually changes (home
    // moved or range zoomed) — never per frame. Then repaint the static chrome layer.
    static double s_coLat = 1e9, s_coLon = 1e9; static float s_coRange = -1.0f;
    if (s.homeLat != s_coLat || s.homeLon != s_coLon || s.rangeKm != s_coRange) {
        const bool firstFix = (s_coRange < 0.0f);
        s_coLat = s.homeLat; s_coLon = s.homeLon; s_coRange = s.rangeKm;
        coastline_project(s.homeLat, s.homeLon, s.rangeKm, s_cx, s_cy, R);
        airports_project(s.homeLat, s.homeLon, s.rangeKm, s_cx, s_cy, R);
        if (s_gridLayer) lv_obj_invalidate(s_gridLayer);
        if (!firstFix) {
            // Scope scale/center changed: old trails were plotted at the previous
            // projection and would be wrong now — drop them and clear the flow layer.
            s_trails.clear();
            s_flow.clear();
            flow_redraw_all();
        }
    }

    std::map<std::string, lv_point_t> prevPos;        // smooth-motion: glide starts here
    for (const AcDraw &a : s_acs) prevPos[a.hex] = a.pos;

    for (const Aircraft &ac : aircraft) {
        const double distKm = geo::haversineKm(s.homeLat, s.homeLon, ac.lat, ac.lon);
        const double brg = geo::bearingDeg(s.homeLat, s.homeLon, ac.lat, ac.lon);
        const geo::Point p = geo::projectToScreen(distKm, brg, s.rangeKm, s_cx, s_cy, R, s.rotationDeg);

        AcDraw d;
        lv_point_t target;
        target.x = (lv_coord_t)lroundf(p.x);
        target.y = (lv_coord_t)lroundf(p.y);
        d.to = target;
        {
            auto pit = prevPos.find(std::string(ac.hex.c_str()));
            if (pit != prevPos.end()) {
                const long dx = (long)target.x - pit->second.x;
                const long dy = (long)target.y - pit->second.y;
                d.from = (dx * dx + dy * dy > 120L * 120L) ? target : pit->second;  // snap if it jumped
            } else d.from = target;                                                  // new contact: appear in place
        }
#if MOTION_INTERP
        d.pos = d.from;                  // begin the glide at the previous position
#else
        d.pos = target;
        d.from = target;
#endif
        d.inRange = p.inRange;
        d.track = ac.track;
        d.color = alt_color(ac.altBaro, ac.onGround);
        d.emergency = acIsEmergency(ac.squawk);
        d.military  = ac.military;
        d.rare      = (rare_type_name(ac.type.c_str()) != nullptr);
        snprintf(d.hex,      sizeof(d.hex),      "%s", ac.hex.c_str());
        snprintf(d.call,     sizeof(d.call),     "%s", ac.flight.c_str());
        snprintf(d.type,     sizeof(d.type),     "%s", ac.type.c_str());
        snprintf(d.category, sizeof(d.category), "%s", ac.category.c_str());
        d.altFt = ac.altBaro;
        d.onGround = ac.onGround;
        d.vsFpm = ac.baroRate;
        d.gsKt = ac.gs;
        d.distKm = (float)distKm;
        d.bearingDeg = (float)brg;
        d.squawk = ac.squawk;
        if (ac.onGround) snprintf(d.altTxt, sizeof(d.altTxt), "GND");
        else             snprintf(d.altTxt, sizeof(d.altTxt), "%.0f ft", (double)ac.altBaro);

        const std::string key = ac.hex.c_str();
        present.insert(key);
        if (d.inRange) {
            std::vector<lv_point_t> &hist = s_trails[key];
            const bool moved = hist.empty() ||
                               abs((int)hist.back().x - (int)target.x) > 0 ||
                               abs((int)hist.back().y - (int)target.y) > 0;
            if (moved) {
                if (s_flowMax > 0 && !hist.empty()) {
                    FlowSeg seg = { hist.back(), target, s_flowGen };
                    s_flow.push_back(seg);
                    while ((int)s_flow.size() > s_flowMax) s_flow.pop_front();
                    flow_draw_seg(seg);
                }
                if (s_trailMax > 0) {
                    hist.push_back(target);
                    while ((int)hist.size() > s_trailMax) hist.erase(hist.begin());
                } else {
                    hist.clear();
                }
            }
            d.trail = hist;
        }
        out.push_back(std::move(d));
    }

    for (auto it = s_trails.begin(); it != s_trails.end();) {
        if (present.find(it->first) == present.end()) it = s_trails.erase(it);
        else ++it;
    }
    if (!s_selHex.empty() && present.find(s_selHex) == present.end()) s_selHex.clear();

    // Fade the flow layer by AGE, not just count: drop segments older than s_flowGenMax
    // polls so old tracks self-clear even in busy airspace (a 5 nm view doesn't stay caked
    // in green). If any were dropped, repaint the flow canvas so they actually disappear.
    if (s_flowGenMax > 0 && !s_flow.empty()) {
        bool pruned = false;
        while (!s_flow.empty() && (uint16_t)(s_flowGen - s_flow.front().gen) > (uint16_t)s_flowGenMax) {
            s_flow.pop_front();
            pruned = true;
        }
        if (pruned) flow_redraw_all();
    }

    // nearest first (the blips + the list); cap to keep work bounded
    std::sort(out.begin(), out.end(),
              [](const AcDraw &a, const AcDraw &b) { return a.distKm < b.distKm; });
    if (out.size() > 20) out.resize(20);

    if (++s_flowRedrawCtr >= FLOW_REDRAW_EVERY) {
        s_flowRedrawCtr = 0;
        flow_redraw_all();
    }

    if (s_rangeLbl) {                                 // keep the range label in sync with settings
        char r[16];
        snprintf(r, sizeof(r), "%.0f km", (double)s.rangeKm);
        lv_label_set_text(s_rangeLbl, r);
    }

    const uint32_t now = lv_tick_get();              // measure actual cadence for the glide clock
    s_pollMs = (s_lastUpdateMs && now > s_lastUpdateMs) ? (now - s_lastUpdateMs) : (uint32_t)POLL_INTERVAL_MS;
    if (s_pollMs < 400)  s_pollMs = 400;
    if (s_pollMs > 8000) s_pollMs = 8000;
    s_lastUpdateMs = now;
    s_animStartMs  = now;

    s_acs = std::move(out);
    if (s_acLayer) lv_obj_invalidate(s_acLayer);
}

int hitTest(int x, int y) {
    int best = -1;
    long bestD = (long)TAP_RADIUS_PX * TAP_RADIUS_PX;
    const bool drg = orb();
    int balls = 0, arrows = 0;
    for (size_t i = 0; i < s_acs.size(); ++i) {
        if (drg) {
            if (s_acs[i].inRange) { if (balls >= ORB_BLIPS) continue; balls++; }
            else { if (arrows >= ORB_ARROWS) continue; arrows++; }
        } else if (!s_acs[i].inRange) continue;
        const long dx = (long)s_acs[i].pos.x - x;
        const long dy = (long)s_acs[i].pos.y - y;
        const long dd = dx * dx + dy * dy;
        if (dd <= bestD) { bestD = dd; best = (int)i; }
    }
    return best;
}

static void fill_info(const AcDraw &a, AcInfo &out) {
    snprintf(out.hex, sizeof(out.hex), "%s", a.hex);
    snprintf(out.call, sizeof(out.call), "%s", a.call);
    snprintf(out.type, sizeof(out.type), "%s", a.type);
    out.altFt = a.altFt; out.onGround = a.onGround;
    out.vsFpm = a.vsFpm; out.gsKt = a.gsKt;
    out.distKm = a.distKm; out.bearingDeg = a.bearingDeg;
    out.squawk = a.squawk; out.emergency = a.emergency;
    out.military = a.military; out.rare = a.rare;
}

void select(int idx) {
    if (idx < 0 || idx >= (int)s_acs.size()) s_selHex.clear();
    else s_selHex = s_acs[idx].hex;
    if (s_acLayer) lv_obj_invalidate(s_acLayer);
}

bool selected(AcInfo &out) {
    if (s_selHex.empty()) return false;
    for (const AcDraw &a : s_acs)
        if (s_selHex == a.hex) { fill_info(a, out); return true; }
    return false;
}

int count() { return (int)s_acs.size(); }

int countInRange() {
    int n = 0;
    for (const AcDraw &a : s_acs) if (a.inRange) ++n;
    return n;
}

bool info(int idx, AcInfo &out) {
    if (idx < 0 || idx >= (int)s_acs.size()) return false;
    fill_info(s_acs[idx], out);
    return true;
}

void tickSweep() { /* sweep self-animates via lv_timer */ }

} // namespace radar

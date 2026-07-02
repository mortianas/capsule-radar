// Capsule Radar — entry point / glue. SKELETON: TODOs mark what to implement.
// Order of work is in CLAUDE.md (milestones). Bring up the Waveshare demo first.
#include <Arduino.h>
#include <WiFi.h>
#include <vector>
#include "config.h"
#include "aircraft.h"
#include "geo.h"
#include "adsb_client.h"
#include "route.h"
#include "route_client.h"
#include "photo.h"
#include "photo_client.h"
#include "radar_view.h"
#include "ui.h"
#include "display.h"                  // M0: CO5300 + LVGL bring-up
#include "imu_qmi8658.h"             // face-down sleep
#include "gps.h"                     // LC76G GNSS (-G variant only)
#include "battery.h"                 // AXP2101 battery gauge
#include "rtc_pcf85063.h"            // PCF85063 RTC (offline clock + date)
#include "audio.h"                   // ES8311 alert pings
#include "rare_types.h"              // notable aircraft watchlist
#include <set>                       // audio: track which contacts are in range
#include <string>
#include <WiFiManager.h>             // captive portal
#include <Preferences.h>            // NVS (persist theme/settings)
#include <time.h>                   // NTP/RTC clock + date
#include <WebServer.h>              // configuration web page
#include <ESPmDNS.h>                // http://capsuleradar.local
#include <ArduinoOTA.h>             // OTA firmware update over WiFi (PlatformIO/espota)
#include <Update.h>                 // browser OTA: self-flash an uploaded .bin
#include <esp_heap_caps.h>          // largest-free-block metric (heap health)

// ---- shared state ----
static std::vector<Aircraft> g_aircraft;      // latest snapshot
static SemaphoreHandle_t     g_ac_mutex;      // guards g_aircraft
static volatile bool         g_acDirty = false; // set when a new snapshot is ready
static AdsbClient            g_adsb;
static RadarSettings         g_settings;
static WiFiManager           g_wm;
static int                   g_brightnessDay = BRIGHTNESS_DEFAULT;   // user brightness (web/NVS)
static int                   g_volume = 60;                          // alert volume 0..100 (web/NVS)
static bool                  g_muted  = false;                       // mute alert pings
static int                   g_alertMode = 2;                        // 0=off 1=emergencies 2=new+emergencies 3=military (web/NVS)
static bool                  g_quietHours = false;                   // if true, mute alerts midnight–07:00 (web/NVS)
static float                 g_proximityKm = 0.0f;                   // proximity alert radius, km (0=off) (web/NVS)
static uint32_t              g_idleDimMs = IDLE_DIM_MS;              // dim after this idle time (0 = never)
static bool                  g_showSweep = true;                     // rotating sweep line on/off (web/NVS)
static bool                  g_bootPhoto = true;                     // show pilot photo on boot splash (web/NVS)
static int                   g_units = 0;                            // 0=Aviation 1=Metric 2=Imperial (web/NVS)
static bool                  g_showAirports = true;                  // airport markers on/off (web/NVS)
static int                   g_rotation = 0;                         // display rotation 0/1/2/3 = 0/90/180/270 (web/NVS)
static bool                  g_useGps = false;                       // auto-set home from the LC76G GPS (-G variant) (web/NVS)
static int                   g_trailLen = 2;                         // aircraft trails 0=off 1=short 2=med 3=long (web/NVS)
static volatile bool         g_onBattery = false;                    // discharging (set on core 1, read on core 0)
static bool                  g_rtcSynced = false;                    // RTC written from NTP this session?
static std::vector<Aircraft> g_snap;                                 // last snapshot (instant re-render on zoom)
static volatile bool         g_requery = false;                      // range changed -> adsb_task re-begins
static float                 g_requeryKm = 0.0f;
static volatile bool         g_feedOk = true;                        // ADS-B feed healthy? (HUD warning)
static int                   g_milSeen  = 0;                         // session: military contacts seen
static int                   g_rareSeen = 0;                         // session: rare contacts seen
static char                  g_rareLog[320] = "";                    // session: last 5 rare sightings

// ---- persistent sighting log (rare / military / emergency) ----
struct Sighting {
    uint32_t ts;        // unix timestamp (0 = invalid)
    char     kind;      // 'R'=rare 'M'=military 'E'=emergency
    char     name[32];  // rare name or ICAO type
    char     call[12];  // callsign
    float    alt;       // altitude ft
    float    dist;      // distance km
};
static const int MAX_SIGHTINGS = 48;
static Sighting  g_slog[MAX_SIGHTINGS];
static int       g_slogN = 0;

static void slog_save() {
    // Pack as pipe-delimited records, one per line, into a single NVS string
    static char buf[4096];
    int pos = 0;
    for (int i = 0; i < g_slogN && pos < 3900; ++i) {
        const Sighting &s = g_slog[i];
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%u|%c|%s|%s|%.0f|%.1f\n",
                        s.ts, s.kind, s.name, s.call, (double)s.alt, (double)s.dist);
    }
    buf[pos] = '\0';
    Preferences p; p.begin("capsuleradar", false);
    p.putString("slog", buf); p.end();
}

static void slog_load() {
    Preferences p; p.begin("capsuleradar", true);
    String raw = p.getString("slog", ""); p.end();
    g_slogN = 0;
    const char *line = raw.c_str();
    while (*line && g_slogN < MAX_SIGHTINGS) {
        Sighting &s = g_slog[g_slogN];
        char name[32], call[12]; float alt, dist; char kind; uint32_t ts;
        if (sscanf(line, "%u|%c|%31[^|]|%11[^|]|%f|%f", &ts, &kind, name, call, &alt, &dist) == 6) {
            s.ts = ts; s.kind = kind;
            snprintf(s.name, sizeof(s.name), "%s", name);
            snprintf(s.call, sizeof(s.call), "%s", call);
            s.alt = alt; s.dist = dist;
            g_slogN++;
        }
        const char *nl = strchr(line, '\n');
        if (!nl) break; line = nl + 1;
    }
}

static void slog_add(char kind, const char *name, const char *call, float alt, float dist) {
    const time_t now = time(nullptr);
    if (now < 1700000000L) return;   // no valid time yet, skip
    // Avoid duplicate within 60 s (same kind + callsign)
    for (int i = 0; i < g_slogN; ++i) {
        if (g_slog[i].kind == kind && strcmp(g_slog[i].call, call) == 0 &&
            (uint32_t)now - g_slog[i].ts < 60) return;
    }
    // Shift down if full
    if (g_slogN >= MAX_SIGHTINGS) {
        memmove(&g_slog[0], &g_slog[1], sizeof(Sighting) * (MAX_SIGHTINGS - 1));
        g_slogN = MAX_SIGHTINGS - 1;
    }
    Sighting &s = g_slog[g_slogN++];
    s.ts = (uint32_t)now; s.kind = kind;
    snprintf(s.name, sizeof(s.name), "%s", name ? name : "");
    snprintf(s.call, sizeof(s.call), "%s", call ? call : "");
    s.alt = alt; s.dist = dist;
    slog_save();
}
static volatile uint32_t     g_lastFeedOkMs = 0;                     // millis() of the last good poll (HUD staleness)
static volatile uint32_t     g_rebootAtMs = 0;                       // !=0: reboot when millis() reaches it (clean start after WiFi config)
static String                g_tz = TZ_STR;                          // POSIX timezone (web-configurable, NVS); applied via configTzTime

// Web-selectable time zones (label + POSIX TZ). The <option> value is the index; the save
// handler maps it back to the POSIX string stored in NVS and used by configTzTime at boot.
// (Index avoids putting POSIX strings with '<>' / ',' into HTML attributes.)
// offMin = standard (winter) UTC offset in minutes; dst = 1 if the zone observes DST.
// The web page uses these to auto-pick the visitor's zone from their browser clock.
static const struct { const char *label; const char *tz; int offMin; int dst; } TZOPTS[] = {
    {"UTC",                      "UTC0",                              0, 0},
    {"London / Lisbon",          "GMT0BST,M3.5.0/1,M10.5.0",          0, 1},
    {"Madrid / Paris / Berlin",  "CET-1CEST,M3.5.0,M10.5.0/3",       60, 1},
    {"Athens / Helsinki",        "EET-2EEST,M3.5.0/3,M10.5.0/4",     120, 1},
    {"New York (US Eastern)",    "EST5EDT,M3.2.0,M11.1.0",          -300, 1},
    {"Chicago (US Central)",     "CST6CDT,M3.2.0,M11.1.0",          -360, 1},
    {"Denver (US Mountain)",     "MST7MDT,M3.2.0,M11.1.0",          -420, 1},
    {"Phoenix (Arizona)",        "MST7",                            -420, 0},
    {"Los Angeles (US Pacific)", "PST8PDT,M3.2.0,M11.1.0",          -480, 1},
    {"Anchorage (Alaska)",       "AKST9AKDT,M3.2.0,M11.1.0",        -540, 1},
    {"Honolulu (Hawaii)",        "HST10",                           -600, 0},
    {"Argentina / Brazil (E)",   "<-03>3",                          -180, 0},
    {"India (IST)",              "<+0530>-5:30",                     330, 0},
    {"China / Singapore",        "<+08>-8",                          480, 0},
    {"Japan / Korea",            "JST-9",                            540, 0},
    {"Sydney (AU Eastern)",      "AEST-10AEDT,M10.1.0,M4.1.0/3",     600, 1},
    {"Auckland (NZ)",            "NZST-12NZDT,M9.5.0,M4.1.0/3",      720, 1},
};
static const int TZOPTS_N = sizeof(TZOPTS) / sizeof(TZOPTS[0]);

// ---- networking task (core 0): fetch + parse, never touches the display ----
static void adsb_task(void*) {
    std::vector<Aircraft> fresh;
    bool wasConnected = false;
    uint32_t lastPoll = 0;
    uint32_t lastFeedOk = millis();          // self-heal: time of last good (or no-WiFi) poll
    for (;;) {
        const bool conn = (WiFi.status() == WL_CONNECTED);
        if (conn && !wasConnected) {
            Serial.printf("[adsb] WiFi up, IP %s\n", WiFi.localIP().toString().c_str());
            configTzTime(g_tz.c_str(), "pool.ntp.org", "time.nist.gov");  // local time (web-configurable TZ)
            Serial.println("[web] config: http://capsuleradar.local/  (or the IP above)");
            // mDNS + OTA are started on core 1 (loop) to keep all mDNS use on one core
        }
        wasConnected = conn;
        // self-heal: a long feed outage while WiFi is up usually means the internal heap
        // fragmented and the TLS handshake can't allocate -> reboot to recover (settings persist).
        if (!conn) lastFeedOk = millis();
        else if (millis() - lastFeedOk > 180000UL) {
            Serial.println("[adsb] feed stuck >180s with WiFi up -> restarting to recover");
            delay(100);
            ESP.restart();
        }
        if (g_requery) {                          // display range changed (double-tap zoom)
            g_adsb.begin(g_settings.homeLat, g_settings.homeLon, g_requeryKm);
            g_requery = false;
            lastPoll = 0;                         // poll immediately at the new radius
        }
        if (conn) {
            // The live aircraft feed is the primary job, so poll FIRST every cycle. That keeps
            // it refreshing even while the user taps around — a slow route/photo lookup (below)
            // can block this single network task, so it must never get ahead of the feed.
            const uint32_t nowMs = millis();
            const uint32_t pollInterval = g_onBattery ? POLL_INTERVAL_BATTERY_MS : POLL_INTERVAL_MS;
            if (lastPoll == 0 || nowMs - lastPoll >= pollInterval) {  // aircraft feed
                lastPoll = nowMs;
                static int failCount = 0;
                // poll() flips to the alternate host on failure, so consecutive polls already
                // alternate hosts; a single transient miss is absorbed by the failCount window.
                if (g_adsb.poll(fresh)) {
                    Serial.printf("[adsb] fetched %u aircraft\n", (unsigned)fresh.size());
                    failCount = 0;
                    g_feedOk = true;
                    lastFeedOk = nowMs;
                    g_lastFeedOkMs = nowMs;          // HUD: mark data as fresh
                    if (xSemaphoreTake(g_ac_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                        g_aircraft.swap(fresh);   // O(1) handoff: no per-Aircraft String copies under the lock
                        g_acDirty = true;
                        xSemaphoreGive(g_ac_mutex);
                    }
                } else {
                    Serial.println("[adsb] poll failed");
                    if (++failCount >= 5) g_feedOk = false;   // sustained outage -> HUD warning
                }
            }
            // Then the on-demand lookups for the selected aircraft. Their timeouts are kept
            // short (see photo_client / route_client) so a slow photo server can't freeze the
            // feed for long; the next loop iteration polls again as soon as they return.
            char wantCall[12];
            if (route_pending(wantCall, sizeof(wantCall))) {
                char from[40] = "", to[40] = "";
                if (route_cache_get(wantCall, from, sizeof(from), to, sizeof(to))) {
                    route_store(wantCall, from, to);                       // NVS hit, no network
                    Serial.printf("[route] %s (cache): '%s' -> '%s'\n", wantCall, from, to);
                } else if (route_fetch(wantCall, from, sizeof(from), to, sizeof(to))) {
                    route_store(wantCall, from, to);
                    route_cache_put(wantCall, from, to);                  // remember across reboots
                    Serial.printf("[route] %s (net): '%s' -> '%s'\n", wantCall, from, to);
                } else {
                    route_store(wantCall, from, to);   // empty -> don't refetch this session
                    Serial.printf("[route] %s: no route\n", wantCall);
                }
            }
            char wantHex[10];
            if (photo_pending(wantHex, sizeof(wantHex))) photo_fetch(wantHex);
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static void loadSettings() {
    Preferences p;
    p.begin("capsuleradar", true);
    g_settings.homeLat = p.getDouble("homeLat", HOME_LAT_DEFAULT);
    g_settings.homeLon = p.getDouble("homeLon", HOME_LON_DEFAULT);
    g_settings.rangeKm = p.getFloat("rangeKm", RANGE_KM_DEFAULT);
    g_brightnessDay    = p.getInt("bright", BRIGHTNESS_DEFAULT);
    g_volume           = p.getInt("vol", 60);
    g_muted            = p.getBool("mute", false);
    g_alertMode        = p.getInt("alertmode", 2);
    g_quietHours       = p.getBool("quiet", false);
    g_proximityKm      = p.getFloat("proxkm", 0.0f);
    g_useGps           = p.getBool("usegps", false);
    g_trailLen         = p.getInt("traillen", 2);
    g_idleDimMs        = p.getUInt("idledim", IDLE_DIM_MS);
    g_units            = p.getInt("units", 0);
    g_bootPhoto        = p.getBool("bootphoto", true);
    g_tz               = p.getString("tz", TZ_STR);
    p.end();
}

// Audio alerts. g_alertMode: 0 = off, 1 = emergencies only, 2 = new aircraft + emergencies.
// g_proximityKm > 0 also pings (once) when any aircraft crosses into that radius.
static void checkAudioEvents() {
    if (!audio_present()) return;

    // Quiet hours: silence all alerts midnight–07:00 if the feature is on
    if (g_quietHours) {
        struct tm ti;
        if (getLocalTime(&ti, 0) && ti.tm_hour < 7) return;
    }

    static std::set<std::string> seen, seenProx, seenRare;
    static bool first = true;
    static uint32_t lastNew = 0;
    std::set<std::string> now, nowProx, nowRare;
    for (const Aircraft &ac : g_snap) {
        const double d = geo::haversineKm(g_settings.homeLat, g_settings.homeLon, ac.lat, ac.lon);
        if (d > g_settings.rangeKm) continue;                 // in-range only
        const std::string hex = ac.hex.c_str();
        now.insert(hex);
        const bool isNew     = !first && !seen.count(hex);
        const bool emergency = acIsEmergency(ac.squawk);  // squawk 7500/7600/7700 only
        const bool isRare    = (rare_type_name(ac.type.c_str()) != nullptr);

        // proximity: fire once, when an aircraft first crosses into the radius (any aircraft)
        if (g_alertMode > 0 && g_proximityKm > 0.0f && d <= g_proximityKm) {
            nowProx.insert(hex);
            if (!first && !seenProx.count(hex)) audio_play(AUDIO_ALERT);
        }

        // rare aircraft: triple ascending beep once per hex, also gated by alert mode
        if (g_alertMode > 0 && isRare) {
            nowRare.insert(hex);
            if (!first && !seenRare.count(hex)) {
                const char *name = rare_type_name(ac.type.c_str());
                Serial.printf("[rare] %s (%s) in range!\n", name, ac.type.c_str());
                audio_play(AUDIO_RARE);
                g_rareSeen++;
                slog_add('R', name, ac.flight.c_str(), ac.altBaro, (float)d);
                // show toast overlay on the radar screen
                ui_show_rare_notify(name, ac.flight.c_str());
                // prepend to rare log (keep last 5 lines)
                char entry[80], prev[320];
                snprintf(entry, sizeof(entry), "%s %s\n",
                         name, ac.flight.c_str()[0] ? ac.flight.c_str() : ac.type.c_str());
                snprintf(prev, sizeof(prev), "%s", g_rareLog);
                snprintf(g_rareLog, sizeof(g_rareLog), "%s%s", entry, prev);
                // truncate to 5 lines
                int lines = 0; char *p = g_rareLog;
                while (*p) { if (*p++ == '\n' && ++lines >= 5) { *p = '\0'; break; } }
                ui_set_session_stats(g_milSeen, g_rareSeen, g_rareLog);
            }
        }

        // new-in-range pings (on entry), gated by the alert mode
        if (isNew) {
            if (ac.military) {
                g_milSeen++;
                slog_add('M', ac.type.c_str(), ac.flight.c_str(), ac.altBaro, (float)d);
                ui_set_session_stats(g_milSeen, g_rareSeen, g_rareLog);
            }
            if (emergency) slog_add('E', ac.type.c_str(), ac.flight.c_str(), ac.altBaro, (float)d);
            if (g_alertMode == 3) {
                if (ac.military) audio_play(AUDIO_MILITARY);
            } else if (g_alertMode == 4) {
                if (ac.military)    audio_play(AUDIO_MILITARY);
                else if (emergency) audio_play(AUDIO_ALERT);
            } else if (emergency && g_alertMode >= 1) {
                audio_play(AUDIO_ALERT);
            } else if (g_alertMode == 2 && millis() - lastNew > 3000) {
                audio_play(AUDIO_NEW);
                lastNew = millis();
            }
        }
    }
    seen.swap(now);
    seenProx.swap(nowProx);
    seenRare.swap(nowRare);
    first = false;
}

// Double-tap zoom: change the display range, persist it, and ask adsb_task to
// re-query at a matching radius (safely, on its own core). Re-render immediately.
static void onRangeChange(float km) {
    g_settings.rangeKm = km;
    Preferences p;
    p.begin("capsuleradar", false);
    p.putFloat("rangeKm", km);
    p.end();
    g_requeryKm = constrain(km * 1.6f, 50.0f, 200.0f);
    g_requery = true;
    radar::update(g_snap, g_settings);   // instant visual zoom from the last snapshot
    ui_set_range_km(km);
    ui_on_data_updated();
}

// Persist the visual theme in NVS (called when the user long-presses to switch).
static void saveTheme(int t) {
    Preferences p;
    p.begin("capsuleradar", false);
    p.putInt("theme", t);
    p.end();
}

// Convert a UTC broken-down time to time_t (mktime assumes local TZ, so flip to UTC0).
static time_t utc_to_time(struct tm *utc) {
    setenv("TZ", "UTC0", 1); tzset();
    const time_t t = mktime(utc);
    setenv("TZ", TZ_STR, 1); tzset();   // restore local TZ for getLocalTime()
    return t;
}

// Seed the ESP system clock from the RTC so the clock/date are right before NTP.
static void rtc_seed_clock() {
    struct tm utc;
    if (!rtc_read(&utc)) { Serial.println("[rtc] no valid time stored"); return; }
    const time_t t = utc_to_time(&utc);
    struct timeval tv = { t, 0 };
    settimeofday(&tv, nullptr);
    Serial.println("[rtc] system clock seeded from RTC");
}

// Brightness combines idle auto-dim and face-down sleep (sleep wins -> screen off).
static bool g_asleep = false;   // face-down
static bool g_idle   = false;   // no touch for a while
static void applyBrightness() {
    int b = g_brightnessDay;
    if (g_idle  && BRIGHTNESS_IDLE  < b) b = BRIGHTNESS_IDLE;   // idle only dims down
    if (g_asleep) b = 0;                                         // face-down -> screen off
    display::setBrightness(b);
}

// ----------------------------- configuration web --------------------------------
static WebServer g_web(80);

static void handleRoot() {
    const int th = radar::theme();
    const int ranges[] = {10, 15, 25, 30, 50, 100, 150, 250};
    // The value submitted stays in km (the device works in km); only the label is shown in
    // the user's chosen distance unit so the config page matches the screen.
    const float    ufac  = (g_units == 0) ? 0.539957f : (g_units == 2 ? 0.621371f : 1.0f);
    const char    *uname = (g_units == 0) ? "nm" : (g_units == 2 ? "mi" : "km");
    String ropts;
    for (int r : ranges) {
        char o[72];
        snprintf(o, sizeof(o), "<option value=%d%s>%.0f %s</option>",
                 r, (r == (int)(g_settings.rangeKm + 0.5f)) ? " selected" : "", r * ufac, uname);
        ropts += o;
    }
    const char *tnames[] = {"Phosphor", "Orb", "Amber CRT", "Military"};
    String topts;
    for (int i = 0; i < 4; ++i) {
        char o[80];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == th ? " selected" : "", tnames[i]);
        topts += o;
    }
    const int idleSecs[] = {10, 20, 30, 60, 120, 300, 1800, 3600, 7200, 14400, 28800};
    const int curIdle = (int)(g_idleDimMs / 1000);
    String iopts;
    for (int sV : idleSecs) {
        char lbl[16];
        if      (sV < 60)   snprintf(lbl, sizeof(lbl), "%d s", sV);
        else if (sV < 3600) snprintf(lbl, sizeof(lbl), "%d min", sV / 60);
        else                snprintf(lbl, sizeof(lbl), "%d h", sV / 3600);
        char o[96];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", sV, sV == curIdle ? " selected" : "", lbl);
        iopts += o;
    }
    { char o[64]; snprintf(o, sizeof(o), "<option value=0%s>Never</option>", curIdle == 0 ? " selected" : ""); iopts += o; }
    const char *unames[] = {"Aviation (ft, kt, nm)", "Metric (m, km/h, km)", "Imperial (ft, mph, mi)"};
    String uopts;
    for (int i = 0; i < 3; ++i) {
        char o[96];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == g_units ? " selected" : "", unames[i]);
        uopts += o;
    }
    const char *rnames[] = {"0\xc2\xb0 (default)", "90\xc2\xb0", "180\xc2\xb0", "270\xc2\xb0"};
    String rotopts;
    for (int i = 0; i < 4; ++i) {
        char o[64];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == g_rotation ? " selected" : "", rnames[i]);
        rotopts += o;
    }
    const char *tlnames[] = {"Off", "Short", "Medium", "Long"};
    String tlopts;
    for (int i = 0; i < 4; ++i) {
        char o[64];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == g_trailLen ? " selected" : "", tlnames[i]);
        tlopts += o;
    }
    const char *anames[] = {"Off", "Emergencies only", "New aircraft + emergencies", "Military only", "Military + emergencies"};
    String aopts;
    for (int i = 0; i < 5; ++i) {
        char o[80];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == g_alertMode ? " selected" : "", anames[i]);
        aopts += o;
    }
    const int proxUnit[] = {0, 2, 5, 10, 25};   // 0 = off; rest in the user's distance unit
    String popts;
    for (int pv : proxUnit) {
        const float pkm = (pv == 0) ? 0.0f : (pv / ufac);   // user unit -> km (value submitted)
        const bool  sel = (pv == 0) ? (g_proximityKm <= 0.0f) : (fabsf(g_proximityKm - pkm) < 0.4f);
        char lbl[24];
        if (pv == 0) snprintf(lbl, sizeof(lbl), "Off");
        else         snprintf(lbl, sizeof(lbl), "%d %s", pv, uname);
        char o[80];
        snprintf(o, sizeof(o), "<option value=%.3f%s>%s</option>", pkm, sel ? " selected" : "", lbl);
        popts += o;
    }
    String tzopts;   // time-zone dropdown (value = index into TZOPTS; mapped to POSIX TZ on save)
    for (int i = 0; i < TZOPTS_N; ++i) {
        char o[128];
        snprintf(o, sizeof(o), "<option value=%d data-off=%d data-dst=%d%s>%s</option>",
                 i, TZOPTS[i].offMin, TZOPTS[i].dst, g_tz == TZOPTS[i].tz ? " selected" : "", TZOPTS[i].label);
        tzopts += o;
    }
    String gpsRow;   // only on the -G variant: offer to auto-set the centre from GPS
    if (gps_present()) {
        gpsRow  = "<label><input type=checkbox class=ck ";
        gpsRow += g_useGps ? "checked" : "";
        gpsRow += " onchange='gp(this.checked)'>Use GPS for location</label>";
        gpsRow += "<div style='font-size:12px;opacity:.6;margin:-2px 0 6px'>"
                  "When on, the location above is used until the GPS gets a fix, then it takes over.</div>";
    }
    static const size_t BUFSZ = 10240;
    static char *buf = (char *)ps_malloc(BUFSZ);   // PSRAM: keep this big page buffer off the scarce
    if (!buf) return;                              //   internal heap (the contiguous RAM mbedTLS needs)
    snprintf(buf, BUFSZ,
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Capsule Radar</title>"
        "<link rel=stylesheet href='https://unpkg.com/leaflet@1.9.4/dist/leaflet.css'>"
        "<script src='https://unpkg.com/leaflet@1.9.4/dist/leaflet.js'></script>"
        "<style>"
        "*{box-sizing:border-box}"
        "body{background:radial-gradient(circle at 50%% -10%%,#0a1f15,#04100a 70%%);color:#cdd6d1;"
        "font-family:system-ui,-apple-system,sans-serif;margin:0 auto;padding:20px;max-width:480px;min-height:100vh}"
        ".hd{display:flex;align-items:center;gap:12px;margin-bottom:16px}"
        ".dot{width:44px;height:44px;border-radius:50%%;border:2px solid #1dff86;position:relative;"
        "overflow:hidden;flex:0 0 auto;box-shadow:0 0 16px rgba(29,255,134,.4)}"
        ".dot::before{content:'';position:absolute;inset:0;animation:sw 3s linear infinite;"
        "background:conic-gradient(from 0deg,rgba(29,255,134,.65),transparent 55%%)}"
        "@keyframes sw{to{transform:rotate(360deg)}}"
        "h1{color:#1dff86;font-size:20px;margin:0}.sub{color:#6f8c7d;font-size:12px;margin:2px 0 0}"
        ".t{color:#1dff86;font-size:11px;letter-spacing:1.5px;text-transform:uppercase;margin-bottom:10px;opacity:.85}"
        "label{display:block;margin:12px 0 4px;color:#9affc8;font-size:13px}"
        "input,select{width:100%%;box-sizing:border-box;padding:10px;border-radius:8px;border:1px solid #2a4a39;"
        "background:#0c1a12;color:#eafff3;font-size:16px}"
        "input:focus,select:focus{outline:none;border-color:#1dff86;box-shadow:0 0 0 2px rgba(29,255,134,.18)}"
        "button{margin-top:16px;width:100%%;padding:12px;border:0;border-radius:8px;background:#1dff86;"
        "color:#04140b;font-weight:700;font-size:16px}button:active{opacity:.85}"
        ".w{background:#ffb23c}.card{background:rgba(10,20,14,.85);border:1px solid #1f3a2b;border-radius:14px;padding:16px;margin-bottom:14px}"
        ".ft{color:#5f7a6c;font-size:12px;text-align:center;margin-top:6px}.ft code{color:#9affc8}"
        ".ck{width:auto;display:inline;margin-right:8px;vertical-align:middle}"
        ".sec{background:#0c1a12!important;color:#1dff86!important;border:1px solid #2a4a39!important}"
        "#map{height:220px;border-radius:10px;margin:6px 0 8px;border:1px solid #2a4a39;z-index:0}"
        "</style></head><body>"
        "<div class=hd><div class=dot></div><div><h1>Capsule Radar</h1><p class=sub>Live ADS-B radar &middot; configuration</p></div></div>"
        "<div class=card><div class=t>Location &amp; range</div><form method=POST action=/save>"
        "<label>Center point &mdash; tap the map or drag the pin</label>"
        "<div id=map></div>"
        "<label>Center latitude</label><input id=lat name=lat value='%.5f'>"
        "<label>Center longitude</label><input id=lon name=lon value='%.5f'>"
        "%s"
        "<label>Display range</label><select name=range>%s</select>"
        "<label>Theme</label><select name=theme>%s</select>"
        "<label>Time zone</label><select name=tz>%s</select>"
        "<button>Save &amp; restart</button></form></div>"
        "<form method=POST action=/saveds>"
        "<div class=card><div class=t>Display</div>"
        "<label>Brightness</label>"
        "<input name=bright type=range min=5 max=255 value='%d' oninput='b(this.value,0)'>"
        "<label>Dim screen after</label><select name=idle>%s</select>"
        "<label><input name=sweep type=checkbox value=1 class=ck %s>Show radar sweep</label>"
        "<label><input name=airports type=checkbox value=1 class=ck %s>Show airports</label>"
        "<label><input name=bootphoto type=checkbox value=1 class=ck %s>Show photo on boot screen</label>"
        "<label>Aircraft trails</label><select name=trail>%s</select>"
        "<label>Screen rotation (USB-C position)</label><select name=rot>%s</select>"
        "<label>Units</label><select name=units>%s</select></div>"
        "<div class=card><div class=t>Sound</div>"
        "<label>Volume</label>"
        "<input name=vol type=range min=0 max=100 value='%d' oninput='v(this.value,0)'>"
        "<label><input name=mute type=checkbox value=1 class=ck %s>Mute alerts</label>"
        "<label><input name=quiet type=checkbox value=1 class=ck %s>Quiet hours (silent midnight&ndash;7am)</label>"
        "<label>Alert on</label><select name=alertmode>%s</select>"
        "<label>Proximity alert</label><select name=proxkm>%s</select>"
        "<button type=button class=sec onclick='t()' style='margin-bottom:8px'>Test ping</button>"
        "<button style='width:100%'>Save Display &amp; Sound</button></div></form>"
        "<div class=card><div class=t>Network</div>"
        "<p style='color:#9affc8;font-size:13px;margin:0 0 4px'>Forget the saved WiFi and reopen the setup portal.</p>"
        "<form method=POST action=/wifi><button class=w>Reset WiFi</button></form></div>"
        "<div class=card><div class=t>Logs &amp; Updates</div>"
        "<a href=/stats style='text-decoration:none'><button type=button style='width:100%;margin-bottom:8px'>Sightings Log</button></a>"
        "<a href=/update style='text-decoration:none'><button type=button class=sec style='width:100%'>Firmware Update</button></a></div>"
        "<p class=ft>Reach me at <code>capsuleradar.local</code> &middot; v" FW_VERSION "</p>"
        "<script>"
        "var C=[%.5f,%.5f];var MAP=L.map('map').setView(C,10);"
        "L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png',{maxZoom:19,attribution:'(c) OpenStreetMap'}).addTo(MAP);"
        "var MK=L.marker(C,{draggable:true}).addTo(MAP);"
        "function S(p){document.getElementById('lat').value=p.lat.toFixed(5);document.getElementById('lon').value=p.lng.toFixed(5);}"
        "MK.on('dragend',function(){S(MK.getLatLng());});"
        "MAP.on('click',function(e){MK.setLatLng(e.latlng);S(e.latlng);});"
        "setTimeout(function(){MAP.invalidateSize();},300);"
        "function b(v,s){fetch('/bright?v='+v+(s?'&save=1':''))}"
        "function v(x,s){fetch('/vol?v='+x+(s?'&save=1':''))}"
        "function m(c){fetch('/vol?mute='+(c?1:0)+'&save=1')}"
        "function t(){fetch('/vol?test=1')}"
        "function d(v){fetch('/idle?v='+v+'&save=1')}"
        "function sw(c){fetch('/sweep?v='+(c?1:0)+'&save=1')}"
        "function ap(c){fetch('/airports?v='+(c?1:0)+'&save=1')}"
        "function tl(v){fetch('/trail?v='+v+'&save=1')}"
        "function ro(v){fetch('/rotate?v='+v+'&save=1')}"
        "function u(v){fetch('/units?v='+v+'&save=1')}"
        "function al(v){fetch('/alerts?mode='+v+'&save=1')}"
        "function px(v){fetch('/alerts?prox='+v+'&save=1')}"
        "function qh(c){fetch('/alerts?quiet='+(c?1:0)+'&save=1')}"
        "function gp(c){fetch('/gps?v='+(c?1:0)+'&save=1')}"
        // auto-pick the visitor's time zone from their browser clock (only if they haven't set one)
        "var TZSET=%d;(function(){if(TZSET)return;"
        "var d=new Date(),j=new Date(d.getFullYear(),0,1).getTimezoneOffset(),"
        "u=new Date(d.getFullYear(),6,1).getTimezoneOffset(),o=-Math.max(j,u),s=(j!=u)?1:0,"
        "e=document.querySelector('select[name=tz]'),b=-1,i;"
        "for(i=0;i<e.options.length;i++){if(+e.options[i].dataset.off===o&&+e.options[i].dataset.dst===s){b=i;break;}}"
        "if(b<0)for(i=0;i<e.options.length;i++){if(+e.options[i].dataset.off===o){b=i;break;}}"
        "if(b>=0)e.selectedIndex=b;})();</script></body></html>",
        g_settings.homeLat, g_settings.homeLon, gpsRow.c_str(), ropts.c_str(), topts.c_str(),
        tzopts.c_str(),
        g_brightnessDay, iopts.c_str(), g_showSweep ? "checked" : "",
        g_showAirports ? "checked" : "", g_bootPhoto ? "checked" : "", tlopts.c_str(), rotopts.c_str(), uopts.c_str(),
        g_volume, g_muted ? "checked" : "", g_quietHours ? "checked" : "", aopts.c_str(), popts.c_str(),
        g_settings.homeLat, g_settings.homeLon, (g_tz == TZ_STR ? 0 : 1));
    g_web.send(200, "text/html", buf);
}

static void handleSave() {
    Preferences p;
    p.begin("capsuleradar", false);
    // Reject out-of-range coordinates so a typo can't leave the radar unusable.
    if (g_web.hasArg("lat")) {
        const double lat = g_web.arg("lat").toDouble();
        if (lat >= -90.0 && lat <= 90.0) p.putDouble("homeLat", lat);
    }
    if (g_web.hasArg("lon")) {
        const double lon = g_web.arg("lon").toDouble();
        if (lon >= -180.0 && lon <= 180.0) p.putDouble("homeLon", lon);
    }
    if (g_web.hasArg("range")) p.putFloat("rangeKm", g_web.arg("range").toFloat());
    if (g_web.hasArg("theme")) p.putInt("theme", g_web.arg("theme").toInt());
    if (g_web.hasArg("tz")) {
        const int i = g_web.arg("tz").toInt();
        if (i >= 0 && i < TZOPTS_N) p.putString("tz", TZOPTS[i].tz);
    }
    p.end();
    g_web.send(200, "text/html",
        "<meta http-equiv=refresh content='4;url=/'><body style='background:#06100a;color:#1dff86;"
        "font-family:sans-serif;padding:24px'>Saved. Restarting&hellip;</body>");
    delay(400);
    ESP.restart();
}

static void handleSaveDS() {   // save all display + sound settings in one POST
    Preferences p;
    p.begin("capsuleradar", false);
    if (g_web.hasArg("bright"))    { g_brightnessDay = constrain((int)g_web.arg("bright").toInt(), 0, 255); applyBrightness(); p.putInt("bright", g_brightnessDay); }
    if (g_web.hasArg("idle"))      { const long s = g_web.arg("idle").toInt(); g_idleDimMs = s <= 0 ? 0 : (uint32_t)s * 1000; p.putUInt("idledim", g_idleDimMs); }
    if (g_web.hasArg("sweep"))     { g_showSweep = g_web.arg("sweep").toInt() != 0; radar::setSweepEnabled(g_showSweep); p.putBool("sweep", g_showSweep); }
    g_bootPhoto = g_web.hasArg("bootphoto"); p.putBool("bootphoto", g_bootPhoto);
    if (g_web.hasArg("airports"))  { g_showAirports = g_web.arg("airports").toInt() != 0; radar::setAirportsEnabled(g_showAirports); p.putBool("airports", g_showAirports); }
    if (g_web.hasArg("trail"))     { g_trailLen = constrain((int)g_web.arg("trail").toInt(), 0, 3); radar::setTrailLength(g_trailLen); p.putInt("traillen", g_trailLen); }
    if (g_web.hasArg("rot"))       { g_rotation = constrain((int)g_web.arg("rot").toInt(), 0, 3); display::setRotation(g_rotation); p.putInt("rot", g_rotation); }
    if (g_web.hasArg("units"))     { g_units = constrain((int)g_web.arg("units").toInt(), 0, 2); ui_set_units(g_units); ui_set_range_km(g_settings.rangeKm); ui_on_data_updated(); p.putInt("units", g_units); }
    if (g_web.hasArg("vol"))       { g_volume = constrain((int)g_web.arg("vol").toInt(), 0, 100); audio_set_volume(g_volume); p.putInt("vol", g_volume); }
    if (g_web.hasArg("mute"))      { g_muted = g_web.arg("mute").toInt() != 0; audio_set_muted(g_muted); p.putBool("mute", g_muted); }
    if (g_web.hasArg("quiet"))     { g_quietHours = g_web.arg("quiet").toInt() != 0; p.putBool("quiet", g_quietHours); }
    if (g_web.hasArg("alertmode")) { g_alertMode = constrain((int)g_web.arg("alertmode").toInt(), 0, 4); p.putInt("alertmode", g_alertMode); }
    if (g_web.hasArg("proxkm"))    { g_proximityKm = g_web.arg("proxkm").toFloat(); p.putFloat("proxkm", g_proximityKm); }
    p.end();
    g_web.send(200, "text/html",
        "<meta http-equiv=refresh content='3;url=/'><body style='background:#06100a;color:#1dff86;"
        "font-family:sans-serif;padding:24px'>Display &amp; Sound saved.</body>");
}

static void handleWifi() {
    g_web.send(200, "text/html",
        "<body style='background:#06100a;color:#ffb23c;font-family:sans-serif;padding:24px'>"
        "WiFi reset. Connect to the <b>CapsuleRadar-Setup</b> network to reconfigure.</body>");
    delay(400);
    g_wm.resetSettings();
    ESP.restart();
}

static void handleBright() {
    if (g_web.hasArg("v")) {
        g_brightnessDay = constrain((int)g_web.arg("v").toInt(), 0, 255);
        applyBrightness();
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("bright", g_brightnessDay);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleVol() {
    if (g_web.hasArg("v"))    { g_volume = constrain((int)g_web.arg("v").toInt(), 0, 100); audio_set_volume(g_volume); }
    if (g_web.hasArg("mute")) { g_muted = g_web.arg("mute").toInt() != 0; audio_set_muted(g_muted); }
    if (g_web.hasArg("save")) {
        Preferences p;
        p.begin("capsuleradar", false);
        p.putInt("vol", g_volume);
        p.putBool("mute", g_muted);
        p.end();
    }
    if (g_web.hasArg("test")) {
        if (g_web.arg("test").toInt() == 2) audio_selftest();   // long tone, ignores mute
        else audio_play(AUDIO_NEW);
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleAlerts() {   // what triggers the alert sound (live)
    if (g_web.hasArg("mode"))  g_alertMode   = constrain((int)g_web.arg("mode").toInt(), 0, 4);
    if (g_web.hasArg("prox"))  g_proximityKm = g_web.arg("prox").toFloat();   // km (0 = off)
    if (g_web.hasArg("quiet")) g_quietHours  = g_web.arg("quiet").toInt() != 0;
    if (g_web.hasArg("save")) {
        Preferences p;
        p.begin("capsuleradar", false);
        p.putInt("alertmode", g_alertMode);
        p.putFloat("proxkm", g_proximityKm);
        p.putBool("quiet", g_quietHours);
        p.end();
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleIdle() {   // idle auto-dim timeout (seconds; 0 = never)
    if (g_web.hasArg("v")) {
        const long s = g_web.arg("v").toInt();
        g_idleDimMs = (s <= 0) ? 0 : (uint32_t)s * 1000;
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putUInt("idledim", g_idleDimMs);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleUnits() {   // measurement units preset (live re-render)
    if (g_web.hasArg("v")) {
        g_units = constrain((int)g_web.arg("v").toInt(), 0, 2);
        ui_set_units(g_units);
        ui_set_range_km(g_settings.rangeKm);   // refresh the zoom-button label
        ui_on_data_updated();                  // re-render card/list/stats in the new units
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("units", g_units);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleSweep() {   // show/hide the rotating sweep line (live)
    if (g_web.hasArg("v")) {
        g_showSweep = g_web.arg("v").toInt() != 0;
        radar::setSweepEnabled(g_showSweep);          // loop()/core 1: safe to touch LVGL
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putBool("sweep", g_showSweep);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleTrail() {   // aircraft trail length 0/1/2/3 (live)
    if (g_web.hasArg("v")) {
        g_trailLen = constrain((int)g_web.arg("v").toInt(), 0, 3);
        radar::setTrailLength(g_trailLen);
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("traillen", g_trailLen);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleAirports() {   // show/hide airport markers (live)
    if (g_web.hasArg("v")) {
        g_showAirports = g_web.arg("v").toInt() != 0;
        radar::setAirportsEnabled(g_showAirports);
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putBool("airports", g_showAirports);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleRotate() {   // display rotation 0/90/180/270 for any USB-C orientation (live)
    if (g_web.hasArg("v")) {
        g_rotation = constrain((int)g_web.arg("v").toInt(), 0, 3);
        display::setRotation((uint8_t)g_rotation);
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("rot", g_rotation);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleGps() {   // auto-set the centre point from the LC76G GPS (-G variant)
    if (g_web.hasArg("v")) {
        g_useGps = g_web.arg("v").toInt() != 0;
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putBool("usegps", g_useGps);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

// ---- browser OTA: upload an app .bin over WiFi and self-flash ----
static void handleStats() {
    const time_t now = time(nullptr);
    const uint32_t h24 = 86400, d7 = 604800;
    int r24=0, m24=0, e24=0, r7=0, m7=0, e7=0;
    for (int i = 0; i < g_slogN; ++i) {
        const uint32_t age = (uint32_t)now - g_slog[i].ts;
        if (g_slog[i].kind=='R') { if(age<=h24) r24++; if(age<=d7) r7++; }
        if (g_slog[i].kind=='M') { if(age<=h24) m24++; if(age<=d7) m7++; }
        if (g_slog[i].kind=='E') { if(age<=h24) e24++; if(age<=d7) e7++; }
    }
    // Build event rows (newest first)
    static char *ebuf = (char*)ps_malloc(8192);
    if (!ebuf) { g_web.send(500,"text/plain","oom"); return; }
    int ep = 0;
    for (int i = g_slogN - 1; i >= 0 && ep < 7800; --i) {
        const Sighting &s = g_slog[i];
        struct tm ti; const time_t st = s.ts; localtime_r(&st, &ti);
        char tstr[20]; strftime(tstr, sizeof(tstr), "%d %b %H:%M", &ti);
        const char *badge, *bcol;
        if      (s.kind=='R') { badge="RARE";      bcol="#FFFFFF"; }
        else if (s.kind=='M') { badge="MILITARY";  bcol="#FF5A3C"; }
        else                  { badge="EMERGENCY"; bcol="#FF5A3C"; }
        char altS[16];
        if (s.alt <= 0) snprintf(altS, sizeof(altS), "GND");
        else            snprintf(altS, sizeof(altS), "%.0f ft", (double)s.alt);
        ep += snprintf(ebuf+ep, 8192-ep,
            "<div class=ev>"
            "<span class=badge style='border-color:%s;color:%s'>%s</span>"
            "<span class=et>%s</span>"
            "<span class=en>%s</span>"
            "<span class=ec>%s</span>"
            "<span class=ea>%s &nbsp; %.1f %s</span>"
            "</div>",
            bcol, bcol, badge, tstr,
            s.name[0] ? s.name : "-",
            s.call[0] ? s.call : "-",
            altS, (double)s.dist * 0.539957f, "nm");
    }
    if (ep == 0) snprintf(ebuf, 8192, "<p style='color:#5f7a6c;text-align:center;margin:32px 0'>No sightings logged yet</p>");

    static char *page = (char*)ps_malloc(12288);
    if (!page) { g_web.send(500,"text/plain","oom"); return; }
    snprintf(page, 12288,
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Capsule Radar &mdash; Sightings</title><style>"
        "*{box-sizing:border-box}"
        "body{background:radial-gradient(circle at 50%% -10%%,#0a1f15,#04100a 70%%);"
        "color:#cdd6d1;font-family:system-ui,sans-serif;margin:0 auto;padding:20px;max-width:520px;min-height:100vh}"
        ".hd{display:flex;align-items:center;gap:12px;margin-bottom:16px}"
        ".dot{width:44px;height:44px;border-radius:50%%;border:2px solid #1dff86;position:relative;"
        "overflow:hidden;flex:0 0 auto;box-shadow:0 0 16px rgba(29,255,134,.4)}"
        ".dot::before{content:'';position:absolute;inset:0;animation:sw 3s linear infinite;"
        "background:conic-gradient(from 0deg,rgba(29,255,134,.65),transparent 55%%)}"
        "@keyframes sw{to{transform:rotate(360deg)}}"
        "h1{color:#1dff86;font-size:20px;margin:0}.sub{color:#6f8c7d;font-size:12px;margin:2px 0 0}"
        ".card{background:rgba(10,20,14,.85);border:1px solid #1f3a2b;border-radius:14px;padding:16px;margin-bottom:14px}"
        ".t{color:#1dff86;font-size:11px;letter-spacing:1.5px;text-transform:uppercase;margin-bottom:12px;opacity:.85}"
        ".grid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px;text-align:center}"
        ".stat{background:#0c1a12;border-radius:10px;padding:12px 6px}"
        ".sv{font-size:28px;font-weight:700;color:#1dff86;line-height:1}"
        ".sv.red{color:#FF5A3C}.sv.wh{color:#fff}"
        ".sl{font-size:11px;color:#5f7a6c;margin-top:4px}"
        ".sh{font-size:12px;color:#9affc8;margin-bottom:6px;opacity:.7}"
        ".ev{display:flex;flex-wrap:wrap;align-items:center;gap:6px;padding:10px 0;"
        "border-bottom:1px solid #1a2e20;font-size:13px}"
        ".badge{border:1px solid;border-radius:4px;padding:1px 6px;font-size:10px;"
        "letter-spacing:1px;font-weight:700;flex:0 0 auto}"
        ".et{color:#5f7a6c;flex:0 0 auto;font-size:12px}"
        ".en{color:#eafff3;font-weight:600;flex:1 1 120px}"
        ".ec{color:#9affc8;flex:0 0 auto}"
        ".ea{color:#5f7a6c;font-size:12px;flex:0 0 auto;margin-left:auto}"
        "a{color:#1dff86;text-decoration:none}"
        "</style></head><body>"
        "<div class=hd><div class=dot></div><div>"
        "<h1>Capsule Radar</h1><p class=sub>Sightings log</p></div></div>"
        "<div class=card><div class=t>Last 24 hours</div>"
        "<div class=grid>"
        "<div class=stat><div class='sv wh'>%d</div><div class=sl>Rare</div></div>"
        "<div class=stat><div class='sv red'>%d</div><div class=sl>Military</div></div>"
        "<div class=stat><div class='sv red'>%d</div><div class=sl>Emergency</div></div>"
        "</div></div>"
        "<div class=card><div class=t>Last 7 days</div>"
        "<div class=grid>"
        "<div class=stat><div class='sv wh'>%d</div><div class=sl>Rare</div></div>"
        "<div class=stat><div class='sv red'>%d</div><div class=sl>Military</div></div>"
        "<div class=stat><div class='sv red'>%d</div><div class=sl>Emergency</div></div>"
        "</div></div>"
        "<div class=card><div class=t>All sightings</div>"
        "<div class=sh>newest first &mdash; %d logged</div>"
        "%s"
        "</div>"
        "<p style='text-align:center;margin-top:10px'><a href=/>&#8592; Settings</a></p>"
        "</body></html>",
        r24, m24, e24, r7, m7, e7, g_slogN, ebuf);
    g_web.send(200, "text/html", page);
}

static void handleUpdatePage() {
    g_web.send(200, "text/html",
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Capsule Radar - Update</title><style>"
        "body{background:radial-gradient(circle at 50% -10%,#0a1f15,#04100a 70%);color:#cdd6d1;"
        "font-family:system-ui,sans-serif;margin:0 auto;padding:20px;max-width:480px;min-height:100vh}"
        "h1{color:#1dff86;font-size:20px}.card{background:rgba(10,20,14,.85);border:1px solid #1f3a2b;border-radius:14px;padding:16px}"
        "input,button{width:100%;box-sizing:border-box;padding:11px;border-radius:8px;margin-top:8px;font-size:16px}"
        "input{background:#0c1a12;color:#eafff3;border:1px solid #2a4a39}"
        "button{border:0;background:#1dff86;color:#04140b;font-weight:700}"
        "#bar{height:12px;background:#0c1a12;border-radius:6px;overflow:hidden;margin-top:14px;display:none}"
        "#fill{height:100%;width:0;background:#1dff86;transition:width .2s}#msg{margin-top:10px;color:#9affc8;font-size:13px}"
        "a{color:#1dff86}p{color:#9affc8;font-size:13px}"
        "</style></head><body><h1>Firmware update (OTA)</h1><div class=card>"
        "<p>Upload the <b>app firmware</b> <code>CapsuleRadar-ota.bin</code> from the GitHub release. "
        "Do NOT use the merged flash image here.</p>"
        "<input type=file id=f accept='.bin'>"
        "<button onclick=u()>Update over WiFi</button>"
        "<div id=bar><div id=fill></div></div><div id=msg></div></div>"
        "<p style='text-align:center;margin-top:14px'><a href=/>&larr; Back to settings</a></p>"
        "<script>function u(){var f=document.getElementById('f').files[0];if(!f){return}"
        "var x=new XMLHttpRequest(),fd=new FormData();fd.append('f',f);"
        "document.getElementById('bar').style.display='block';"
        "x.upload.onprogress=function(e){if(e.lengthComputable)document.getElementById('fill').style.width=(e.loaded/e.total*100)+'%'};"
        "x.onload=function(){document.getElementById('msg').innerText=x.responseText+' - rebooting...'};"
        "x.onerror=function(){document.getElementById('msg').innerText='Upload failed'};"
        "x.open('POST','/update');x.send(fd);}</script></body></html>");
}

static void handleUpdateUpload() {
    HTTPUpload &up = g_web.upload();
    if (up.status == UPLOAD_FILE_START) {
        Serial.printf("[update] start: %s\n", up.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_END) {
        if (Update.end(true)) Serial.printf("[update] done: %u bytes\n", (unsigned)up.totalSize);
        else Update.printError(Serial);
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\nCapsule Radar boot");

    if (PIN_LCD_SCLK < 0 || PIN_I2C_SDA < 0) {
        Serial.println("[!] Pins in config.h are still -1. Copy them from the Waveshare demo.");
    }
    Serial.printf("PSRAM: %u bytes free\n", (unsigned)ESP.getFreePsram());

    loadSettings();
    route_cache_begin();   // clear stale route cache if the label format changed
    slog_load();           // restore persistent sighting log from NVS

    // --- Display + LVGL (M0) ----------------------------------------------
    // CO5300 AMOLED over QSPI + LVGL draw buffers in PSRAM, then a hello screen.
    // The panel is powered from the always-on DC1 rail, so it lights without the
    // PMIC. Touch (CST9217 indev) + AXP2101 come in later milestones.
    ui_set_boot_photo(g_bootPhoto);   // must be before display::begin() → ui_create()
    if (!display::begin()) {
        Serial.println("[!] display::begin() failed — check QSPI pins / power.");
    }

    // restore the saved theme, then persist any future change
    {
        Preferences p;
        p.begin("capsuleradar", true);
        const int t = p.getInt("theme", THEME_PHOSPHOR);
        g_showSweep = p.getBool("sweep", true);
        g_showAirports = p.getBool("airports", true);
        g_rotation = p.getInt("rot", 0);
        p.end();
        radar::setTheme(t);
        radar::setSweepEnabled(g_showSweep);
        radar::setAirportsEnabled(g_showAirports);
        radar::setTrailLength(g_trailLen);
        display::setRotation((uint8_t)g_rotation);
    }
    radar::setThemeChangedCb(saveTheme);
    ui_set_range_cb(onRangeChange);              // on-screen zoom button
    ui_set_units(g_units);                       // apply saved unit preset
    ui_set_range_km(g_settings.rangeKm);         // show the loaded range

    imu_begin();       // face-down sleep (no-op if the IMU isn't detected)
    battery_begin();   // AXP2101 (no-op if not detected / no battery)
    gps_begin();       // LC76G GNSS (no-op if not the -G variant)
    battery_enable_codec_rail();   // power the ES8311 analog rail before audio init

    setenv("TZ", TZ_STR, 1); tzset();   // local time for display even before NTP
    rtc_begin();
    rtc_seed_clock();                   // offline clock/date from the PCF85063
    if (audio_begin()) {                // ES8311 alert pings (no-op if codec absent)
        audio_set_volume(g_volume);
        audio_set_muted(g_muted);
    }

    // --- Radar UI ----------------------------------------------------------
    // radar::init() runs inside display::begin() (LVGL must be up first).

    // --- WiFi (captive portal, non-blocking) ------------------------------
    // First boot opens the "CapsuleRadar-Setup" AP to enter WiFi creds. Non-blocking
    // so the radar keeps animating while you configure WiFi from your phone.
    g_wm.setConfigPortalBlocking(false);
    g_wm.setTitle("Capsule Radar");
    // light phosphor-green theme for the captive portal (small CSS, injected into <head>)
    g_wm.setCustomHeadElement(
        "<style>"
        "body{background:#06100a;color:#cdd6d1;font-family:system-ui,sans-serif}"
        "h1,h2,h3{color:#1dff86}"
        "button,input[type=submit],.btn{background:#1dff86!important;color:#04140b!important;"
        "border:0!important;border-radius:8px!important;font-weight:700}"
        "input,select{background:#0c1a12!important;color:#eafff3!important;"
        "border:1px solid #2a4a39!important;border-radius:8px!important}"
        "a{color:#1dff86}.q{filter:hue-rotate(90deg)}"
        "</style>");
    // After the portal saves new credentials, reboot for a clean start: WiFiManager's
    // own port-80 server (and mDNS) don't cleanly hand over to our web server / STA
    // interface in non-blocking mode, so the config page is flaky until a fresh boot.
    g_wm.setSaveConfigCallback([]() {
        Serial.println("[wifi] new credentials saved -> rebooting for a clean web/mDNS start");
        g_rebootAtMs = millis() + 2500;   // let the portal deliver its 'saved' page first
    });
    if (g_wm.autoConnect("CapsuleRadar-Setup"))
        Serial.println("[wifi] connected");
    else
        Serial.println("[wifi] config portal open - join 'CapsuleRadar-Setup' to set WiFi; UI stays live");

    // --- OTA ---------------------------------------------------------------
    // ArduinoOTA is started from loop() once WiFi connects (see otaUp there).

    // --- ADS-B client + task ----------------------------------------------
    float queryKm = g_settings.rangeKm * 1.6f;          // query wider than the display range
    if (queryKm < 50.0f)  queryKm = 50.0f;
    if (queryKm > 200.0f) queryKm = 200.0f;
    g_adsb.begin(g_settings.homeLat, g_settings.homeLon, queryKm);
    g_ac_mutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(adsb_task, "adsb", 16384, nullptr, 1, nullptr, 0);  // TLS needs a big stack

    // configuration web page (http://capsuleradar.local/)
    g_web.on("/", handleRoot);
    g_web.on("/save", HTTP_POST, handleSave);
    g_web.on("/saveds", HTTP_POST, handleSaveDS);
    g_web.on("/wifi", HTTP_POST, handleWifi);
    g_web.on("/bright", handleBright);
    g_web.on("/vol", handleVol);
    g_web.on("/alerts", handleAlerts);
    g_web.on("/idle", handleIdle);
    g_web.on("/sweep", handleSweep);
    g_web.on("/airports", handleAirports);
    g_web.on("/trail", handleTrail);
    g_web.on("/rotate", handleRotate);
    g_web.on("/gps", handleGps);
    g_web.on("/units", handleUnits);
    g_web.on("/stats", handleStats);
    g_web.on("/update", HTTP_GET, handleUpdatePage);
    g_web.on("/update", HTTP_POST,
        []() {
            const bool ok = !Update.hasError();
            g_web.send(200, "text/plain", ok ? "OK" : "FAIL");
            delay(800);
            if (ok) ESP.restart();
        },
        handleUpdateUpload);
    g_web.begin();

    Serial.println("setup done");
}

void loop() {
    display::loop();                // drive LVGL (render dirty areas + run timers)
    g_wm.process();                 // service the WiFi config portal (non-blocking)
    g_web.handleClient();           // serve the configuration web page
    if (g_useGps) gps_poll();       // pull NMEA from the LC76G (only when GPS auto-location is on)

    // scheduled reboot after a fresh WiFi config (see setSaveConfigCallback)
    if (g_rebootAtMs && (int32_t)(millis() - g_rebootAtMs) >= 0) { delay(50); ESP.restart(); }

    // OTA: set up once WiFi is up, then service it every loop (flash over the air)
    static bool otaUp = false;
    if (!otaUp && WiFi.status() == WL_CONNECTED) {
        ArduinoOTA.setHostname("capsuleradar");        // -> capsuleradar.local (registers mDNS)
        ArduinoOTA.begin();
        MDNS.addService("http", "tcp", 80);            // advertise the config web page
        otaUp = true;
        Serial.println("[ota] ready: pio run -e esp32-s3-amoled-175-ota -t upload");
    }
    if (otaUp) ArduinoOTA.handle();

    // Push a fresh ADS-B snapshot to the radar (copy under the mutex, render outside).
    if (g_acDirty) {
        if (xSemaphoreTake(g_ac_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            g_snap.swap(g_aircraft);   // O(1) handoff under the lock; render on g_snap outside it.
            g_acDirty = false;         // g_aircraft now holds the previous snapshot (overwritten next poll)
            xSemaphoreGive(g_ac_mutex);
            radar::update(g_snap, g_settings); // rebuild the glyph/trail layer
            ui_on_data_updated();              // refresh card/list/stats
            checkAudioEvents();                // ping new-in-range / emergency / military
        }
    }

    // periodic: HUD clock + wifi/battery indicators
    static uint32_t lastStatus = 0;
    if (millis() - lastStatus > 5000) {
        lastStatus = millis();
#if DEBUG_MEM
        static uint32_t lastFrames = 0;
        const uint32_t fr = display_frames();
        const unsigned fps = (fr - lastFrames) / 5;
        lastFrames = fr;
        Serial.printf("[mem] heap %u (min %u, biggest %u) | psram %u free | up %lus | aircraft %d | fps %u\n",
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      (unsigned)ESP.getFreePsram(), (unsigned long)(millis() / 1000),
                      (int)g_snap.size(), fps);
#endif
        char clk[8] = "--:--";
        struct tm ti;
        const bool haveTime = getLocalTime(&ti, 0);
        if (haveTime) {
            snprintf(clk, sizeof(clk), "%02d:%02d", ti.tm_hour, ti.tm_min);
            char date[20];
            strftime(date, sizeof(date), "%d %b %Y", &ti);   // e.g. "08 Jun 2026"
            ui_set_date(date);
        }
        const bool wifiUp = (WiFi.status() == WL_CONNECTED);
        const int  rssi   = wifiUp ? (int)WiFi.RSSI() : -127;
        // "fresh" = we got aircraft data recently. Catches a stalled feed (weak WiFi dropping
        // polls intermittently) that never trips the consecutive-fail counter -> aircraft
        // freeze but the icon would otherwise stay white.
        const bool feedFresh = wifiUp && (millis() - g_lastFeedOkMs < 18000UL);
        ui_set_status(wifiUp, feedFresh, rssi, clk);
        char net[80];
        if (WiFi.status() == WL_CONNECTED)
            snprintf(net, sizeof(net), "Configure at\ncapsuleradar.local\n%s", WiFi.localIP().toString().c_str());
        else
            snprintf(net, sizeof(net), "WiFi setup:\njoin CapsuleRadar-Setup");
        ui_set_netinfo(net);
        const bool bpresent = battery_present();
        ui_set_battery(battery_percent(), battery_charging(), bpresent);
        g_onBattery = bpresent && !battery_charging();
        // GPS HUD/Stats: 0 = off/no module (hidden), 1 = acquiring, 2 = fix
        const int gpsState = (!g_useGps || !gps_present()) ? 0 : (gps_has_fix() ? 2 : 1);
        ui_set_gps(gpsState, gps_satellites());
        // once NTP has a real fix, persist it to the RTC (core 1 only)
        if (!g_rtcSynced && time(nullptr) > 1700000000L) {
            time_t now = time(nullptr);
            struct tm utc;
            gmtime_r(&now, &utc);
            if (rtc_write(&utc)) { g_rtcSynced = true; Serial.println("[rtc] saved NTP time"); }
        }
        // GPS auto-location (-G variant): re-centre the radar when the fix moves enough.
        if (g_useGps) {
            double glat, glon;
            if (gps_location(&glat, &glon) &&
                geo::haversineKm(g_settings.homeLat, g_settings.homeLon, glat, glon) > 1.0) {
                g_settings.homeLat = glat; g_settings.homeLon = glon;   // radar/coastline recenter
                // re-query the new area — set the radius too (same formula as boot/zoom), or
                // adsb_task would re-begin with a stale/zero g_requeryKm and fetch 0 aircraft.
                g_requeryKm = constrain(g_settings.rangeKm * 1.6f, 50.0f, 200.0f);
                g_requery = true;                                       // adsb_task re-queries the new area
                Serial.printf("[gps] re-centred to %.4f, %.4f\n", glat, glon);
            }
        }
    }

    // face-down -> screen off (IMU); flip face-up to wake
    static uint32_t lastImu = 0;
    static int fdCount = 0;
    if (millis() - lastImu > 400) {
        lastImu = millis();
        const int fd = imu_facedown();              // 1 down, 0 not, -1 read error
        if (fd > 0)       { if (fdCount < 8) fdCount++; }
        else if (fd == 0) fdCount = 0;              // -1 (I2C hiccup): leave the counter as-is
        const bool sleep = (fdCount >= 4);   // ~1.6 s face-down
        const bool idle  = g_idleDimMs > 0 && display::inactiveMs() > g_idleDimMs;
        if (sleep != g_asleep || idle != g_idle) {
            g_asleep = sleep;
            g_idle = idle;
            applyBrightness();
        }
    }

    delay(5);
}

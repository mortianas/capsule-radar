// Fetch nearby aircraft from airplanes.live (fallback adsb.lol) and parse the
// readsb JSON into a vector<Aircraft>.
//
// Memory safety (important on the ESP32): we parse straight from the HTTP stream
// (no full-body String), use an ArduinoJson field filter so only the ~12 fields we
// need are kept, and hard-cap the number of aircraft (ADSB_MAX_AIRCRAFT). The radar
// then keeps only the nearest ~20 for display.
#include "adsb_client.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>   // v7
#include <esp_heap_caps.h>

// Parse the JSON in PSRAM, not internal RAM. Otherwise the per-poll JSON alloc/free
// churn fragments the internal heap and, after a while, mbedTLS can't find a large
// enough contiguous block for the TLS handshake (-32512), freezing the feed.
struct PsramJsonAllocator : ArduinoJson::Allocator {
    void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void  deallocate(void* p) override { heap_caps_free(p); }
    void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
static PsramJsonAllocator s_jsonPsram;

void AdsbClient::begin(double homeLat, double homeLon, float rangeKm) {
    _lat = homeLat; _lon = homeLon; _rangeKm = rangeKm;
}

bool AdsbClient::poll(std::vector<Aircraft>& out) {
    if (WiFi.status() != WL_CONNECTED) return false;
    // Prefer the primary host, and give it a quick second try before touching the fallback:
    // the primary is reliable in practice, while the fallback can be slow to time out from
    // some networks (turning one transient primary blip into a long no-data gap + amber HUD).
    if (fetchFrom(ADSB_PRIMARY_HOST, out)) return true;
    if (fetchFrom(ADSB_PRIMARY_HOST, out)) return true;   // transient blip -> retry the healthy host
    return fetchFrom(ADSB_FALLBACK_HOST, out);            // last resort
}

bool AdsbClient::fetchFrom(const char* host, std::vector<Aircraft>& out) {
    const double nm = _rangeKm * 0.539957;            // km -> nautical miles (API radius unit)
    char url[160];

#if USE_RELAY
    // Fetch from the LOCAL relay over PLAIN HTTP. The relay (a Pi/server) does the TLS to
    // the cloud and re-serves plain HTTP, so the board never touches mbedTLS -> the whole
    // TLS-handshake-contiguous-RAM wedge (the reboot cause) simply cannot happen. The relay
    // auto-scales to the radius in the path, so behaviour is otherwise identical.
    (void)host;
    snprintf(url, sizeof(url), "http://%s:%d/v2/point/%.4f/%.4f/%.0f",
             RELAY_HOST, RELAY_PORT, _lat, _lon, nm);
    static WiFiClient client;   // plain TCP — no encryption, no 16 KB mbedTLS buffers
#else
    snprintf(url, sizeof(url), "https://%s/v2/point/%.4f/%.4f/%.0f", host, _lat, _lon, nm);
    // ONE shared TLS client for BOTH hosts. A fresh mbedTLS handshake needs a ~16 KB
    // *contiguous* block of internal RAM; keeping two clients live left too little for a
    // third fresh handshake -> every poll failed until the self-heal reboot. One client +
    // stop() on host switch keeps at most one 16 KB buffer live. (Still only a mitigation;
    // USE_RELAY above removes the problem entirely.)
    static WiFiClientSecure client;
    static const char* lastHost = nullptr;
    static bool initDone = false;
    if (!initDone) {
#if ADSB_HTTPS_INSECURE
        client.setInsecure();
#endif
        initDone = true;
    }
    if (lastHost == nullptr || strcmp(lastHost, host) != 0) {
        client.stop();          // switching host: free the old TLS context before the new handshake
        lastHost = host;
    }
#endif

    HTTPClient http;
    http.setReuse(true);             // keep the TCP+TLS connection alive between polls
    http.setConnectTimeout(6000);    // fail reasonably fast: a slow host must not block the
    http.setTimeout(8000);           // task (and the user's route/photo lookups) for too long
    if (!http.begin(client, url)) { Serial.printf("[adsb] begin failed (%s)\n", host); client.stop(); return false; }
    http.addHeader("User-Agent", ADSB_USER_AGENT);
    http.addHeader("Accept", "application/json");

    const int code = http.GET();
    if (code != 200) {
        // Fully tear down the client on any failure so the next poll opens a fresh
        // TCP+TLS connection instead of reusing a possibly-wedged one.
        Serial.printf("[adsb] HTTP %d (%s)\n", code, host);
        http.end();
        client.stop();
        return false;
    }

    // Only keep the fields we use -> much smaller parsed document.
    JsonDocument filter(&s_jsonPsram);
    const char* keys[] = { "ac", "aircraft" };
    const char* flds[] = { "hex", "flight", "t", "lat", "lon", "alt_baro",
                           "track", "true_heading", "gs", "baro_rate",
                           "squawk", "seen_pos", "dbFlags", "category" };
    for (const char* k : keys)
        for (const char* f : flds)
            filter[k][0][f] = true;

    JsonDocument doc(&s_jsonPsram);
    DeserializationError err = deserializeJson(doc, http.getStream(),
                                               DeserializationOption::Filter(filter));
    http.end();
    if (err) return false;

    JsonArrayConst arr = doc["ac"].as<JsonArrayConst>();
    if (arr.isNull()) arr = doc["aircraft"].as<JsonArrayConst>();
    if (arr.isNull()) return false;

    std::vector<Aircraft> tmp;
    const uint32_t now = millis();
    for (JsonObjectConst a : arr) {
        if ((int)tmp.size() >= ADSB_MAX_AIRCRAFT) break;   // hard cap: protect RAM

        if (a["lat"].isNull() || a["lon"].isNull()) continue;  // need a position

        Aircraft ac;
        ac.hex    = (const char*)(a["hex"] | "");
        if (ac.hex.length() == 0) continue;
        ac.flight = String((const char*)(a["flight"] | "")); ac.flight.trim();
        ac.type   = (const char*)(a["t"] | "");
        ac.lat    = a["lat"].as<double>();
        ac.lon    = a["lon"].as<double>();

        if (a["alt_baro"].is<const char*>()) { ac.onGround = true; ac.altBaro = 0; continue; }  // skip ground aircraft
        else                                  ac.altBaro = a["alt_baro"] | 0.0f;

        ac.track    = a["track"].is<float>() ? a["track"].as<float>() : (a["true_heading"] | NAN);
        ac.gs       = a["gs"] | NAN;
        ac.baroRate = a["baro_rate"] | NAN;
        ac.squawk   = a["squawk"].is<const char*>() ? atoi(a["squawk"]) : (a["squawk"] | -1);
        ac.seenPos  = a["seen_pos"] | 0;
        ac.military  = ((a["dbFlags"] | 0u) & 0x1) != 0;
        ac.category  = (const char*)(a["category"] | "");
        ac.lastUpdateMs = now;

        tmp.push_back(std::move(ac));
    }

    out.swap(tmp);
    _lastOkMs = now;
    return true;
}

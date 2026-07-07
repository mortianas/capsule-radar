#pragma once
// Rare / notable aircraft watchlist for the UK.
// When any of these type codes appears in range for the first time, AUDIO_RARE fires
// and the radar draws a white highlight ring around the icon.
//
// Sources: ICAO Doc 8643, airplanes.live type field, ADS-B Exchange.
// Not all aircraft broadcast ADS-B (especially military); entries are included
// so that the ones that do are caught.

struct RareType { const char *icao; const char *name; };

static const RareType RARE_TYPES[] = {

    // ── WWII ALLIED ─────────────────────────────────────────────────────────
    { "SPIT", "Spitfire" },
    { "HURI", "Hurricane" },
    { "LANC", "Lancaster" },
    { "MOSQ", "Mosquito" },
    { "B17",  "B-17 Flying Fortress" },
    { "B29",  "B-29 Superfortress" },
    { "P51",  "P-51 Mustang" },
    { "P47",  "P-47 Thunderbolt" },
    { "P38",  "P-38 Lightning" },
    { "DC3",  "DC-3 / C-47 Dakota" },
    { "TIGM", "Tiger Moth" },
    { "BEAU", "Bristol Beaufighter" },
    { "BLEN", "Bristol Blenheim" },
    { "GLST", "Gloster Gladiator" },

    // ── BATTLE OF BRITAIN MEMORIAL FLIGHT ───────────────────────────────────
    // (types already above: SPIT, HURI, LANC, DC3)
    { "CHPM", "Chipmunk (BBMF)" },

    // ── COLD WAR UK ─────────────────────────────────────────────────────────
    { "HUNT", "Hawker Hunter" },
    { "LITE", "English Electric Lightning" },
    { "METE", "Gloster Meteor" },
    { "VAMP", "de Havilland Vampire" },
    { "BUCC", "Blackburn Buccaneer" },
    { "VULC", "Avro Vulcan" },
    { "NIMR", "Hawker Siddeley Nimrod" },
    { "VICT", "Handley Page Victor" },
    { "TSR2", "BAC TSR-2" },

    // ── COLD WAR / AIRSHOW SOVIET ────────────────────────────────────────────
    { "MG15", "MiG-15 Fagot" },
    { "MG17", "MiG-17 Fresco" },
    { "MG21", "MiG-21 Fishbed" },
    { "MG29", "MiG-29 Fulcrum" },
    { "SU27", "Su-27 Flanker" },

    // ── MODERN UK / NATO FAST JETS ───────────────────────────────────────────
    { "EUFI", "Eurofighter Typhoon" },
    { "F35",  "F-35 Lightning II" },
    { "F35B", "F-35B Lightning II" },
    { "F35C", "F-35C Lightning II" },
    { "F15",  "F-15 Eagle" },
    { "F16",  "F-16 Fighting Falcon" },
    { "F22",  "F-22 Raptor" },
    { "JAGR", "SEPECAT Jaguar" },
    { "TOR",  "Tornado" },
    { "TORNA","Tornado" },
    { "HAWK", "Hawk T1/T2 (Red Arrows)" },

    // ── MODERN UK / NATO SPECIAL MISSION ────────────────────────────────────
    { "E3CF", "E-3D Sentry AWACS (RAF)" },
    { "E3TF", "E-3A Sentry AWACS (NATO)" },
    { "RC35", "RC-135 Rivet Joint" },
    { "P8",   "P-8 Poseidon" },
    { "E7",   "E-7A Wedgetail" },
    { "U2",   "U-2 Dragon Lady" },
    { "MRTT", "A330 MRTT Voyager" },

    // ── MODERN UK / NATO HEAVIES ─────────────────────────────────────────────
    { "C130", "C-130 Hercules" },
    { "C17",  "C-17 Globemaster III" },
    { "A400", "A400M Atlas" },
    { "C5M",  "C-5M Super Galaxy" },

    // ── MODERN UK / NATO ROTARY ──────────────────────────────────────────────
    { "CH47", "CH-47 Chinook" },
    { "AH64", "Apache AH-64" },
    { "LYNX", "Westland Lynx" },
    { "MRLN", "Merlin HC3" },
    { "WCAT", "Wildcat AH1" },
    { "NH90", "NH90" },
    { "EH101","Merlin / EH101" },
    { "AW139","AW139" },
    { "H64",  "Black Hawk" },
    { "H53",  "Sea Stallion" },

    // ── US STRATEGIC / RARE ──────────────────────────────────────────────────
    { "B52",  "B-52 Stratofortress" },
    { "B2",   "B-2 Spirit" },
    { "E4",   "E-4B Nightwatch" },
    { "A10",  "A-10 Thunderbolt II" },
    { "F22",  "F-22 Raptor" },
    { "SR71", "SR-71 Blackbird" },

    // ── RUSSIAN MILITARY (OVERT ADS-B) ───────────────────────────────────────
    { "TU95", "Tu-95 Bear" },
    { "T160", "Tu-160 Blackjack" },
    { "IL78", "Il-78 Midas tanker" },
    { "IL76", "Il-76 Candid" },
    { "AN12", "An-12 Cub" },

    // ── RARE CIVILIAN ────────────────────────────────────────────────────────
    { "A3ST", "Airbus Beluga" },
    { "BLGX", "Airbus Beluga XL" },
    { "A124", "Antonov An-124 Ruslan" },
    { "B748", "Boeing 747-8" },
    { "B77X", "Boeing 777X" },
    { "CONC", "Concorde" },

    { nullptr, nullptr }  // sentinel
};

// Returns the display name if the type code is on the watchlist, nullptr otherwise.
inline const char* rare_type_name(const char *icao) {
    if (!icao || !icao[0]) return nullptr;
    for (int i = 0; RARE_TYPES[i].icao; ++i)
        if (strcmp(icao, RARE_TYPES[i].icao) == 0) return RARE_TYPES[i].name;
    return nullptr;
}

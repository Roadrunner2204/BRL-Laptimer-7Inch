/**
 * session_store.cpp — session serialization using ArduinoJson 7
 */

#include "session_store.h"
#include "sd_mgr.h"
#include "../data/lap_data.h"
#include "../data/track_db.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <SD.h>

static char s_session_path[48] = {};
static char s_track_name[48]   = {};

// ---------------------------------------------------------------------------
// Generate session ID: "YYYYMMDD_HHMMSS" if RTC available, else "millis_XXXXXXXX"
// ---------------------------------------------------------------------------
static void make_session_id(char *buf, size_t len) {
    // No RTC yet — use millis for uniqueness
    uint32_t ts = millis();
    snprintf(buf, len, "sess_%08lX", (unsigned long)ts);
}

void session_store_begin(const char *track_name) {
    strncpy(s_track_name, track_name, sizeof(s_track_name) - 1);

    char id[20];
    make_session_id(id, sizeof(id));
    strncpy(g_state.session.session_id, id, sizeof(g_state.session.session_id));

    snprintf(s_session_path, sizeof(s_session_path), "/sessions/%s.json", id);

    // Write initial JSON skeleton
    if (!g_state.sd_available) return;

    JsonDocument doc;
    doc["id"]    = id;
    doc["track"] = s_track_name;
    doc["laps"]  = JsonArray();

    char buf[256];
    serializeJson(doc, buf, sizeof(buf));
    sd_write_file(s_session_path, buf, strlen(buf));
    Serial.printf("[STORE] Session begun: %s\n", s_session_path);
}

void session_store_save_lap(uint8_t lap_idx) {
    if (!g_state.sd_available) return;
    if (strlen(s_session_path) == 0) return;

    LapSession &sess = g_state.session;
    if (lap_idx >= sess.lap_count) return;
    RecordedLap &rl = sess.laps[lap_idx];
    if (!rl.valid) return;

    // Read existing JSON
    static char file_buf[8192];
    if (!sd_read_file(s_session_path, file_buf, sizeof(file_buf))) {
        Serial.println("[STORE] Read session failed");
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, file_buf) != DeserializationError::Ok) {
        Serial.println("[STORE] Parse session failed");
        return;
    }

    JsonArray laps = doc["laps"].as<JsonArray>();
    JsonObject lap_obj = laps.add<JsonObject>();
    lap_obj["lap"]      = lap_idx + 1;
    lap_obj["total_ms"] = rl.total_ms;

    JsonArray sectors = lap_obj["sectors"].to<JsonArray>();
    for (uint8_t i = 0; i <= rl.sectors_used && i < MAX_SECTORS; i++) {
        sectors.add(rl.sector_ms[i]);
    }

    // Track points: write as compact array of [lat, lon, ms]
    if (rl.points && rl.point_count > 0) {
        JsonArray pts = lap_obj["track_points"].to<JsonArray>();
        // Limit to every 5th point to save space (~2 Hz)
        for (uint16_t i = 0; i < rl.point_count; i += 5) {
            JsonArray pt = pts.add<JsonArray>();
            pt.add(serialized(String(rl.points[i].lat, 7)));
            pt.add(serialized(String(rl.points[i].lon, 7)));
            pt.add(rl.points[i].lap_ms);
        }
    }

    // Serialize back to file
    File f = SD.open(s_session_path, FILE_WRITE);
    if (!f) {
        Serial.println("[STORE] Write failed");
        return;
    }
    serializeJson(doc, f);
    f.close();
    Serial.printf("[STORE] Lap %d saved\n", lap_idx + 1);
}

int session_store_list_summaries(SessionSummary *out, int max_count) {
    if (!g_state.sd_available || !out || max_count <= 0) return 0;

    File dir = SD.open("/sessions");
    if (!dir) return 0;

    int count = 0;
    static char s_sum_buf[4096];

    File f = dir.openNextFile();
    while (f && count < max_count) {
        if (!f.isDirectory()) {
            String fname = f.name();
            if (fname.endsWith(".json")) {
                // Build full path
                int slash = fname.lastIndexOf('/');
                String base = (slash >= 0) ? fname.substring(slash + 1) : fname;
                char fpath[64];
                snprintf(fpath, sizeof(fpath), "/sessions/%s", base.c_str());

                SessionSummary &s = out[count];
                memset(&s, 0, sizeof(s));
                // Extract session ID (filename without .json)
                String id = base.substring(0, base.length() - 5);
                strncpy(s.id, id.c_str(), sizeof(s.id) - 1);

                if (sd_read_file(fpath, s_sum_buf, sizeof(s_sum_buf))) {
                    JsonDocument doc;
                    if (deserializeJson(doc, s_sum_buf) == DeserializationError::Ok) {
                        strncpy(s.track, doc["track"] | "", sizeof(s.track) - 1);
                        JsonArray laps = doc["laps"].as<JsonArray>();
                        s.lap_count = 0;
                        uint32_t best = 0;
                        for (JsonObject lap : laps) {
                            s.lap_count++;
                            uint32_t t = lap["total_ms"] | 0u;
                            if (t > 0 && (best == 0 || t < best)) best = t;
                        }
                        s.best_ms = best;
                    }
                }
                count++;
            }
        }
        f = dir.openNextFile();
    }
    return count;
}

int session_store_list(char ids[][20], int max_count) {
    if (!g_state.sd_available) return 0;

    File dir = SD.open("/sessions");
    if (!dir) return 0;

    int count = 0;
    File f = dir.openNextFile();
    while (f && count < max_count) {
        if (!f.isDirectory()) {
            String name = f.name();
            if (name.endsWith(".json")) {
                int slash = name.lastIndexOf('/');
                String id = (slash >= 0) ? name.substring(slash + 1) : name;
                id = id.substring(0, id.length() - 5);
                strncpy(ids[count], id.c_str(), 19);
                ids[count][19] = '\0';
                count++;
            }
        }
        f = dir.openNextFile();
    }
    return count;
}

// ---------------------------------------------------------------------------
// User track persistence
// ---------------------------------------------------------------------------
void session_store_load_user_tracks() {
    if (!g_state.sd_available) return;

    File dir = SD.open("/tracks");
    if (!dir) return;

    g_user_track_count = 0;
    static char buf[2048];
    File f = dir.openNextFile();
    while (f && g_user_track_count < MAX_USER_TRACKS) {
        if (!f.isDirectory()) {
            // f.name() may return just the filename or the full path depending on SDK version.
            // Always build a safe absolute path ourselves.
            String fname = f.name();

            // Skip builtin coordinate-override files (builtin_NN.json)
            // They are stored in the same directory but are NOT user tracks.
            if (fname.startsWith("builtin_")) { f = dir.openNextFile(); continue; }
            if (!fname.endsWith(".json"))      { f = dir.openNextFile(); continue; }

            // Build full path: strip any leading path component, then prepend /tracks/
            int slash = fname.lastIndexOf('/');
            String base = (slash >= 0) ? fname.substring(slash + 1) : fname;
            char fpath[80];
            snprintf(fpath, sizeof(fpath), "/tracks/%s", base.c_str());

            if (!sd_read_file(fpath, buf, sizeof(buf))) {
                Serial.printf("[STORE] Failed to read %s\n", fpath);
                f = dir.openNextFile();
                continue;
            }

            JsonDocument doc;
            if (deserializeJson(doc, buf) != DeserializationError::Ok) {
                Serial.printf("[STORE] JSON parse error: %s\n", fpath);
                f = dir.openNextFile();
                continue;
            }

            // Validate: must have a non-empty name field (builtin overrides don't)
            const char *tname = doc["name"] | "";
            if (strlen(tname) == 0) { f = dir.openNextFile(); continue; }

            TrackDef &td = g_user_tracks[g_user_track_count];
            memset(&td, 0, sizeof(td));
            strncpy(td.name,    tname,                sizeof(td.name) - 1);
            strncpy(td.country, doc["country"] | "",  sizeof(td.country) - 1);
            td.length_km    = doc["length_km"] | 0.0f;
            td.is_circuit   = doc["is_circuit"] | true;
            td.user_created = true;

            td.sf_lat1 = doc["sf"][0] | 0.0;
            td.sf_lon1 = doc["sf"][1] | 0.0;
            td.sf_lat2 = doc["sf"][2] | 0.0;
            td.sf_lon2 = doc["sf"][3] | 0.0;

            if (!td.is_circuit) {
                td.fin_lat1 = doc["fin"][0] | 0.0;
                td.fin_lon1 = doc["fin"][1] | 0.0;
                td.fin_lat2 = doc["fin"][2] | 0.0;
                td.fin_lon2 = doc["fin"][3] | 0.0;
            }

            JsonArray secs = doc["sectors"].as<JsonArray>();
            td.sector_count = 0;
            for (JsonObject s : secs) {
                if (td.sector_count >= MAX_SECTORS) break;
                td.sectors[td.sector_count].lat = s["lat"] | 0.0;
                td.sectors[td.sector_count].lon = s["lon"] | 0.0;
                strncpy(td.sectors[td.sector_count].name,
                        s["name"] | "", SECTOR_NAME_LEN - 1);
                td.sector_count++;
            }

            g_user_track_count++;
            Serial.printf("[STORE] Loaded user track: %s\n", td.name);
        }
        f = dir.openNextFile();
    }
    Serial.printf("[STORE] Total user tracks loaded: %d\n", g_user_track_count);
}

bool session_store_save_user_track(const TrackDef *td) {
    if (!g_state.sd_available || !td) return false;
    if (!SD.exists("/tracks")) SD.mkdir("/tracks");

    // Sanitize name for filename
    char fname[80];
    char safe[48];
    strncpy(safe, td->name, sizeof(safe) - 1);
    for (char *p = safe; *p; p++) {
        if (*p == ' ' || *p == '/' || *p == '\\') *p = '_';
    }
    snprintf(fname, sizeof(fname), "/tracks/%s.json", safe);

    JsonDocument doc;
    doc["name"]       = td->name;
    doc["country"]    = td->country;
    doc["length_km"]  = td->length_km;
    doc["is_circuit"] = td->is_circuit;

    JsonArray sf = doc["sf"].to<JsonArray>();
    sf.add(td->sf_lat1); sf.add(td->sf_lon1);
    sf.add(td->sf_lat2); sf.add(td->sf_lon2);

    if (!td->is_circuit) {
        JsonArray fin = doc["fin"].to<JsonArray>();
        fin.add(td->fin_lat1); fin.add(td->fin_lon1);
        fin.add(td->fin_lat2); fin.add(td->fin_lon2);
    }

    JsonArray secs = doc["sectors"].to<JsonArray>();
    for (uint8_t i = 0; i < td->sector_count; i++) {
        JsonObject s = secs.add<JsonObject>();
        s["lat"]  = td->sectors[i].lat;
        s["lon"]  = td->sectors[i].lon;
        s["name"] = td->sectors[i].name;
    }

    File f = SD.open(fname, FILE_WRITE);
    if (!f) return false;
    serializeJson(doc, f);
    f.close();
    Serial.printf("[STORE] User track saved: %s\n", fname);
    return true;
}

bool session_store_delete_user_track(int u_slot) {
    if (u_slot < 0 || u_slot >= g_user_track_count) return false;

    // Delete file from SD
    if (g_state.sd_available) {
        const TrackDef &td = g_user_tracks[u_slot];
        char fname[80], safe[48];
        strncpy(safe, td.name, sizeof(safe) - 1);
        safe[sizeof(safe)-1] = '\0';
        for (char *p = safe; *p; p++)
            if (*p == ' ' || *p == '/' || *p == '\\') *p = '_';
        snprintf(fname, sizeof(fname), "/tracks/%s.json", safe);
        if (SD.exists(fname)) SD.remove(fname);
        Serial.printf("[STORE] User track deleted: %s\n", fname);
    }

    // Compact array
    for (int i = u_slot; i < g_user_track_count - 1; i++)
        g_user_tracks[i] = g_user_tracks[i + 1];
    g_user_track_count--;
    memset(&g_user_tracks[g_user_track_count], 0, sizeof(TrackDef));
    return true;
}

bool session_store_save_builtin_override(int builtin_idx, const TrackDef *td) {
    if (!g_state.sd_available || !td) return false;
    if (builtin_idx < 0 || builtin_idx >= MAX_BUILTIN_TRACKS) return false;
    if (!SD.exists("/tracks")) SD.mkdir("/tracks");

    char fname[48];
    snprintf(fname, sizeof(fname), "/tracks/builtin_%02d.json", builtin_idx);

    JsonDocument doc;
    doc["builtin_idx"] = builtin_idx;
    JsonArray sf = doc["sf"].to<JsonArray>();
    sf.add(td->sf_lat1); sf.add(td->sf_lon1);
    sf.add(td->sf_lat2); sf.add(td->sf_lon2);

    if (!td->is_circuit) {
        JsonArray fin = doc["fin"].to<JsonArray>();
        fin.add(td->fin_lat1); fin.add(td->fin_lon1);
        fin.add(td->fin_lat2); fin.add(td->fin_lon2);
    }

    JsonArray secs = doc["sectors"].to<JsonArray>();
    for (uint8_t i = 0; i < td->sector_count; i++) {
        JsonObject s = secs.add<JsonObject>();
        s["lat"]  = td->sectors[i].lat;
        s["lon"]  = td->sectors[i].lon;
        s["name"] = td->sectors[i].name;
    }

    File f = SD.open(fname, FILE_WRITE);
    if (!f) return false;
    serializeJson(doc, f);
    f.close();
    Serial.printf("[STORE] Builtin override saved: %s\n", fname);
    return true;
}

void session_store_load_builtin_overrides() {
    if (!g_state.sd_available) return;

    for (int i = 0; i < MAX_BUILTIN_TRACKS && i < TRACK_DB_BUILTIN_COUNT; i++) {
        char fname[48];
        snprintf(fname, sizeof(fname), "/tracks/builtin_%02d.json", i);
        if (!SD.exists(fname)) continue;

        static char buf[1024];
        if (!sd_read_file(fname, buf, sizeof(buf))) continue;

        JsonDocument doc;
        if (deserializeJson(doc, buf) != DeserializationError::Ok) continue;

        // Start from original built-in definition, then override coords
        g_builtin_overrides[i] = TRACK_DB[i];
        TrackDef &td = g_builtin_overrides[i];
        td.user_created = false;

        td.sf_lat1 = doc["sf"][0] | td.sf_lat1;
        td.sf_lon1 = doc["sf"][1] | td.sf_lon1;
        td.sf_lat2 = doc["sf"][2] | td.sf_lat2;
        td.sf_lon2 = doc["sf"][3] | td.sf_lon2;

        if (!td.is_circuit) {
            td.fin_lat1 = doc["fin"][0] | td.fin_lat1;
            td.fin_lon1 = doc["fin"][1] | td.fin_lon1;
            td.fin_lat2 = doc["fin"][2] | td.fin_lat2;
            td.fin_lon2 = doc["fin"][3] | td.fin_lon2;
        }

        JsonArray secs = doc["sectors"].as<JsonArray>();
        if (secs) {
            td.sector_count = 0;
            for (JsonObject s : secs) {
                if (td.sector_count >= MAX_SECTORS) break;
                td.sectors[td.sector_count].lat = s["lat"] | 0.0;
                td.sectors[td.sector_count].lon = s["lon"] | 0.0;
                strncpy(td.sectors[td.sector_count].name,
                        s["name"] | "", SECTOR_NAME_LEN - 1);
                td.sector_count++;
            }
        }

        g_builtin_override_set[i] = true;
        Serial.printf("[STORE] Builtin override loaded for idx %d\n", i);
    }
}

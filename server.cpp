// Family location server.
// Listens on 0.0.0.0:8000 so the same program answers on the home LAN and on Tailscale.
// Plain HTTP, no password, as the test plan specified. Keep port 8000 off the public internet.
//
//   GET  /health
//   POST /send      {userId, groupId, lat, lon, ts?, acc?, prov?, wifi?, ssid?, alive?, speed?, ...}
//   GET  /fetch?userId=&groupId=
//   POST /profile   {userId, groupId, name, phone, image?}   image is base64 JPEG
//   GET  /group?groupId=
//   GET  /avatar?groupId=&userId=
//   POST /place     {groupId, id, name, lat, lon, radius, image?, ssid?}
//   GET  /placeimg?groupId=&id=
//   POST /place/delete {groupId, id}
//   GET  /servercfg
//   POST /servercfg {historyKeep, aliveOn, aliveSecs}

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#ifdef _WIN32
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#endif

#include "httplib.h"
#include "json.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

struct Member {
    std::string userId;
    std::string groupId;
    std::string name;
    std::string phone;
    double lat = 0;
    double lon = 0;
    long long ts = 0;
    bool hasLoc = false;
    bool hasImage = false;
    long long imageTs = 0;
    long long arrived = 0;
    double anchorLat = 0;
    double anchorLon = 0;
    std::string area;
    bool traveling = false;
    double speed = 0;
    long long departed = 0;
    std::string departedFrom;
    int battery = -1;
    int signal = -1;
    bool locationOff = false;
    int every = 0;
    unsigned long long imageHash = 0;
    double acc = -1;
    std::string prov;
    bool wifi = false;
    std::string ssid;
    long long seen = 0;
    long long acceptedAt = 0;
    bool pendingOut = false;
    double pendingLat = 0;
    double pendingLon = 0;
};

struct Place {
    std::string id;
    std::string groupId;
    std::string name;
    double lat = 0;
    double lon = 0;
    double radius = 100;
    bool hasImage = false;
    long long imageTs = 0;
    unsigned long long imageHash = 0;
    std::string ssid;
};

struct CheckIn {
    long long ts = 0;
    std::string groupId;
    std::string userId;
    std::string name;
    std::string area;
    double lat = 0;
    double lon = 0;
    int battery = -1;
};

static std::vector<CheckIn> g_history;
static std::mutex g_mu;
static std::mutex g_wait_mu;
static std::condition_variable g_cv;
static std::atomic<bool> g_stopping{false};
static long long g_rev = 1;
static std::map<std::string, Member> g_members;
static std::map<std::string, Place> g_places;
static httplib::Server* g_server = nullptr;
static std::string g_status = "Starting";
static fs::path g_root;
static std::atomic<int> g_historyKeep{50};
static std::atomic<bool> g_aliveOn{true};
static std::atomic<int> g_aliveSecs{75};
static std::string g_tail_saved;
static std::mutex g_cfg_mu;

static long long server_now() {
    return static_cast<long long>(std::time(nullptr));
}

static int clamp_range(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static std::string key_of(const std::string& group, const std::string& id) {
    return group + ":" + id;
}

static bool valid_coord(double lat, double lon) {
    return lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
}

static double haversine_m(double lat1, double lon1, double lat2, double lon2) {
    const double r = 6371000.0;
    double p1 = lat1 * 3.141592653589793 / 180.0;
    double p2 = lat2 * 3.141592653589793 / 180.0;
    double dp = (lat2 - lat1) * 3.141592653589793 / 180.0;
    double dl = (lon2 - lon1) * 3.141592653589793 / 180.0;
    double a = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2 * r * std::atan2(std::sqrt(a), std::sqrt(1 - a));
}

static std::string clip(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    return s.substr(0, n);
}

static std::string file_token(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
            out.push_back(static_cast<char>(c));
        else
            out.push_back('_');
    }
    if (out.empty()) out = "x";
    if (out.size() > 80) out.resize(80);
    return out;
}

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static std::vector<unsigned char> decode64(const std::string& in) {
    std::vector<unsigned char> out;
    int val = 0;
    int valb = -8;
    for (unsigned char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int d = b64_val(static_cast<char>(c));
        if (d < 0) continue;
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<unsigned char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

static fs::path avatar_path(const std::string& group, const std::string& user) {
    return g_root / "avatars" / (file_token(group) + "_" + file_token(user) + ".jpg");
}

static fs::path place_image_path(const std::string& group, const std::string& id) {
    return g_root / "places" / (file_token(group) + "_" + file_token(id) + ".jpg");
}

static bool write_place_image(const std::string& group, const std::string& id, const std::vector<unsigned char>& bytes) {
    std::error_code ec;
    fs::create_directories(g_root / "places", ec);
    fs::path path = place_image_path(group, id);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) return false;
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        fs::rename(tmp, path, ec);
    }
    return !ec && fs::exists(path);
}

static void save_locked() {
    json members = json::array();
    for (const auto& kv : g_members) {
        const Member& m = kv.second;
        members.push_back({
            {"userId", m.userId},
            {"groupId", m.groupId},
            {"name", m.name},
            {"phone", m.phone},
            {"lat", m.lat},
            {"lon", m.lon},
            {"ts", m.ts},
            {"hasLoc", m.hasLoc},
            {"hasImage", m.hasImage},
            {"imageTs", m.imageTs},
            {"arrived", m.arrived},
            {"anchorLat", m.anchorLat},
            {"anchorLon", m.anchorLon},
            {"area", m.area},
            {"traveling", m.traveling},
            {"speed", m.speed},
            {"departed", m.departed},
            {"departedFrom", m.departedFrom},
            {"battery", m.battery},
            {"signal", m.signal},
            {"locationOff", m.locationOff},
            {"every", m.every},
            {"imageHash", m.imageHash},
            {"acc", m.acc},
            {"prov", m.prov},
            {"wifi", m.wifi},
            {"ssid", m.ssid},
            {"seen", m.seen},
            {"acceptedAt", m.acceptedAt},
            {"pendingOut", m.pendingOut},
            {"pendingLat", m.pendingLat},
            {"pendingLon", m.pendingLon},
        });
    }
    json places = json::array();
    for (const auto& kv : g_places) {
        const Place& p = kv.second;
        places.push_back({
            {"id", p.id},
            {"groupId", p.groupId},
            {"name", p.name},
            {"lat", p.lat},
            {"lon", p.lon},
            {"radius", p.radius},
            {"hasImage", p.hasImage},
            {"imageTs", p.imageTs},
            {"imageHash", p.imageHash},
            {"ssid", p.ssid},
        });
    }
    json root;
    root["members"] = members;
    root["places"] = places;
    fs::create_directories(g_root);
    fs::path tmp = g_root / "family.json.tmp";
    fs::path dest = g_root / "family.json";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << root.dump(2);
    }
    std::error_code ec;
    fs::rename(tmp, dest, ec);
    if (ec) fs::copy_file(tmp, dest, fs::copy_options::overwrite_existing, ec);
}

static void save_history_locked();

static void commit_locked() {
    save_locked();
    save_history_locked();
    {
        std::lock_guard<std::mutex> lock(g_wait_mu);
        g_rev++;
    }
    g_cv.notify_all();
}

static unsigned long long image_hash(const std::vector<unsigned char>& bytes) {
    unsigned long long h = 1469598103934665603ULL;
    for (unsigned char c : bytes) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

static bool write_avatar(const std::string& group, const std::string& user, const std::vector<unsigned char>& bytes) {
    std::error_code ec;
    fs::create_directories(g_root / "avatars", ec);
    fs::path path = avatar_path(group, user);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) return false;
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        fs::rename(tmp, path, ec);
    }
    return !ec && fs::exists(path);
}

static void save_history_locked() {
    json rows = json::array();
    for (const CheckIn& c : g_history) {
        rows.push_back({
            {"ts", c.ts},
            {"groupId", c.groupId},
            {"userId", c.userId},
            {"name", c.name},
            {"area", c.area},
            {"lat", c.lat},
            {"lon", c.lon},
            {"battery", c.battery},
        });
    }
    fs::path tmp = g_root / "history.json.tmp";
    fs::path dest = g_root / "history.json";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << rows.dump();
    }
    std::error_code ec;
    fs::rename(tmp, dest, ec);
    if (ec) {
        fs::remove(dest, ec);
        fs::rename(tmp, dest, ec);
    }
}

static void load_history() {
    fs::path dest = g_root / "history.json";
    if (!fs::exists(dest)) return;
    std::ifstream in(dest);
    if (!in) return;
    json rows;
    try {
        in >> rows;
    } catch (...) {
        return;
    }
    if (!rows.is_array()) return;
    g_history.clear();
    for (const auto& j : rows) {
        CheckIn c;
        c.ts = j.value("ts", 0LL);
        c.groupId = j.value("groupId", "");
        c.userId = j.value("userId", "");
        if (c.userId.empty() || c.groupId.empty()) continue;
        c.name = j.value("name", "");
        c.area = j.value("area", "");
        c.lat = j.value("lat", 0.0);
        c.lon = j.value("lon", 0.0);
        c.battery = j.value("battery", -1);
        g_history.push_back(c);
    }
}

static void note_checkin_locked(const Member& m) {
    for (int i = static_cast<int>(g_history.size()) - 1; i >= 0; --i) {
        CheckIn& last = g_history[static_cast<size_t>(i)];
        if (last.groupId != m.groupId || last.userId != m.userId) continue;
        bool sameSpot = last.area == m.area && haversine_m(last.lat, last.lon, m.lat, m.lon) < 20.0;
        if (sameSpot && m.ts >= last.ts && m.ts - last.ts < 180) {
            last.ts = m.ts;
            last.lat = m.lat;
            last.lon = m.lon;
            last.battery = m.battery;
            last.name = m.name.empty() ? m.userId : m.name;
            return;
        }
        break;
    }
    CheckIn c;
    c.ts = m.ts;
    c.groupId = m.groupId;
    c.userId = m.userId;
    c.name = m.name.empty() ? m.userId : m.name;
    c.area = m.area;
    c.lat = m.lat;
    c.lon = m.lon;
    c.battery = m.battery;
    g_history.push_back(c);
}

static void trim_history_locked() {
    int limit = g_historyKeep.load();
    if (limit < 10) limit = 10;
    if (limit > 5000) limit = 5000;
    std::map<std::string, int> count;
    std::vector<char> keep(g_history.size(), 0);
    for (int i = static_cast<int>(g_history.size()) - 1; i >= 0; --i) {
        const CheckIn& c = g_history[static_cast<size_t>(i)];
        std::string k = c.groupId + "\n" + c.userId;
        int n = count[k];
        if (n < limit) {
            keep[static_cast<size_t>(i)] = 1;
            count[k] = n + 1;
        }
    }
    if (count.empty() && g_history.empty()) return;
    size_t kept = 0;
    for (char bit : keep) if (bit) kept++;
    if (kept == g_history.size()) return;
    std::vector<CheckIn> next;
    next.reserve(kept);
    for (size_t i = 0; i < g_history.size(); ++i) {
        if (keep[i]) next.push_back(g_history[i]);
    }
    g_history.swap(next);
}

static void load_store() {
    fs::path dest = g_root / "family.json";
    if (!fs::exists(dest)) return;
    std::ifstream in(dest);
    if (!in) return;
    json root;
    try {
        in >> root;
    } catch (...) {
        return;
    }
    if (root.contains("members")) {
        for (const auto& j : root["members"]) {
            Member m;
            m.userId = j.value("userId", "");
            m.groupId = j.value("groupId", "");
            if (m.userId.empty() || m.groupId.empty()) continue;
            m.name = j.value("name", "");
            m.phone = j.value("phone", "");
            m.lat = j.value("lat", 0.0);
            m.lon = j.value("lon", 0.0);
            m.ts = j.value("ts", 0LL);
            m.hasLoc = j.value("hasLoc", false);
            m.hasImage = j.value("hasImage", false);
            m.imageTs = j.value("imageTs", 0LL);
            m.arrived = j.value("arrived", 0LL);
            m.anchorLat = j.value("anchorLat", m.lat);
            m.anchorLon = j.value("anchorLon", m.lon);
            m.area = j.value("area", "");
            m.traveling = j.value("traveling", false);
            m.speed = j.value("speed", 0.0);
            m.departed = j.value("departed", 0LL);
            m.departedFrom = j.value("departedFrom", "");
            m.battery = j.value("battery", -1);
            m.signal = j.value("signal", -1);
            m.locationOff = j.value("locationOff", false);
            m.every = j.value("every", 0);
            m.imageHash = j.value("imageHash", 0ULL);
            m.acc = j.value("acc", -1.0);
            m.prov = j.value("prov", "");
            m.wifi = j.value("wifi", false);
            m.ssid = j.value("ssid", "");
            m.seen = j.value("seen", 0LL);
            m.acceptedAt = j.value("acceptedAt", 0LL);
            m.pendingOut = j.value("pendingOut", false);
            m.pendingLat = j.value("pendingLat", 0.0);
            m.pendingLon = j.value("pendingLon", 0.0);
            g_members[key_of(m.groupId, m.userId)] = m;
        }
    }
    if (root.contains("places")) {
        for (const auto& j : root["places"]) {
            Place p;
            p.id = j.value("id", "");
            p.groupId = j.value("groupId", "");
            if (p.id.empty() || p.groupId.empty()) continue;
            p.name = j.value("name", "");
            p.lat = j.value("lat", 0.0);
            p.lon = j.value("lon", 0.0);
            p.radius = j.value("radius", 100.0);
            p.hasImage = j.value("hasImage", false);
            p.imageTs = j.value("imageTs", 0LL);
            p.imageHash = j.value("imageHash", 0ULL);
            p.ssid = j.value("ssid", "");
            g_places[key_of(p.groupId, p.id)] = p;
        }
    }
    load_history();
    size_t before = g_history.size();
    trim_history_locked();
    if (g_history.size() != before) save_history_locked();
}

static Member& ensure_member(const std::string& group, const std::string& user) {
    std::string k = key_of(group, user);
    auto it = g_members.find(k);
    if (it == g_members.end()) {
        Member m;
        m.userId = user;
        m.groupId = group;
        it = g_members.emplace(k, m).first;
    }
    return it->second;
}

static void json_error(httplib::Response& res, int code, const std::string& msg) {
    json j;
    j["error"] = msg;
    res.status = code;
    res.set_content(j.dump(), "application/json");
}

static bool json_bool(const json& j, const char* key, bool fallback) {
    if (!j.contains(key) || j[key].is_null()) return fallback;
    if (j[key].is_boolean()) return j[key].get<bool>();
    if (j[key].is_number()) return j[key].get<int>() != 0;
    if (j[key].is_string()) {
        std::string s = j[key].get<std::string>();
        return s == "true" || s == "1";
    }
    return fallback;
}

static double place_radius(const Place& p) {
    return p.radius < 20 ? 20 : p.radius;
}

static double bearing_deg(double lat1, double lon1, double lat2, double lon2) {
    double p1 = lat1 * 3.141592653589793 / 180.0;
    double p2 = lat2 * 3.141592653589793 / 180.0;
    double dl = (lon2 - lon1) * 3.141592653589793 / 180.0;
    double y = std::sin(dl) * std::cos(p2);
    double x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
    double deg = std::atan2(y, x) * 180.0 / 3.141592653589793;
    if (deg < 0) deg += 360.0;
    return deg;
}

static double leave_radius(const Place& p) {
    double rad = place_radius(p);
    double plus = rad + 40.0;
    double scaled = rad * 1.35;
    return plus > scaled ? plus : scaled;
}

static const Place* place_by_name_locked(const std::string& group, const std::string& name) {
    if (name.empty()) return nullptr;
    for (const auto& kv : g_places) {
        if (kv.second.groupId == group && kv.second.name == name) return &kv.second;
    }
    return nullptr;
}

static const Place* place_inside_locked(const std::string& group, double lat, double lon) {
    const Place* best = nullptr;
    double bestR = 0;
    for (const auto& kv : g_places) {
        const Place& p = kv.second;
        if (p.groupId != group) continue;
        double rad = place_radius(p);
        if (haversine_m(lat, lon, p.lat, p.lon) <= rad && (!best || rad < bestR)) {
            best = &p;
            bestR = rad;
        }
    }
    return best;
}

static void apply_area_change_locked(Member& m, double lat, double lon, long long ts) {
    const Place* current = place_by_name_locked(m.groupId, m.area);
    std::string nxt;
    if (current && haversine_m(lat, lon, current->lat, current->lon) <= leave_radius(*current)) {
        nxt = current->name;
    } else {
        const Place* inside = place_inside_locked(m.groupId, lat, lon);
        nxt = inside ? inside->name : "";
    }
    if (nxt == m.area) return;
    if (!nxt.empty()) m.arrived = ts;
    m.area = nxt;
}

static void remember_place_ssid_locked(const Member& m) {
    if (!m.wifi || m.ssid.empty()) return;
    Place* best = nullptr;
    double bestR = 0;
    for (auto& kv : g_places) {
        Place& p = kv.second;
        if (p.groupId != m.groupId || !p.ssid.empty()) continue;
        double rad = place_radius(p);
        if (haversine_m(m.lat, m.lon, p.lat, p.lon) <= rad && (!best || rad < bestR)) {
            best = &p;
            bestR = rad;
        }
    }
    if (best) best->ssid = m.ssid;
}

static bool wifi_outside_place_locked(const std::string& group, const std::string& ssid, double lat, double lon) {
    bool matched = false;
    bool inside = false;
    for (const auto& kv : g_places) {
        const Place& p = kv.second;
        if (p.groupId != group || p.ssid.empty() || p.ssid != ssid) continue;
        matched = true;
        if (haversine_m(lat, lon, p.lat, p.lon) <= place_radius(p)) inside = true;
    }
    return matched && !inside;
}

static void anchor_ref(const Member& m, double& lat, double& lon) {
    lat = m.anchorLat;
    lon = m.anchorLon;
    if (m.hasLoc && lat == 0.0 && lon == 0.0 && (m.lat != 0.0 || m.lon != 0.0)) {
        lat = m.lat;
        lon = m.lon;
    }
}

static void depart_locked(Member& m, long long ts) {
    if (m.traveling) return;
    m.traveling = true;
    m.departed = ts;
    m.departedFrom = m.area.empty() ? "this spot" : m.area;
}

static void load_server_cfg() {
    fs::path path = g_root / "server.json";
    if (!fs::exists(path)) return;
    std::ifstream in(path);
    if (!in) return;
    json j;
    try {
        in >> j;
    } catch (...) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_cfg_mu);
    if (j.contains("tailscale") && j["tailscale"].is_string()) g_tail_saved = j["tailscale"].get<std::string>();
    if (j.contains("historyKeep") && j["historyKeep"].is_number())
        g_historyKeep.store(clamp_range(j["historyKeep"].get<int>(), 10, 5000));
    if (j.contains("aliveOn")) {
        if (j["aliveOn"].is_boolean()) g_aliveOn.store(j["aliveOn"].get<bool>());
        else if (j["aliveOn"].is_number()) g_aliveOn.store(j["aliveOn"].get<int>() != 0);
    }
    if (j.contains("aliveSecs") && j["aliveSecs"].is_number())
        g_aliveSecs.store(clamp_range(j["aliveSecs"].get<int>(), 20, 3600));
}

static void save_server_cfg() {
    std::string tail;
    int keep = g_historyKeep.load();
    bool on = g_aliveOn.load();
    int secs = g_aliveSecs.load();
    {
        std::lock_guard<std::mutex> lock(g_cfg_mu);
        tail = g_tail_saved;
        keep = g_historyKeep.load();
        on = g_aliveOn.load();
        secs = g_aliveSecs.load();
    }
    json j;
    j["tailscale"] = tail;
    j["historyKeep"] = keep;
    j["aliveOn"] = on;
    j["aliveSecs"] = secs;
    std::error_code ec;
    fs::create_directories(g_root, ec);
    fs::path tmp = g_root / "server.json.tmp";
    fs::path dest = g_root / "server.json";
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) return;
        out << j.dump(2);
    }
    fs::rename(tmp, dest, ec);
    if (ec) {
        fs::remove(dest, ec);
        fs::rename(tmp, dest, ec);
    }
}

static json server_cfg_json() {
    json j;
    j["historyKeep"] = g_historyKeep.load();
    j["aliveOn"] = g_aliveOn.load();
    j["aliveSecs"] = g_aliveSecs.load();
    return j;
}

static void setup_routes(httplib::Server& server) {
    server.set_payload_max_length(2 * 1024 * 1024);

    server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("OK", "text/plain");
    });

    server.Post("/send", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = json::parse(req.body);
            if (!j.contains("userId") || !j["userId"].is_string() || !j.contains("groupId") || !j["groupId"].is_string()) {
                json_error(res, 400, "Missing fields");
                return;
            }
            bool alive = json_bool(j, "alive", false);
            bool locationOff = json_bool(j, "locationOff", false);
            if (!alive && !locationOff && (!j.contains("lat") || !j.contains("lon"))) {
                json_error(res, 400, "Missing fields");
                return;
            }
            std::string userId = clip(j["userId"].get<std::string>(), 64);
            std::string groupId = clip(j["groupId"].get<std::string>(), 64);
            if (userId.empty() || groupId.empty()) {
                json_error(res, 400, "Bad location");
                return;
            }
            int signal = j.contains("signal") && j["signal"].is_number() ? j["signal"].get<int>() : -1;
            if (signal < -2) signal = -2;
            if (signal > 4) signal = 4;
            int every = j.contains("every") && j["every"].is_number() ? j["every"].get<int>() : 0;
            if (every < 0) every = 0;
            if (every > 86400) every = 86400;
            long long ts = 0;
            if (j.contains("ts") && j["ts"].is_number()) ts = j["ts"].get<long long>();
            if (ts <= 0) ts = server_now();
            long long seen = server_now();
            bool hasAcc = j.contains("acc") && j["acc"].is_number();
            double acc = hasAcc ? j["acc"].get<double>() : -1;
            if (hasAcc && (!std::isfinite(acc) || acc < 0)) hasAcc = false;
            std::string prov = j.contains("prov") && j["prov"].is_string() ? clip(j["prov"].get<std::string>(), 32) : "";
            bool hasWifi = j.contains("wifi");
            bool wifi = hasWifi ? json_bool(j, "wifi", false) : false;
            bool hasSsid = j.contains("ssid") && j["ssid"].is_string();
            std::string ssid = hasSsid ? clip(j["ssid"].get<std::string>(), 64) : "";
            auto apply_status = [&](Member& m) {
                if (j.contains("battery") && j["battery"].is_number()) {
                    int battery = j["battery"].get<int>();
                    if (battery >= 0 && battery <= 100) m.battery = battery;
                }
                if (j.contains("signal")) m.signal = signal;
                if (j.contains("every")) m.every = every;
                if (hasWifi) m.wifi = wifi;
                if (hasSsid) m.ssid = ssid;
                m.seen = seen;
            };
            auto saved = [&]() {
                json resp;
                resp["status"] = "saved";
                res.set_content(resp.dump(), "application/json");
            };
            if (alive) {
                std::lock_guard<std::mutex> lock(g_mu);
                Member& m = ensure_member(groupId, userId);
                apply_status(m);
                if (j.contains("locationOff")) m.locationOff = locationOff;
                commit_locked();
                saved();
                return;
            }
            if (locationOff) {
                std::lock_guard<std::mutex> lock(g_mu);
                Member& m = ensure_member(groupId, userId);
                apply_status(m);
                m.locationOff = true;
                m.ts = ts;
                commit_locked();
                saved();
                return;
            }
            double lat = j["lat"].get<double>();
            double lon = j["lon"].get<double>();
            if (!valid_coord(lat, lon) || !std::isfinite(lat) || !std::isfinite(lon)) {
                json_error(res, 400, "Bad location");
                return;
            }
            bool hasSpeed = j.contains("speed") && j["speed"].is_number();
            double speed = hasSpeed ? j["speed"].get<double>() : 0;
            if (hasSpeed && !std::isfinite(speed)) hasSpeed = false;
            {
                std::lock_guard<std::mutex> lock(g_mu);
                Member& m = ensure_member(groupId, userId);
                apply_status(m);
                m.locationOff = false;
                bool reject = false;
                if (m.hasLoc && m.acceptedAt > 0) {
                    long long dt = seen - m.acceptedAt;
                    if (dt >= 1 && dt <= 120) {
                        double implied = haversine_m(m.lat, m.lon, lat, lon) / static_cast<double>(dt);
                        if (implied > 70.0) reject = true;
                    }
                }
                bool coarse = prov != "gps" && (!hasAcc || acc > 50.0);
                bool recentGps = m.prov == "gps" && m.acceptedAt > 0 && seen - m.acceptedAt < 90;
                if (coarse && recentGps) reject = true;
                if (wifi && !ssid.empty() && wifi_outside_place_locked(groupId, ssid, lat, lon)) {
                    if (m.hasLoc && !(m.anchorLat == 0.0 && m.anchorLon == 0.0)) {
                        m.lat = m.anchorLat;
                        m.lon = m.anchorLon;
                    }
                    m.traveling = false;
                    m.speed = 0;
                    commit_locked();
                    saved();
                    return;
                }
                if (reject) {
                    commit_locked();
                    saved();
                    return;
                }
                if (hasAcc && acc > 100.0) {
                    commit_locked();
                    saved();
                    return;
                }
                double aLat = 0, aLon = 0;
                anchor_ref(m, aLat, aLon);
                bool hasPrev = m.hasLoc;
                double distAnchor = hasPrev ? haversine_m(aLat, aLon, lat, lon) : 0;
                bool recordHistory = true;
                const Place* held = place_by_name_locked(m.groupId, m.area);
                bool insideHeld = held && haversine_m(lat, lon, held->lat, held->lon) <= leave_radius(*held);
                const Place* here = place_inside_locked(m.groupId, lat, lon);
                bool insideSomewhere = insideHeld || here != nullptr;
                // Under about 4.5 mph is noise from a phone that is sitting still, or a slow walk.
                bool slow = !hasSpeed || speed < 2.0;
                long long gap = m.acceptedAt > 0 ? seen - m.acceptedAt : 0;
                bool spike = hasPrev && gap >= 1 && gap <= 120 && slow && distAnchor / static_cast<double>(gap) > 4.0;
                if (spike && !insideSomewhere) {
                    commit_locked();
                    saved();
                    return;
                }
                auto snap_anchor = [&]() {
                    if (m.anchorLat == 0.0 && m.anchorLon == 0.0) {
                        m.anchorLat = aLat;
                        m.anchorLon = aLon;
                    }
                    if (m.anchorLat == 0.0 && m.anchorLon == 0.0) {
                        m.anchorLat = lat;
                        m.anchorLon = lon;
                    }
                    m.lat = m.anchorLat;
                    m.lon = m.anchorLon;
                    m.traveling = false;
                    m.speed = 0;
                    m.pendingOut = false;
                };
                // Still at a saved place, or only a short GPS wander: keep the anchor and clear traveling.
                bool settled = insideSomewhere && (!hasSpeed || speed < 3.5);
                bool wander = !hasPrev || distAnchor < 80.0;
                if (!hasPrev) {
                    m.anchorLat = lat;
                    m.anchorLon = lon;
                    m.lat = lat;
                    m.lon = lon;
                    m.traveling = false;
                    m.speed = 0;
                    apply_area_change_locked(m, lat, lon, ts);
                    if (m.arrived <= 0) m.arrived = ts;
                    m.pendingOut = false;
                } else if (settled || (slow && wander)) {
                    if (here && m.area != here->name) apply_area_change_locked(m, lat, lon, ts);
                    snap_anchor();
                    recordHistory = false;
                } else {
                    bool confirmed = false;
                    if (m.pendingOut) {
                        double first = haversine_m(aLat, aLon, m.pendingLat, m.pendingLon);
                        double b1 = bearing_deg(aLat, aLon, m.pendingLat, m.pendingLon);
                        double b2 = bearing_deg(aLat, aLon, lat, lon);
                        double turn = std::fabs(b1 - b2);
                        if (turn > 180.0) turn = 360.0 - turn;
                        confirmed = turn < 70.0 && distAnchor + 20.0 >= first && distAnchor > 40.0;
                    }
                    if (!confirmed) {
                        m.pendingOut = true;
                        m.pendingLat = lat;
                        m.pendingLon = lon;
                        snap_anchor();
                        recordHistory = false;
                    } else {
                        m.pendingOut = false;
                        depart_locked(m, ts);
                        m.lat = lat;
                        m.lon = lon;
                        m.anchorLat = lat;
                        m.anchorLon = lon;
                        apply_area_change_locked(m, lat, lon, ts);
                        if (hasSpeed) m.speed = speed;
                    }
                }
                m.ts = ts;
                m.hasLoc = true;
                m.acceptedAt = seen;
                m.acc = hasAcc ? acc : -1;
                m.prov = prov;
                if (m.traveling && hasSpeed) m.speed = speed;
                remember_place_ssid_locked(m);
                if (recordHistory) {
                    note_checkin_locked(m);
                    trim_history_locked();
                }
                commit_locked();
            }
            saved();
        } catch (const std::exception& e) {
            json_error(res, 400, std::string("Invalid JSON: ") + e.what());
        }
    });

    server.Get("/fetch", [](const httplib::Request& req, httplib::Response& res) {
        std::string userId = req.get_param_value("userId");
        std::string groupId = req.get_param_value("groupId");
        if (userId.empty() || groupId.empty()) {
            json_error(res, 400, "Missing userId or groupId");
            return;
        }
        Member copy;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            auto it = g_members.find(key_of(groupId, userId));
            if (it != g_members.end() && it->second.hasLoc) {
                copy = it->second;
                found = true;
            }
        }
        if (!found) {
            json_error(res, 404, "No data for user");
            return;
        }
        json resp;
        resp["userId"] = copy.userId;
        resp["groupId"] = copy.groupId;
        resp["name"] = copy.name;
        resp["phone"] = copy.phone;
        resp["lat"] = copy.lat;
        resp["lon"] = copy.lon;
        resp["ts"] = copy.ts;
        res.set_content(resp.dump(), "application/json");
    });

    server.Post("/profile", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = json::parse(req.body);
            if (!j.contains("userId") || !j.contains("groupId")) {
                json_error(res, 400, "Missing fields");
                return;
            }
            std::string userId = clip(j["userId"].get<std::string>(), 64);
            std::string groupId = clip(j["groupId"].get<std::string>(), 64);
            if (userId.empty() || groupId.empty()) {
                json_error(res, 400, "Missing fields");
                return;
            }
            std::string name = clip(j.value("name", ""), 80);
            std::string phone = clip(j.value("phone", ""), 40);
            bool wroteImage = false;
            long long imageTs = 0;
            unsigned long long imageHash = 0;
            if (j.contains("image") && j["image"].is_string()) {
                auto bytes = decode64(j["image"].get<std::string>());
                if (bytes.size() > 1400000) {
                    json_error(res, 400, "Image too large");
                    return;
                }
                if (!bytes.empty()) {
                    imageHash = image_hash(bytes);
                    bool unchanged = false;
                    {
                        std::lock_guard<std::mutex> lock(g_mu);
                        auto it = g_members.find(key_of(groupId, userId));
                        unchanged = it != g_members.end() && it->second.hasImage && it->second.imageHash == imageHash
                            && fs::exists(avatar_path(groupId, userId));
                    }
                    if (!unchanged) {
                        if (!write_avatar(groupId, userId, bytes)) {
                            json_error(res, 500, "Could not store photo");
                            return;
                        }
                        wroteImage = true;
                        imageTs = std::chrono::duration_cast<std::chrono::seconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count();
                    }
                }
            }
            {
                std::lock_guard<std::mutex> lock(g_mu);
                Member& m = ensure_member(groupId, userId);
                m.name = name;
                m.phone = phone;
                if (wroteImage) {
                    m.hasImage = true;
                    m.imageTs = imageTs;
                    m.imageHash = imageHash;
                }
                commit_locked();
            }
            json resp;
            resp["status"] = "saved";
            res.set_content(resp.dump(), "application/json");
        } catch (const std::exception& e) {
            json_error(res, 400, std::string("Invalid JSON: ") + e.what());
        }
    });

    server.Get("/group", [](const httplib::Request& req, httplib::Response& res) {
        std::string groupId = req.get_param_value("groupId");
        if (groupId.empty()) {
            json_error(res, 400, "Missing groupId");
            return;
        }
        json members = json::array();
        json places = json::array();
        long long nowS = server_now();
        bool aliveOn = g_aliveOn.load();
        int aliveSecs = g_aliveSecs.load();
        {
            std::lock_guard<std::mutex> lock(g_mu);
            for (const auto& kv : g_members) {
                const Member& m = kv.second;
                if (m.groupId != groupId) continue;
                json one;
                one["userId"] = m.userId;
                one["name"] = m.name.empty() ? m.userId : m.name;
                one["phone"] = m.phone;
                one["hasLoc"] = m.hasLoc;
                one["hasImage"] = m.hasImage;
                one["imageTs"] = m.imageTs;
                one["battery"] = m.battery;
                one["signal"] = m.signal;
                one["locationOff"] = m.locationOff;
                one["every"] = m.every;
                one["acc"] = m.acc;
                one["prov"] = m.prov;
                one["wifi"] = m.wifi;
                one["ssid"] = m.ssid;
                one["seen"] = m.seen;
                one["unreachable"] = aliveOn && m.seen > 0 && nowS > m.seen + static_cast<long long>(aliveSecs) + 25;
                if (m.hasLoc) {
                    one["lat"] = m.lat;
                    one["lon"] = m.lon;
                    one["ts"] = m.ts;
                    one["arrived"] = m.arrived > 0 ? m.arrived : m.ts;
                    one["area"] = m.area;
                    one["traveling"] = m.traveling;
                    one["speed"] = m.speed;
                    one["departed"] = m.departed;
                    one["departedFrom"] = m.departedFrom;
                }
                members.push_back(one);
            }
            for (const auto& kv : g_places) {
                const Place& p = kv.second;
                if (p.groupId != groupId) continue;
                places.push_back({
                    {"id", p.id},
                    {"name", p.name},
                    {"lat", p.lat},
                    {"lon", p.lon},
                    {"radius", p.radius},
                    {"hasImage", p.hasImage},
                    {"imageTs", p.imageTs},
                    {"ssid", p.ssid},
                });
            }
        }
        json resp;
        resp["groupId"] = groupId;
        resp["members"] = members;
        resp["places"] = places;
        resp["aliveEvery"] = aliveOn ? aliveSecs : 0;
        {
            std::lock_guard<std::mutex> lock(g_wait_mu);
            resp["rev"] = g_rev;
        }
        res.set_content(resp.dump(), "application/json");
    });

    server.Get("/avatar", [](const httplib::Request& req, httplib::Response& res) {
        std::string userId = req.get_param_value("userId");
        std::string groupId = req.get_param_value("groupId");
        if (userId.empty() || groupId.empty()) {
            json_error(res, 400, "Missing userId or groupId");
            return;
        }
        fs::path path = avatar_path(groupId, userId);
        if (!fs::exists(path)) {
            res.status = 404;
            res.set_content("missing", "text/plain");
            return;
        }
        std::ifstream in(path, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        res.set_header("Cache-Control", "no-store");
        res.set_content(ss.str(), "image/jpeg");
    });

    server.Get("/placeimg", [](const httplib::Request& req, httplib::Response& res) {
        std::string id = req.get_param_value("id");
        std::string groupId = req.get_param_value("groupId");
        if (id.empty() || groupId.empty()) {
            json_error(res, 400, "Missing id or groupId");
            return;
        }
        fs::path path = place_image_path(groupId, id);
        if (!fs::exists(path)) {
            res.status = 404;
            res.set_content("missing", "text/plain");
            return;
        }
        std::ifstream in(path, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        res.set_header("Cache-Control", "no-store");
        res.set_content(ss.str(), "image/jpeg");
    });

    server.Post("/place", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = json::parse(req.body);
            std::string groupId = clip(j.value("groupId", ""), 64);
            std::string id = clip(j.value("id", ""), 64);
            std::string name = clip(j.value("name", ""), 80);
            if (groupId.empty() || id.empty() || name.empty() || !j.contains("lat") || !j.contains("lon")) {
                json_error(res, 400, "Missing fields");
                return;
            }
            double lat = j["lat"].get<double>();
            double lon = j["lon"].get<double>();
            double radius = j.value("radius", 100.0);
            if (!valid_coord(lat, lon) || radius < 20 || radius > 5000) {
                json_error(res, 400, "Bad place");
                return;
            }
            bool clearImage = false;
            bool wroteImage = false;
            long long imageTs = 0;
            unsigned long long imageHash = 0;
            if (j.contains("image") && j["image"].is_string()) {
                auto bytes = decode64(j["image"].get<std::string>());
                if (bytes.empty()) {
                    clearImage = true;
                } else if (bytes.size() > 500000) {
                    json_error(res, 400, "Image too large");
                    return;
                } else {
                    imageHash = image_hash(bytes);
                    if (!write_place_image(groupId, id, bytes)) {
                        json_error(res, 500, "Could not store photo");
                        return;
                    }
                    wroteImage = true;
                    imageTs = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
                }
            }
            {
                std::lock_guard<std::mutex> lock(g_mu);
                Place p;
                auto old = g_places.find(key_of(groupId, id));
                if (old != g_places.end()) p = old->second;
                p.id = id;
                p.groupId = groupId;
                p.name = name;
                p.lat = lat;
                p.lon = lon;
                p.radius = radius;
                if (j.contains("ssid") && j["ssid"].is_string()) p.ssid = clip(j["ssid"].get<std::string>(), 64);
                if (wroteImage) {
                    p.hasImage = true;
                    p.imageTs = imageTs;
                    p.imageHash = imageHash;
                } else if (clearImage) {
                    p.hasImage = false;
                    p.imageTs = 0;
                    p.imageHash = 0;
                }
                g_places[key_of(groupId, id)] = p;
                commit_locked();
            }
            if (clearImage) {
                std::error_code ec;
                fs::remove(place_image_path(groupId, id), ec);
            }
            json resp;
            resp["status"] = "saved";
            res.set_content(resp.dump(), "application/json");
        } catch (const std::exception& e) {
            json_error(res, 400, std::string("Invalid JSON: ") + e.what());
        }
    });

    server.Post("/place/delete", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = json::parse(req.body);
            std::string groupId = j.value("groupId", "");
            std::string id = j.value("id", "");
            if (groupId.empty() || id.empty()) {
                json_error(res, 400, "Missing fields");
                return;
            }
            {
                std::lock_guard<std::mutex> lock(g_mu);
                g_places.erase(key_of(groupId, id));
                commit_locked();
            }
            {
                std::error_code ec;
                fs::remove(place_image_path(groupId, id), ec);
            }
            json resp;
            resp["status"] = "deleted";
            res.set_content(resp.dump(), "application/json");
        } catch (const std::exception& e) {
            json_error(res, 400, std::string("Invalid JSON: ") + e.what());
        }
    });

    server.Post("/group/delete", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = json::parse(req.body);
            std::string groupId = j.value("groupId", "");
            if (groupId.empty()) {
                json_error(res, 400, "Missing groupId");
                return;
            }
            std::vector<std::string> users;
            std::vector<std::string> placeIds;
            {
                std::lock_guard<std::mutex> lock(g_mu);
                for (auto it = g_members.begin(); it != g_members.end();) {
                    if (it->second.groupId == groupId) {
                        users.push_back(it->second.userId);
                        it = g_members.erase(it);
                    } else {
                        ++it;
                    }
                }
                for (auto it = g_places.begin(); it != g_places.end();) {
                    if (it->second.groupId == groupId) {
                        placeIds.push_back(it->second.id);
                        it = g_places.erase(it);
                    } else {
                        ++it;
                    }
                }
                commit_locked();
            }
            for (const std::string& user : users) {
                std::error_code ec;
                fs::remove(avatar_path(groupId, user), ec);
            }
            for (const std::string& id : placeIds) {
                std::error_code ec;
                fs::remove(place_image_path(groupId, id), ec);
            }
            json resp;
            resp["status"] = "deleted";
            res.set_content(resp.dump(), "application/json");
        } catch (const std::exception& e) {
            json_error(res, 400, std::string("Invalid JSON: ") + e.what());
        }
    });

    server.Get("/servercfg", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(server_cfg_json().dump(), "application/json");
    });

    server.Post("/servercfg", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = json::parse(req.body);
            int keep = g_historyKeep.load();
            bool on = g_aliveOn.load();
            int secs = g_aliveSecs.load();
            if (j.contains("historyKeep")) {
                if (!j["historyKeep"].is_number()) {
                    json_error(res, 400, "historyKeep out of range");
                    return;
                }
                keep = j["historyKeep"].get<int>();
                if (keep < 10 || keep > 5000) {
                    json_error(res, 400, "historyKeep out of range");
                    return;
                }
            }
            if (j.contains("aliveOn")) {
                if (j["aliveOn"].is_boolean()) on = j["aliveOn"].get<bool>();
                else if (j["aliveOn"].is_number()) on = j["aliveOn"].get<int>() != 0;
                else {
                    json_error(res, 400, "aliveOn");
                    return;
                }
            }
            if (j.contains("aliveSecs")) {
                if (!j["aliveSecs"].is_number()) {
                    json_error(res, 400, "aliveSecs out of range");
                    return;
                }
                secs = j["aliveSecs"].get<int>();
                if (secs < 20 || secs > 3600) {
                    json_error(res, 400, "aliveSecs out of range");
                    return;
                }
            }
            g_historyKeep.store(keep);
            g_aliveOn.store(on);
            g_aliveSecs.store(secs);
            save_server_cfg();
            {
                std::lock_guard<std::mutex> lock(g_mu);
                trim_history_locked();
                save_history_locked();
            }
            res.set_content(server_cfg_json().dump(), "application/json");
        } catch (const std::exception& e) {
            json_error(res, 400, std::string("Invalid JSON: ") + e.what());
        }
    });

    server.Get("/wait", [](const httplib::Request& req, httplib::Response& res) {
        long long since = 0;
        try {
            since = std::stoll(req.get_param_value("rev"));
        } catch (...) {
            since = 0;
        }
        long long now = 0;
        {
            std::unique_lock<std::mutex> lock(g_wait_mu);
            g_cv.wait_for(lock, std::chrono::seconds(20), [&] { return g_stopping.load() || g_rev != since; });
            now = g_rev;
        }
        json resp;
        resp["rev"] = now;
        res.set_content(resp.dump(), "application/json");
    });
}

static fs::path exe_dir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0) return fs::current_path();
    return fs::path(buf).parent_path();
#else
    std::error_code ec;
    fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (ec) return fs::current_path();
    return self.parent_path();
#endif
}

static void configure_data_dir() {
    g_root = exe_dir() / "data";
    fs::create_directories(g_root / "avatars");
    load_server_cfg();
    load_store();
}

#ifdef _WIN32

static const int ID_LIST_PEOPLE = 100;
static const int ID_LIST_PLACES = 101;
static const int ID_OPEN = 103;
static HWND g_hwnd = nullptr;
static HWND g_people = nullptr;
static HWND g_placeList = nullptr;
static HFONT g_font = nullptr;

static std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

static std::string stamp(long long ts) {
    if (ts <= 0) return "";
    std::time_t t = static_cast<std::time_t>(ts);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%m-%d %H:%M", &tm);
    return buf;
}

static void add_column(HWND list, int i, const wchar_t* name, int w) {
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.pszText = const_cast<wchar_t*>(name);
    col.cx = w;
    col.iSubItem = i;
    SendMessageW(list, LVM_INSERTCOLUMNW, i, reinterpret_cast<LPARAM>(&col));
}

static void add_item(HWND list, int row, const std::vector<std::wstring>& cells) {
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.pszText = const_cast<wchar_t*>(cells[0].c_str());
    SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
    for (int c = 1; c < static_cast<int>(cells.size()); c++) {
        LVITEMW sub{};
        sub.iSubItem = c;
        sub.pszText = const_cast<wchar_t*>(cells[c].c_str());
        SendMessageW(list, LVM_SETITEMTEXTW, row, reinterpret_cast<LPARAM>(&sub));
    }
}

static HWND make_list(HWND parent, int id, int x, int y, int w, int height);

#include "map_win.inc"

static void fill_lists() {
    refresh_address_labels();
    if (!g_live) {
        if (g_roster) InvalidateRect(g_roster, nullptr, FALSE);
        if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    std::vector<Member> members;
    std::vector<Place> places;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        for (const auto& kv : g_members) members.push_back(kv.second);
        for (const auto& kv : g_places) places.push_back(kv.second);
    }
    std::string sig;
    for (const Member& m : members) {
        sig += m.groupId + m.userId + m.name + std::to_string(m.lat) + std::to_string(m.lon) + std::to_string(m.imageTs) + m.area + std::to_string(m.battery) + std::to_string(m.signal) + (m.locationOff ? "L" : "") + (m.traveling ? "T" : "") + std::to_string(m.every) + std::to_string(m.ts);
    }
    for (const Place& p : places) sig += p.id + p.name + std::to_string(p.lat) + std::to_string(p.lon) + std::to_string(p.radius) + std::to_string(p.imageTs);
    bool changed = sig != g_list_sig;
    g_list_sig = sig;
    g_row_people = members;
    g_row_places = places;
    g_n_people = static_cast<int>(members.size());
    g_n_places = static_cast<int>(places.size());
    if (g_roster) InvalidateRect(g_roster, nullptr, FALSE);
    if (changed && g_map) InvalidateRect(g_map, nullptr, FALSE);
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void stop_everything() {
    g_stopping = true;
    g_tile_quit = true;
    g_job_cv.notify_all();
    g_cv.notify_all();
    if (g_server) {
        httplib::Server* server = g_server;
        g_server = nullptr;
        server->stop();
    }
}

static void open_data() {
    fs::create_directories(g_root);
    ShellExecuteW(g_hwnd, L"open", g_root.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

struct SettingsState {
    HWND tail = nullptr;
    HWND keepEdit = nullptr;
    HWND secsEdit = nullptr;
    HWND aliveBox = nullptr;
    bool ok = false;
    bool done = false;
    bool alive = true;
    int keep = 50;
    int secs = 75;
    std::wstring tailText;
};

static LRESULT CALLBACK settings_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<SettingsState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_COMMAND:
        if (!st) break;
        if (LOWORD(wp) == 1) {
            wchar_t tail[512];
            wchar_t keep[32];
            wchar_t secs[32];
            tail[0] = 0;
            keep[0] = 0;
            secs[0] = 0;
            if (st->tail) GetWindowTextW(st->tail, tail, 512);
            if (st->keepEdit) GetWindowTextW(st->keepEdit, keep, 32);
            if (st->secsEdit) GetWindowTextW(st->secsEdit, secs, 32);
            int keepN = _wtoi(keep);
            int secsN = _wtoi(secs);
            if (keepN < 10 || keepN > 5000) {
                MessageBoxW(hwnd, L"History kept per person must be from 10 to 5000.", L"Settings", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            if (secsN < 20 || secsN > 3600) {
                MessageBoxW(hwnd, L"The alive check must be from 20 to 3600 seconds.", L"Settings", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            st->keep = keepN;
            st->secs = secsN;
            st->alive = st->aliveBox && SendMessageW(st->aliveBox, BM_GETCHECK, 0, 0) == BST_CHECKED;
            st->tailText = tail;
            st->ok = true;
            st->done = true;
            DestroyWindow(hwnd);
        }
        if (LOWORD(wp) == 2) {
            st->done = true;
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:
        if (st) st->done = true;
        DestroyWindow(hwnd);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return 0;
}

static void open_settings(HWND owner) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = settings_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"FamilySettings";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&wc);
        registered = true;
    }
    SettingsState st;
    st.keep = g_historyKeep.load();
    st.secs = g_aliveSecs.load();
    st.alive = g_aliveOn.load();
    {
        std::lock_guard<std::mutex> lock(g_cfg_mu);
        st.tailText = widen(g_tail_saved);
    }
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"FamilySettings", L"Settings",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 520, 420,
        owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));
    CreateWindowExW(0, L"STATIC", L"Saved Tailscale address", WS_CHILD | WS_VISIBLE, 16, 14, 470, 18, hwnd, nullptr, nullptr, nullptr);
    CreateWindowExW(0, L"STATIC", L"Phones away from home use this. Include http:// and :8000.", WS_CHILD | WS_VISIBLE, 16, 34, 470, 18, hwnd, nullptr, nullptr, nullptr);
    st.tail = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", st.tailText.c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 16, 56, 476, 26, hwnd, nullptr, nullptr, nullptr);
    CreateWindowExW(0, L"STATIC", L"Location history kept per person (10 to 5000)", WS_CHILD | WS_VISIBLE, 16, 96, 470, 18, hwnd, nullptr, nullptr, nullptr);
    st.keepEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(st.keep).c_str(), WS_CHILD | WS_VISIBLE | ES_NUMBER, 16, 118, 160, 26, hwnd, nullptr, nullptr, nullptr);
    st.aliveBox = CreateWindowExW(0, L"BUTTON", L"Ask phones if they are reachable", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 16, 160, 360, 24, hwnd, nullptr, nullptr, nullptr);
    SendMessageW(st.aliveBox, BM_SETCHECK, st.alive ? BST_CHECKED : BST_UNCHECKED, 0);
    CreateWindowExW(0, L"STATIC", L"Seconds between alive checks (20 to 3600)", WS_CHILD | WS_VISIBLE, 16, 196, 470, 18, hwnd, nullptr, nullptr, nullptr);
    st.secsEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(st.secs).c_str(), WS_CHILD | WS_VISIBLE | ES_NUMBER, 16, 218, 160, 26, hwnd, nullptr, nullptr, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 280, 330, 96, 30, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(1)), nullptr, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 388, 330, 96, 30, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(2)), nullptr, nullptr);
    EnumChildWindows(hwnd, [](HWND child, LPARAM font) -> BOOL {
        SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(g_font));
    EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(st.tail);
    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (!st.ok) return;
    g_historyKeep.store(st.keep);
    g_aliveOn.store(st.alive);
    g_aliveSecs.store(st.secs);
    save_tail_text(st.tailText);
    {
        std::lock_guard<std::mutex> lock(g_mu);
        trim_history_locked();
        save_history_locked();
    }
    fill_lists();
}

static void layout_main(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    int w = rc.right;
    int h = rc.bottom;
    if (w < 80 || h < 80) return;
    const int margin = 16;
    const int header = 86;
    const int footer = 56;
    int top = header + 12;
    int content = h - top - footer - 8;
    if (content < 80) content = 80;
    int rosterW = w / 3;
    if (rosterW < 300) rosterW = 300;
    if (rosterW > 440) rosterW = 440;
    int mapW = w - margin * 3 - rosterW;
    if (mapW < 180) {
        mapW = (w - margin * 3) / 2;
        rosterW = w - margin * 3 - mapW;
        if (rosterW < 160) rosterW = 160;
        mapW = w - margin * 3 - rosterW;
        if (mapW < 80) mapW = 80;
    }
    if (g_map) {
        MoveWindow(g_map, margin, top, mapW, content, TRUE);
        round_child(g_map, 16);
    }
    if (g_roster) MoveWindow(g_roster, margin + mapW + margin, top, rosterW, content, TRUE);
}

static void sync_menu(HWND hwnd) {
    HMENU bar = GetMenu(hwnd);
    if (!bar) return;
    CheckMenuItem(bar, 121, MF_BYCOMMAND | (g_satellite ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(bar, 125, MF_BYCOMMAND | (!g_live ? MF_CHECKED : MF_UNCHECKED));
}

static const UINT WM_TRAYICON = WM_APP + 21;
static NOTIFYICONDATAW g_nid{};
static bool g_tray_on = false;
static bool g_hiding_to_tray = false;

static void remove_tray() {
    if (!g_tray_on) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_tray_on = false;
}

static void add_tray(HWND hwnd) {
    if (g_tray_on) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    lstrcpynW(nid.szTip, L"Family Location Server", static_cast<int>(sizeof(nid.szTip) / sizeof(nid.szTip[0])));
    if (Shell_NotifyIconW(NIM_ADD, &nid)) {
        g_nid = nid;
        g_tray_on = true;
    }
}

static void show_from_tray(HWND hwnd) {
    ShowWindow(hwnd, SW_SHOW);
    ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
    remove_tray();
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    try {
    switch (msg) {
    case WM_CREATE:
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint_shell(hwnd);
        return 0;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) {
            if (!g_hiding_to_tray) {
                g_hiding_to_tray = true;
                ShowWindow(hwnd, SW_HIDE);
                add_tray(hwnd);
                g_hiding_to_tray = false;
            }
            return 0;
        }
        layout_main(hwnd);
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_CLOSE) {
            DestroyWindow(hwnd);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_INITMENUPOPUP:
        sync_menu(hwnd);
        return 0;
    case WM_TRAYICON:
        if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) {
            show_from_tray(hwnd);
        } else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 310, L"Open");
            AppendMenuW(menu, MF_STRING, 201, L"Exit");
            POINT pt{};
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            PostMessageW(hwnd, WM_NULL, 0, 0);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_OPEN) open_data();
        if (LOWORD(wp) == 200) open_settings(hwnd);
        if (LOWORD(wp) == 310) show_from_tray(hwnd);
        if (LOWORD(wp) == 201) DestroyWindow(hwnd);
        if (LOWORD(wp) == 211) open_checkin_db(hwnd);
        if (LOWORD(wp) == 121) {
            toggle_satellite();
            sync_menu(hwnd);
        }
        if (LOWORD(wp) == 125) {
            g_live = !g_live;
            if (g_live) g_list_sig.clear();
            fill_lists();
            sync_menu(hwnd);
        }
        return 0;
    case WM_TIMER:
        fill_lists();
        return 0;
    case WM_DESTROY:
        remove_tray();
        KillTimer(hwnd, 1);
        stop_everything();
        PostQuitMessage(0);
        ExitProcess(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    } catch (...) {
        return 0;
    }
}

static HWND make_list(HWND parent, int id, int x, int y, int w, int height) {
    HWND hwnd = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
        x, y, w, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    ListView_SetExtendedListViewStyle(hwnd, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    return hwnd;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    SetProcessDPIAware();
    configure_data_dir();

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);
    HFONT ui = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_font = ui ? ui : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"FamilyLocationServer";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Family Location Server",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 860,
        nullptr, nullptr, inst, nullptr);

    Gdiplus::GdiplusStartupInput gdip_in;
    Gdiplus::GdiplusStartup(&g_gdip, &gdip_in, nullptr);
    load_tail_setting();

    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, 200, L"Settings...");
    AppendMenuW(file, MF_STRING, 211, L"Check-in database...");
    AppendMenuW(file, MF_STRING, ID_OPEN, L"Open saved data");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, 201, L"Exit");
    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, 121, L"Satellite");
    AppendMenuW(view, MF_STRING, 125, L"Pause updates");
    HMENU bar = CreateMenu();
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"File");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"View");
    SetMenu(g_hwnd, bar);

    create_map(g_hwnd);
    create_roster(g_hwnd);

    g_server = new httplib::Server();
    setup_routes(*g_server);
    std::thread([] {
        if (!g_server->listen("0.0.0.0", 8000)) {
            g_status = "Port 8000 is already in use";
        }
    }).detach();
    g_status = "Listening on port 8000";

    fill_lists();
    ShowWindow(g_hwnd, show);
    SetTimer(g_hwnd, 1, 1000, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    stop_everything();
    ExitProcess(0);
    return 0;
}

#else

int main() {
    configure_data_dir();
    httplib::Server server;
    g_server = &server;
    setup_routes(server);
    std::cout << "Server listening on port 8000\n";
    if (!server.listen("0.0.0.0", 8000)) {
        std::cerr << "Port 8000 is already in use\n";
        return 1;
    }
    return 0;
}

#endif

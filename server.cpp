// Family location server.
// Listens on 0.0.0.0:8000 so the same program answers on the home LAN and on Tailscale.
// Plain HTTP, no password, as the test plan specified. Keep port 8000 off the public internet.
//
//   GET  /health
//   POST /send      {userId, groupId, lat, lon, ts?}
//   GET  /fetch?userId=&groupId=
//   POST /profile   {userId, groupId, name, phone, image?}   image is base64 JPEG
//   GET  /group?groupId=
//   GET  /avatar?groupId=&userId=
//   POST /place     {groupId, id, name, lat, lon, radius}
//   POST /place/delete {groupId, id}

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
};

struct Place {
    std::string id;
    std::string groupId;
    std::string name;
    double lat = 0;
    double lon = 0;
    double radius = 100;
};

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

static void commit_locked() {
    save_locked();
    {
        std::lock_guard<std::mutex> lock(g_wait_mu);
        g_rev++;
    }
    g_cv.notify_all();
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
            g_places[key_of(p.groupId, p.id)] = p;
        }
    }
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

static void setup_routes(httplib::Server& server) {
    server.set_payload_max_length(2 * 1024 * 1024);

    server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("OK", "text/plain");
    });

    server.Post("/send", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = json::parse(req.body);
            if (!j.contains("userId") || !j.contains("groupId") || !j.contains("lat") || !j.contains("lon")) {
                json_error(res, 400, "Missing fields");
                return;
            }
            std::string userId = clip(j["userId"].get<std::string>(), 64);
            std::string groupId = clip(j["groupId"].get<std::string>(), 64);
            double lat = j["lat"].get<double>();
            double lon = j["lon"].get<double>();
            if (userId.empty() || groupId.empty() || !valid_coord(lat, lon)) {
                json_error(res, 400, "Bad location");
                return;
            }
            long long ts = 0;
            if (j.contains("ts")) ts = j["ts"].get<long long>();
            if (ts <= 0) {
                ts = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
            }
            {
                std::lock_guard<std::mutex> lock(g_mu);
                Member& m = ensure_member(groupId, userId);
                bool moved = !m.hasLoc || m.arrived <= 0 || haversine_m(m.anchorLat, m.anchorLon, lat, lon) > 120.0;
                if (moved) {
                    m.arrived = ts;
                    m.anchorLat = lat;
                    m.anchorLon = lon;
                }
                m.lat = lat;
                m.lon = lon;
                m.ts = ts;
                m.hasLoc = true;
                m.area.clear();
                for (const auto& kv : g_places) {
                    const Place& p = kv.second;
                    if (p.groupId != groupId) continue;
                    if (haversine_m(lat, lon, p.lat, p.lon) <= p.radius) {
                        m.area = p.name;
                        break;
                    }
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
            if (j.contains("image") && j["image"].is_string()) {
                auto bytes = decode64(j["image"].get<std::string>());
                if (bytes.size() > 400000) {
                    json_error(res, 400, "Image too large");
                    return;
                }
                if (!bytes.empty()) {
                    fs::create_directories(g_root / "avatars");
                    fs::path path = avatar_path(groupId, userId);
                    std::ofstream out(path, std::ios::binary);
                    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                    wroteImage = true;
                    imageTs = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
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
                if (m.hasLoc) {
                    one["lat"] = m.lat;
                    one["lon"] = m.lon;
                    one["ts"] = m.ts;
                    one["arrived"] = m.arrived > 0 ? m.arrived : m.ts;
                    one["area"] = m.area;
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
                });
            }
        }
        json resp;
        resp["groupId"] = groupId;
        resp["members"] = members;
        resp["places"] = places;
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
            {
                std::lock_guard<std::mutex> lock(g_mu);
                Place p;
                p.id = id;
                p.groupId = groupId;
                p.name = name;
                p.lat = lat;
                p.lon = lon;
                p.radius = radius;
                g_places[key_of(groupId, id)] = p;
                commit_locked();
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
                    if (it->second.groupId == groupId) it = g_places.erase(it);
                    else ++it;
                }
                commit_locked();
            }
            for (const std::string& user : users) {
                std::error_code ec;
                fs::remove(avatar_path(groupId, user), ec);
            }
            json resp;
            resp["status"] = "deleted";
            res.set_content(resp.dump(), "application/json");
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
    load_store();
}

#ifdef _WIN32

static const int ID_LIST_PEOPLE = 100;
static const int ID_LIST_PLACES = 101;
static const int ID_STATUS = 102;
static const int ID_OPEN = 103;
static const int ID_NOTE = 104;
static HWND g_hwnd = nullptr;
static HWND g_people = nullptr;
static HWND g_placeList = nullptr;
static HWND g_statusLabel = nullptr;
static HWND g_note = nullptr;
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

static void set_text(HWND h, const std::wstring& s) {
    SetWindowTextW(h, s.c_str());
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

#include "map_win.inc"

static void fill_lists() {
    refresh_address_labels();
    if (!g_live) {
        set_text(g_statusLabel, widen(g_status) + L"    live updates paused");
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
        sig += m.groupId + m.userId + m.name + std::to_string(m.lat) + std::to_string(m.lon) + std::to_string(m.imageTs) + m.area;
    }
    for (const Place& p : places) sig += p.id + p.name + std::to_string(p.lat);
    bool changed = sig != g_list_sig;
    g_list_sig = sig;
    g_row_people = members;
    g_row_places = places;
    int nPeople = static_cast<int>(members.size());
    int nPlaces = static_cast<int>(places.size());
    set_text(g_statusLabel, widen(g_status) + L"    " + std::to_wstring(nPeople) + L" people    " + std::to_wstring(nPlaces) + L" places");
    if (g_roster) InvalidateRect(g_roster, nullptr, FALSE);
    if (changed && g_map) InvalidateRect(g_map, nullptr, FALSE);
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
    HWND edit = nullptr;
    bool ok = false;
    bool done = false;
    std::wstring value;
};

static LRESULT CALLBACK settings_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<SettingsState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_COMMAND:
        if (!st) break;
        if (LOWORD(wp) == 1) {
            wchar_t buf[512];
            buf[0] = 0;
            if (st->edit) GetWindowTextW(st->edit, buf, 512);
            st->value = buf;
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
    st.value = widen(g_tail_saved);
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"FamilySettings", L"Settings",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 480, 210,
        owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));
    HWND title = CreateWindowExW(0, L"STATIC", L"Saved Tailscale address", WS_CHILD | WS_VISIBLE, 16, 14, 440, 18, hwnd, nullptr, nullptr, nullptr);
    HWND hint = CreateWindowExW(0, L"STATIC", L"Phones away from home use this. Include http:// and :8000.", WS_CHILD | WS_VISIBLE, 16, 36, 440, 18, hwnd, nullptr, nullptr, nullptr);
    st.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", st.value.c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 16, 62, 440, 26, hwnd, nullptr, nullptr, nullptr);
    HWND save = CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 248, 108, 96, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(1)), nullptr, nullptr);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 356, 108, 96, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(2)), nullptr, nullptr);
    SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    SendMessageW(hint, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    SendMessageW(st.edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    SendMessageW(save, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    SendMessageW(cancel, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(st.edit);
    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (st.ok) save_tail_text(st.value);
}

static void layout_main(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    int w = rc.right;
    int h = rc.bottom;
    if (w < 80 || h < 80) return;
    const int margin = 16;
    if (g_homeLabel) MoveWindow(g_homeLabel, margin, 10, w - margin * 2, 22, TRUE);
    if (g_tailLive) MoveWindow(g_tailLive, margin, 34, w - margin * 2, 22, TRUE);
    int top = 66;
    int content = h - top - 56;
    if (content < 80) content = 80;
    int rosterW = w / 3;
    if (rosterW < 280) rosterW = 280;
    if (rosterW > 460) rosterW = 460;
    int mapW = w - margin * 3 - rosterW;
    if (mapW < 180) {
        mapW = (w - margin * 3) / 2;
        rosterW = w - margin * 3 - mapW;
    }
    if (g_map) MoveWindow(g_map, margin, top, mapW, content, TRUE);
    if (g_roster) MoveWindow(g_roster, margin + mapW + margin, top, rosterW, content, TRUE);
    if (g_statusLabel) MoveWindow(g_statusLabel, margin, top + content + 8, w - margin * 2, 18, TRUE);
    if (g_note) MoveWindow(g_note, margin, top + content + 28, w - margin * 2, 22, TRUE);
}

static void sync_menu(HWND hwnd) {
    HMENU bar = GetMenu(hwnd);
    if (!bar) return;
    CheckMenuItem(bar, 121, MF_BYCOMMAND | (g_satellite ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(bar, 125, MF_BYCOMMAND | (!g_live ? MF_CHECKED : MF_UNCHECKED));
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        return 0;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) layout_main(hwnd);
        return DefWindowProcW(hwnd, msg, wp, lp);
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
    case WM_COMMAND:
        if (LOWORD(wp) == ID_OPEN) open_data();
        if (LOWORD(wp) == 200) open_settings(hwnd);
        if (LOWORD(wp) == 201) DestroyWindow(hwnd);
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
        KillTimer(hwnd, 1);
        stop_everything();
        PostQuitMessage(0);
        ExitProcess(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

static HWND make_label(HWND parent, const wchar_t* text, int x, int y, int w, int height, int id) {
    HWND hwnd = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return hwnd;
}

static HWND make_button(HWND parent, const wchar_t* text, int x, int y, int w, int height, int id) {
    HWND hwnd = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP, x, y, w, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return hwnd;
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
    g_font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"FamilyLocationServer";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Family location server",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1200, 820,
        nullptr, nullptr, inst, nullptr);

    Gdiplus::GdiplusStartupInput gdip_in;
    Gdiplus::GdiplusStartup(&g_gdip, &gdip_in, nullptr);
    load_tail_setting();

    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, 200, L"Settings...");
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

    g_homeLabel = make_label(g_hwnd, L"Home", 16, 8, 400, 20, 0);
    g_tailLive = make_label(g_hwnd, L"Tailscale", 16, 30, 400, 20, 0);

    create_map(g_hwnd);
    create_roster(g_hwnd);

    g_statusLabel = make_label(g_hwnd, L"Starting", 16, 610, 400, 20, ID_STATUS);
    g_note = make_label(g_hwnd,
        L"File, Settings holds the saved Tailscale address. Minimize stays on the taskbar. Close stops the program.",
        16, 636, 400, 22, ID_NOTE);

    g_server = new httplib::Server();
    setup_routes(*g_server);
    std::thread([] {
        if (!g_server->listen("0.0.0.0", 8000)) {
            g_status = "Port 8000 is already in use";
        }
    }).detach();
    g_status = "Listening on port 8000  (LAN and Tailscale)";

    ShowWindow(g_hwnd, show);
    SetTimer(g_hwnd, 1, 1000, nullptr);
    fill_lists();

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

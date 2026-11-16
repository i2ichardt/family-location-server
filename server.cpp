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

#include <chrono>
#include <cmath>
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
                m.lat = lat;
                m.lon = lon;
                m.ts = ts;
                m.hasLoc = true;
                save_locked();
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
                save_locked();
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
                save_locked();
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
                save_locked();
            }
            json resp;
            resp["status"] = "deleted";
            res.set_content(resp.dump(), "application/json");
        } catch (const std::exception& e) {
            json_error(res, 400, std::string("Invalid JSON: ") + e.what());
        }
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

static const UINT WM_TRAY = WM_APP + 1;
static const int ID_TRAY_OPEN = 1;
static const int ID_TRAY_EXIT = 2;
static const int ID_LIST_PEOPLE = 100;
static const int ID_LIST_PLACES = 101;
static const int ID_STATUS = 102;
static const int ID_OPEN = 103;
static const int ID_NOTE = 104;
static HWND g_hwnd = nullptr;
static HWND g_people = nullptr;
static HWND g_placeList = nullptr;
static HWND g_statusLabel = nullptr;
static NOTIFYICONDATAW g_nid{};
static bool g_tray = false;
static bool g_really_quit = false;
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

static void fill_lists() {
    std::vector<Member> members;
    std::vector<Place> places;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        for (const auto& kv : g_members) members.push_back(kv.second);
        for (const auto& kv : g_places) places.push_back(kv.second);
    }
    SendMessageW(g_people, LVM_DELETEALLITEMS, 0, 0);
    int row = 0;
    for (const Member& m : members) {
        std::string where = m.hasLoc ? (std::to_string(m.lat).substr(0, 9) + ", " + std::to_string(m.lon).substr(0, 10)) : "no pin yet";
        std::string who = m.name.empty() ? m.userId : m.name;
        add_item(g_people, row++, {widen(m.groupId), widen(who), widen(m.phone), widen(where), widen(stamp(m.ts))});
    }
    SendMessageW(g_placeList, LVM_DELETEALLITEMS, 0, 0);
    row = 0;
    for (const Place& p : places) {
        add_item(g_placeList, row++, {
            widen(p.groupId),
            widen(p.name),
            widen(std::to_string(p.lat).substr(0, 9)),
            widen(std::to_string(p.lon).substr(0, 10)),
            widen(std::to_string(static_cast<int>(p.radius)) + " m"),
        });
    }
    int nPeople = static_cast<int>(members.size());
    int nPlaces = static_cast<int>(places.size());
    std::wstring status = widen(g_status) + L"    " + std::to_wstring(nPeople) + L" people    " + std::to_wstring(nPlaces) + L" places";
    set_text(g_statusLabel, status);
    if (g_tray) {
        std::wstring tip = L"Family location  " + std::to_wstring(nPeople) + L" people";
        if (tip.size() > 120) tip.resize(120);
        wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    }
}

static void add_tray(HWND hwnd) {
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"Family location server");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_tray = true;
}

static void remove_tray() {
    if (!g_tray) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_tray = false;
}

static void hide_to_tray(HWND hwnd) {
    ShowWindow(hwnd, SW_HIDE);
}

static void show_again(HWND hwnd) {
    ShowWindow(hwnd, SW_SHOW);
    ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
}

static void open_data() {
    fs::create_directories(g_root);
    ShellExecuteW(g_hwnd, L"open", g_root.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        return 0;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) {
            hide_to_tray(hwnd);
            return 0;
        }
        return 0;
    case WM_CLOSE:
        if (!g_really_quit) {
            hide_to_tray(hwnd);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_OPEN) open_data();
        if (LOWORD(wp) == ID_TRAY_OPEN) show_again(hwnd);
        if (LOWORD(wp) == ID_TRAY_EXIT) {
            g_really_quit = true;
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_TRAY:
        if (lp == WM_LBUTTONDBLCLK || lp == WM_LBUTTONUP) show_again(hwnd);
        if (lp == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN, L"Open");
            AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit");
            SetForegroundWindow(hwnd);
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
        }
        return 0;
    case WM_TIMER:
        fill_lists();
        return 0;
    case WM_DESTROY:
        remove_tray();
        if (g_server) g_server->stop();
        PostQuitMessage(0);
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
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 920, 640,
        nullptr, nullptr, inst, nullptr);

    make_label(g_hwnd, L"People", 16, 8, 200, 18, 0);
    g_people = make_list(g_hwnd, ID_LIST_PEOPLE, 16, 28, 880, 250);
    add_column(g_people, 0, L"Group", 110);
    add_column(g_people, 1, L"Name", 180);
    add_column(g_people, 2, L"Phone", 140);
    add_column(g_people, 3, L"Location", 240);
    add_column(g_people, 4, L"Updated", 120);

    make_label(g_hwnd, L"Shared places", 16, 286, 200, 18, 0);
    g_placeList = make_list(g_hwnd, ID_LIST_PLACES, 16, 306, 880, 180);
    add_column(g_placeList, 0, L"Group", 110);
    add_column(g_placeList, 1, L"Place", 220);
    add_column(g_placeList, 2, L"Lat", 140);
    add_column(g_placeList, 3, L"Lon", 140);
    add_column(g_placeList, 4, L"Radius", 100);

    g_statusLabel = make_label(g_hwnd, L"Starting", 16, 496, 700, 20, ID_STATUS);
    make_button(g_hwnd, L"Open saved data", 730, 492, 160, 28, ID_OPEN);
    make_label(g_hwnd,
        L"No password and no HTTPS. The phone uses http://<this PC>:8000 on your Wi-Fi, or the Tailscale address when it is away. Do not forward port 8000 to the internet. Closing this window hides it in the tray. Right-click the tray icon and choose Exit to stop.",
        16, 528, 880, 48, ID_NOTE);

    g_server = new httplib::Server();
    setup_routes(*g_server);
    std::thread([] {
        if (!g_server->listen("0.0.0.0", 8000)) {
            g_status = "Port 8000 is already in use";
        }
    }).detach();
    g_status = "Listening on port 8000  (LAN and Tailscale)";

    add_tray(g_hwnd);
    ShowWindow(g_hwnd, show);
    SetTimer(g_hwnd, 1, 1000, nullptr);
    fill_lists();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
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

#include <algorithm>
#include <cstdio>
#include <ole2.h>
#ifndef PROPID
typedef ULONG PROPID;
#endif
#include <gdiplus.h>
#include <iphlpapi.h>
#include <queue>
#include <set>
#include <winhttp.h>
#include <windowsx.h>

static HWND g_homeLabel = nullptr;
static HWND g_tailLive = nullptr;
static HWND g_map = nullptr;
static std::vector<Member> g_row_people;
static std::vector<Place> g_row_places;
static bool g_live = true;
static std::string g_list_sig;
static HWND g_roster = nullptr;
static std::string g_tail_saved;
static bool g_tail_seeded = false;
static bool g_satellite = false;

static std::string narrow_w(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

static std::string ipv4_of(const sockaddr* sa) {
    if (!sa || sa->sa_family != AF_INET) return "";
    auto* sin = reinterpret_cast<const sockaddr_in*>(sa);
    const unsigned char* p = reinterpret_cast<const unsigned char*>(&sin->sin_addr);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    return buf;
}

static void collect_ips(std::vector<std::string>& home, std::vector<std::string>& tail) {
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf(size);
    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size);
    }
    if (rc != NO_ERROR) return;
    for (auto* a = addrs; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            std::string ip = ipv4_of(u->Address.lpSockaddr);
            if (ip.empty() || ip == "127.0.0.1") continue;
            if (ip.rfind("100.", 0) == 0) tail.push_back(ip);
            else home.push_back(ip);
        }
    }
}

static void load_tail_setting() {
    fs::path path = g_root / "server.json";
    if (!fs::exists(path)) return;
    std::ifstream in(path);
    json j;
    try {
        in >> j;
    } catch (...) {
        return;
    }
    g_tail_saved = j.value("tailscale", "");
}

static void save_tail_setting() {
    json j;
    j["tailscale"] = g_tail_saved;
    std::ofstream out(g_root / "server.json", std::ios::binary);
    out << j.dump(2);
}

static void refresh_address_labels() {
    std::vector<std::string> home, tail;
    collect_ips(home, tail);
    std::string home_text = "Home  ";
    if (home.empty()) home_text += "no LAN address yet. Check Wi-Fi, then use http://<that address>:8000";
    else {
        for (size_t i = 0; i < home.size(); i++) {
            if (i) home_text += "    ";
            home_text += "http://" + home[i] + ":8000";
        }
    }
    std::string tail_text = "Tailscale on this PC  ";
    if (tail.empty()) tail_text += "not detected. Set the phone address in File, then Settings.";
    else {
        for (size_t i = 0; i < tail.size(); i++) {
            if (i) tail_text += "    ";
            tail_text += "http://" + tail[i] + ":8000";
        }
    }
    if (g_homeLabel) set_text(g_homeLabel, widen(home_text));
    if (g_tailLive) set_text(g_tailLive, widen(tail_text));
    if (!g_tail_seeded) {
        g_tail_seeded = true;
        if (g_tail_saved.empty() && !tail.empty()) {
            g_tail_saved = "http://" + tail[0] + ":8000";
            save_tail_setting();
        }
    }
}

static void save_tail_text(const std::wstring& raw) {
    g_tail_saved = narrow_w(raw);
    while (!g_tail_saved.empty() && (g_tail_saved.back() == ' ' || g_tail_saved.back() == '/')) g_tail_saved.pop_back();
    size_t start = 0;
    while (start < g_tail_saved.size() && g_tail_saved[start] == ' ') start++;
    if (start > 0) g_tail_saved.erase(0, start);
    if (!g_tail_saved.empty() && g_tail_saved.rfind("http://", 0) != 0 && g_tail_saved.rfind("https://", 0) != 0)
        g_tail_saved = "http://" + g_tail_saved;
    save_tail_setting();
}

static int selected_row(HWND list) {
    return static_cast<int>(SendMessageW(list, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
}

static void delete_person() {
    int row = selected_row(g_people);
    if (row < 0 || row >= static_cast<int>(g_row_people.size())) {
        MessageBoxW(g_hwnd, L"Select a person first.", L"Family", MB_OK | MB_ICONINFORMATION);
        return;
    }
    Member m = g_row_people[row];
    std::wstring who = widen(m.name.empty() ? m.userId : m.name);
    if (MessageBoxW(g_hwnd, (L"Remove " + who + L" from " + widen(m.groupId) + L"?").c_str(), L"Family", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_members.erase(key_of(m.groupId, m.userId));
        commit_locked();
    }
    std::error_code ec;
    fs::remove(avatar_path(m.groupId, m.userId), ec);
}

static void delete_group() {
    std::string group;
    int person = selected_row(g_people);
    int place = selected_row(g_placeList);
    if (person >= 0 && person < static_cast<int>(g_row_people.size())) group = g_row_people[person].groupId;
    else if (place >= 0 && place < static_cast<int>(g_row_places.size())) group = g_row_places[place].groupId;
    if (group.empty()) {
        MessageBoxW(g_hwnd, L"Select a person or a place in the group you want to delete.", L"Family", MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring ask = L"Delete group " + widen(group) + L" and everyone in it? This removes their pins, photos, and places.";
    if (MessageBoxW(g_hwnd, ask.c_str(), L"Family", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    std::vector<std::string> users;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        for (auto it = g_members.begin(); it != g_members.end();) {
            if (it->second.groupId == group) {
                users.push_back(it->second.userId);
                it = g_members.erase(it);
            } else ++it;
        }
        for (auto it = g_places.begin(); it != g_places.end();) {
            if (it->second.groupId == group) it = g_places.erase(it);
            else ++it;
        }
        commit_locked();
    }
    for (const std::string& user : users) {
        std::error_code ec;
        fs::remove(avatar_path(group, user), ec);
    }
}

static void delete_place() {
    int row = selected_row(g_placeList);
    if (row < 0 || row >= static_cast<int>(g_row_places.size())) {
        MessageBoxW(g_hwnd, L"Select a place first.", L"Family", MB_OK | MB_ICONINFORMATION);
        return;
    }
    Place p = g_row_places[row];
    if (MessageBoxW(g_hwnd, (L"Delete place " + widen(p.name) + L"?").c_str(), L"Family", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_places.erase(key_of(p.groupId, p.id));
        commit_locked();
    }
}

struct MapState {
    double lat = 39.95;
    double lon = -75.16;
    int zoom = 4;
    bool moved = false;
    bool dragging = false;
    int last_x = 0;
    int last_y = 0;
    std::mutex mu;
    std::map<std::string, std::pair<IStream*, Gdiplus::Bitmap*>> tiles;
    std::set<std::string> pending;
};
static MapState g_map_state;
static std::queue<std::string> g_tile_jobs;
static std::mutex g_job_mu;
static std::condition_variable g_job_cv;
static bool g_tile_quit = false;
static ULONG_PTR g_gdip = 0;
static const UINT WM_TILE = WM_APP + 5;

static void project_xy(double lat, double lon, int zoom, double& x, double& y) {
    double n = 256.0 * static_cast<double>(1 << zoom);
    x = (lon + 180.0) / 360.0 * n;
    double s = std::sin(lat * 3.141592653589793 / 180.0);
    y = (0.5 - std::log((1.0 + s) / (1.0 - s)) / (4.0 * 3.141592653589793)) * n;
}

static void unproject_xy(double x, double y, int zoom, double& lat, double& lon) {
    double n = 256.0 * static_cast<double>(1 << zoom);
    lon = x / n * 360.0 - 180.0;
    double merc = 3.141592653589793 - 2.0 * 3.141592653589793 * y / n;
    lat = std::atan(std::sinh(merc)) * 180.0 / 3.141592653589793;
}

static std::vector<unsigned char> https_get(const std::wstring& host, const std::wstring& path) {
    std::vector<unsigned char> out;
    HINTERNET ses = WinHttpOpen(L"FamilyLocation/2", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return out;
    HINTERNET con = WinHttpConnect(ses, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!con) {
        WinHttpCloseHandle(ses);
        return out;
    }
    HINTERNET req = WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) {
        WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        return out;
    }
    BOOL ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        && WinHttpReceiveResponse(req, nullptr);
    if (ok) {
        DWORD avail = 0;
        do {
            if (!WinHttpQueryDataAvailable(req, &avail)) break;
            if (avail == 0) break;
            size_t at = out.size();
            out.resize(at + avail);
            DWORD read = 0;
            if (!WinHttpReadData(req, out.data() + at, avail, &read)) break;
            out.resize(at + read);
        } while (avail > 0);
    }
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return out;
}

static void tile_worker() {
    while (true) {
        std::string key;
        {
            std::unique_lock<std::mutex> lock(g_job_mu);
            g_job_cv.wait(lock, [] { return g_tile_quit || !g_tile_jobs.empty(); });
            if (g_tile_quit && g_tile_jobs.empty()) return;
            key = g_tile_jobs.front();
            g_tile_jobs.pop();
        }
        int sat = 0, z = 0, x = 0, y = 0;
        if (std::sscanf(key.c_str(), "%d/%d/%d/%d", &sat, &z, &x, &y) != 4) continue;
        wchar_t path[256];
        std::wstring host;
        if (sat) {
            host = L"server.arcgisonline.com";
            std::swprintf(path, 256, L"/ArcGIS/rest/services/World_Imagery/MapServer/tile/%d/%d/%d", z, y, x);
        } else {
            host = L"tile.openstreetmap.org";
            std::swprintf(path, 256, L"/%d/%d/%d.png", z, x, y);
        }
        auto bytes = https_get(host, path);
        if (bytes.size() < 32 || !g_map) continue;
        auto* msg = new std::pair<std::string, std::vector<unsigned char>>(key, std::move(bytes));
        if (!PostMessageW(g_map, WM_TILE, 0, reinterpret_cast<LPARAM>(msg))) delete msg;
    }
}

static void want_tile(const std::string& key) {
    {
        std::lock_guard<std::mutex> lock(g_map_state.mu);
        if (g_map_state.tiles.count(key) || g_map_state.pending.count(key)) return;
        if (g_map_state.pending.size() > 40) return;
        g_map_state.pending.insert(key);
    }
    {
        std::lock_guard<std::mutex> lock(g_job_mu);
        g_tile_jobs.push(key);
    }
    g_job_cv.notify_one();
}

static void remember_tile(const std::string& key, const std::vector<unsigned char>& bytes) {
    if (bytes.empty()) {
        std::lock_guard<std::mutex> lock(g_map_state.mu);
        g_map_state.pending.erase(key);
        return;
    }
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (!mem) return;
    void* dst = GlobalLock(mem);
    std::memcpy(dst, bytes.data(), bytes.size());
    GlobalUnlock(mem);
    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(mem, TRUE, &stream) != S_OK) {
        GlobalFree(mem);
        return;
    }
    auto* bmp = Gdiplus::Bitmap::FromStream(stream);
    std::lock_guard<std::mutex> lock(g_map_state.mu);
    g_map_state.pending.erase(key);
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) {
        delete bmp;
        stream->Release();
        return;
    }
    if (g_map_state.tiles.size() > 160) {
        for (auto& kv : g_map_state.tiles) {
            delete kv.second.second;
            if (kv.second.first) kv.second.first->Release();
        }
        g_map_state.tiles.clear();
    }
    g_map_state.tiles[key] = {stream, bmp};
}

static std::map<std::string, Gdiplus::Bitmap*> g_faces;
static std::map<std::string, long long> g_face_stamp;

static Gdiplus::Bitmap* face_bitmap(const Member& m) {
    if (!m.hasImage) return nullptr;
    std::string key = m.groupId + ":" + m.userId;
    auto have = g_face_stamp.find(key);
    auto bmp = g_faces.find(key);
    if (have != g_face_stamp.end() && have->second == m.imageTs && bmp != g_faces.end()) return bmp->second;
    fs::path path = avatar_path(m.groupId, m.userId);
    if (!fs::exists(path)) return nullptr;
    std::wstring wpath = path.wstring();
    auto* loaded = Gdiplus::Bitmap::FromFile(wpath.c_str());
    if (!loaded || loaded->GetLastStatus() != Gdiplus::Ok) {
        delete loaded;
        return nullptr;
    }
    if (bmp != g_faces.end() && bmp->second) delete bmp->second;
    g_faces[key] = loaded;
    g_face_stamp[key] = m.imageTs;
    return loaded;
}

static LRESULT CALLBACK map_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_LBUTTONDOWN:
        g_map_state.dragging = true;
        g_map_state.last_x = GET_X_LPARAM(lp);
        g_map_state.last_y = GET_Y_LPARAM(lp);
        SetCapture(hwnd);
        return 0;
    case WM_LBUTTONUP:
        g_map_state.dragging = false;
        ReleaseCapture();
        return 0;
    case WM_MOUSEMOVE:
        if (g_map_state.dragging) {
            int x = GET_X_LPARAM(lp);
            int y = GET_Y_LPARAM(lp);
            double px, py;
            project_xy(g_map_state.lat, g_map_state.lon, g_map_state.zoom, px, py);
            px -= (x - g_map_state.last_x);
            py -= (y - g_map_state.last_y);
            unproject_xy(px, py, g_map_state.zoom, g_map_state.lat, g_map_state.lon);
            g_map_state.last_x = x;
            g_map_state.last_y = y;
            g_map_state.moved = true;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (delta > 0 && g_map_state.zoom < 18) g_map_state.zoom++;
        if (delta < 0 && g_map_state.zoom > 3) g_map_state.zoom--;
        g_map_state.moved = true;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_TILE: {
        auto* got = reinterpret_cast<std::pair<std::string, std::vector<unsigned char>>*>(lp);
        if (got) {
            remember_tile(got->first, got->second);
            delete got;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP dib = CreateCompatibleBitmap(hdc, rc.right > 0 ? rc.right : 1, rc.bottom > 0 ? rc.bottom : 1);
        HGDIOBJ oldbmp = SelectObject(mem, dib);
        Gdiplus::Graphics g(mem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.Clear(Gdiplus::Color(255, 231, 226, 214));
        if (!g_map_state.moved) {
            double lat = 0, lon = 0;
            int n = 0;
            for (const Member& m : g_row_people) {
                if (!m.hasLoc) continue;
                lat += m.lat;
                lon += m.lon;
                n++;
            }
            if (n) {
                g_map_state.lat = lat / n;
                g_map_state.lon = lon / n;
                if (g_map_state.zoom < 12) g_map_state.zoom = 13;
            }
        }
        double cx, cy;
        project_xy(g_map_state.lat, g_map_state.lon, g_map_state.zoom, cx, cy);
        double left = cx - rc.right / 2.0;
        double top = cy - rc.bottom / 2.0;
        int x0 = static_cast<int>(std::floor(left / 256.0));
        int y0 = static_cast<int>(std::floor(top / 256.0));
        int x1 = static_cast<int>(std::floor((left + rc.right) / 256.0));
        int y1 = static_cast<int>(std::floor((top + rc.bottom) / 256.0));
        int max_i = (1 << g_map_state.zoom) - 1;
        for (int tx = x0; tx <= x1; tx++) {
            for (int ty = y0; ty <= y1; ty++) {
                if (tx < 0 || ty < 0 || tx > max_i || ty > max_i) continue;
                std::string key = std::to_string(g_satellite ? 1 : 0) + "/" + std::to_string(g_map_state.zoom) + "/" + std::to_string(tx) + "/" + std::to_string(ty);
                Gdiplus::Bitmap* bmp = nullptr;
                {
                    std::lock_guard<std::mutex> lock(g_map_state.mu);
                    auto it = g_map_state.tiles.find(key);
                    if (it != g_map_state.tiles.end()) bmp = it->second.second;
                }
                int dx = static_cast<int>(tx * 256 - left);
                int dy = static_cast<int>(ty * 256 - top);
                if (bmp) g.DrawImage(bmp, dx, dy, 256, 256);
                else want_tile(key);
            }
        }
        std::vector<Member> people = g_row_people;
        std::vector<Place> places = g_row_places;
        Gdiplus::Font font(L"Segoe UI", 11);
        for (const Place& p : places) {
            double px, py;
            project_xy(p.lat, p.lon, g_map_state.zoom, px, py);
            int sx = static_cast<int>(px - left);
            int sy = static_cast<int>(py - top);
            Gdiplus::SolidBrush fill(Gdiplus::Color(180, 20, 90, 50));
            g.FillEllipse(&fill, sx - 7, sy - 7, 14, 14);
        }
        for (const Member& m : people) {
            if (!m.hasLoc) continue;
            double px, py;
            project_xy(m.lat, m.lon, g_map_state.zoom, px, py);
            int sx = static_cast<int>(px - left);
            int sy = static_cast<int>(py - top);
            Gdiplus::Bitmap* face = face_bitmap(m);
            Gdiplus::GraphicsPath clip;
            clip.AddEllipse(sx - 16, sy - 16, 32, 32);
            if (face) {
                g.SetClip(&clip);
                g.DrawImage(face, sx - 16, sy - 16, 32, 32);
                g.ResetClip();
            } else {
                Gdiplus::SolidBrush fill(Gdiplus::Color(255, 200, 16, 46));
                g.FillEllipse(&fill, sx - 16, sy - 16, 32, 32);
            }
            Gdiplus::Pen ring(Gdiplus::Color(255, 255, 250, 244), 2);
            g.DrawEllipse(&ring, sx - 16, sy - 16, 32, 32);
            Gdiplus::SolidBrush ink(Gdiplus::Color(255, 26, 25, 22));
            std::wstring name = widen(m.name.empty() ? m.userId : m.name);
            g.DrawString(name.c_str(), -1, &font, Gdiplus::PointF(static_cast<float>(sx + 20), static_cast<float>(sy - 8)), &ink);
        }
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldbmp);
        DeleteObject(dib);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

static void create_map(HWND parent) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = map_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FamilyMapView";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g_map = CreateWindowExW(WS_EX_CLIENTEDGE, L"FamilyMapView", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, parent, nullptr, wc.hInstance, nullptr);
    std::thread(tile_worker).detach();
}

static void toggle_satellite() {
    g_satellite = !g_satellite;
    {
        std::lock_guard<std::mutex> lock(g_map_state.mu);
        g_map_state.pending.clear();
    }
    if (g_map) InvalidateRect(g_map, nullptr, FALSE);
}

struct RosterHit {
    RECT r;
    int action;
    std::string group;
    std::string id;
};
static std::vector<RosterHit> g_hits;
static int g_roster_scroll = 0;
static int g_roster_content = 0;

static std::wstring stay_line(const Member& m) {
    if (!m.hasLoc) return L"No location yet";
    long long now = static_cast<long long>(std::time(nullptr));
    if (m.traveling) {
        int mph = static_cast<int>(m.speed * 2.23694 + 0.5);
        if (mph < 1) mph = 1;
        long long start = m.departed > 0 ? m.departed : m.ts;
        long long mins = now > start ? (now - start) / 60 : 0;
        std::wstring from = m.departedFrom.empty() ? L"this spot" : widen(m.departedFrom);
        std::wstring ago = mins < 1 ? L"just now" : (std::to_wstring(mins) + L" mins ago");
        return L"Traveling at " + std::to_wstring(mph) + L" MPH · Departed from " + from + L" - " + ago;
    }
    long long start = m.arrived > 0 ? m.arrived : m.ts;
    long long mins = now > start ? (now - start) / 60 : 0;
    std::wstring where = m.area.empty() ? L"this spot" : widen(m.area);
    std::wstring ago = mins < 1 ? L"just now" : (std::to_wstring(mins) + L" mins ago");
    return L"Arrived to " + where + L" - " + ago;
}

static void remove_member(const std::string& group, const std::string& user) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_members.erase(key_of(group, user));
        commit_locked();
    }
    std::error_code ec;
    fs::remove(avatar_path(group, user), ec);
}

static void remove_place_id(const std::string& group, const std::string& id) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_places.erase(key_of(group, id));
    commit_locked();
}

static void remove_group_id(const std::string& group) {
    std::vector<std::string> users;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        for (auto it = g_members.begin(); it != g_members.end();) {
            if (it->second.groupId == group) {
                users.push_back(it->second.userId);
                it = g_members.erase(it);
            } else ++it;
        }
        for (auto it = g_places.begin(); it != g_places.end();) {
            if (it->second.groupId == group) it = g_places.erase(it);
            else ++it;
        }
        commit_locked();
    }
    for (const std::string& user : users) {
        std::error_code ec;
        fs::remove(avatar_path(group, user), ec);
    }
}

struct PromptState {
    HWND edit1 = nullptr;
    HWND edit2 = nullptr;
    bool ok = false;
    bool done = false;
    std::wstring out1;
    std::wstring out2;
};

static LRESULT CALLBACK prompt_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<PromptState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_COMMAND:
        if (!st) break;
        if (LOWORD(wp) == 1) {
            wchar_t a[256];
            wchar_t b[256];
            a[0] = 0;
            b[0] = 0;
            if (st->edit1) GetWindowTextW(st->edit1, a, 256);
            if (st->edit2) GetWindowTextW(st->edit2, b, 256);
            st->out1 = a;
            st->out2 = b;
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

static bool ask_fields(HWND owner, const wchar_t* title, const wchar_t* label1, std::wstring& value1, const wchar_t* label2, std::wstring* value2) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = prompt_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"FamilyPrompt";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&wc);
        registered = true;
    }
    PromptState st;
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"FamilyPrompt", title,
        WS_POPUP | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 420, value2 ? 230 : 180,
        owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));
    CreateWindowExW(0, L"STATIC", label1, WS_CHILD | WS_VISIBLE, 16, 12, 380, 18, hwnd, nullptr, nullptr, nullptr);
    st.edit1 = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", value1.c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 16, 32, 380, 26, hwnd, nullptr, nullptr, nullptr);
    if (value2) {
        CreateWindowExW(0, L"STATIC", label2, WS_CHILD | WS_VISIBLE, 16, 66, 380, 18, hwnd, nullptr, nullptr, nullptr);
        st.edit2 = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", value2->c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 16, 86, 380, 26, hwnd, nullptr, nullptr, nullptr);
    }
    int by = value2 ? 130 : 74;
    CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE, 200, by, 90, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(1)), nullptr, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE, 300, by, 90, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(2)), nullptr, nullptr);
    EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (!st.ok) return false;
    value1 = st.out1;
    if (value2) *value2 = st.out2;
    return true;
}

static void edit_person_ui(const std::string& group, const std::string& user) {
    Member m;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_members.find(key_of(group, user));
        if (it == g_members.end()) return;
        m = it->second;
    }
    std::wstring name = widen(m.name);
    std::wstring phone = widen(m.phone);
    if (!ask_fields(g_hwnd, L"Edit person", L"Name", name, L"Phone", &phone)) return;
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_members.find(key_of(group, user));
    if (it == g_members.end()) return;
    it->second.name = clip(narrow_w(name), 80);
    it->second.phone = clip(narrow_w(phone), 40);
    commit_locked();
}

static void edit_place_ui(const std::string& group, const std::string& id) {
    Place p;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_places.find(key_of(group, id));
        if (it == g_places.end()) return;
        p = it->second;
    }
    std::wstring name = widen(p.name);
    std::wstring radius = std::to_wstring(static_cast<int>(p.radius));
    if (!ask_fields(g_hwnd, L"Edit place", L"Place name", name, L"Radius in meters", &radius)) return;
    int meters = _wtoi(radius.c_str());
    if (meters < 20) meters = 20;
    if (meters > 5000) meters = 5000;
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_places.find(key_of(group, id));
    if (it == g_places.end()) return;
    it->second.name = clip(narrow_w(name), 80);
    it->second.radius = meters;
    commit_locked();
}

static void rename_group_ui(const std::string& group) {
    std::wstring name = widen(group);
    if (!ask_fields(g_hwnd, L"Rename group", L"Group name", name, nullptr, nullptr)) return;
    std::string next = clip(narrow_w(name), 64);
    if (next.empty() || next == group) return;
    std::vector<std::string> users;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        std::vector<Member> moved_m;
        std::vector<Place> moved_p;
        for (auto it = g_members.begin(); it != g_members.end();) {
            if (it->second.groupId == group) {
                users.push_back(it->second.userId);
                Member m = it->second;
                m.groupId = next;
                moved_m.push_back(m);
                it = g_members.erase(it);
            } else ++it;
        }
        for (auto it = g_places.begin(); it != g_places.end();) {
            if (it->second.groupId == group) {
                Place p = it->second;
                p.groupId = next;
                moved_p.push_back(p);
                it = g_places.erase(it);
            } else ++it;
        }
        for (const Member& m : moved_m) g_members[key_of(m.groupId, m.userId)] = m;
        for (const Place& p : moved_p) g_places[key_of(p.groupId, p.id)] = p;
        commit_locked();
    }
    for (const std::string& user : users) {
        std::error_code ec;
        fs::rename(avatar_path(group, user), avatar_path(next, user), ec);
    }
}

static RECT hit_rect(int x, int y, int w, int h) {
    RECT r{x, y, x + w, y + h};
    return r;
}

static void paint_button(Gdiplus::Graphics& g, const RECT& r, const wchar_t* text, Gdiplus::Font& font) {
    Gdiplus::SolidBrush bg(Gdiplus::Color(255, 244, 241, 234));
    Gdiplus::Pen pen(Gdiplus::Color(255, 210, 204, 194));
    g.FillRectangle(&bg, static_cast<INT>(r.left), static_cast<INT>(r.top), static_cast<INT>(r.right - r.left), static_cast<INT>(r.bottom - r.top));
    g.DrawRectangle(&pen, static_cast<INT>(r.left), static_cast<INT>(r.top), static_cast<INT>(r.right - r.left - 1), static_cast<INT>(r.bottom - r.top - 1));
    Gdiplus::SolidBrush ink(Gdiplus::Color(255, 26, 25, 22));
    g.DrawString(text, -1, &font, Gdiplus::PointF(static_cast<float>(r.left + 8), static_cast<float>(r.top + 4)), &ink);
}

static LRESULT CALLBACK roster_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        g_roster_scroll -= delta / 4;
        if (g_roster_scroll < 0) g_roster_scroll = 0;
        RECT rc;
        GetClientRect(hwnd, &rc);
        int max_scroll = g_roster_content - rc.bottom;
        if (max_scroll < 0) max_scroll = 0;
        if (g_roster_scroll > max_scroll) g_roster_scroll = max_scroll;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        int x = GET_X_LPARAM(lp);
        int y = GET_Y_LPARAM(lp) + g_roster_scroll;
        for (const RosterHit& h : g_hits) {
            if (x >= h.r.left && x < h.r.right && y >= h.r.top && y < h.r.bottom) {
                if (h.action == 1) rename_group_ui(h.group);
                if (h.action == 2 && MessageBoxW(g_hwnd, (L"Delete group " + widen(h.group) + L" and everyone in it?").c_str(), L"Family", MB_YESNO | MB_ICONWARNING) == IDYES) remove_group_id(h.group);
                if (h.action == 3) edit_person_ui(h.group, h.id);
                if (h.action == 4 && MessageBoxW(g_hwnd, L"Remove this person?", L"Family", MB_YESNO | MB_ICONWARNING) == IDYES) remove_member(h.group, h.id);
                if (h.action == 5) edit_place_ui(h.group, h.id);
                if (h.action == 6 && MessageBoxW(g_hwnd, L"Delete this place?", L"Family", MB_YESNO | MB_ICONWARNING) == IDYES) remove_place_id(h.group, h.id);
                InvalidateRect(hwnd, nullptr, FALSE);
                break;
            }
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP dib = CreateCompatibleBitmap(hdc, rc.right > 0 ? rc.right : 1, rc.bottom > 0 ? rc.bottom : 1);
        HGDIOBJ old = SelectObject(mem, dib);
        Gdiplus::Graphics g(mem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.Clear(Gdiplus::Color(255, 247, 244, 238));
        g.TranslateTransform(0, static_cast<float>(-g_roster_scroll));
        Gdiplus::Font title(L"Segoe UI", 15, Gdiplus::FontStyleBold);
        Gdiplus::Font body(L"Segoe UI", 12);
        Gdiplus::Font small(L"Segoe UI", 10);
        Gdiplus::SolidBrush ink(Gdiplus::Color(255, 26, 25, 22));
        Gdiplus::SolidBrush muted(Gdiplus::Color(255, 92, 88, 79));
        g_hits.clear();
        int y = 8;
        int width = rc.right - 16;
        std::vector<std::string> groups;
        for (const Member& m : g_row_people) if (std::find(groups.begin(), groups.end(), m.groupId) == groups.end()) groups.push_back(m.groupId);
        for (const Place& p : g_row_places) if (std::find(groups.begin(), groups.end(), p.groupId) == groups.end()) groups.push_back(p.groupId);
        if (groups.empty()) {
            g.DrawString(L"No one has checked in yet.", -1, &body, Gdiplus::PointF(12, 12), &muted);
            y = 40;
        }
        for (const std::string& group : groups) {
            g.DrawString(widen(group).c_str(), -1, &title, Gdiplus::PointF(12, static_cast<float>(y)), &ink);
            RECT rename = hit_rect(width - 150, y, 70, 24);
            RECT delg = hit_rect(width - 72, y, 70, 24);
            paint_button(g, rename, L"Rename", small);
            paint_button(g, delg, L"Delete", small);
            g_hits.push_back({rename, 1, group, ""});
            g_hits.push_back({delg, 2, group, ""});
            y += 36;
            g.DrawString(L"People", -1, &small, Gdiplus::PointF(12, static_cast<float>(y)), &muted);
            y += 20;
            bool any_person = false;
            for (const Member& m : g_row_people) {
                if (m.groupId != group) continue;
                any_person = true;
                Gdiplus::SolidBrush card(Gdiplus::Color(255, 255, 252, 247));
                g.FillRectangle(&card, 8, y, width, 64);
                Gdiplus::Bitmap* face = face_bitmap(m);
                if (face) {
                    Gdiplus::GraphicsPath clip;
                    clip.AddEllipse(16, y + 12, 40, 40);
                    g.SetClip(&clip);
                    g.DrawImage(face, 16, y + 12, 40, 40);
                    g.ResetClip();
                } else {
                    Gdiplus::SolidBrush dot(Gdiplus::Color(255, 200, 16, 46));
                    g.FillEllipse(&dot, 16, y + 12, 40, 40);
                }
                std::wstring who = widen(m.name.empty() ? m.userId : m.name);
                g.DrawString(who.c_str(), -1, &body, Gdiplus::PointF(64, static_cast<float>(y + 8)), &ink);
                std::wstring sub = stay_line(m);
                if (!m.phone.empty()) sub += L"  ·  " + widen(m.phone);
                g.DrawString(sub.c_str(), -1, &small, Gdiplus::PointF(64, static_cast<float>(y + 30)), &muted);
                RECT edit = hit_rect(width - 132, y + 18, 58, 26);
                RECT del = hit_rect(width - 68, y + 18, 58, 26);
                paint_button(g, edit, L"Edit", small);
                paint_button(g, del, L"Delete", small);
                g_hits.push_back({edit, 3, group, m.userId});
                g_hits.push_back({del, 4, group, m.userId});
                y += 72;
            }
            if (!any_person) {
                g.DrawString(L"No people in this group.", -1, &small, Gdiplus::PointF(16, static_cast<float>(y)), &muted);
                y += 22;
            }
            g.DrawString(L"Places", -1, &small, Gdiplus::PointF(12, static_cast<float>(y)), &muted);
            y += 20;
            bool any_place = false;
            for (const Place& p : g_row_places) {
                if (p.groupId != group) continue;
                any_place = true;
                Gdiplus::SolidBrush card(Gdiplus::Color(255, 255, 252, 247));
                g.FillRectangle(&card, 8, y, width, 48);
                g.DrawString(widen(p.name).c_str(), -1, &body, Gdiplus::PointF(16, static_cast<float>(y + 6)), &ink);
                std::wstring meta = std::to_wstring(static_cast<int>(p.radius)) + L" m";
                g.DrawString(meta.c_str(), -1, &small, Gdiplus::PointF(16, static_cast<float>(y + 26)), &muted);
                RECT edit = hit_rect(width - 132, y + 10, 58, 26);
                RECT del = hit_rect(width - 68, y + 10, 58, 26);
                paint_button(g, edit, L"Edit", small);
                paint_button(g, del, L"Delete", small);
                g_hits.push_back({edit, 5, group, p.id});
                g_hits.push_back({del, 6, group, p.id});
                y += 56;
            }
            if (!any_place) {
                g.DrawString(L"No shared places.", -1, &small, Gdiplus::PointF(16, static_cast<float>(y)), &muted);
                y += 22;
            }
            y += 12;
        }
        g_roster_content = y + 8;
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(dib);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

static void create_roster(HWND parent) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = roster_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FamilyRoster";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g_roster = CreateWindowExW(0, L"FamilyRoster", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, parent, nullptr, wc.hInstance, nullptr);
}



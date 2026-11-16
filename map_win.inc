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
static HWND g_tailEdit = nullptr;
static HWND g_map = nullptr;
static std::vector<Member> g_row_people;
static std::vector<Place> g_row_places;
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
    if (tail.empty()) tail_text += "not detected. Paste the 100.x address below if Tailscale is connected.";
    else {
        for (size_t i = 0; i < tail.size(); i++) {
            if (i) tail_text += "    ";
            tail_text += "http://" + tail[i] + ":8000";
        }
    }
    if (g_homeLabel) set_text(g_homeLabel, widen(home_text));
    if (g_tailLive) set_text(g_tailLive, widen(tail_text));
    if (!g_tail_seeded && g_tailEdit) {
        g_tail_seeded = true;
        std::string show = g_tail_saved;
        if (show.empty() && !tail.empty()) show = "http://" + tail[0] + ":8000";
        set_text(g_tailEdit, widen(show));
        if (g_tail_saved.empty() && !show.empty()) {
            g_tail_saved = show;
            save_tail_setting();
        }
    }
}

static void save_tail_from_edit() {
    if (!g_tailEdit) return;
    wchar_t buf[512];
    GetWindowTextW(g_tailEdit, buf, 512);
    g_tail_saved = narrow_w(buf);
    while (!g_tail_saved.empty() && g_tail_saved.back() == '/') g_tail_saved.pop_back();
    if (!g_tail_saved.empty() && g_tail_saved.rfind("http://", 0) != 0 && g_tail_saved.rfind("https://", 0) != 0) {
        g_tail_saved = "http://" + g_tail_saved;
        set_text(g_tailEdit, widen(g_tail_saved));
    }
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
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        Gdiplus::Graphics g(hdc);
        g.Clear(Gdiplus::Color(255, 231, 226, 214));
        if (!g_map_state.moved) {
            double lat = 0, lon = 0;
            int n = 0;
            std::lock_guard<std::mutex> lock(g_mu);
            for (const auto& kv : g_members) {
                if (!kv.second.hasLoc) continue;
                lat += kv.second.lat;
                lon += kv.second.lon;
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
        std::vector<Member> people;
        std::vector<Place> places;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            for (const auto& kv : g_members) if (kv.second.hasLoc) people.push_back(kv.second);
            for (const auto& kv : g_places) places.push_back(kv.second);
        }
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
            double px, py;
            project_xy(m.lat, m.lon, g_map_state.zoom, px, py);
            int sx = static_cast<int>(px - left);
            int sy = static_cast<int>(py - top);
            Gdiplus::SolidBrush fill(Gdiplus::Color(255, 200, 16, 46));
            Gdiplus::SolidBrush ink(Gdiplus::Color(255, 26, 25, 22));
            g.FillEllipse(&fill, sx - 9, sy - 9, 18, 18);
            std::wstring name = widen(m.name.empty() ? m.userId : m.name);
            g.DrawString(name.c_str(), -1, &font, Gdiplus::PointF(static_cast<float>(sx + 12), static_cast<float>(sy - 8)), &ink);
        }
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
    g_map = CreateWindowExW(WS_EX_CLIENTEDGE, L"FamilyMapView", L"", WS_CHILD | WS_VISIBLE, 16, 96, 700, 500, parent, nullptr, wc.hInstance, nullptr);
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

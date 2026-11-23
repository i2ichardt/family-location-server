#include <algorithm>
#include <cstdio>
#include <ole2.h>
#ifndef PROPID
typedef ULONG PROPID;
#endif
#include <gdiplus.h>
#include <iphlpapi.h>
#include <deque>
#include <set>
#include <winhttp.h>
#include <windowsx.h>

static HWND g_map = nullptr;
static std::string g_home_line = "Home LAN:";
static std::string g_tail_line = "Tailscale:";
static int g_n_people = 0;
static int g_n_places = 0;
static std::vector<Member> g_row_people;
static std::vector<Place> g_row_places;
static bool g_live = true;
static std::string g_list_sig;
static HWND g_roster = nullptr;
static HWND g_historyBtn = nullptr;
static void open_history(HWND owner, const std::string& group, const std::string& user, const std::string& name);
static bool g_tail_seeded = false;
static bool g_satellite = false;

static void fill_round_rect(Gdiplus::Graphics& g, const Gdiplus::Brush& brush, float x, float y, float w, float h, float r) {
    if (w < 1 || h < 1) return;
    if (r < 1) r = 1;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    Gdiplus::GraphicsPath path;
    float d = r * 2;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
    g.FillPath(&brush, &path);
}

static void round_child(HWND hwnd, int radius) {
    if (!hwnd) return;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    if (rc.right < 2 || rc.bottom < 2) return;
    HRGN rgn = CreateRoundRectRgn(0, 0, rc.right + 1, rc.bottom + 1, radius, radius);
    SetWindowRgn(hwnd, rgn, TRUE);
}

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
    load_server_cfg();
}

static void save_tail_setting() {
    save_server_cfg();
}

static void refresh_address_labels() {
    std::vector<std::string> home, tail;
    collect_ips(home, tail);
    std::string home_text = "Home LAN:  ";
    if (home.empty()) home_text += "no LAN address yet";
    else {
        for (size_t i = 0; i < home.size(); i++) {
            if (i) home_text += "    ";
            home_text += "http://" + home[i] + ":8000";
        }
    }
    std::string saved;
    {
        std::lock_guard<std::mutex> lock(g_cfg_mu);
        saved = g_tail_saved;
    }
    std::string tail_text = "Tailscale:  ";
    if (tail.empty()) tail_text += saved.empty() ? "not detected" : saved;
    else {
        for (size_t i = 0; i < tail.size(); i++) {
            if (i) tail_text += "    ";
            tail_text += "http://" + tail[i] + ":8000";
        }
    }
    g_home_line = home_text;
    g_tail_line = tail_text;
    if (!g_tail_seeded) {
        g_tail_seeded = true;
        if (saved.empty() && !tail.empty()) {
            {
                std::lock_guard<std::mutex> lock(g_cfg_mu);
                g_tail_saved = "http://" + tail[0] + ":8000";
            }
            save_tail_setting();
        }
    }
}

static void save_tail_text(const std::wstring& raw) {
    std::string text = narrow_w(raw);
    while (!text.empty() && (text.back() == ' ' || text.back() == '/')) text.pop_back();
    size_t start = 0;
    while (start < text.size() && text[start] == ' ') start++;
    if (start > 0) text.erase(0, start);
    if (!text.empty() && text.rfind("http://", 0) != 0 && text.rfind("https://", 0) != 0)
        text = "http://" + text;
    {
        std::lock_guard<std::mutex> lock(g_cfg_mu);
        g_tail_saved = text;
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
static std::deque<std::string> g_tile_jobs;
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

static int tile_rank(const std::string& key, int cz, int cx, int cy) {
    int sat = 0, z = 0, x = 0, y = 0;
    if (std::sscanf(key.c_str(), "%d/%d/%d/%d", &sat, &z, &x, &y) != 4) return 100000000;
    if (z != cz) return 50000000 + std::abs(z - cz) * 100000;
    int dx = x - cx;
    int dy = y - cy;
    return dx * dx + dy * dy;
}

static void tile_worker() {
    while (true) {
        std::string key;
        {
            std::unique_lock<std::mutex> lock(g_job_mu);
            g_job_cv.wait(lock, [] { return g_tile_quit || !g_tile_jobs.empty(); });
            if (g_tile_quit && g_tile_jobs.empty()) return;
            key = g_tile_jobs.front();
            g_tile_jobs.pop_front();
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
        auto drop_pending = [&]() {
            std::lock_guard<std::mutex> lock(g_map_state.mu);
            g_map_state.pending.erase(key);
        };
        if (bytes.size() < 32 || !g_map) {
            drop_pending();
            continue;
        }
        auto* msg = new std::pair<std::string, std::vector<unsigned char>>(key, std::move(bytes));
        if (!PostMessageW(g_map, WM_TILE, 0, reinterpret_cast<LPARAM>(msg))) {
            delete msg;
            drop_pending();
        }
    }
}

static void want_tile(const std::string& key, bool front) {
    {
        std::lock_guard<std::mutex> lock(g_map_state.mu);
        if (g_map_state.tiles.count(key) || g_map_state.pending.count(key)) return;
        g_map_state.pending.insert(key);
    }
    std::vector<std::string> dropped;
    {
        std::lock_guard<std::mutex> lock(g_job_mu);
        if (front) g_tile_jobs.push_front(key);
        else g_tile_jobs.push_back(key);
        const size_t cap = 80;
        if (g_tile_jobs.size() > cap) {
            double px = 0, py = 0;
            int z = g_map_state.zoom;
            project_xy(g_map_state.lat, g_map_state.lon, z, px, py);
            int cx = static_cast<int>(std::floor(px / 256.0));
            int cy = static_cast<int>(std::floor(py / 256.0));
            while (g_tile_jobs.size() > cap) {
                size_t worst = 0;
                int worstD = -1;
                for (size_t i = 0; i < g_tile_jobs.size(); i++) {
                    int d = tile_rank(g_tile_jobs[i], z, cx, cy);
                    if (d > worstD) {
                        worstD = d;
                        worst = i;
                    }
                }
                dropped.push_back(g_tile_jobs[worst]);
                g_tile_jobs.erase(g_tile_jobs.begin() + static_cast<std::deque<std::string>::difference_type>(worst));
            }
        }
    }
    if (!dropped.empty()) {
        std::lock_guard<std::mutex> lock(g_map_state.mu);
        for (const std::string& k : dropped) g_map_state.pending.erase(k);
    }
    g_job_cv.notify_one();
}

static void queue_tile_xy(int zoom, int tx, int ty, bool front) {
    if (zoom < 0 || zoom > 22) return;
    int max_i = (1 << zoom) - 1;
    if (tx < 0 || ty < 0 || tx > max_i || ty > max_i) return;
    std::string key = std::to_string(g_satellite ? 1 : 0) + "/" + std::to_string(zoom) + "/" + std::to_string(tx) + "/" + std::to_string(ty);
    want_tile(key, front);
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
    g_map_state.tiles[key] = {stream, bmp};
    if (g_map_state.tiles.size() > 240) {
        double px = 0, py = 0;
        int z = g_map_state.zoom;
        project_xy(g_map_state.lat, g_map_state.lon, z, px, py);
        int cx = static_cast<int>(std::floor(px / 256.0));
        int cy = static_cast<int>(std::floor(py / 256.0));
        std::vector<std::pair<int, std::string>> rank;
        rank.reserve(g_map_state.tiles.size());
        for (const auto& kv : g_map_state.tiles) rank.push_back({tile_rank(kv.first, z, cx, cy), kv.first});
        std::sort(rank.begin(), rank.end(), [](const std::pair<int, std::string>& a, const std::pair<int, std::string>& b) {
            return a.first > b.first;
        });
        size_t extra = g_map_state.tiles.size() > 180 ? g_map_state.tiles.size() - 180 : 0;
        for (size_t i = 0; i < extra && i < rank.size(); i++) {
            auto it = g_map_state.tiles.find(rank[i].second);
            if (it == g_map_state.tiles.end()) continue;
            delete it->second.second;
            if (it->second.first) it->second.first->Release();
            g_map_state.tiles.erase(it);
        }
    }
}

static std::map<std::string, Gdiplus::Bitmap*> g_faces;
static std::map<std::string, long long> g_face_stamp;

static Gdiplus::Bitmap* bitmap_from_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return nullptr;
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), {});
    if (bytes.empty()) return nullptr;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (!mem) return nullptr;
    void* dst = GlobalLock(mem);
    if (!dst) {
        GlobalFree(mem);
        return nullptr;
    }
    std::memcpy(dst, bytes.data(), bytes.size());
    GlobalUnlock(mem);
    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(mem, TRUE, &stream) != S_OK) {
        GlobalFree(mem);
        return nullptr;
    }
    Gdiplus::Bitmap* loaded = Gdiplus::Bitmap::FromStream(stream);
    Gdiplus::Bitmap* copy = nullptr;
    if (loaded && loaded->GetLastStatus() == Gdiplus::Ok && loaded->GetWidth() > 0 && loaded->GetHeight() > 0) {
        copy = new Gdiplus::Bitmap(loaded->GetWidth(), loaded->GetHeight(), PixelFormat32bppARGB);
        Gdiplus::Graphics draw(copy);
        draw.DrawImage(loaded, 0, 0, loaded->GetWidth(), loaded->GetHeight());
    }
    delete loaded;
    stream->Release();
    return copy;
}

static Gdiplus::Bitmap* face_bitmap(const Member& m) {
    if (!m.hasImage) return nullptr;
    std::string key = m.groupId + ":" + m.userId;
    auto have = g_face_stamp.find(key);
    auto bmp = g_faces.find(key);
    if (have != g_face_stamp.end() && have->second == m.imageTs && bmp != g_faces.end()) return bmp->second;
    fs::path path = avatar_path(m.groupId, m.userId);
    if (!fs::exists(path)) return nullptr;
    Gdiplus::Bitmap* loaded = bitmap_from_file(path);
    if (!loaded) return nullptr;
    if (bmp != g_faces.end() && bmp->second) delete bmp->second;
    g_faces[key] = loaded;
    g_face_stamp[key] = m.imageTs;
    return loaded;
}

static std::map<std::string, Gdiplus::Bitmap*> g_place_imgs;
static std::map<std::string, long long> g_place_stamp;

static Gdiplus::Bitmap* place_bitmap(const Place& p) {
    if (!p.hasImage) return nullptr;
    std::string key = p.groupId + ":" + p.id;
    auto have = g_place_stamp.find(key);
    auto bmp = g_place_imgs.find(key);
    if (have != g_place_stamp.end() && have->second == p.imageTs && bmp != g_place_imgs.end()) return bmp->second;
    fs::path path = place_image_path(p.groupId, p.id);
    if (!fs::exists(path)) return nullptr;
    Gdiplus::Bitmap* loaded = bitmap_from_file(path);
    if (!loaded) return nullptr;
    if (bmp != g_place_imgs.end() && bmp->second) delete bmp->second;
    g_place_imgs[key] = loaded;
    g_place_stamp[key] = p.imageTs;
    return loaded;
}

static void draw_house_icon(Gdiplus::Graphics& g, float x, float y, float s) {
    Gdiplus::SolidBrush bg(Gdiplus::Color(255, 219, 234, 254));
    fill_round_rect(g, bg, x, y, s, s, 10);
    Gdiplus::SolidBrush ink(Gdiplus::Color(255, 29, 78, 216));
    Gdiplus::PointF roof[3] = {
        Gdiplus::PointF(x + s * 0.50f, y + s * 0.18f),
        Gdiplus::PointF(x + s * 0.78f, y + s * 0.46f),
        Gdiplus::PointF(x + s * 0.22f, y + s * 0.46f),
    };
    g.FillPolygon(&ink, roof, 3);
    g.FillRectangle(&ink, x + s * 0.32f, y + s * 0.46f, s * 0.36f, s * 0.30f);
}

static void center_on(double lat, double lon) {
    if (!valid_coord(lat, lon)) return;
    g_map_state.lat = lat;
    g_map_state.lon = lon;
    g_map_state.moved = true;
    if (g_map_state.zoom < 16) g_map_state.zoom = 16;
    if (g_map) InvalidateRect(g_map, nullptr, FALSE);
}

static std::string g_sel_group;
static std::string g_sel_id;
static bool g_place_drag = false;
static int g_press_x = 0;
static int g_press_y = 0;

static void map_origin(HWND hwnd, double& left, double& top) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    double cx, cy;
    project_xy(g_map_state.lat, g_map_state.lon, g_map_state.zoom, cx, cy);
    left = cx - rc.right / 2.0;
    top = cy - rc.bottom / 2.0;
}

static int place_at_pixel(HWND hwnd, int x, int y) {
    double left, top;
    map_origin(hwnd, left, top);
    int best = -1;
    double bestD = 26.0 * 26.0;
    for (int i = 0; i < static_cast<int>(g_row_places.size()); i++) {
        double px, py;
        project_xy(g_row_places[i].lat, g_row_places[i].lon, g_map_state.zoom, px, py);
        double dx = (px - left) - x;
        double dy = (py - top) - y;
        double d = dx * dx + dy * dy;
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

static void apply_place_edit(const std::string& group, const std::string& id, double lat, double lon, double radius, bool save) {
    if (radius < 20) radius = 20;
    if (radius > 5000) radius = 5000;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_places.find(key_of(group, id));
        if (it == g_places.end()) return;
        it->second.lat = lat;
        it->second.lon = lon;
        it->second.radius = radius;
        if (save) commit_locked();
    }
    for (Place& p : g_row_places) {
        if (p.groupId == group && p.id == id) {
            p.lat = lat;
            p.lon = lon;
            p.radius = radius;
        }
    }
}

static bool selected_place(Place& out) {
    for (const Place& p : g_row_places) {
        if (p.groupId == g_sel_group && p.id == g_sel_id) {
            out = p;
            return true;
        }
    }
    return false;
}

static LRESULT CALLBACK map_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    try {
    switch (msg) {
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp);
        int y = GET_Y_LPARAM(lp);
        g_press_x = x;
        g_press_y = y;
        int hit = place_at_pixel(hwnd, x, y);
        if (hit >= 0) {
            g_sel_group = g_row_places[hit].groupId;
            g_sel_id = g_row_places[hit].id;
            g_place_drag = true;
            g_map_state.dragging = false;
        } else {
            g_place_drag = false;
            g_map_state.dragging = true;
            g_map_state.last_x = x;
            g_map_state.last_y = y;
        }
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        int x = GET_X_LPARAM(lp);
        int y = GET_Y_LPARAM(lp);
        if (g_place_drag) {
            Place cur;
            if (selected_place(cur)) apply_place_edit(cur.groupId, cur.id, cur.lat, cur.lon, cur.radius, true);
        } else {
            int dx = x - g_press_x;
            int dy = y - g_press_y;
            if (dx * dx + dy * dy < 16 && place_at_pixel(hwnd, x, y) < 0) {
                g_sel_group.clear();
                g_sel_id.clear();
            }
        }
        g_place_drag = false;
        g_map_state.dragging = false;
        ReleaseCapture();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_place_drag) {
            Place cur;
            if (selected_place(cur)) {
                double left, top;
                map_origin(hwnd, left, top);
                double lat, lon;
                unproject_xy(left + GET_X_LPARAM(lp), top + GET_Y_LPARAM(lp), g_map_state.zoom, lat, lon);
                apply_place_edit(cur.groupId, cur.id, lat, lon, cur.radius, false);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (g_map_state.dragging) {
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
        Place cur;
        if (selected_place(cur)) {
            int notches = delta / WHEEL_DELTA;
            if (notches == 0) notches = delta > 0 ? 1 : -1;
            double radius = cur.radius + notches * 15.0;
            apply_place_edit(cur.groupId, cur.id, cur.lat, cur.lon, radius, true);
        } else {
            if (delta > 0 && g_map_state.zoom < 18) g_map_state.zoom++;
            if (delta < 0 && g_map_state.zoom > 3) g_map_state.zoom--;
            g_map_state.moved = true;
        }
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
        g.Clear(Gdiplus::Color(255, 226, 232, 240));
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
                else want_tile(key, true);
            }
        }
        for (int tx = x0 - 2; tx <= x1 + 2; tx++) {
            for (int ty = y0 - 2; ty <= y1 + 2; ty++) {
                if (tx >= x0 && tx <= x1 && ty >= y0 && ty <= y1) continue;
                queue_tile_xy(g_map_state.zoom, tx, ty, false);
            }
        }
        auto queue_ll = [&](double lat, double lon) {
            double px = 0, py = 0;
            project_xy(lat, lon, g_map_state.zoom, px, py);
            queue_tile_xy(g_map_state.zoom, static_cast<int>(std::floor(px / 256.0)), static_cast<int>(std::floor(py / 256.0)), false);
        };
        for (const Member& m : g_row_people) if (m.hasLoc) queue_ll(m.lat, m.lon);
        for (const Place& p : g_row_places) queue_ll(p.lat, p.lon);
        std::vector<Member> people = g_row_people;
        std::vector<Place> places = g_row_places;
        for (const Place& p : places) {
            double px, py;
            project_xy(p.lat, p.lon, g_map_state.zoom, px, py);
            float sx = static_cast<float>(px - left);
            float sy = static_cast<float>(py - top);
            double cosLat = std::cos(p.lat * 3.141592653589793 / 180.0);
            if (cosLat < 0.01) cosLat = 0.01;
            double metersPerPx = cosLat * 40075016.685578488 / (256.0 * static_cast<double>(1 << g_map_state.zoom));
            float rad = static_cast<float>(p.radius / metersPerPx);
            if (rad >= 3.0f) {
                bool selected = p.groupId == g_sel_group && p.id == g_sel_id;
                Gdiplus::SolidBrush wash(selected ? Gdiplus::Color(70, 47, 107, 255) : Gdiplus::Color(52, 200, 16, 46));
                g.FillEllipse(&wash, sx - rad, sy - rad, rad * 2.0f, rad * 2.0f);
                Gdiplus::Pen ring(selected ? Gdiplus::Color(255, 47, 107, 255) : Gdiplus::Color(210, 200, 16, 46), selected ? 3.0f : (rad > 40.0f ? 2.5f : 1.5f));
                g.DrawEllipse(&ring, sx - rad, sy - rad, rad * 2.0f, rad * 2.0f);
            }
            Gdiplus::SolidBrush fill(Gdiplus::Color(230, 20, 90, 50));
            g.FillEllipse(&fill, sx - 7.0f, sy - 7.0f, 14.0f, 14.0f);
            Gdiplus::Font labelFont(L"Segoe UI", 10, Gdiplus::FontStyleBold);
            std::wstring label = widen(p.name.empty() ? "Place" : p.name) + L"  " + std::to_wstring(static_cast<int>(p.radius)) + L" m";
            Gdiplus::RectF measured;
            g.MeasureString(label.c_str(), -1, &labelFont, Gdiplus::PointF(0, 0), &measured);
            Gdiplus::SolidBrush chip(Gdiplus::Color(225, 247, 244, 238));
            Gdiplus::RectF chipRect(sx + 12.0f, sy - measured.Height * 0.5f - 2.0f, measured.Width + 12.0f, measured.Height + 4.0f);
            g.FillRectangle(&chip, chipRect);
            Gdiplus::Pen chipEdge(Gdiplus::Color(180, 26, 25, 22), 1.0f);
            g.DrawRectangle(&chipEdge, chipRect);
            Gdiplus::SolidBrush ink(Gdiplus::Color(255, 26, 25, 22));
            g.DrawString(label.c_str(), -1, &labelFont, Gdiplus::PointF(chipRect.X + 6.0f, chipRect.Y + 2.0f), &ink);
        }
        struct Cluster {
            double lat = 0;
            double lon = 0;
            std::vector<const Member*> members;
        };
        std::vector<Cluster> clusters;
        for (const Member& m : people) {
            if (!m.hasLoc) continue;
            int found = -1;
            for (int i = 0; i < static_cast<int>(clusters.size()); i++) {
                if (haversine_m(clusters[i].lat, clusters[i].lon, m.lat, m.lon) <= 50.0) {
                    found = i;
                    break;
                }
            }
            if (found < 0) {
                Cluster c;
                c.lat = m.lat;
                c.lon = m.lon;
                c.members.push_back(&m);
                clusters.push_back(c);
            } else clusters[found].members.push_back(&m);
        }
        Gdiplus::Font font(L"Segoe UI", 11);
        for (const Cluster& cluster : clusters) {
            double px, py;
            project_xy(cluster.lat, cluster.lon, g_map_state.zoom, px, py);
            int sx = static_cast<int>(px - left);
            int sy = static_cast<int>(py - top);
            int n = static_cast<int>(cluster.members.size());
            int face = 30;
            int gap = 6;
            int pad = 8;
            int bubbleW = n * face + (n - 1) * gap + pad * 2;
            int bubbleH = face + pad * 2;
            int leftX = sx - bubbleW / 2;
            int topY = sy - bubbleH - 12;
            Gdiplus::GraphicsPath bubble;
            int rad = bubbleH / 2;
            bubble.AddArc(leftX, topY, rad, rad, 180, 90);
            bubble.AddArc(leftX + bubbleW - rad, topY, rad, rad, 270, 90);
            bubble.AddArc(leftX + bubbleW - rad, topY + bubbleH - rad, rad, rad, 0, 90);
            bubble.AddArc(leftX, topY + bubbleH - rad, rad, rad, 90, 90);
            bubble.CloseFigure();
            Gdiplus::SolidBrush paper(Gdiplus::Color(255, 255, 252, 247));
            g.FillPath(&paper, &bubble);
            Gdiplus::Pen edge(Gdiplus::Color(255, 26, 25, 22), 1.5f);
            g.DrawPath(&edge, &bubble);
            Gdiplus::PointF tail[3] = {
                Gdiplus::PointF(static_cast<float>(sx - 7), static_cast<float>(topY + bubbleH - 1)),
                Gdiplus::PointF(static_cast<float>(sx + 7), static_cast<float>(topY + bubbleH - 1)),
                Gdiplus::PointF(static_cast<float>(sx), static_cast<float>(sy - 1)),
            };
            g.FillPolygon(&paper, tail, 3);
            g.DrawPolygon(&edge, tail, 3);
            for (int i = 0; i < n; i++) {
                int fx = leftX + pad + i * (face + gap);
                int fy = topY + pad;
                const Member* m = cluster.members[i];
                Gdiplus::Bitmap* pic = face_bitmap(*m);
                Gdiplus::GraphicsPath clip;
                clip.AddEllipse(fx, fy, face, face);
                if (pic) {
                    g.SetClip(&clip);
                    g.DrawImage(pic, fx, fy, face, face);
                    g.ResetClip();
                } else {
                    Gdiplus::SolidBrush fill(Gdiplus::Color(255, 200, 16, 46));
                    g.FillPath(&fill, &clip);
                }
                Gdiplus::Pen ring(Gdiplus::Color(255, 255, 250, 244), 2);
                g.DrawEllipse(&ring, fx, fy, face, face);
            }
            if (n == 1) {
                Gdiplus::SolidBrush ink(Gdiplus::Color(255, 26, 25, 22));
                std::wstring name = widen(cluster.members[0]->name.empty() ? cluster.members[0]->userId : cluster.members[0]->name);
                g.DrawString(name.c_str(), -1, &font, Gdiplus::PointF(static_cast<float>(sx + bubbleW / 2 + 6), static_cast<float>(topY + 8)), &ink);
            }
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
    } catch (...) {
        return 0;
    }
}

static void create_map(HWND parent) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = map_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FamilyMapView";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g_map = CreateWindowExW(0, L"FamilyMapView", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 10, 10, parent, nullptr, wc.hInstance, nullptr);
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

static std::wstring ago_phrase(long long mins) {
    if (mins < 1) return L"just now";
    if (mins < 60) return std::to_wstring(mins) + (mins == 1 ? L" min ago" : L" mins ago");
    long long hours = mins / 60;
    long long rest = mins % 60;
    std::wstring text = std::to_wstring(hours) + (hours == 1 ? L" hour" : L" hours");
    if (rest > 0) text += L" " + std::to_wstring(rest) + (rest == 1 ? L" min" : L" mins");
    return text + L" ago";
}

static std::wstring stay_line(const Member& m) {
    long long now = static_cast<long long>(std::time(nullptr));
    std::wstring base;
    if (!m.hasLoc) base = L"No location yet";
    else if (m.traveling) {
        int mph = static_cast<int>(m.speed * 2.23694 + 0.5);
        if (mph < 1) mph = 1;
        long long start = m.departed > 0 ? m.departed : m.ts;
        long long mins = now > start ? (now - start) / 60 : 0;
        std::wstring from = m.departedFrom.empty() ? L"this spot" : widen(m.departedFrom);
        base = L"Traveling at " + std::to_wstring(mph) + L" MPH · Departed from " + from + L" - " + ago_phrase(mins);
    } else {
        long long start = m.arrived > 0 ? m.arrived : m.ts;
        long long mins = now > start ? (now - start) / 60 : 0;
        std::wstring where = m.area.empty() ? L"this spot" : widen(m.area);
        base = L"Arrived to " + where + L" - " + ago_phrase(mins);
    }
    if (m.locationOff) return base + L" · Location is off";
    bool aliveOn = g_aliveOn.load();
    int aliveSecs = g_aliveSecs.load();
    bool unreachable = aliveOn && m.seen > 0 && now > m.seen + static_cast<long long>(aliveSecs) + 25;
    if (unreachable) return L"Lost Connection!";
    if (!aliveOn) {
        long long heard = m.seen > 0 ? m.seen : m.ts;
        bool missed = m.every > 0 && heard > 0 && now > heard + static_cast<long long>(m.every * 1.5) + 20;
        if (missed) return base + L" · No recent check-in";
    }
    return base;
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
    std::error_code ec;
    fs::remove(place_image_path(group, id), ec);
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
    if (!ask_fields(g_hwnd, L"Edit place", L"Place name", name, L"Arrive and leave radius in meters", &radius)) return;
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

static std::string fresh_place_id(const std::string& group) {
    std::string id = "p" + std::to_string(server_now());
    if (!g_places.count(key_of(group, id))) return id;
    for (int n = 1; n < 1000; n++) {
        std::string next = id + "_" + std::to_string(n);
        if (!g_places.count(key_of(group, next))) return next;
    }
    return id + "_x";
}

static void create_place_ui(const std::string& group, bool askGroup) {
    std::wstring name;
    std::wstring groupName = widen(group);
    std::string useGroup = group;
    if (askGroup) {
        if (groupName.empty()) groupName = L"Family";
        if (!ask_fields(g_hwnd, L"New place", L"Group name", groupName, L"Place name", &name)) return;
        useGroup = clip(narrow_w(groupName), 64);
    } else {
        if (!ask_fields(g_hwnd, L"New place", L"Place name. It is created at the map center, 100 m across.", name, nullptr, nullptr)) return;
    }
    std::string nm = clip(narrow_w(name), 80);
    while (!nm.empty() && (nm.back() == ' ' || nm.back() == '\t')) nm.pop_back();
    size_t sp = 0;
    while (sp < nm.size() && (nm[sp] == ' ' || nm[sp] == '\t')) sp++;
    if (sp > 0) nm.erase(0, sp);
    while (!useGroup.empty() && (useGroup.back() == ' ' || useGroup.back() == '\t')) useGroup.pop_back();
    if (nm.empty() || useGroup.empty()) {
        MessageBoxW(g_hwnd, L"Enter a place name.", L"Family", MB_OK | MB_ICONINFORMATION);
        return;
    }
    Place p;
    p.name = nm;
    p.groupId = useGroup;
    p.lat = g_map_state.lat;
    p.lon = g_map_state.lon;
    p.radius = 100;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        p.id = fresh_place_id(p.groupId);
        g_places[key_of(p.groupId, p.id)] = p;
        commit_locked();
    }
    g_sel_group = p.groupId;
    g_sel_id = p.id;
    center_on(p.lat, p.lon);
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
    Gdiplus::SolidBrush bg(Gdiplus::Color(255, 255, 255, 255));
    float x = static_cast<float>(r.left);
    float y = static_cast<float>(r.top);
    float w = static_cast<float>(r.right - r.left);
    float h = static_cast<float>(r.bottom - r.top);
    fill_round_rect(g, bg, x, y, w, h, 8);
    Gdiplus::Pen pen(Gdiplus::Color(255, 203, 213, 225), 1);
    g.DrawRectangle(&pen, x, y, w - 1, h - 1);
    Gdiplus::SolidBrush ink(Gdiplus::Color(255, 30, 41, 59));
    g.DrawString(text, -1, &font, Gdiplus::PointF(x + 8, y + 4), &ink);
}

static void draw_pill(Gdiplus::Graphics& g, float x, float y, float w, float h, Gdiplus::Color bg, const wchar_t* text, Gdiplus::Font& font) {
    Gdiplus::SolidBrush brush(bg);
    fill_round_rect(g, brush, x, y, w, h, h / 2.0f);
    Gdiplus::SolidBrush ink(Gdiplus::Color(255, 255, 255, 255));
    Gdiplus::StringFormat fmt;
    fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    g.DrawString(text, -1, &font, Gdiplus::RectF(x, y, w, h), &fmt, &ink);
}

static LRESULT CALLBACK roster_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    try {
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
                if (h.action == 7) {
                    std::string name = h.id;
                    for (const Member& m : g_row_people) {
                        if (m.groupId == h.group && m.userId == h.id) {
                            name = m.name.empty() ? m.userId : m.name;
                            break;
                        }
                    }
                    open_history(g_hwnd, h.group, h.id, name);
                }
                if (h.action == 5) edit_place_ui(h.group, h.id);
                if (h.action == 6 && MessageBoxW(g_hwnd, L"Delete this place?", L"Family", MB_YESNO | MB_ICONWARNING) == IDYES) remove_place_id(h.group, h.id);
                if (h.action == 8) {
                    for (const Member& m : g_row_people) {
                        if (m.groupId == h.group && m.userId == h.id) {
                            if (!m.hasLoc) MessageBoxW(g_hwnd, L"No location for this person yet.", L"Family", MB_OK | MB_ICONINFORMATION);
                            else center_on(m.lat, m.lon);
                            break;
                        }
                    }
                }
                if (h.action == 9) {
                    for (const Place& p : g_row_places) {
                        if (p.groupId == h.group && p.id == h.id) {
                            g_sel_group = p.groupId;
                            g_sel_id = p.id;
                            center_on(p.lat, p.lon);
                            break;
                        }
                    }
                }
                if (h.action == 10) create_place_ui(h.group, false);
                if (h.action == 11) create_place_ui("", true);
                InvalidateRect(hwnd, nullptr, FALSE);
                if (g_map) InvalidateRect(g_map, nullptr, FALSE);
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
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
        g.Clear(Gdiplus::Color(255, 241, 245, 249));
        g.TranslateTransform(0, static_cast<float>(-g_roster_scroll));
        Gdiplus::Font title(L"Segoe UI", 13, Gdiplus::FontStyleBold);
        Gdiplus::Font body(L"Segoe UI", 12, Gdiplus::FontStyleBold);
        Gdiplus::Font small(L"Segoe UI", 9);
        Gdiplus::Font section(L"Segoe UI", 10, Gdiplus::FontStyleBold);
        Gdiplus::SolidBrush ink(Gdiplus::Color(255, 15, 23, 42));
        Gdiplus::SolidBrush muted(Gdiplus::Color(255, 100, 116, 139));
        Gdiplus::SolidBrush cardShadow(Gdiplus::Color(16, 15, 23, 42));
        Gdiplus::StringFormat clipText;
        clipText.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        clipText.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        g_hits.clear();
        int y = 4;
        int width = rc.right - 8;
        if (width < 40) width = 40;
        std::vector<std::string> groups;
        for (const Member& m : g_row_people) if (std::find(groups.begin(), groups.end(), m.groupId) == groups.end()) groups.push_back(m.groupId);
        for (const Place& p : g_row_places) if (std::find(groups.begin(), groups.end(), p.groupId) == groups.end()) groups.push_back(p.groupId);
        long long nowS = server_now();
        bool aliveOn = g_aliveOn.load();
        int aliveSecs = g_aliveSecs.load();
        if (groups.empty()) {
            g.DrawString(L"No one has checked in yet.", -1, &body, Gdiplus::PointF(8, 8), &muted);
            RECT add = hit_rect(8, 40, 108, 28);
            paint_button(g, add, L"Add place", small);
            g_hits.push_back({add, 11, "", ""});
            y = 80;
        }
        for (const std::string& group : groups) {
            Gdiplus::RectF titleRect(8, static_cast<float>(y), static_cast<float>(width - 168), 24);
            g.DrawString(widen(group).c_str(), -1, &title, titleRect, &clipText, &ink);
            RECT rename = hit_rect(width - 150, y, 70, 26);
            RECT delg = hit_rect(width - 74, y, 66, 26);
            paint_button(g, rename, L"Rename", small);
            paint_button(g, delg, L"Delete", small);
            g_hits.push_back({rename, 1, group, ""});
            g_hits.push_back({delg, 2, group, ""});
            y += 34;
            g.DrawString(L"FAMILY", -1, &section, Gdiplus::PointF(8, static_cast<float>(y)), &muted);
            y += 22;
            bool any_person = false;
            for (const Member& m : g_row_people) {
                if (m.groupId != group) continue;
                any_person = true;
                int cardH = 132;
                fill_round_rect(g, cardShadow, 6, static_cast<float>(y + 2), static_cast<float>(width - 4), static_cast<float>(cardH), 14);
                Gdiplus::SolidBrush card(Gdiplus::Color(255, 255, 255, 255));
                fill_round_rect(g, card, 4, static_cast<float>(y), static_cast<float>(width - 4), static_cast<float>(cardH), 14);
                RECT faceHit = hit_rect(16, y + 12, 44, 44);
                g_hits.push_back({faceHit, 8, group, m.userId});
                Gdiplus::Bitmap* face = face_bitmap(m);
                if (face) {
                    Gdiplus::GraphicsPath clip;
                    clip.AddEllipse(16, y + 12, 44, 44);
                    g.SetClip(&clip);
                    g.DrawImage(face, 16, y + 12, 44, 44);
                    g.ResetClip();
                } else {
                    Gdiplus::SolidBrush dot(Gdiplus::Color(255, 37, 99, 235));
                    g.FillEllipse(&dot, 16, y + 12, 44, 44);
                    std::wstring letter = widen(m.name.empty() ? m.userId : m.name);
                    if (!letter.empty()) letter = letter.substr(0, 1);
                    Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));
                    g.DrawString(letter.c_str(), -1, &body, Gdiplus::PointF(30, static_cast<float>(y + 22)), &white);
                }
                Gdiplus::Pen ring(Gdiplus::Color(255, 226, 232, 240), 1);
                g.DrawEllipse(&ring, 16, y + 12, 44, 44);
                bool unreachable = aliveOn && m.seen > 0 && nowS > m.seen + static_cast<long long>(aliveSecs) + 25;
                const wchar_t* pill = L"Online";
                Gdiplus::Color pillBg(255, 22, 163, 74);
                if (unreachable) {
                    pill = L"Offline";
                    pillBg = Gdiplus::Color(255, 220, 38, 38);
                } else if (m.seen <= 0 && !m.hasLoc) {
                    pill = L"Waiting";
                    pillBg = Gdiplus::Color(255, 100, 116, 139);
                }
                draw_pill(g, static_cast<float>(width - 86), static_cast<float>(y + 12), 72, 22, pillBg, pill, small);
                Gdiplus::RectF nameRect(68, static_cast<float>(y + 12), static_cast<float>(width - 164), 22);
                std::wstring who = widen(m.name.empty() ? m.userId : m.name);
                g.DrawString(who.c_str(), -1, &body, nameRect, &clipText, &ink);
                Gdiplus::RectF stayRect(68, static_cast<float>(y + 36), static_cast<float>(width - 84), 18);
                if (unreachable) {
                    Gdiplus::SolidBrush lostBrush(Gdiplus::Color(255, 200, 16, 46));
                    g.DrawString(L"Lost Connection!", -1, &small, stayRect, &clipText, &lostBrush);
                } else {
                    g.DrawString(stay_line(m).c_str(), -1, &small, stayRect, &clipText, &muted);
                }
                std::wstring phone = m.phone.empty() ? L"" : widen(m.phone);
                if (unreachable && m.seen > 0) {
                    long long heardMins = nowS > m.seen ? (nowS - m.seen) / 60 : 0;
                    std::wstring heard = heardMins < 1 ? L"Last response just now" : L"Last response " + ago_phrase(heardMins);
                    phone = phone.empty() ? heard : heard + L"   " + phone;
                }
                int shownBattery = unreachable ? 0 : m.battery;
                if (shownBattery >= 0) {
                    if (!phone.empty()) phone += L"   ";
                    phone += L"Battery " + std::to_wstring(shownBattery) + L"%";
                }
                if (m.signal >= 0) {
                    if (!phone.empty()) phone += L"   ";
                    phone += L"Signal " + std::to_wstring(m.signal) + L"/4";
                } else if (m.signal <= -2) {
                    if (!phone.empty()) phone += L"   ";
                    phone += L"No signal";
                }
                if (m.wifi) {
                    if (!phone.empty()) phone += L"   ";
                    phone += L"Wi-Fi";
                    if (!m.ssid.empty()) phone += L" " + widen(m.ssid);
                }
                if (!phone.empty()) {
                    Gdiplus::RectF phoneRect(16, static_cast<float>(y + 62), static_cast<float>(width - 28), 18);
                    g.DrawString(phone.c_str(), -1, &small, phoneRect, &clipText, &muted);
                }
                RECT hist = hit_rect(width - 224, y + cardH - 36, 68, 26);
                RECT edit = hit_rect(width - 150, y + cardH - 36, 60, 26);
                RECT del = hit_rect(width - 84, y + cardH - 36, 66, 26);
                paint_button(g, hist, L"History", small);
                paint_button(g, edit, L"Edit", small);
                paint_button(g, del, L"Delete", small);
                g_hits.push_back({hist, 7, group, m.userId});
                g_hits.push_back({edit, 3, group, m.userId});
                g_hits.push_back({del, 4, group, m.userId});
                y += cardH + 10;
            }
            if (!any_person) {
                g.DrawString(L"No people in this group.", -1, &small, Gdiplus::PointF(12, static_cast<float>(y)), &muted);
                y += 22;
            }
            g.DrawString(L"SAVED PLACES", -1, &section, Gdiplus::PointF(8, static_cast<float>(y + 4)), &muted);
            RECT addPlace = hit_rect(width - 108, y, 96, 26);
            paint_button(g, addPlace, L"Add place", small);
            g_hits.push_back({addPlace, 10, group, ""});
            y += 32;
            bool any_place = false;
            for (const Place& p : g_row_places) {
                if (p.groupId != group) continue;
                any_place = true;
                int cardH = 108;
                fill_round_rect(g, cardShadow, 6, static_cast<float>(y + 2), static_cast<float>(width - 4), static_cast<float>(cardH), 14);
                Gdiplus::SolidBrush card(Gdiplus::Color(255, 255, 255, 255));
                fill_round_rect(g, card, 4, static_cast<float>(y), static_cast<float>(width - 4), static_cast<float>(cardH), 14);
                RECT iconHit = hit_rect(16, y + 12, 44, 44);
                g_hits.push_back({iconHit, 9, group, p.id});
                Gdiplus::Bitmap* pic = place_bitmap(p);
                if (pic) {
                    Gdiplus::GraphicsPath clip;
                    float ix = 16.0f;
                    float iy = static_cast<float>(y + 12);
                    float side = 44.0f;
                    float rr = 10.0f;
                    clip.AddArc(ix, iy, rr, rr, 180.0f, 90.0f);
                    clip.AddArc(ix + side - rr, iy, rr, rr, 270.0f, 90.0f);
                    clip.AddArc(ix + side - rr, iy + side - rr, rr, rr, 0.0f, 90.0f);
                    clip.AddArc(ix, iy + side - rr, rr, rr, 90.0f, 90.0f);
                    clip.CloseFigure();
                    g.SetClip(&clip);
                    g.DrawImage(pic, 16, y + 12, 44, 44);
                    g.ResetClip();
                } else {
                    draw_house_icon(g, 16, static_cast<float>(y + 12), 44);
                }
                Gdiplus::RectF nameRect(70, static_cast<float>(y + 14), static_cast<float>(width - 90), 22);
                g.DrawString(widen(p.name.empty() ? "Place" : p.name).c_str(), -1, &body, nameRect, &clipText, &ink);
                std::wstring meta = std::to_wstring(static_cast<int>(p.radius)) + L" m radius";
                Gdiplus::RectF metaRect(70, static_cast<float>(y + 38), static_cast<float>(width - 90), 18);
                g.DrawString(meta.c_str(), -1, &small, metaRect, &clipText, &muted);
                RECT edit = hit_rect(width - 150, y + cardH - 36, 60, 26);
                RECT del = hit_rect(width - 84, y + cardH - 36, 66, 26);
                paint_button(g, edit, L"Edit", small);
                paint_button(g, del, L"Delete", small);
                g_hits.push_back({edit, 5, group, p.id});
                g_hits.push_back({del, 6, group, p.id});
                y += cardH + 10;
            }
            if (!any_place) {
                g.DrawString(L"No shared places yet. Add one at the map center.", -1, &small, Gdiplus::PointF(12, static_cast<float>(y)), &muted);
                y += 22;
            }
            y += 8;
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
    } catch (...) {
        return 0;
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

static std::vector<CheckIn> history_snapshot() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_history;
}

static std::string coord_text(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.5f", v);
    return buf;
}

static HWND g_dbWnd = nullptr;
static HWND g_dbList = nullptr;

static void fill_db_list() {
    if (!g_dbList) return;
    SendMessageW(g_dbList, LVM_DELETEALLITEMS, 0, 0);
    std::vector<CheckIn> rows = history_snapshot();
    int row = 0;
    for (int i = static_cast<int>(rows.size()) - 1; i >= 0; --i) {
        const CheckIn& c = rows[static_cast<size_t>(i)];
        std::wstring bat = c.battery < 0 ? L"" : (std::to_wstring(c.battery) + L"%");
        std::wstring where = c.area.empty() ? L"—" : widen(c.area);
        add_item(g_dbList, row, {
            widen(stamp(c.ts)),
            widen(c.name.empty() ? c.userId : c.name),
            where,
            widen(coord_text(c.lat)),
            widen(coord_text(c.lon)),
            bat,
        });
        row++;
    }
}

static LRESULT CALLBACK db_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (g_dbList) MoveWindow(g_dbList, 12, 40, rc.right - 24, rc.bottom - 52, TRUE);
        return 0;
    }
    case WM_TIMER:
        fill_db_list();
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        g_dbWnd = nullptr;
        g_dbList = nullptr;
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

static void open_checkin_db(HWND owner) {
    if (g_dbWnd) {
        fill_db_list();
        SetForegroundWindow(g_dbWnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = db_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"FamilyCheckins";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&wc);
        registered = true;
    }
    g_dbWnd = CreateWindowExW(0, L"FamilyCheckins", L"Check-in database",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 860, 520,
        owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    CreateWindowExW(0, L"STATIC", L"Recent coordinates for troubleshooting. A pin is kept when someone moves, changes place, or stays more than a few minutes.",
        WS_CHILD | WS_VISIBLE, 12, 8, 820, 28, g_dbWnd, nullptr, nullptr, nullptr);
    g_dbList = make_list(g_dbWnd, 1, 12, 40, 820, 430);
    add_column(g_dbList, 0, L"Time", 130);
    add_column(g_dbList, 1, L"Person", 140);
    add_column(g_dbList, 2, L"Place", 140);
    add_column(g_dbList, 3, L"Latitude", 110);
    add_column(g_dbList, 4, L"Longitude", 110);
    add_column(g_dbList, 5, L"Battery", 80);
    fill_db_list();
    SetTimer(g_dbWnd, 1, 3000, nullptr);
    ShowWindow(g_dbWnd, SW_SHOW);
}

static HWND g_histWnd = nullptr;
static std::vector<CheckIn> g_hist_rows;
static int g_hist_scroll = 0;
static int g_hist_content = 0;
static std::string g_hist_group;
static std::string g_hist_user;
static std::wstring g_hist_name;

static void reload_hist_rows() {
    g_hist_rows = history_snapshot();
    std::reverse(g_hist_rows.begin(), g_hist_rows.end());
    if (!g_hist_user.empty()) {
        std::vector<CheckIn> kept;
        for (const CheckIn& c : g_hist_rows) {
            if (c.groupId == g_hist_group && c.userId == g_hist_user) kept.push_back(c);
        }
        g_hist_rows.swap(kept);
    }
}

static LRESULT CALLBACK hist_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        reload_hist_rows();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL: {
        g_hist_scroll -= GET_WHEEL_DELTA_WPARAM(wp) / 4;
        if (g_hist_scroll < 0) g_hist_scroll = 0;
        RECT rc;
        GetClientRect(hwnd, &rc);
        int max_scroll = g_hist_content - rc.bottom;
        if (max_scroll < 0) max_scroll = 0;
        if (g_hist_scroll > max_scroll) g_hist_scroll = max_scroll;
        InvalidateRect(hwnd, nullptr, FALSE);
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
        g.Clear(Gdiplus::Color(255, 244, 241, 234));
        g.TranslateTransform(0, static_cast<float>(-g_hist_scroll));
        Gdiplus::Font title(L"Segoe UI", 18, Gdiplus::FontStyleBold);
        Gdiplus::Font name(L"Segoe UI", 14, Gdiplus::FontStyleBold);
        Gdiplus::Font body(L"Segoe UI", 11);
        Gdiplus::SolidBrush ink(Gdiplus::Color(255, 28, 27, 25));
        Gdiplus::SolidBrush muted(Gdiplus::Color(255, 92, 88, 79));
        std::wstring heading = g_hist_name.empty() ? L"Recent check-ins" : (g_hist_name + L"'s check-ins");
        g.DrawString(heading.c_str(), -1, &title, Gdiplus::PointF(20, 16), &ink);
        int y = 58;
        int width = rc.right - 40;
        if (width < 200) width = 200;
        if (g_hist_rows.empty()) {
            g.DrawString(L"No check-ins yet. They show up after a phone sends its location.", -1, &body, Gdiplus::PointF(20, static_cast<float>(y)), &muted);
            y += 30;
        }
        std::string lastKey;
        for (const CheckIn& c : g_hist_rows) {
            std::string key = c.groupId + ":" + c.userId;
            if (key != lastKey) {
                lastKey = key;
                y += 8;
                std::wstring who = widen(c.name.empty() ? c.userId : c.name);
                g.DrawString(who.c_str(), -1, &name, Gdiplus::PointF(20, static_cast<float>(y)), &ink);
                y += 28;
            }
            Gdiplus::SolidBrush card(Gdiplus::Color(255, 255, 252, 247));
            g.FillRectangle(&card, 20, y, width, 58);
            std::wstring where = c.area.empty() ? L"Away from saved places" : widen(c.area);
            g.DrawString(where.c_str(), -1, &body, Gdiplus::PointF(32, static_cast<float>(y + 8)), &ink);
            std::wstring detail = widen(stamp(c.ts)) + L"   " + widen(coord_text(c.lat)) + L", " + widen(coord_text(c.lon));
            if (c.battery >= 0) detail += L"   Battery " + std::to_wstring(c.battery) + L"%";
            g.DrawString(detail.c_str(), -1, &body, Gdiplus::PointF(32, static_cast<float>(y + 30)), &muted);
            y += 66;
        }
        g_hist_content = y + 16;
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(dib);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        g_histWnd = nullptr;
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

static void open_history(HWND owner, const std::string& group, const std::string& user, const std::string& name) {
    g_hist_group = group;
    g_hist_user = user;
    g_hist_name = widen(name.empty() ? user : name);
    reload_hist_rows();
    g_hist_scroll = 0;
    std::wstring title = g_hist_name + L" history";
    if (g_histWnd) {
        SetWindowTextW(g_histWnd, title.c_str());
        InvalidateRect(g_histWnd, nullptr, FALSE);
        SetForegroundWindow(g_histWnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = hist_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"FamilyHistory";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&wc);
        registered = true;
    }
    g_histWnd = CreateWindowExW(0, L"FamilyHistory", title.c_str(),
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 560, 720,
        owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetTimer(g_histWnd, 1, 3000, nullptr);
    ShowWindow(g_histWnd, SW_SHOW);
}



static void paint_shell(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);
    struct End {
        HWND hwnd;
        PAINTSTRUCT* ps;
        ~End() { EndPaint(hwnd, ps); }
    } end{hwnd, &ps};
    RECT rc{};
    GetClientRect(hwnd, &rc);
    int w = rc.right > 0 ? rc.right : 1;
    int h = rc.bottom > 0 ? rc.bottom : 1;
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP dib = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ old = SelectObject(mem, dib);
    Gdiplus::Graphics g(mem);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    Gdiplus::SolidBrush page(Gdiplus::Color(255, 241, 245, 249));
    g.FillRectangle(&page, 0, 0, w, h);
    const int headerH = 86;
    Gdiplus::SolidBrush blue(Gdiplus::Color(255, 29, 78, 216));
    g.FillRectangle(&blue, 0, 0, w, headerH);
    float pinX = 18;
    float pinY = 14;
    float pin = 22;
    Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));
    g.FillEllipse(&white, pinX, pinY, pin, pin);
    Gdiplus::PointF tri[3] = {
        Gdiplus::PointF(pinX + pin * 0.18f, pinY + pin * 0.62f),
        Gdiplus::PointF(pinX + pin * 0.82f, pinY + pin * 0.62f),
        Gdiplus::PointF(pinX + pin * 0.50f, pinY + pin * 1.22f),
    };
    g.FillPolygon(&white, tri, 3);
    Gdiplus::SolidBrush hole(Gdiplus::Color(255, 29, 78, 216));
    g.FillEllipse(&hole, pinX + pin * 0.30f, pinY + pin * 0.24f, pin * 0.40f, pin * 0.40f);
    Gdiplus::Font title(L"Segoe UI", 18, Gdiplus::FontStyleBold);
    Gdiplus::Font sub(L"Segoe UI", 11);
    Gdiplus::SolidBrush titleInk(Gdiplus::Color(255, 255, 255, 255));
    Gdiplus::SolidBrush subInk(Gdiplus::Color(230, 255, 255, 255));
    g.DrawString(L"Family Location Server", -1, &title, Gdiplus::PointF(52, 10), &titleInk);
    Gdiplus::StringFormat clip;
    clip.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    clip.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    float textW = static_cast<float>(w - 250);
    if (textW < 40) textW = 40;
    g.DrawString(widen(g_home_line).c_str(), -1, &sub, Gdiplus::RectF(52, 42, textW, 18), &clip, &subInk);
    g.DrawString(widen(g_tail_line).c_str(), -1, &sub, Gdiplus::RectF(52, 60, textW, 18), &clip, &subInk);
    bool online = g_status.find("Listening") != std::string::npos;
    draw_pill(g, static_cast<float>(w - 176), 26, 154, 32,
        online ? Gdiplus::Color(255, 22, 163, 74) : Gdiplus::Color(255, 220, 38, 38),
        online ? L"Server Online" : L"Offline", sub);
    int footerTop = h - 56;
    if (footerTop < headerH) footerTop = headerH;
    Gdiplus::SolidBrush footBg(Gdiplus::Color(255, 248, 250, 252));
    g.FillRectangle(&footBg, 0, footerTop, w, h - footerTop);
    Gdiplus::Pen hair(Gdiplus::Color(255, 226, 232, 240), 1);
    g.DrawLine(&hair, 0, footerTop, w, footerTop);
    Gdiplus::Font foot(L"Segoe UI", 11);
    Gdiplus::SolidBrush footInk(Gdiplus::Color(255, 71, 85, 105));
    std::wstring status = online ? L"Listening on port 8000" : widen(g_status);
    if (!g_live) status += L"  ·  updates paused";
    status += L"   ·   " + std::to_wstring(g_n_people) + L" people   ·   " + std::to_wstring(g_n_places) + L" places";
    g.DrawString(status.c_str(), -1, &foot, Gdiplus::RectF(18, static_cast<float>(footerTop + 6), static_cast<float>(w - 36), 20), &clip, &footInk);
    Gdiplus::Font hint(L"Segoe UI", 9);
    g.DrawString(L"Drag a place to move it. Scroll the wheel on a selected place to change its radius. History, edit, and delete stay on each card.",
        -1, &hint, Gdiplus::RectF(18, static_cast<float>(footerTop + 28), static_cast<float>(w - 36), 18), &clip, &footInk);
    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
}

#ifdef _WIN32
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#endif

#include "httplib.h"
#include "json.hpp"

#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <string>

struct LocationData {
    double lat = 0;
    double lon = 0;
    long long timestamp = 0;
};

std::mutex data_mutex;
std::map<std::string, LocationData> latest_locations;

long long now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool read_number(const nlohmann::json& j, const char* key, double& out) {
    if (!j.contains(key) || !j[key].is_number()) return false;
    out = j[key].get<double>();
    return true;
}

int main() {
    httplib::Server server;

    server.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(
            "FamilyLocationServer\n"
            "GET  /health\n"
            "POST /send   {\"userId\",\"groupId\",\"lat\",\"lon\",\"ts\"?}\n"
            "GET  /fetch?userId=&groupId=\n",
            "text/plain");
    });

    server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("OK", "text/plain");
    });

    server.Post("/send", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = nlohmann::json::parse(req.body);
            if (!j.contains("userId") || !j["userId"].is_string() ||
                !j.contains("groupId") || !j["groupId"].is_string() ||
                !j.contains("lat") || !j.contains("lon")) {
                res.status = 400;
                res.set_content("{\"error\":\"Missing fields\"}", "application/json");
                return;
            }

            std::string user_id = j["userId"].get<std::string>();
            std::string group_id = j["groupId"].get<std::string>();
            double lat = 0;
            double lon = 0;
            if (user_id.empty() || group_id.empty() || !read_number(j, "lat", lat) ||
                !read_number(j, "lon", lon)) {
                res.status = 400;
                res.set_content("{\"error\":\"Missing fields\"}", "application/json");
                return;
            }
            if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
                res.status = 400;
                res.set_content("{\"error\":\"Latitude or longitude out of range\"}",
                                "application/json");
                return;
            }

            long long ts = now_seconds();
            if (j.contains("ts") && j["ts"].is_number()) {
                ts = j["ts"].get<long long>();
            }

            {
                std::lock_guard<std::mutex> guard(data_mutex);
                latest_locations[group_id + ":" + user_id] = LocationData{lat, lon, ts};
            }

            nlohmann::json resp;
            resp["status"] = "saved";
            res.set_content(resp.dump(), "application/json");
            std::cout << "saved " << group_id << ":" << user_id << " " << lat << "," << lon
                      << "\n";
        } catch (const std::exception& e) {
            res.status = 400;
            nlohmann::json resp;
            resp["error"] = std::string("Invalid JSON: ") + e.what();
            res.set_content(resp.dump(), "application/json");
        }
    });

    server.Get("/fetch", [](const httplib::Request& req, httplib::Response& res) {
        auto user_id = req.get_param_value("userId");
        auto group_id = req.get_param_value("groupId");
        if (user_id.empty() || group_id.empty()) {
            res.status = 400;
            res.set_content("{\"error\":\"Missing userId or groupId\"}", "application/json");
            return;
        }

        LocationData loc;
        {
            std::lock_guard<std::mutex> guard(data_mutex);
            auto it = latest_locations.find(group_id + ":" + user_id);
            if (it == latest_locations.end()) {
                res.status = 404;
                res.set_content("{\"error\":\"No data for user\"}", "application/json");
                return;
            }
            loc = it->second;
        }

        nlohmann::json resp;
        resp["userId"] = user_id;
        resp["groupId"] = group_id;
        resp["lat"] = loc.lat;
        resp["lon"] = loc.lon;
        resp["ts"] = loc.timestamp;
        res.set_content(resp.dump(), "application/json");
    });

    std::cout << "Server listening on port 8000\n";
    if (!server.listen("0.0.0.0", 8000)) {
        std::cerr << "Failed to listen on 0.0.0.0:8000\n";
        return 1;
    }
    return 0;
}

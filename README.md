# Family Location Server

Windows test server for the location check. It keeps the latest pin for each person in memory and listens on port 8000.

## Download

The built program is [FamilyLocationServer.exe](https://github.com/i2ichardt/family-location-server/releases/latest/download/FamilyLocationServer.exe) (also in this repo).

1. Run `FamilyLocationServer.exe` and leave the window open.
2. If Windows blocks the unsigned file, choose More info, then Run anyway.
3. Allow it on Private networks. If nothing connects, run `allow-firewall.ps1` in PowerShell as Administrator.

## Phone

Use the Location Test app: https://github.com/i2ichardt/location-test

- Same Wi-Fi: `http://<PC LAN address>:8000`
- Tailscale: `http://<PC 100.x address>:8000`

## API

- `GET /health` returns `OK`
- `POST /send` with JSON `{"userId","groupId","lat","lon"}`
- `GET /fetch?userId=Alice&groupId=Fam1`

No password and no HTTPS. Keep port 8000 on your tailnet only.

## Rebuild

Needs a C++17 compiler. Headers are in `include/` (`cpp-httplib` and `nlohmann/json`, both MIT).

```
cmake -S . -B build
cmake --build build --config Release
```

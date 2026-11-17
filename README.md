# Family location server

Windows program for the family map. Phones send a pin, a name, a phone number, and a photo. This program keeps the latest pin for each person, plus the places the group shares. It listens on port **8000** on every address of this PC, so the same window answers on home Wi-Fi and on Tailscale.

The phone app is [location-test](https://github.com/i2ichardt/location-test).

## Download

The compiled program is on the [v1.7 release](https://github.com/i2ichardt/family-location-server/releases/tag/v1.7).

- [FamilyLocationServer.exe](https://github.com/i2ichardt/family-location-server/releases/download/v1.7/FamilyLocationServer.exe)
- [FamilyLocationServer-windows.zip](https://github.com/i2ichardt/family-location-server/releases/download/v1.7/FamilyLocationServer-windows.zip) — program, how-to, and firewall script

## On the PC

The window lists **Home** as `http://<LAN address>:8000` and **Tailscale** as `http://<100.x address>:8000`. The saved Tailscale address, the one phones use when they are away, is under **File → Settings**.

The map shows the people who have sent a pin. People standing together share one pin, side by side. Each saved place is drawn with a circle the size of its arrive and leave distance, labeled with the meters. Drag a place's dot to move it. Click a place, then scroll the mouse wheel, to change that distance. Click empty map to clear the selection so the wheel zooms again. **View → Satellite** switches to aerial photos. **View → Pause updates** freezes the map and the list. Profile photos are the pins and update when a phone saves a new one. Each person has a **History** button for that person's check-ins. **File → Check-in database** is the full trail as coordinates. Battery percentage shows on each person when the phone sends it.

## Run it

1. Save `FamilyLocationServer.exe` in a folder. `allow-firewall.ps1` can sit next to it.
2. Double-click the program.
3. The window shows people and shared places. The status line should say it is listening on port 8000.
4. If Windows says it protected your PC, choose **More info**, then **Run anyway**. Allow the firewall prompt on **Private** networks.
5. Minimize the window to keep the server running. It stays on the taskbar. Closing the window stops the program and ends the process. There is no system-tray icon.

Profiles, photos, pins, and places are saved in a `data` folder next to the program. **File → Open saved data** opens that folder. Closing the window does not erase them.

## Addresses the phone should save

Find both on this PC and type them into the phone's Settings. Leave the phone on **Auto**. Auto tries the home address first. If the phone is not on that Wi-Fi, the home address does not answer and the app uses Tailscale.

| Where the phone is | Address |
|---|---|
| Same Wi-Fi | `http://192.168.x.x:8000` from `ipconfig` |
| Anywhere else | `http://100.x.x.x:8000` from `tailscale ip -4` |

Use `http://`, not `https://`. A MagicDNS name works too: `http://PCNAME.tailnet.ts.net:8000`.

Every phone in the family uses the same group name.

## If a phone cannot connect

- The window is open, or the program is minimized on the taskbar. Closing it stops the server.
- The URL starts with `http://` and ends with `:8000`.
- Both Tailscale apps say Connected when you are testing the Tailscale address.
- Right-click `allow-firewall.ps1`, choose **Run with PowerShell**, and approve the administrator prompt. That opens TCP port 8000 on private networks only.

## What the phones send

| Call | What it stores |
|---|---|
| `POST /profile` | Name, phone number, and the profile photo |
| `POST /send` | Latest latitude and longitude for that person |
| `GET /group?groupId=Fam1` | Everyone in the group, plus shared places |
| `POST /place` | A place the whole group can see |
| `GET /health` | `OK` if the program is running |

`GET /fetch` still returns one person's latest pin.

## Keep it private

There is no password and no HTTPS. Tailscale encrypts the path between the phone and this PC. Anyone on your tailnet who can open port 8000 can read the pins. Do not forward port 8000 on your router.

## Rebuild

`server.cpp` is C++17. Headers in `include/` are cpp-httplib and nlohmann/json, both MIT.

```
cmake -S . -B build
cmake --build build --config Release
```

# Family Location Server

Windows program for the location check. The phone sends one pin. This program keeps the latest pin for each person and hands it back. It listens on port **8000**. Nothing is saved after you close the window.

The phone app is [location-test](https://github.com/i2ichardt/location-test).

## Download

[Download FamilyLocationServer.exe](https://github.com/i2ichardt/family-location-server/raw/main/FamilyLocationServer.exe)

On a phone, tap that link, then the download arrow. On the PC, open the link and save the file.

## Run it

1. Put `FamilyLocationServer.exe` in a folder you can find. `allow-firewall.ps1` can sit next to it.
2. Double-click the program. Leave the window open.
3. It should say **Server listening on port 8000**.
4. Windows may say it protected your PC, because the file is unsigned. Choose **More info**, then **Run anyway**.
5. If a firewall prompt appears, allow it on **Private** networks only.

Do not close the window. Closing it stops the server. Pins are forgotten when it stops.

## Address to type on the phone

The phone needs the address of **this PC**, not the phone.

**Same Wi-Fi**

1. On the PC, open Command Prompt and run `ipconfig`.
2. Under the Wi-Fi adapter, copy the **IPv4 Address**. It looks like `192.168.1.20`.
3. On the phone, set Server URL to `http://192.168.1.20:8000` (use your number).

**Away from home, through Tailscale**

1. Install Tailscale on the PC and on the phone. Sign both into the same account.
2. On the PC, open Tailscale and copy the address that starts with `100.`.
3. On the phone, set Server URL to `http://100.x.x.x:8000` (use your number).
4. Tailscale has to say Connected on both devices.

Use `http://`, not `https://`. There is no certificate on this test server.

## Check that it works

On the phone, open Location Test and tap **Check server**. The status should show `200` and `OK`.

Then tap **Send location**, then **Fetch location**. Fetch should show the same user, group, latitude, and longitude you just sent.

From the PC you can also open a browser to `http://127.0.0.1:8000/health`. It should say `OK`.

## If the phone cannot connect

- The black window is still open and still says it is listening.
- The URL starts with `http://`, has the PC address, and ends with `:8000`. No extra slash or path.
- Phone and PC are on the same Wi-Fi, or both Tailscale apps are Connected.
- You did not type the phone's own address.
- Windows Firewall is blocking it. Right-click `allow-firewall.ps1`, choose **Run with PowerShell**, and approve the administrator prompt. That opens TCP port 8000 on private networks only.

## What it stores

One latest pin per user and group, in memory only.

| Call | What it does |
|---|---|
| `GET /health` | Returns `OK` if the program is running. |
| `POST /send` | Body is JSON: `{"userId","groupId","lat","lon"}`. Replaces that person's pin. |
| `GET /fetch?userId=Alice&groupId=Fam1` | Returns that person's latest pin. |

`userId` and `groupId` must match on send and fetch. `Alice` and `alice` are different. The sample in the phone app is user `Alice`, group `Fam1`.

## Keep it private

There is no password and no HTTPS. Anyone who can reach port 8000 can send and read pins.

- Do not forward port 8000 on your router.
- Do not put this on a public IP.
- Use your home Wi-Fi, or Tailscale. Tailscale is the right choice when the phone is not on the same Wi-Fi.

## Rebuild

The source is `server.cpp`. Headers in `include/` are cpp-httplib and nlohmann/json (both MIT). You need a C++17 compiler.

```
cmake -S . -B build
cmake --build build --config Release
```

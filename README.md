# Family location server

The phone opens on a map. Pins, names, and photos stay on this PC. At home the app uses the PC’s LAN address. Away from that Wi-Fi it uses the Tailscale address. Both are saved in the phone’s Settings.

The Android app is [location-test](https://github.com/i2ichardt/location-test).

## Download

The compiled server is [server 1.11](https://github.com/i2ichardt/family-location-server/releases/tag/v1.11).

[Download FamilyLocationServer-windows.zip](https://github.com/i2ichardt/family-location-server/releases/download/v1.11/FamilyLocationServer-windows.zip)

## Set it up

1. **Join one tailnet.** Install Tailscale on the Windows 11 PC and on each phone. Sign in with the same account and pick the Personal plan. It is free for non-commercial use, up to 6 users, with as many devices as you need.

2. **Start the server.** Unzip the Windows download and run FamilyLocationServer.exe. It listens on port 8000 for both the home network and Tailscale. The saved Tailscale address is under File, then Settings. Minimize hides the window and leaves a tray icon. Click the icon to open it again. Closing the window stops it. If Windows blocks the unsigned file, choose More info, then Run anyway, and allow it on Private networks.

3. **Install the app** on each phone from the location-test releases. Allow location when it asks.

4. **Save both addresses** in the phone’s Settings. Home Wi-Fi is this PC’s LAN address from `ipconfig`, like `http://192.168.1.5:8000`. Tailscale is `tailscale ip -4`, like `http://100.x.x.x:8000`. Leave the choice on Auto. Use the same group name on every phone.

## Which address to type

| Phone | Server URL |
| --- | --- |
| On the same Wi-Fi | `http://PC-LAN-address:8000` |
| Mobile data, or any network | `http://100.x.x.x:8000` |

Tailscale encrypts the tunnel. This server does not. It has no password and no HTTPS. Anyone on the tailnet who knows the address can read the pins. Do not forward port 8000 to the public internet.

## The window

The map is on the left. Family and saved places are on the right. The footer shows port 8000 and the people and places counts.

Click a person or place icon to center the map. Add place creates a shared place at the map center. Drag a place to move it. Select a place and use the mouse wheel to change its radius. Each person has History, Edit, and Delete.

File, then Settings sets the Tailscale address, how many records to keep per person (default 50), and the alive check. The server asks each phone if it is still there. If no answer comes back in time, that person is not contactable until they respond.

A fix that is too fast to be real, a coarse network fix right after GPS, or a point that says someone left while their phone is still on that place’s Wi-Fi is dropped. Someone who is still keeps the saved anchor, so a person in bed does not flip between left home and arrived home.

The server keeps profiles, photos, pins, and places in a data folder next to the program. See HOWTO.txt in the zip.

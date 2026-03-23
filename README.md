# ESP32 Chat Portal

A **self-contained chat portal** running on an ESP32. The device creates its own Wi‑Fi network and hosts a web-based chat application with real‑time messaging, user authentication, and admin controls. **No messages are stored permanently** – everything resets when the ESP32 is powered off or restarted.

## Overview
This project demonstrates how an ESP32 can function as a fully self-contained communication system without relying on external infrastructure. It is designed for offline environments, local communication, and privacy-focused use cases.

## Use Cases
- Offline communication in classrooms or events
- Local chat system without internet
- Privacy-focused temporary messaging
- Learning project for ESP32 networking and WebSockets

## Features

-📡 Wi-Fi Access Point – no router required
-🌐 Captive Portal – automatic redirection on connect
-👤 User Authentication – unique username & password (RAM only)
-⚡ Real-time Messaging via WebSockets
-🛠 Admin Panel
   Kick users (temporary bans)
   Mute all users
   Clear chat
   Edit messages (silent overwrite)
   Edit usernames
   Enable/disable chat
   Change Wi-Fi password
   Shut down portal
-🌙 Dark UI – clean and readable
-🧠 Memory-safe design – max 8 users
-🔒 No persistent storage – full reset on reboot (privacy-first)

## Hardware Requirements

- An **ESP32** development board 
- Micro‑USB cable for power and programming

## Software Requirements

- **Arduino IDE** 
- Libraries (install via Library Manager):
  - **WebSockets** by Markus Sattler
  - **ArduinoJson** by Benoit Blanchon (version 6)

## Setup

1. **Clone or download** this repository.
2. Open the `.ino` file in Arduino IDE.
3. **Change the Wi‑Fi credentials and admin PIN** at the top of the sketch (recommended):
   ```cpp
   const char* ap_ssid = "YourSSID";       // change to your preferred network name
   const char* ap_password = "YourPass";   // change to a strong password (min 8 chars)
   const char* admin_pin = "YourAdminPin"; // change to a secure admin PIN
4. Select your board
5. Open Serial Monitor (115200baud) to see ESP's IP address
## Usage
Connect to the Wi‑Fi network created by the ESP32 (default SSID: *YourSSID*, password: *YourPass*).

The captive portal should open automatically with the login page. If not, open *http://192.168.4.1* in a browser.

Register with a username (≥4 chars) and password (≥8 chars).

To become admin, log in with username *admin* and the admin PIN you set in the sketch.

Start chatting! The admin panel appears on the left (only for admin).

## Limitations
-Maximum 8 simultaneous users (ESP32 memory limit).
-Messages are not saved after the device restarts.
-New users see only messages sent after they join.
-Wi‑Fi range is typical for ESP32 (~30–100 meters line of sight).

## License
This project is licensed under the MIT License — see the [LICENSE] file for details.

## Author
Ram Barabde – [https://github.com/Ram2699](GitHub)

## Credits
Developed with assistance from AI tools(DeepSeek).
## Acknowledgements
Built with [https://github.com/Links2004/arduinoWebSockets](WebSockets) and [https://arduinojson.org/](ArduinoJson libraries).

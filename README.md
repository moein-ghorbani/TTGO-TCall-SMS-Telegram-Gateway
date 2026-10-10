# TTGO T-Call -- SMS to Telegram Gateway with GPRS Fallback and Missed-Call Alert

ESP32 + SIM800L gateway that forwards incoming SMS (including multipart Persian/Unicode) and missed-call notifications to Telegram. Works over Wi-Fi via an HTTP proxy (CONNECT tunnel + Mbed TLS) with automatic GPRS fallback when Wi-Fi is unavailable.

Original firmware path on Windows: `Documents/Arduino/TTGO_TCall_SMS_Telegram_GPRS_MissedCall/TTGO_TCall_SMS_Telegram_GPRS_MissedCall.ino` (private). This folder is a sanitized public copy -- no credentials, IDs, or private network addresses. Original file is untouched.

## Features

- **SMS in PDU mode** (`AT+CMGF=0`, `AT+CPMS`, `AT+CNMI=2,1`) -- handles real modem storage.
- **Full PDU decoding**: GSM 7-bit, 8-bit, UCS-2 with UTF-16 surrogate-pair handling. Converts to UTF-8.
- **Multipart SMS** (concatenated): 8-bit and 16-bit reference, reassembles up to 16 parts, 10-minute timeout, sender + reference grouping.
- **Telegram delivery** over `api.telegram.org:443` via **HTTP CONNECT proxy tunnel** + **Mbed TLS** (no BearSSL). Parses `HTTP/1.1 200` + `"ok":true`.
- **Dual route**: primary Wi-Fi to internal proxy; fallback GPRS to external proxy (hostname, then IP fallback). `ensureGPRS()` with 30 s retry, auto-disconnects GPRS when Wi-Fi returns.
- **Missed-call detection**: parses `RING` / `+CLIP: "number"` / `NO CARRIER`, forwards as Telegram message to two chats.
- **Robust modem init**: `ATE0`, `AT+CMEE=2`, `AT+CPIN?`, `AT+CLIP=1`, `AT+CSCS="UCS2"`, power-key sequencing.
- **Base64 proxy auth**, hex/UCS-2 utilities, timing guards (SMS every 5 s, Wi-Fi check every 10 s).

## Hardware

- Board: **LILYGO TTGO T-Call** (ESP32 + SIM800L).
- Modem pins (as in sanitized sketch):
  - `MODEM_RX 26`, `MODEM_TX 27`, `MODEM_POWER 23`, `MODEM_PWRKEY 4`, `MODEM_RESET 5`, `MODEM_BAUD 115200` on `SerialAT(1)`.
  - Power sequence: POWER HIGH, PWRKEY HIGH, RESET HIGH, then PWRKEY LOW 1 s -> HIGH, 5 s settle.
- SIM: any data-enabled SIM for GPRS fallback.

Note: This differs from the separate `ESP32-S2 Mini + SIM800L` private project (pins 21/33/34/8/9). Use the pins above for TTGO T-Call.

## Wiring

No external wiring beyond the TTGO T-Call on-board SIM800L. Optional external antenna for SIM800L recommended for weak signal.

## Dependencies (Arduino IDE)

Install via Library Manager:

- `TinyGSM` (with `TINY_GSM_MODEM_SIM800`)
- `ArduinoJson` (v6 or v7 -- sketch uses `JsonDocument`)
- `Mbed TLS` (ESP32 Arduino core includes `mbedtls/*`)

Board package: `esp32` by Espressif, select **TTGO T-Call** / **ESP32 Dev Module**.

## Configuration

All secrets are placeholders in the public sketch. Edit the top section:

```cpp
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

const char* PROXY_HOST = "YOUR_PROXY_HOST";       // e.g. 192.168.1.10
const uint16_t PROXY_PORT = 808;                  // e.g. 8080
const char* PROXY_USER = "YOUR_PROXY_USERNAME";
const char* PROXY_PASS = "YOUR_PROXY_PASSWORD";

const char* EXTERNAL_PROXY_HOST = "YOUR_EXTERNAL_PROXY_HOST"; // e.g. proxy.example.com
const char* EXTERNAL_PROXY_IP   = "YOUR_EXTERNAL_PROXY_IP";   // e.g. 203.0.113.10
const uint16_t EXTERNAL_PROXY_PORT = 8080;

const char* GPRS_APN  = "YOUR_GPRS_APN";  // e.g. mcinet / mtnirancell
const char* GPRS_USER = "";               // if required
const char* GPRS_PASS = "";

const char* BOT_TOKEN = "YOUR_TELEGRAM_BOT_TOKEN"; // from @BotFather
const char* CHAT_ID_1 = "YOUR_TELEGRAM_CHAT_ID_1"; // e.g. 123456789
const char* CHAT_ID_2 = "YOUR_TELEGRAM_CHAT_ID_2"; // optional, set to "" to disable second recipient
```

Telegram host/port are `api.telegram.org:443` (change only if self-hosting).

## Setup

1. Copy this folder, open `TTGO_TCall_SMS_Telegram_GPRS_MissedCall_Public.ino` in Arduino IDE.
2. Fill the placeholders above. For Telegram, create a bot via `@BotFather` and get `chat_id` via `@userinfobot` or `getUpdates`.
3. Select board + port, set `Upload Speed 921600`, flash.
4. Open Serial Monitor at `115200` -- you should see:
   ```
   TTGO T-Call SMS -> Telegram
   [MODEM] Initializing...
   [WiFi] Connecting... / Connected.
   [SYSTEM] Ready.
   ```
5. Send a test SMS to the SIM -- it should appear on Telegram as `New SMS / From: +...`.

## How It Works

- `loop()` polls `AT+CMGL=4` every 5 s, parses `+CMGL:` headers + PDU, decodes sender/text/UDH, reassembles multipart, calls `sendTelegramToBoth()`, then `AT+CMGD=index` on success.
- `sendTelegram()` opens a TCP socket to the proxy (Wi-Fi `WiFiClient` or GPRS `TinyGsmClient`), issues `CONNECT api.telegram.org:443`, negotiates TLS with `MBEDTLS_SSL_VERIFY_NONE`, POSTs JSON to `/bot<token>/sendMessage`.
- `handleCallLine()` watches `RING`/`+CLIP`/`NO CARRIER` interleaved in `+CMGL` or URCs.

## Security Notes for Public Release

- Never commit `WIFI_SSID/PASS`, `BOT_TOKEN`, `CHAT_ID`, or proxy `HOST/USER/PASS/IP/GPRS_APN`. Use the placeholders.
- `GPRS_USER/PASS` left empty by default; fill only if your carrier requires it.
- Keep `TELEGRAM_HOST` verification off only if you understand the trade-off (`MBEDTLS_SSL_VERIFY_NONE` in sketch).

## Folder Contents

- `TTGO_TCall_SMS_Telegram_GPRS_MissedCall_Public.ino` -- sanitized sketch (37541 bytes, 2112 lines). Persian strings translated to English; all private values replaced.
- `README.md` -- this file.
- Suggested `.gitignore` (create if publishing as repo):
  ```
  .vscode/
  *.hex
  *.bin
  build/
  ```

## License

Choose one before publishing (e.g. MIT). No license is implied by this sanitized copy.

## Original vs Public

- `TTGO_TCall_SMS_Telegram_GPRS_MissedCall/` -- private original, untouched, contains your real credentials and Persian UI strings.
- `TTGO_TCall_SMS_Telegram_GPRS_MissedCall_Public/` -- public sanitized copy in this folder. Diff: 12 credential placeholders + 3 message translations + generic `Primary WiFi` labels.

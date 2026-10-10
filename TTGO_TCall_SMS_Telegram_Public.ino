#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>

#define TINY_GSM_MODEM_SIM800
#include <TinyGsmClient.h>

#include <mbedtls/ssl.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/error.h>

// ============================================================
// WiFi
// ============================================================

const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// ============================================================
// CCProxy
// ============================================================

const char* PROXY_HOST = "YOUR_PROXY_HOST";       // e.g. 192.168.1.10
const uint16_t PROXY_PORT = 808;                    // e.g. 8080

const char* PROXY_USER = "YOUR_PROXY_USERNAME";
const char* PROXY_PASS = "YOUR_PROXY_PASSWORD";

// External proxy used when primary WiFi is unavailable.
const char* EXTERNAL_PROXY_HOST = "YOUR_EXTERNAL_PROXY_HOST"; // e.g. proxy.example.com
const char* EXTERNAL_PROXY_IP   = "YOUR_EXTERNAL_PROXY_IP";   // e.g. 203.0.113.10
const uint16_t EXTERNAL_PROXY_PORT = 8080;                  // e.g. 8080

// SIM800L GPRS fallback.
const char* GPRS_APN  = "YOUR_GPRS_APN";  // e.g. mcinet / mtnirancell
const char* GPRS_USER = "";
const char* GPRS_PASS = "";

// ============================================================
// Telegram
// ============================================================

const char* BOT_TOKEN =
  "YOUR_TELEGRAM_BOT_TOKEN";

const char* CHAT_ID_1 = "YOUR_TELEGRAM_CHAT_ID_1"; // e.g. 123456789
const char* CHAT_ID_2 = "YOUR_TELEGRAM_CHAT_ID_2"; // e.g. 987654321 (optional, set to "" to disable)

const char* TELEGRAM_HOST = "api.telegram.org";
const uint16_t TELEGRAM_PORT = 443;

// ============================================================
// SIM800L / TTGO T-Call
// ============================================================

#define MODEM_RX       26
#define MODEM_TX       27
#define MODEM_POWER    23
#define MODEM_PWRKEY    4
#define MODEM_RESET     5

#define MODEM_BAUD 115200

HardwareSerial SerialAT(1);
TinyGsm modem(SerialAT);
TinyGsmClient gsmClient(modem);

// Missed-call state.
bool incomingCallActive = false;
String incomingCallNumber = "";
bool gprsReady = false;

// ============================================================
// Timing
// ============================================================

unsigned long lastSMSCheck = 0;
unsigned long lastWiFiCheck = 0;

const unsigned long SMS_INTERVAL = 5000;
const unsigned long WIFI_INTERVAL = 10000;
const unsigned long GPRS_RETRY_INTERVAL = 30000;
unsigned long lastGprsAttempt = 0;

// ============================================================
// Multipart SMS
// ============================================================

struct MultipartSMS {
  bool active = false;

  String sender;
  uint16_t reference = 0;

  uint8_t total = 0;
  uint8_t received = 0;

  String parts[16];

  unsigned long lastPartTime = 0;
};

MultipartSMS multipart;

const unsigned long MULTIPART_TIMEOUT = 10UL * 60UL * 1000UL;

// ============================================================
// Base64
// ============================================================

String base64Encode(const String& input) {

  const char table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

  String output;

  int val = 0;
  int valb = -6;

  for (unsigned char c : input) {

    val = (val << 8) + c;
    valb += 8;

    while (valb >= 0) {

      output += table[(val >> valb) & 0x3F];

      valb -= 6;
    }
  }

  if (valb > -6) {

    output +=
      table[((val << 8) >> (valb + 8)) & 0x3F];
  }

  while (output.length() % 4) {
    output += '=';
  }

  return output;
}

// ============================================================
// Hex helpers
// ============================================================

int hexValue(char c) {

  if (c >= '0' && c <= '9') return c - '0';

  if (c >= 'A' && c <= 'F') return c - 'A' + 10;

  if (c >= 'a' && c <= 'f') return c - 'a' + 10;

  return -1;
}

uint8_t hexByte(const String& s, int pos) {

  int h = hexValue(s[pos]);
  int l = hexValue(s[pos + 1]);

  return (h << 4) | l;
}

// ============================================================
// UTF-8 conversion
// ============================================================

void appendUTF8(String& out, uint32_t c) {

  if (c <= 0x7F) {

    out += char(c);
  }

  else if (c <= 0x7FF) {

    out += char(0xC0 | (c >> 6));
    out += char(0x80 | (c & 0x3F));
  }

  else if (c <= 0xFFFF) {

    // UTF-16 surrogate code points are not valid UTF-8 scalar values.
    // They are handled as a pair by decodeUCS2().
    if (c >= 0xD800 && c <= 0xDFFF) {
      return;
    }

    out += char(0xE0 | (c >> 12));
    out += char(0x80 | ((c >> 6) & 0x3F));
    out += char(0x80 | (c & 0x3F));
  }

  else if (c <= 0x10FFFF) {

    out += char(0xF0 | (c >> 18));
    out += char(0x80 | ((c >> 12) & 0x3F));
    out += char(0x80 | ((c >> 6) & 0x3F));
    out += char(0x80 | (c & 0x3F));
  }
}

// ============================================================
// GSM 7-bit basic decoding
// ============================================================

const char GSM7_BASIC[] =
  "@£$¥èéùìòÇ\nØø\rÅåΔ_ΦΓΛΩΠΨΣΘΞ"
  "\x1BÆæßÉ !\"#¤%&'()*+,-./"
  "0123456789:;<=>?"
  "¡ABCDEFGHIJKLMNOPQRSTUVWXYZÄÖÑÜ§¿"
  "abcdefghijklmnopqrstuvwxyzäöñüà";

String decodeGSM7(
  const String& data,
  int bitOffset,
  int septetCount
) {

  String out;

  int bitPos = bitOffset;

  for (int i = 0; i < septetCount; i++) {

    int bytePos = bitPos / 8;
    int shift = bitPos % 8;

    if (bytePos >= data.length() / 2) {
      break;
    }

    uint8_t b1 =
      hexByte(
        data,
        bytePos * 2
      );

    uint16_t value =
      b1 >> shift;

    if (
      shift > 1 &&
      bytePos + 1 < data.length() / 2
    ) {

      uint8_t b2 =
        hexByte(
          data,
          bytePos * 2 + 2
        );

      value |=
        ((uint16_t)b2 << (8 - shift));
    }

    uint8_t septet =
      value & 0x7F;

    if (
      septet <
      sizeof(GSM7_BASIC) - 1
    ) {

      out += GSM7_BASIC[septet];
    }

    bitPos += 7;
  }

  return out;
}

// ============================================================
// UCS2 decoder
// ============================================================

String decodeUCS2(const String& hex) {

  String out;

  for (
    int i = 0;
    i + 3 < hex.length();
    i += 4
  ) {

    uint16_t c =
      ((uint16_t)hexValue(hex[i]) << 12) |
      ((uint16_t)hexValue(hex[i + 1]) << 8) |
      ((uint16_t)hexValue(hex[i + 2]) << 4) |
      hexValue(hex[i + 3]);

    // UTF-16 surrogate pair -> Unicode scalar value.
    if (c >= 0xD800 && c <= 0xDBFF && i + 7 < hex.length()) {

      uint16_t low =
        ((uint16_t)hexValue(hex[i + 4]) << 12) |
        ((uint16_t)hexValue(hex[i + 5]) << 8) |
        ((uint16_t)hexValue(hex[i + 6]) << 4) |
        hexValue(hex[i + 7]);

      if (low >= 0xDC00 && low <= 0xDFFF) {

        uint32_t codePoint =
          0x10000UL +
          (((uint32_t)c - 0xD800UL) << 10) +
          ((uint32_t)low - 0xDC00UL);

        appendUTF8(out, codePoint);
        i += 4;
        continue;
      }
    }

    appendUTF8(out, c);
  }

  return out;
}

// ============================================================
// Decode SMS PDU
// ============================================================

bool decodeSMSPDU(
  const String& pdu,
  String& sender,
  String& text,
  uint8_t& part,
  uint8_t& total,
  uint16_t& reference
) {

  sender = "";
  text = "";

  part = 1;
  total = 1;
  reference = 0;

  if (pdu.length() < 20) {
    return false;
  }

  int pos = 0;

  // SMSC length
  int smscLen =
    hexByte(
      pdu,
      pos
    );

  pos += 2;

  pos += smscLen * 2;

  if (pos + 2 > pdu.length()) {
    return false;
  }

  // First octet
  uint8_t firstOctet =
    hexByte(
      pdu,
      pos
    );

  pos += 2;

  // SMS-DELIVER
  if ((firstOctet & 0x03) != 0x00) {
    return false;
  }

  // Originating address length
  uint8_t senderDigits =
    hexByte(
      pdu,
      pos
    );

  pos += 2;

  // Type of address
  uint8_t toa =
    hexByte(
      pdu,
      pos
    );

  pos += 2;

  int senderBytes =
    (senderDigits + 1) / 2;

  String numberHex =
    pdu.substring(
      pos,
      pos + senderBytes * 2
    );

  pos += senderBytes * 2;

  for (
    int i = 0;
    i < senderDigits;
    i++
  ) {

    int nibblePos =
      (i % 2 == 0)
        ? i
        : i + 1;

    if (
      nibblePos >=
      numberHex.length()
    ) {
      break;
    }

    char c =
      numberHex[nibblePos];

    if (c == 'F') {
      break;
    }

    sender += c;
  }

  if (toa == 0x91) {
    sender = "+" + sender;
  }

  // PID
  if (pos + 2 > pdu.length()) {
    return false;
  }

  pos += 2;

  // DCS
  if (pos + 2 > pdu.length()) {
    return false;
  }

  uint8_t dcs =
    hexByte(
      pdu,
      pos
    );

  pos += 2;

  // Timestamp
  if (pos + 14 > pdu.length()) {
    return false;
  }

  pos += 14;

  // User data length
  if (pos + 2 > pdu.length()) {
    return false;
  }

  uint8_t udl =
    hexByte(
      pdu,
      pos
    );

  pos += 2;

  bool hasUDH =
    (firstOctet & 0x40);

  int udhBytes = 0;

  if (hasUDH) {

    if (pos + 2 > pdu.length()) {
      return false;
    }

    udhBytes =
      hexByte(
        pdu,
        pos
      );

    int udhStart = pos;

    pos += 2;

    if (udhBytes >= 5) {

      uint8_t iei =
        hexByte(
          pdu,
          pos
        );

      uint8_t iedl =
        hexByte(
          pdu,
          pos + 2
        );

      // 8-bit concatenation reference
      if (
        iei == 0x00 &&
        iedl >= 3
      ) {

        reference =
          hexByte(
            pdu,
            pos + 4
          );

        total =
          hexByte(
            pdu,
            pos + 6
          );

        part =
          hexByte(
            pdu,
            pos + 8
          );
      }

      // 16-bit concatenation reference
      else if (
        iei == 0x08 &&
        iedl >= 4
      ) {

        reference =
          ((uint16_t)
            hexByte(
              pdu,
              pos + 4
            ) << 8) |
          hexByte(
            pdu,
            pos + 6
          );

        total =
          hexByte(
            pdu,
            pos + 8
          );

        part =
          hexByte(
            pdu,
            pos + 10
          );
      }
    }

    pos =
      udhStart +
      2 +
      udhBytes * 2;
  }

  int dataHexLength =
    pdu.length() - pos;

  if (dataHexLength <= 0) {
    return true;
  }

  // UCS2
  if ((dcs & 0x0C) == 0x08) {

    String data =
      pdu.substring(pos);

    text =
      decodeUCS2(data);

    return true;
  }

  // 8-bit
  if ((dcs & 0x0C) == 0x04) {

    String data =
      pdu.substring(pos);

    for (
      int i = 0;
      i + 1 < data.length();
      i += 2
    ) {

      text +=
        char(
          hexByte(
            data,
            i
          )
        );
    }

    return true;
  }

  // GSM 7-bit
  int septets = udl;

  if (hasUDH) {

    int headerBits =
      (udhBytes + 1) * 8;

    septets =
      ((udl * 8) - headerBits) / 7;
  }

  text =
    decodeGSM7(
      pdu.substring(pos),
      0,
      septets
    );

  return true;
}

// ============================================================
// Clear multipart
// ============================================================

void clearMultipart() {

  multipart.active = false;
  multipart.sender = "";
  multipart.reference = 0;
  multipart.total = 0;
  multipart.received = 0;
  multipart.lastPartTime = 0;

  for (int i = 0; i < 16; i++) {
    multipart.parts[i] = "";
  }
}

// ============================================================
// Add multipart part
// ============================================================

bool addMultipartPart(
  const String& sender,
  uint16_t reference,
  uint8_t total,
  uint8_t part,
  const String& text
) {

  if (
    total == 0 ||
    total > 16
  ) {
    return false;
  }

  if (
    part == 0 ||
    part > total
  ) {
    return false;
  }

  if (
    !multipart.active ||
    multipart.sender != sender ||
    multipart.reference != reference ||
    multipart.total != total
  ) {

    clearMultipart();

    multipart.active = true;
    multipart.sender = sender;
    multipart.reference = reference;
    multipart.total = total;
  }

  int index =
    part - 1;

  if (
    multipart.parts[index].length() == 0
  ) {

    multipart.received++;
  }

  multipart.parts[index] =
    text;

  multipart.lastPartTime =
    millis();

  Serial.printf(
    "[SMS] Multipart: %d/%d\n",
    multipart.received,
    multipart.total
  );

  return
    multipart.received ==
    multipart.total;
}

// ============================================================
// Build multipart message
// ============================================================

String buildMultipartMessage() {

  String result;

  for (
    int i = 0;
    i < multipart.total;
    i++
  ) {

    result +=
      multipart.parts[i];
  }

  return result;
}

// ============================================================
// Wait for modem response
// ============================================================

bool waitModem(
  const char* expected,
  unsigned long timeout = 3000
) {

  String response;

  unsigned long start =
    millis();

  while (
    millis() - start <
    timeout
  ) {

    while (SerialAT.available()) {

      char c =
        SerialAT.read();

      response += c;

      if (
        response.indexOf(
          expected
        ) >= 0
      ) {

        return true;
      }

      if (
        response.indexOf(
          "ERROR"
        ) >= 0
      ) {

        return false;
      }
    }

    delay(5);
  }

  return false;
}

// ============================================================
// Send AT command
// ============================================================

bool sendAT(
  const String& command,
  const char* expected = "OK",
  unsigned long timeout = 3000
) {

  while (SerialAT.available()) {
    SerialAT.read();
  }

  SerialAT.println(command);

  return waitModem(
    expected,
    timeout
  );
}

// ============================================================
// Modem initialization
// ============================================================

bool initModem() {

  Serial.println(
    "[MODEM] Initializing..."
  );

  sendAT("ATE0");

  sendAT("AT+CMEE=2");

  if (
    !sendAT(
      "AT+CPIN?",
      "READY",
      5000
    )
  ) {

    Serial.println(
      "[MODEM] SIM not ready."
    );
  }

  sendAT("AT+CMGF=0");

  sendAT(
    "AT+CPMS=\"SM\",\"SM\",\"SM\""
  );

  sendAT(
    "AT+CNMI=2,1,0,0,0"
  );

  sendAT("AT+CLIP=1");

  sendAT(
    "AT+CSCS=\"UCS2\""
  );

  Serial.println(
    "[MODEM] Initialization complete."
  );

  return true;
}

// ============================================================
// Delete SMS
// ============================================================

void deleteSMS(int index) {

  sendAT(
    "AT+CMGD=" +
    String(index),
    "OK",
    3000
  );
}

// ============================================================
// Proxy CONNECT tunnel
// ============================================================

bool connectProxyTunnel(
  Client& proxy,
  const char* proxyHost,
  uint16_t proxyPort
) {

  Serial.printf(
    "[Proxy] Connecting to %s:%u\n",
    proxyHost,
    proxyPort
  );

  if (!proxy.connect(proxyHost, proxyPort)) {
    Serial.println("[Proxy] TCP connection FAILED.");
    return false;
  }

  String credentials =
    String(PROXY_USER) + ":" + String(PROXY_PASS);

  String auth = base64Encode(credentials);

  proxy.print(
    "CONNECT " + String(TELEGRAM_HOST) + ":" +
    String(TELEGRAM_PORT) + " HTTP/1.1\r\n"
  );
  proxy.print(
    "Host: " + String(TELEGRAM_HOST) + ":" +
    String(TELEGRAM_PORT) + "\r\n"
  );
  proxy.print("Proxy-Authorization: Basic " + auth + "\r\n");
  proxy.print("Proxy-Connection: Keep-Alive\r\n");
  proxy.print("\r\n");

  unsigned long start = millis();
  String response;

  while (millis() - start < 10000) {
    while (proxy.available()) {
      char c = proxy.read();
      response += c;

      if (response.endsWith("\r\n\r\n")) {
        Serial.println("[Proxy] CONNECT response:");
        Serial.println(response);

        if (response.indexOf(" 200 ") >= 0 ||
            response.indexOf(" 200\r") >= 0) {
          Serial.println("[Proxy] CONNECT tunnel established.");
          return true;
        }

        Serial.println("[Proxy] CONNECT FAILED.");
        return false;
      }
    }
    delay(2);
  }

  Serial.println("[Proxy] CONNECT timeout.");
  return false;
}

// ============================================================
// Mbed TLS <-> Arduino Client bridge
// ============================================================

static int tlsClientSend(void* ctx, const unsigned char* buf, size_t len) {
  Client* client = static_cast<Client*>(ctx);
  size_t written = client->write(buf, len);

  if (written > 0) return (int)written;
  return MBEDTLS_ERR_SSL_WANT_WRITE;
}

static int tlsClientRecv(void* ctx, unsigned char* buf, size_t len) {
  Client* client = static_cast<Client*>(ctx);

  if (!client->connected() && !client->available()) {
    return MBEDTLS_ERR_SSL_WANT_READ;
  }

  int n = client->read(buf, len);
  if (n > 0) return n;

  return MBEDTLS_ERR_SSL_WANT_READ;
}

// ============================================================
// GPRS fallback
// ============================================================

bool ensureGPRS() {

  if (gprsReady && modem.isGprsConnected()) {
    return true;
  }

  unsigned long now = millis();
  if (now - lastGprsAttempt < GPRS_RETRY_INTERVAL) {
    return false;
  }

  lastGprsAttempt = now;
  gprsReady = false;

  Serial.println("[GPRS] Starting GPRS fallback...");

  if (!modem.isNetworkConnected()) {
    Serial.println("[GPRS] Waiting for GSM network...");

    if (!modem.waitForNetwork(60000L)) {
      Serial.println("[GPRS] GSM network unavailable.");
      return false;
    }
  }

  if (modem.isGprsConnected()) {
    gprsReady = true;
    Serial.println("[GPRS] Already connected.");
    return true;
  }

  Serial.printf("[GPRS] Connecting APN: %s\n", GPRS_APN);

  if (!modem.gprsConnect(GPRS_APN, GPRS_USER, GPRS_PASS)) {
    Serial.println("[GPRS] Connection FAILED.");
    return false;
  }

  gprsReady = true;
  Serial.println("[GPRS] Connected.");
  return true;
}

void stopGPRSIfWiFiIsAvailable() {
  if (WiFi.status() == WL_CONNECTED && modem.isGprsConnected()) {
    Serial.println("[GPRS] WiFi available; disconnecting GPRS.");
    modem.gprsDisconnect();
    gprsReady = false;
  }
}

// ============================================================
// Incoming call / missed call parser
// ============================================================

bool sendTelegram(const char* chatId, const String& message);

void handleCallLine(const String& rawLine) {

  String line = rawLine;
  line.trim();

  if (line.length() == 0) return;

  if (line == "RING") {
    incomingCallActive = true;
    incomingCallNumber = "";
    Serial.println("[CALL] Incoming call.");
    return;
  }

  if (line.startsWith("+CLIP:")) {
    int q1 = line.indexOf('"');
    int q2 = q1 >= 0 ? line.indexOf('"', q1 + 1) : -1;

    if (q1 >= 0 && q2 > q1) {
      incomingCallNumber = line.substring(q1 + 1, q2);
      Serial.println("[CALL] Number: " + incomingCallNumber);
    }
    return;
  }

  if (line == "NO CARRIER" && incomingCallActive) {
    String number = incomingCallNumber;
    if (number.length() == 0) number = "Unknown";

    Serial.println("[CALL] Missed call: " + number);

    String message =
      "📞 Missed call\n\n" +
      String("Number: ") + number;

    bool first = sendTelegram(CHAT_ID_1, message);
    delay(500);
    bool second = sendTelegram(CHAT_ID_2, message);
    bool sent = first && second;

    if (sent) {
      Serial.println("[CALL] Telegram SUCCESS.");
    } else {
      Serial.println("[CALL] Telegram FAILED.");
    }

    incomingCallActive = false;
    incomingCallNumber = "";
  }
}

void parseCallEvents(const String& response) {

  int start = 0;

  while (start < response.length()) {
    int end = response.indexOf('\n', start);
    if (end < 0) end = response.length();

    String line = response.substring(start, end);
    handleCallLine(line);

    start = end + 1;
  }
}

// ============================================================
// TLS error helper
// ============================================================

void printTLSError(
  const char* where,
  int error
) {

  char errorBuffer[128];

  errorBuffer[0] = '\0';

  mbedtls_strerror(
    error,
    errorBuffer,
    sizeof(errorBuffer)
  );

  Serial.printf(
    "[TLS] %s failed: -0x%04X (%s)\n",
    where,
    -error,
    errorBuffer
  );
}

// ============================================================
// TLS write
// ============================================================

bool tlsWriteAll(
  mbedtls_ssl_context* ssl,
  const uint8_t* data,
  size_t length
) {

  size_t sent = 0;

  unsigned long lastProgress =
    millis();

  while (sent < length) {

    int ret =
      mbedtls_ssl_write(
        ssl,
        data + sent,
        length - sent
      );

    if (ret > 0) {

      sent += ret;

      lastProgress =
        millis();

      continue;
    }

    if (
      ret ==
        MBEDTLS_ERR_SSL_WANT_READ ||
      ret ==
        MBEDTLS_ERR_SSL_WANT_WRITE
    ) {

      if (
        millis() - lastProgress >
        15000
      ) {

        Serial.println(
          "[TLS] Write timeout."
        );

        return false;
      }

      delay(2);

      continue;
    }

    printTLSError(
      "SSL write",
      ret
    );

    return false;
  }

  return true;
}

// ============================================================
// TLS read
// ============================================================

int tlsReadSome(
  mbedtls_ssl_context* ssl,
  uint8_t* buffer,
  size_t length
) {

  unsigned long start =
    millis();

  while (
    millis() - start < 15000
  ) {

    int ret =
      mbedtls_ssl_read(
        ssl,
        buffer,
        length
      );

    if (ret > 0) {
      return ret;
    }

    if (
      ret ==
        MBEDTLS_ERR_SSL_WANT_READ ||
      ret ==
        MBEDTLS_ERR_SSL_WANT_WRITE
    ) {

      delay(2);

      continue;
    }

    return ret;
  }

  return MBEDTLS_ERR_SSL_TIMEOUT;
}

// ============================================================
// Telegram HTTPS through CONNECT + Mbed TLS
// ============================================================

bool sendTelegram(
  const char* chatId,
  const String& message
) {

  Serial.println();
  Serial.printf("[Telegram] Sending to Chat %s...\n", chatId);

  Client* client = nullptr;
  bool usingWiFi = (WiFi.status() == WL_CONNECTED);
  bool usingGprs = false;

  WiFiClient wifiClient;

  if (usingWiFi) {
    Serial.println("[Route] WiFi primary -> internal proxy.");
    client = &wifiClient;
  } else {
    Serial.println("[Route] Primary WiFi unavailable -> GPRS -> external proxy.");

    if (!ensureGPRS()) {
      Serial.println("[Telegram] GPRS unavailable.");
      return false;
    }

    gsmClient.stop();
    client = &gsmClient;
    usingGprs = true;
  }

  client->setTimeout(15000);

  bool tunnel = false;

  if (usingWiFi) {
    tunnel = connectProxyTunnel(
      *client,
      PROXY_HOST,
      PROXY_PORT
    );
  } else {
    tunnel = connectProxyTunnel(
      *client,
      EXTERNAL_PROXY_HOST,
      EXTERNAL_PROXY_PORT
    );

    if (!tunnel) {
      Serial.println("[Proxy] External hostname failed; trying public IP...");
      client->stop();
      tunnel = connectProxyTunnel(
        *client,
        EXTERNAL_PROXY_IP,
        EXTERNAL_PROXY_PORT
      );
    }
  }

  if (!tunnel) {
    Serial.println("[Telegram] Proxy tunnel failed.");
    client->stop();
    return false;
  }

  // ----------------------------------------------------------
  // Mbed TLS contexts
  // ----------------------------------------------------------

  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  mbedtls_ctr_drbg_context ctrDrbg;
  mbedtls_entropy_context entropy;

  mbedtls_ssl_init(&ssl);
  mbedtls_ssl_config_init(&conf);
  mbedtls_ctr_drbg_init(&ctrDrbg);
  mbedtls_entropy_init(&entropy);

  int ret = mbedtls_ctr_drbg_seed(
    &ctrDrbg,
    mbedtls_entropy_func,
    &entropy,
    nullptr,
    0
  );

  if (ret != 0) {
    printTLSError("CTR-DRBG seed", ret);
    client->stop();
    mbedtls_entropy_free(&entropy);
    mbedtls_ctr_drbg_free(&ctrDrbg);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ssl_free(&ssl);
    return false;
  }

  ret = mbedtls_ssl_config_defaults(
    &conf,
    MBEDTLS_SSL_IS_CLIENT,
    MBEDTLS_SSL_TRANSPORT_STREAM,
    MBEDTLS_SSL_PRESET_DEFAULT
  );

  if (ret != 0) {
    printTLSError("SSL config defaults", ret);
    client->stop();
    mbedtls_entropy_free(&entropy);
    mbedtls_ctr_drbg_free(&ctrDrbg);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ssl_free(&ssl);
    return false;
  }

  mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
  mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctrDrbg);

  ret = mbedtls_ssl_set_hostname(&ssl, TELEGRAM_HOST);
  if (ret != 0) {
    printTLSError("Set TLS hostname", ret);
    client->stop();
    mbedtls_entropy_free(&entropy);
    mbedtls_ctr_drbg_free(&ctrDrbg);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ssl_free(&ssl);
    return false;
  }

  ret = mbedtls_ssl_setup(&ssl, &conf);
  if (ret != 0) {
    printTLSError("SSL setup", ret);
    client->stop();
    mbedtls_entropy_free(&entropy);
    mbedtls_ctr_drbg_free(&ctrDrbg);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ssl_free(&ssl);
    return false;
  }

  mbedtls_ssl_set_bio(
    &ssl,
    client,
    tlsClientSend,
    tlsClientRecv,
    nullptr
  );

  Serial.println("[TLS] Starting TLS handshake...");

  unsigned long handshakeStart = millis();

  while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {

    if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
        ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
      printTLSError("TLS handshake", ret);
      client->stop();
      mbedtls_entropy_free(&entropy);
      mbedtls_ctr_drbg_free(&ctrDrbg);
      mbedtls_ssl_config_free(&conf);
      mbedtls_ssl_free(&ssl);
      return false;
    }

    if (millis() - handshakeStart > 30000) {
      Serial.println("[TLS] Handshake timeout.");
      client->stop();
      mbedtls_entropy_free(&entropy);
      mbedtls_ctr_drbg_free(&ctrDrbg);
      mbedtls_ssl_config_free(&conf);
      mbedtls_ssl_free(&ssl);
      return false;
    }

    delay(2);
  }

  Serial.println("[Telegram] TLS handshake SUCCESS.");

  // ----------------------------------------------------------
  // Build Telegram JSON
  // ----------------------------------------------------------

  String body;
  JsonDocument doc;
  doc["chat_id"] = chatId;
  doc["text"] = message;
  serializeJson(doc, body);

  String path =
    "/bot" + String(BOT_TOKEN) + "/sendMessage";

  String request;
  request.reserve(256 + body.length());

  request += "POST " + path + " HTTP/1.1\r\n";
  request += "Host: " + String(TELEGRAM_HOST) + "\r\n";
  request += "Content-Type: application/json\r\n";
  request += "Content-Length: " + String(body.length()) + "\r\n";
  request += "Connection: close\r\n";
  request += "\r\n";
  request += body;

  Serial.printf(
    "[Telegram] HTTPS request size: %u bytes\n",
    (unsigned)request.length()
  );

  if (!tlsWriteAll(
    &ssl,
    (const uint8_t*)request.c_str(),
    request.length()
  )) {
    Serial.println("[Telegram] HTTPS request FAILED.");
    mbedtls_ssl_close_notify(&ssl);
    client->stop();
    mbedtls_entropy_free(&entropy);
    mbedtls_ctr_drbg_free(&ctrDrbg);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ssl_free(&ssl);
    return false;
  }

  Serial.println("[Telegram] HTTPS request sent.");

  // ----------------------------------------------------------
  // Read Telegram response
  // ----------------------------------------------------------

  String response;
  uint8_t buffer[512];
  unsigned long responseStart = millis();

  while (millis() - responseStart < 15000) {

    ret = tlsReadSome(&ssl, buffer, sizeof(buffer));

    if (ret > 0) {
      for (int i = 0; i < ret; i++) {
        response += char(buffer[i]);
      }
      responseStart = millis();
      continue;
    }

    if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ||
        ret == MBEDTLS_ERR_SSL_TIMEOUT) {
      break;
    }

    if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
        ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
      continue;
    }

    if (ret < 0) {
      printTLSError("SSL read", ret);
    }
    break;
  }

  Serial.println("[Telegram] Response:");
  Serial.println(response);

  bool success =
    response.indexOf("HTTP/1.1 200") >= 0 &&
    response.indexOf("\"ok\":true") >= 0;

  if (success) {
    Serial.println("[Telegram] SUCCESS.");
  } else {
    Serial.println("[Telegram] FAILED.");
  }

  mbedtls_ssl_close_notify(&ssl);
  client->stop();

  mbedtls_entropy_free(&entropy);
  mbedtls_ctr_drbg_free(&ctrDrbg);
  mbedtls_ssl_config_free(&conf);
  mbedtls_ssl_free(&ssl);

  return success;
}

// ============================================================
// Send to both Telegram chats
// ============================================================

bool sendTelegramToBoth(
  const String& sender,
  const String& sms
) {

  String message =
    "📩 New SMS\n\n"
    "From: " +
    sender +
    "\n\n" +
    sms;

  bool first =
    sendTelegram(
      CHAT_ID_1,
      message
    );

  delay(500);

  bool second =
    sendTelegram(
      CHAT_ID_2,
      message
    );

  return first && second;
}

// ============================================================
// Read SMS list
// ============================================================

void checkSMS() {

  while (SerialAT.available()) {
    SerialAT.read();
  }

  SerialAT.println(
    "AT+CMGL=4"
  );

  String response;

  unsigned long start =
    millis();

  while (
    millis() - start < 5000
  ) {

    while (SerialAT.available()) {

      response +=
        char(
          SerialAT.read()
        );
    }

    delay(5);
  }

  parseCallEvents(response);

  int searchPos = 0;

  while (true) {

    int header =
      response.indexOf(
        "+CMGL:",
        searchPos
      );

    if (header < 0) {
      break;
    }

    int lineEnd =
      response.indexOf(
        '\n',
        header
      );

    if (lineEnd < 0) {
      break;
    }

    String headerLine =
      response.substring(
        header,
        lineEnd
      );

    headerLine.trim();

    int colon =
      headerLine.indexOf(':');

    int comma1 =
      headerLine.indexOf(
        ',',
        colon
      );

    int comma2 =
      headerLine.indexOf(
        ',',
        comma1 + 1
      );

    if (
      colon < 0 ||
      comma1 < 0 ||
      comma2 < 0
    ) {

      searchPos =
        lineEnd + 1;

      continue;
    }

    int smsIndex =
      headerLine.substring(
        colon + 1,
        comma1
      ).toInt();

    int pduStart =
      lineEnd + 1;

    while (
      pduStart <
      response.length() &&
      (
        response[pduStart] == '\r' ||
        response[pduStart] == '\n'
      )
    ) {

      pduStart++;
    }

    int pduEnd =
      response.indexOf(
        '\n',
        pduStart
      );

    if (pduEnd < 0) {
      break;
    }

    String pdu =
      response.substring(
        pduStart,
        pduEnd
      );

    pdu.trim();

    String sender;
    String text;

    uint8_t part = 1;
    uint8_t total = 1;

    uint16_t reference = 0;

    if (
      decodeSMSPDU(
        pdu,
        sender,
        text,
        part,
        total,
        reference
      )
    ) {

      Serial.println();
      Serial.println(
        "========== SMS =========="
      );

      Serial.println(
        "From: " +
        sender
      );

      Serial.println();

      Serial.println(
        "Text: " +
        text
      );

      Serial.println(
        "=========================="
      );

      Serial.println();

      if (total > 1) {

        bool complete =
          addMultipartPart(
            sender,
            reference,
            total,
            part,
            text
          );

        if (complete) {

          String fullMessage =
            buildMultipartMessage();

          Serial.println();
          Serial.println(
            "[SMS] Multipart complete."
          );

          Serial.println(
            "[SMS] Full message:"
          );

          Serial.println(
            fullMessage
          );

          bool sent =
            sendTelegramToBoth(
              sender,
              fullMessage
            );

          if (sent) {

            Serial.println(
              "[SMS] Multipart Telegram SUCCESS."
            );

            deleteSMS(
              smsIndex
            );

            clearMultipart();
          }

          else {

            Serial.println(
              "[SMS] Multipart Telegram FAILED."
            );
          }
        }
      }

      else {

        bool sent =
          sendTelegramToBoth(
            sender,
            text
          );

        if (sent) {

          Serial.println(
            "[SMS] Telegram SUCCESS."
          );

          deleteSMS(
            smsIndex
          );
        }

        else {

          Serial.println(
            "[SMS] Telegram FAILED."
          );
        }
      }
    }

    searchPos =
      pduEnd + 1;
  }
}

// ============================================================
// WiFi
// ============================================================

void connectWiFi() {

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    return;
  }

  Serial.println();

  Serial.println(
    "[WiFi] Connecting..."
  );

  WiFi.mode(
    WIFI_STA
  );

  WiFi.setSleep(
    false
  );

  WiFi.begin(
    WIFI_SSID,
    WIFI_PASS
  );

  unsigned long start =
    millis();

  while (
    WiFi.status() !=
      WL_CONNECTED &&
    millis() - start <
      20000
  ) {

    delay(500);

    Serial.print(".");
  }

  Serial.println();

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    Serial.println(
      "[WiFi] Connected."
    );

    Serial.print(
      "[WiFi] IP: "
    );

    Serial.println(
      WiFi.localIP()
    );
  }

  else {

    Serial.println(
      "[WiFi] Connection FAILED."
    );
  }
}

// ============================================================
// Setup
// ============================================================

void setup() {

  Serial.begin(
    115200
  );

  delay(1000);

  Serial.println();

  Serial.println(
    "===================================="
  );

  Serial.println(
    " TTGO T-Call SMS -> Telegram"
  );

  Serial.println(
    " HTTP CCProxy + HTTPS CONNECT"
  );

  Serial.println(
    " Mbed TLS over CONNECT socket"
  );

  Serial.println(
    "===================================="
  );

  // Modem power
  pinMode(
    MODEM_POWER,
    OUTPUT
  );

  digitalWrite(
    MODEM_POWER,
    HIGH
  );

  pinMode(
    MODEM_PWRKEY,
    OUTPUT
  );

  digitalWrite(
    MODEM_PWRKEY,
    HIGH
  );

  pinMode(
    MODEM_RESET,
    OUTPUT
  );

  digitalWrite(
    MODEM_RESET,
    HIGH
  );

  SerialAT.begin(
    MODEM_BAUD,
    SERIAL_8N1,
    MODEM_RX,
    MODEM_TX
  );

  delay(3000);

  // Power key
  digitalWrite(
    MODEM_PWRKEY,
    LOW
  );

  delay(1000);

  digitalWrite(
    MODEM_PWRKEY,
    HIGH
  );

  delay(5000);

  initModem();

  connectWiFi();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[SYSTEM] Primary WiFi unavailable; GPRS fallback will be used.");
  }

  clearMultipart();

  Serial.println();

  Serial.println(
    "[SYSTEM] Ready."
  );
}

// ============================================================
// Loop
// ============================================================

void loop() {

  unsigned long now =
    millis();

  // WiFi reconnect
  if (
    now - lastWiFiCheck >=
    WIFI_INTERVAL
  ) {

    lastWiFiCheck =
      now;

    if (
      WiFi.status() !=
      WL_CONNECTED
    ) {

      Serial.println(
        "[WiFi] Disconnected."
      );

      connectWiFi();

      if (WiFi.status() == WL_CONNECTED) {
        stopGPRSIfWiFiIsAvailable();
      }
    }

    else {
      stopGPRSIfWiFiIsAvailable();
    }
  }

  // Passive modem URCs (e.g. RING / +CLIP / NO CARRIER).
  // Any pending call event is consumed here between SMS polls.
  if (SerialAT.available()) {
    String urc;
    unsigned long urcStart = millis();

    while (SerialAT.available() && millis() - urcStart < 50) {
      char c = SerialAT.read();
      urc += c;
      if (c == '\n') {
        parseCallEvents(urc);
        urc = "";
      }
    }

    if (urc.length()) {
      parseCallEvents(urc);
    }
  }

  // SMS
  if (
    now - lastSMSCheck >=
    SMS_INTERVAL
  ) {

    lastSMSCheck =
      now;

    // SMS is stored on the SIM, so it can be processed over either route.
    checkSMS();
  }

  // Multipart timeout
  if (
    multipart.active &&
    now - multipart.lastPartTime >
      MULTIPART_TIMEOUT
  ) {

    Serial.println(
      "[SMS] Multipart timeout."
    );

    clearMultipart();
  }

  delay(10);
}
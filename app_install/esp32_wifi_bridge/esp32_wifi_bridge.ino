/*
 * Flipped App Store - ESP32 WiFi Bridge Firmware
 *
 * Flash this to an ESP32/ESP32-S2 WiFi dev board connected to the
 * Flipper Zero via UART (GPIO pins).
 *
 * Wiring:
 *   ESP32 TX -> Flipper RX (pin 14 / PA7)
 *   ESP32 RX -> Flipper TX (pin 13 / PA6)
 *   ESP32 GND -> Flipper GND
 *   ESP32 3V3 -> Flipper 3V3 (or power via USB)
 *
 * UART Protocol:
 *   AT\r\n                -> OK\r\n
 *   WIFI_STATUS\r\n       -> CONNECTED\r\n | DISCONNECTED\r\n
 *   WIFI_SSID <ssid>\r\n  -> OK\r\n | ERROR:<msg>\r\n
 *   WIFI_PASS <pass>\r\n  -> OK\r\n
 *   WIFI_CONNECT\r\n      -> CONNECTED\r\n | ERROR:<msg>\r\n
 *   DOWNLOAD <url>\r\n    -> SIZE:<bytes>\r\n<raw data>\r\nDONE\r\n
 *                         -> ERROR:<msg>\r\n
 */

#include <WiFi.h>
#include <HTTPClient.h>

// UART to Flipper Zero
#define FLIPPER_SERIAL Serial1
#define FLIPPER_BAUD 115200
#define FLIPPER_RX_PIN 16  // Adjust for your board
#define FLIPPER_TX_PIN 17  // Adjust for your board

// WiFi credentials (can be set via UART commands or hardcoded)
static String wifi_ssid = "";
static String wifi_pass = "";

// Max download size (1MB)
#define MAX_DOWNLOAD_SIZE (1024 * 1024)

void setup() {
    Serial.begin(115200);
    Serial.println("Flipped WiFi Bridge starting...");

    FLIPPER_SERIAL.begin(FLIPPER_BAUD, SERIAL_8N1, FLIPPER_RX_PIN, FLIPPER_TX_PIN);

    // Try to load saved WiFi credentials from preferences
    // or connect to previously saved network
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);

    // If credentials were saved previously, WiFi.begin() will use them
    if (wifi_ssid.length() > 0) {
        WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
        Serial.printf("Connecting to %s...\n", wifi_ssid.c_str());

        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
            delay(250);
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        } else {
            Serial.println("WiFi connection failed");
        }
    }

    Serial.println("Ready for commands");
}

void send_response(const char* response) {
    FLIPPER_SERIAL.print(response);
    FLIPPER_SERIAL.print("\r\n");
    FLIPPER_SERIAL.flush();
}

void handle_download(const char* url) {
    if (WiFi.status() != WL_CONNECTED) {
        send_response("ERROR:Not connected to WiFi");
        return;
    }

    Serial.printf("Downloading: %s\n", url);

    HTTPClient http;
    http.begin(url);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(15000);

    int httpCode = http.GET();

    if (httpCode != HTTP_CODE_OK) {
        char err[64];
        snprintf(err, sizeof(err), "ERROR:HTTP %d", httpCode);
        send_response(err);
        http.end();
        return;
    }

    int contentLength = http.getSize();
    if (contentLength <= 0) {
        send_response("ERROR:Unknown content length");
        http.end();
        return;
    }

    if (contentLength > MAX_DOWNLOAD_SIZE) {
        send_response("ERROR:File too large");
        http.end();
        return;
    }

    // Send size header
    char header[32];
    snprintf(header, sizeof(header), "SIZE:%d", contentLength);
    send_response(header);

    // Small delay to let Flipper process the header
    delay(50);

    // Stream file data to Flipper
    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[512];
    int remaining = contentLength;

    while (remaining > 0 && stream->connected()) {
        int toRead = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
        int bytesRead = stream->readBytes(buf, toRead);

        if (bytesRead <= 0) {
            break;
        }

        FLIPPER_SERIAL.write(buf, bytesRead);
        FLIPPER_SERIAL.flush();
        remaining -= bytesRead;

        // Small yield to prevent watchdog
        yield();
    }

    http.end();

    // Send completion marker
    delay(10);
    send_response("\r\nDONE");

    Serial.printf("Download complete, sent %d bytes\n", contentLength - remaining);
}

void handle_command(String& cmd) {
    cmd.trim();
    if (cmd.length() == 0) return;

    Serial.printf("CMD: %s\n", cmd.c_str());

    if (cmd == "AT") {
        send_response("OK");
    } else if (cmd == "WIFI_STATUS") {
        if (WiFi.status() == WL_CONNECTED) {
            send_response("CONNECTED");
        } else {
            send_response("DISCONNECTED");
        }
    } else if (cmd.startsWith("WIFI_SSID ")) {
        wifi_ssid = cmd.substring(10);
        send_response("OK");
    } else if (cmd.startsWith("WIFI_PASS ")) {
        wifi_pass = cmd.substring(10);
        send_response("OK");
    } else if (cmd == "WIFI_CONNECT") {
        if (wifi_ssid.length() == 0) {
            send_response("ERROR:No SSID set");
            return;
        }

        WiFi.disconnect();
        delay(100);
        WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());

        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
            delay(250);
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());
            send_response("CONNECTED");
        } else {
            send_response("ERROR:Connection failed");
        }
    } else if (cmd.startsWith("DOWNLOAD ")) {
        String url = cmd.substring(9);
        url.trim();
        handle_download(url.c_str());
    } else {
        send_response("ERROR:Unknown command");
    }
}

void loop() {
    static String inputBuffer = "";

    while (FLIPPER_SERIAL.available()) {
        char c = FLIPPER_SERIAL.read();

        if (c == '\n') {
            handle_command(inputBuffer);
            inputBuffer = "";
        } else if (c != '\r') {
            if (inputBuffer.length() < 1024) {
                inputBuffer += c;
            }
        }
    }

    delay(1);
}

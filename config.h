#pragma once

// Wi-Fi
#define WIFI_SSID   "Wokwi-GUEST"
#define WIFI_PASS   ""

// Relay
#define RELAY_HOST  "newcastle-for-dan-annotation.trycloudflare.com"          // ganti pake server ni nanti
#define RELAY_PORT  443                    // 443 when USE_TLS = 1
#define RELAY_PATH  "/?device=esp32-01&token=tokenrelayrelay"
#define USE_TLS     1                       // 1 = wss:// (requires certs.h)

// Display pins (hardware SPI uses the board's default SPI pins; see section 2.2)
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  #define TFT_CS    10
  #define TFT_DC     9
  #define TFT_RST    8
#else                                       // classic ESP32
  #define TFT_CS     5
  #define TFT_DC    21
  #define TFT_RST   22
#endif
#define TFT_ROTATION 0                      // 0 = portrait, 1 = landscape

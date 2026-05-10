/*
  GardenKnobFirmware.ino
  Rotary Garden Watering Controller
  Target: ELECROW / CrowPanel 1.28 inch HMI ESP32 Rotary Display 240x240 IPS Round Touch Knob

  Board notes:
    MCU: ESP32-S3
    Display: GC9A01 SPI 240x240
    Touch: CST816D I2C
    Encoder: GPIO 45 / 42 / 41
    AP default: GardenKnob / gardenknob / 192.168.6.1
    Relay default: http://192.168.4.1

  Required Arduino libraries:
    - ESP32 board package
    - ArduinoJson
    - GFX Library for Arduino by Moon On Our Nation
    - Adafruit NeoPixel

  This firmware is intentionally a controller/control-panel. It does not drive relay GPIO.
*/

#include "GardenKnobTypes.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <Adafruit_NeoPixel.h>

// ----------------------------- Firmware -------------------------------------

#define FW_VERSION "0.1.0-uiicons"

// ----------------------------- Hardware Pins --------------------------------
// Elecrow CrowPanel 1.28" HMI ESP32 Rotary Display pin map.
#define TFT_SCLK 10
#define TFT_MOSI 11
#define TFT_MISO -1
#define TFT_DC    3
#define TFT_CS    9
#define TFT_RST  14
#define TFT_BLK  46

#define LCD_PWR_EN1 1
#define LCD_PWR_EN2 2

#define TOUCH_SDA   6
#define TOUCH_SCL   7
#define TOUCH_INT   5
#define TOUCH_RST  13
#define TOUCH_ADDR 0x15

#define ENCODER_A_PIN 45
#define ENCODER_B_PIN 42
#define ENCODER_BTN   41

#define LED_PIN 48
#define LED_NUM 5
#define POWER_LIGHT_PIN 40

// ----------------------------- Display Constants ----------------------------

static const int SCREEN_W = 240;
static const int SCREEN_H = 240;
static const int CX = 120;
static const int CY = 120;

// Round-screen safe area.
// Avoid controls/text in clipped lower corners or near the circular edge.
static const int SAFE_TOP = 18;
static const int SAFE_BOTTOM = 206;
static const int SAFE_LEFT = 22;
static const int SAFE_RIGHT = 218;
static const int SAFE_BOTTOM_BUTTON_Y = 176;
static const int SAFE_BOTTOM_HINT_Y = 198;
static const int SAFE_R = 105;
static const int INNER_R = 92;

// 16-bit RGB565 palette. High contrast, readable outdoors.
static const uint16_t C_BG       = 0x0000; // black
static const uint16_t C_TEXT     = 0xFFFF; // white
static const uint16_t C_DIM      = 0x7BEF; // gray
static const uint16_t C_PANEL    = 0x2104; // dark gray
static const uint16_t C_OUTLINE  = 0xBDF7;
static const uint16_t C_BLUE     = 0x04FF;
static const uint16_t C_GREEN    = 0x07E0;
static const uint16_t C_RED      = 0xF800;
static const uint16_t C_AMBER    = 0xFD20;
static const uint16_t C_PURPLE   = 0x801F;
static const uint16_t C_SOIL     = 0x7A20;
static const uint16_t C_WATER    = 0x057F;

static const uint16_t C_ZONE1    = 0x03BF; // blue
static const uint16_t C_ZONE2    = 0x05E0; // green
static const uint16_t C_ZONE3    = 0xFE80; // amber
static const uint16_t C_ZONE4    = 0xA81F; // purple
static const uint16_t C_ZONE5    = 0xF9C6; // red/orange
static const uint16_t C_CYAN     = 0x07FF;
static const uint16_t C_BUTTON   = 0x3186;
static const uint16_t C_BUTTON2  = 0x528A;
static const uint16_t C_SHADOW   = 0x0841;
static const uint16_t C_RING_DARK = 0x1082;
static const uint16_t C_RING_LITE = 0x39E7;
static const uint16_t C_GLOW_DIM  = 0x18E3;
static const uint16_t C_DEEP_BLUE = 0x0110;
static const uint16_t C_GRID      = 0x10A2;
static const uint16_t C_MINT      = 0x87F0;
static const uint16_t C_CARD      = 0x2104;
static const uint16_t C_CARD2     = 0x3186;
static const uint16_t C_SOFT_TEXT = 0xC618;

// ----------------------------- Home Icon Sprites ----------------------------

static const int HOME_ICON_W = 24;
static const int HOME_ICON_H = 24;
static const uint16_t SPRITE_TRANSPARENT = 0x0000;

static const uint16_t ICON_RUN_24[576] PROGMEM = {
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x65FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x65FF,
  0x65FF, 0x65FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x65FF,
  0x65FF, 0x65FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x65FF, 0x65FF,
  0x65FF, 0x65FF, 0x65FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x65FF, 0x65FF,
  0x65FF, 0x65FF, 0x65FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x65FF, 0x65FF, 0x467F,
  0x467F, 0x467F, 0x65FF, 0x65FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x65FF, 0x467F, 0x467F, 0xFFFF,
  0x467F, 0x467F, 0x467F, 0x467F, 0x65FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x65FF, 0x467F, 0xFFFF, 0xFFFF,
  0xFFFF, 0x467F, 0x467F, 0x467F, 0x65FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x65FF, 0x467F, 0x467F, 0x467F, 0xFFFF,
  0x467F, 0x467F, 0x467F, 0x467F, 0x467F, 0x65FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x65FF, 0x467F, 0x467F, 0x467F, 0x467F,
  0x467F, 0x467F, 0x467F, 0x467F, 0x467F, 0x65FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x467F, 0x467F, 0x467F, 0x467F,
  0x467F, 0x467F, 0x467F, 0x467F, 0x467F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x65FF, 0x467F, 0x467F, 0x467F,
  0x467F, 0x467F, 0x467F, 0x467F, 0x65FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x5DAB, 0x5DAB, 0x5DAB, 0x5DAB, 0x467F, 0x467F, 0x467F,
  0x467F, 0x467F, 0x467F, 0x467F, 0x5DAB, 0x5DAB, 0x5DAB, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x5DAB, 0x2B66, 0x2B66, 0x2B66, 0x5DAB, 0x65FF, 0x65FF, 0x467F,
  0x467F, 0x467F, 0x65FF, 0x65FF, 0x2B66, 0x2B66, 0x2B66, 0x5DAB, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x5DAB, 0x2B66, 0x2B66, 0x2B66, 0x2B66, 0x5DAB, 0x2B66, 0x2B66, 0x2B66,
  0x2B66, 0x2B66, 0x2B66, 0x5DAB, 0x2B66, 0x2B66, 0x2B66, 0x2B66, 0x5DAB, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x5DAB, 0x2B66, 0x2B66, 0x2B66, 0x2B66, 0x5DAB, 0x2B66, 0x2B66, 0x2B66,
  0x2B66, 0x2B66, 0x2B66, 0x5DAB, 0x2B66, 0x2B66, 0x2B66, 0x2B66, 0x5DAB, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x5DAB, 0x2B66, 0x2B66, 0x2B66, 0x2B66, 0x5DAB, 0x2B66, 0x2B66, 0x2B66,
  0x2B66, 0x2B66, 0x2B66, 0x5DAB, 0x2B66, 0x2B66, 0x2B66, 0x2B66, 0x5DAB, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x5DAB, 0x2B66, 0x2B66, 0x2B66, 0x5DAB, 0x2B66, 0x2B66, 0x2B66,
  0x2B66, 0x2B66, 0x2B66, 0x5DAB, 0x2B66, 0x2B66, 0x2B66, 0x5DAB, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x5DAB, 0x5DAB, 0x5DAB, 0x5DAB, 0x5DAB, 0x5DAB, 0x5DAB,
  0x5DAB, 0x5DAB, 0x5DAB, 0x5DAB, 0x5DAB, 0x5DAB, 0x5DAB, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000
};

static const uint16_t ICON_SCHED_24[576] PROGMEM = {
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0xFE07, 0xFE07, 0xFE07, 0xFFFF, 0xFFFF, 0xFE07, 0xFE07, 0xFE07,
  0xFE07, 0xFE07, 0xFE07, 0xFE07, 0xFFFF, 0xFFFF, 0xFE07, 0xFE07, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0xFE07, 0xFE07, 0xFE07, 0xFE07, 0xFFFF, 0xFFFF, 0xFE07, 0xFE07, 0xFE07,
  0xFE07, 0xFE07, 0xFE07, 0xFE07, 0xFFFF, 0xFFFF, 0xFE07, 0xFE07, 0xFE07, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0xFE07, 0xFE07, 0xFE07, 0xFE07, 0xFFFF, 0xFFFF, 0xFE07, 0xFE07, 0xFE07,
  0xFE07, 0xFE07, 0xFE07, 0xFE07, 0xFFFF, 0xFFFF, 0xFE07, 0xFE07, 0xFE07, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0xFE07, 0xFE07, 0xFE07, 0xFE07, 0xFFFF, 0xFFFF, 0xFE07, 0xFE07, 0xFE07,
  0xFE07, 0xFE07, 0xFE07, 0xFE07, 0xFFFF, 0xFFFF, 0xFE07, 0xFE07, 0xFE07, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0xFFFF, 0xFE07, 0xFE07, 0xFE07, 0xFE07, 0xFE07, 0xFE07, 0xFE07, 0xFE07,
  0xFE07, 0xFE07, 0xFE07, 0xFE07, 0xFE07, 0xFE07, 0xFE07, 0xFE07, 0xFFFF, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0xFFFF, 0x18E4, 0x18E4, 0x18E4, 0x18E4, 0x18E4, 0x18E4, 0x18E4, 0x18E4,
  0x18E4, 0x18E4, 0x18E4, 0x18E4, 0x18E4, 0x18E4, 0x18E4, 0x18E4, 0xFFFF, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0xFFFF, 0x18E4, 0x4228, 0x4228, 0x18E4, 0x18E4, 0x18E4, 0x18E4, 0xFFFF,
  0xFFFF, 0xFFFF, 0x18E4, 0x18E4, 0x18E4, 0x18E4, 0x18E4, 0x18E4, 0xFFFF, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0xFFFF, 0x18E4, 0x4228, 0x4228, 0x18E4, 0x18E4, 0xFFFF, 0xFFFF, 0x18E4,
  0xFFFF, 0x18E4, 0xFFFF, 0xFFFF, 0x18E4, 0x18E4, 0x18E4, 0x18E4, 0xFFFF, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0xFFFF, 0x18E4, 0x18E4, 0x18E4, 0x18E4, 0x18E4, 0xFFFF, 0x18E4, 0x18E4,
  0xFFFF, 0x18E4, 0x18E4, 0xFFFF, 0x18E4, 0x18E4, 0x18E4, 0x18E4, 0xFFFF, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0xFFFF, 0x18E4, 0x18E4, 0x18E4, 0x18E4, 0xFFFF, 0x18E4, 0x18E4, 0x18E4,
  0xFFFF, 0x18E4, 0x18E4, 0x18E4, 0xFFFF, 0x18E4, 0x18E4, 0x18E4, 0xFFFF, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0xFFFF, 0x18E4, 0x18E4, 0x18E4, 0x18E4, 0xFFFF, 0x18E4, 0x18E4, 0x18E4,
  0xFFFF, 0x18E4, 0x18E4, 0x18E4, 0xFFFF, 0x18E4, 0x18E4, 0x18E4, 0xFFFF, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0xFFFF, 0x18E4, 0x18E4, 0x18E4, 0x18E4, 0xFFFF, 0x18E4, 0x18E4, 0x18E4,
  0x18E4, 0xFFFF, 0xFFFF, 0x18E4, 0xFFFF, 0x18E4, 0x18E4, 0x18E4, 0xFFFF, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0xFFFF, 0x18E4, 0x18E4, 0x18E4, 0x18E4, 0x18E4, 0xFFFF, 0x18E4, 0x18E4,
  0x18E4, 0x18E4, 0x18E4, 0xFFFF, 0x18E4, 0x18E4, 0x18E4, 0x18E4, 0xFFFF, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x18E4, 0x18E4, 0x18E4, 0x18E4, 0xFFFF, 0xFFFF, 0x18E4,
  0x18E4, 0x18E4, 0xFFFF, 0xFFFF, 0x18E4, 0x18E4, 0x18E4, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x18E4, 0x18E4, 0x18E4, 0x18E4, 0x18E4, 0xFFFF,
  0xFFFF, 0xFFFF, 0x18E4, 0x18E4, 0x18E4, 0x18E4, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
  0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000
};

static const uint16_t ICON_SPIGOT_24[576] PROGMEM = {
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x2166, 0x2166, 0x2166, 0x2166, 0x2166, 0x2166,
  0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x2166, 0x2166, 0x2166, 0x2166, 0x2166, 0x2166,
  0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x2166, 0x2166, 0xFFFF, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x2166, 0x2166, 0xFFFF, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
  0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0xFFFF, 0x2166, 0x2166, 0x2166, 0x2166, 0x2166, 0x2166, 0x2166, 0x2166,
  0x2166, 0x2166, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0xFFFF, 0x2166, 0x2166, 0x2166, 0x2166, 0x2166, 0x2166, 0x2166, 0x2166,
  0x2166, 0x2166, 0xFFFF, 0x2166, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0xFFFF, 0x2166, 0x2166, 0x2166, 0x2166, 0x2166, 0x2166, 0x2166, 0x2166,
  0x2166, 0x2166, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
  0xFFFF, 0xFFFF, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x467F, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x467F, 0x467F, 0x467F, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x467F, 0x467F, 0x467F, 0x467F, 0x467F, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x467F, 0x467F, 0x467F, 0x467F, 0x467F, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x467F, 0x467F, 0x467F, 0x467F, 0x467F, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x467F, 0x467F, 0x467F, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000
};

static const uint16_t ICON_STATUS_24[576] PROGMEM = {
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x4FAF, 0x4FAF, 0x4FAF, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x4FAF, 0x4FAF, 0x4FAF, 0x4FAF, 0x4FAF, 0x0000, 0xFFFF, 0xFFFF,
  0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x4FAF, 0x4FAF, 0x4FAF, 0x4FAF, 0x4FAF, 0xFFFF, 0xFFFF, 0xFFFF,
  0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x4FAF, 0x4FAF, 0x4FAF, 0x4FAF, 0x4FAF, 0xFFFF, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x4FAF, 0x4FAF, 0x4FAF, 0x0000, 0x0000, 0xFFFF, 0xFFFF,
  0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF,
  0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x4FAF,
  0x4FAF, 0x4FAF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x4FAF, 0x4FAF,
  0x4FAF, 0x4FAF, 0x4FAF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x4FAF, 0x4FAF,
  0x4FAF, 0x4FAF, 0x4FAF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x4FAF, 0x4FAF,
  0x4FAF, 0x4FAF, 0x4FAF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x4FAF,
  0x4FAF, 0x4FAF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000
};

static const uint16_t ICON_STOP_24[576] PROGMEM = {
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF,
  0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFAAA,
  0xFAAA, 0xFAAA, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA,
  0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA,
  0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA,
  0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA,
  0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFFFF, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0xFFFF, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA,
  0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFFFF, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0xFFFF, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA,
  0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFFFF, 0x0000, 0x0000,
  0x0000, 0x0000, 0xFFFF, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
  0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFFFF, 0x0000,
  0x0000, 0x0000, 0xFFFF, 0xFAAA, 0xFAAA, 0xFAAA, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
  0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFAAA, 0xFAAA, 0xFAAA, 0xFFFF, 0x0000,
  0x0000, 0x0000, 0xFFFF, 0xFAAA, 0xFAAA, 0xFAAA, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
  0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFAAA, 0xFAAA, 0xFAAA, 0xFFFF, 0x0000,
  0x0000, 0x0000, 0xFFFF, 0xFAAA, 0xFAAA, 0xFAAA, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
  0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFAAA, 0xFAAA, 0xFAAA, 0xFFFF, 0x0000,
  0x0000, 0x0000, 0xFFFF, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
  0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFFFF, 0x0000,
  0x0000, 0x0000, 0x0000, 0xFFFF, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA,
  0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFFFF, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0xFFFF, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA,
  0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFFFF, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA,
  0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFFFF, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA,
  0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA,
  0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA,
  0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFAAA, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFAAA,
  0xFAAA, 0xFAAA, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF,
  0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000
};

// ----------------------------- Libraries ------------------------------------

Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, GFX_NOT_DEFINED, FSPI, true);
Arduino_GFX *gfx = new Arduino_GC9A01(bus, TFT_RST, 0, true);

Adafruit_NeoPixel pixels(LED_NUM, LED_PIN, NEO_GRB + NEO_KHZ800);
Preferences prefs;
WebServer server(80);
DNSServer dnsServer;

IPAddress apIP(192, 168, 6, 1);
IPAddress apGateway(192, 168, 6, 1);
IPAddress apSubnet(255, 255, 255, 0);

// ----------------------------- Early Types ----------------------------------
// Arduino IDE generates function prototypes near the top of .ino files.
// These shared types must be declared before that prototype pass sees functions
// such as toScreen(), localRemaining(), readTouch(), and go().




// Forward declarations for helpers that are called before their definitions.
// These stop Arduino's auto-prototype generator from creating broken prototypes.
void drawUI();
void updateZoneLeds(bool force = false);
String buildUiStateSignature();
void requestRedrawIfUiStateChanged();
void go(ScreenId s);
void emergencyPress();
bool waterIsActive();
void noteUserInteraction();
void setScreen(ScreenId s, bool userNavigation);
void openScheduleEditor();
void cancelTouchForButtonPress();
bool touchSuppressed();
void drawPlainPageDots(int active, int total, uint16_t accent);
void drawPlainTitle(const String &title, const String &subtitle, uint16_t accent);
void drawPlainCard(int x, int y, int w, int h, uint16_t outline = C_PANEL);
void drawCleanCard(int x, int y, int w, int h, uint16_t fill = C_CARD, uint16_t outline = C_PANEL);
void drawMainCarouselIcon(int idx, int cx, int cy, uint16_t accent);
void drawBackEdge();
const uint16_t * homeMenuIconData(int idx);
void drawSprite565(int x, int y, int w, int h, const uint16_t *bmp, uint16_t transparent = SPRITE_TRANSPARENT);
void drawSprite565Scaled(int x, int y, int w, int h, const uint16_t *bmp, int scale, uint16_t transparent = SPRITE_TRANSPARENT);
void drawHomeCarouselEdgeIndicator(int active, int total, uint16_t accent);
void drawHomeHeader(const String &title, const String &subtitle, int idx, uint16_t accent);
void drawSelectedZoneOnly(int zoneNumber);
bool zoneIsActive(int zoneNumber);
void drawZonePolyFilledRecentered(uint8_t polyIndex, float minX, float maxX, float minY, float maxY, uint16_t fill, uint16_t outline);
ScreenPt toZoneScreen(const MapPt &p, float minX, float maxX, float minY, float maxY);
extern bool needsRedraw;
extern uint32_t commandMessageUntil;

// ----------------------------- Config ---------------------------------------

struct Config {
  String deviceName = "GardenKnob";
  String apSsid = "GardenKnob";
  String apPass = "gardenknob";
  String staSsid = "";
  String staPass = "";
  String relayBaseUrl = "http://192.168.4.1";
  String relayToken = "";
  bool emergencyDirectStop = false;
  uint8_t brightness = 210;
  uint8_t lastSelectedZone = 1;
  uint16_t manualDefaultMinutes = 15;
};

Config cfg;

void loadConfig() {
  prefs.begin("gardenknob", true);
  cfg.deviceName = prefs.getString("deviceName", "GardenKnob");
  cfg.apSsid = prefs.getString("apSsid", "GardenKnob");
  cfg.apPass = prefs.getString("apPass", "gardenknob");
  cfg.staSsid = prefs.getString("staSsid", "");
  cfg.staPass = prefs.getString("staPass", "");
  cfg.relayBaseUrl = prefs.getString("relayUrl", "http://192.168.4.1");
  cfg.relayToken = prefs.getString("relayToken", "");
  cfg.emergencyDirectStop = prefs.getBool("emStop", false);
  cfg.brightness = prefs.getUChar("bright", 210);
  cfg.lastSelectedZone = prefs.getUChar("lastZone", 1);
  if (cfg.lastSelectedZone < 1 || cfg.lastSelectedZone > 5) cfg.lastSelectedZone = 1;
  cfg.manualDefaultMinutes = prefs.getUShort("defMin", 15);
  if (cfg.manualDefaultMinutes < 1 || cfg.manualDefaultMinutes > 240) cfg.manualDefaultMinutes = 15;
  prefs.end();
}

void saveConfig() {
  prefs.begin("gardenknob", false);
  prefs.putString("deviceName", cfg.deviceName);
  prefs.putString("apSsid", cfg.apSsid);
  prefs.putString("apPass", cfg.apPass);
  prefs.putString("staSsid", cfg.staSsid);
  prefs.putString("staPass", cfg.staPass);
  prefs.putString("relayUrl", cfg.relayBaseUrl);
  prefs.putString("relayToken", cfg.relayToken);
  prefs.putBool("emStop", cfg.emergencyDirectStop);
  prefs.putUChar("bright", cfg.brightness);
  prefs.putUChar("lastZone", cfg.lastSelectedZone);
  prefs.putUShort("defMin", cfg.manualDefaultMinutes);
  prefs.end();
}

void factoryReset() {
  prefs.begin("gardenknob", false);
  prefs.clear();
  prefs.end();
  delay(300);
  ESP.restart();
}

// ----------------------------- Geometry -------------------------------------




// Six polygons representing the e-ink garden geometry. Coordinates are normalized
// to a 100x100 local garden map and then scaled into the circular safe display area.
static const MapPt POLY0[] = { {8,58}, {36,55}, {42,90}, {10,90} };              // bottom-left
static const MapPt POLY1[] = { {36,55}, {63,50}, {72,88}, {42,90} };             // middle
static const MapPt POLY2[] = { {63,50}, {94,44}, {95,88}, {72,88} };             // right-most
static const MapPt POLY3[] = { {8,23}, {38,20}, {36,55}, {8,58} };              // upper-left diagonal
static const MapPt POLY4[] = { {8,8}, {48,8}, {38,20}, {8,23} };                // top-left
static const MapPt POLY5[] = { {48,8}, {93,10}, {94,44}, {63,50}, {38,20} };    // top-right-most

Poly polys[6] = {
  { POLY0, 4, 0, 0 },
  { POLY1, 4, 0, 0 },
  { POLY2, 4, 0, 0 },
  { POLY3, 4, 0, 0 },
  { POLY4, 4, 0, 0 },
  { POLY5, 5, 0, 0 }
};

static const uint8_t zoneToPolys[5][2] = {
  {2, 255}, // Zone 1 = polygon 2
  {5, 255}, // Zone 2 = polygon 5
  {4, 255}, // Zone 3 = polygon 4
  {0, 1},   // Zone 4 = polygons 0 and 1
  {3, 255}  // Zone 5 = polygon 3
};

float mapMinX = 8;
float mapMaxX = 95;
float mapMinY = 8;
float mapMaxY = 90;
float mapScale = 1.0f;
float mapOffX = 0.0f;
float mapOffY = 0.0f;

ScreenPt toScreen(const MapPt &p) {
  ScreenPt s;
  s.x = (int16_t)roundf(mapOffX + p.x * mapScale);
  s.y = (int16_t)roundf(mapOffY + p.y * mapScale);
  return s;
}

void computePolyCentroids() {
  for (int i = 0; i < 6; i++) {
    float sx = 0, sy = 0;
    for (int j = 0; j < polys[i].n; j++) {
      sx += polys[i].pts[j].x;
      sy += polys[i].pts[j].y;
    }
    polys[i].cx = sx / polys[i].n;
    polys[i].cy = sy / polys[i].n;
  }
}

void setupMapTransform() {
  float w = mapMaxX - mapMinX;
  float h = mapMaxY - mapMinY;
  float target = 196.0f;
  mapScale = min(target / w, target / h);
  float scaledW = w * mapScale;
  float scaledH = h * mapScale;
  mapOffX = CX - scaledW / 2.0f - mapMinX * mapScale;
  mapOffY = CY - scaledH / 2.0f - mapMinY * mapScale - 2.0f;
}

bool pointInPolyScreen(uint8_t polyIndex, int px, int py) {
  bool inside = false;
  const Poly &poly = polys[polyIndex];
  for (int i = 0, j = poly.n - 1; i < poly.n; j = i++) {
    ScreenPt pi = toScreen(poly.pts[i]);
    ScreenPt pj = toScreen(poly.pts[j]);
    bool intersect = ((pi.y > py) != (pj.y > py)) &&
      (px < (pj.x - pi.x) * (py - pi.y) / (float)((pj.y - pi.y) == 0 ? 1 : (pj.y - pi.y)) + pi.x);
    if (intersect) inside = !inside;
  }
  return inside;
}

int polygonToZoneNumber(uint8_t polyIndex) {
  for (int z = 0; z < 5; z++) {
    if (zoneToPolys[z][0] == polyIndex || zoneToPolys[z][1] == polyIndex) return z + 1;
  }
  return -1;
}

// ----------------------------- Relay State ----------------------------------




RunItem zoneRuns[5];
RunItem spigotRun;
ZoneSchedule zoneSchedules[5];

// Runtime zone colors. Updated from relay API payloads when exposed.
// Fallback values match GardenSimpleRelay6.ino ZONE_COLORS:
// {59,130,246}, {34,197,94}, {245,158,11}, {168,85,247}, {236,72,153}.
ZoneRgb relayZoneColors[5] = {
  {59, 130, 246},
  {34, 197, 94},
  {245, 158, 11},
  {168, 85, 247},
  {236, 72, 153}
};
bool relayZoneColorsLoaded = false;

bool relayConnected = false;
bool scheduleLoaded = false;
bool weatherLoaded = false;
String relayError = "Relay not connected";
String relayStatusText = "Relay disconnected";
String lastCommandText = "";
bool commandPending = false;
String commandPendingText = "";
bool configSavedNotice = false;
uint32_t configSavedNoticeUntil = 0;
bool restartPending = false;
uint32_t restartAtMs = 0;
bool staReconnectPending = false;
uint32_t staReconnectAtMs = 0;
uint32_t lastStatePollMs = 0;
uint32_t lastSchedulePollMs = 0;
uint32_t lastTimePollMs = 0;
uint32_t lastWeatherPollMs = 0;
uint32_t lastGoodStateMs = 0;
uint32_t commandMessageUntil = 0;

uint32_t lastLedUpdateMs = 0;
uint8_t ledCycleIndex = 0;
static const uint32_t LED_CYCLE_MS = 2500;

uint32_t uiAnimStartMs = 0;
uint32_t lastUiSecondMs = 0;
int diagnosticsPage = 0;
bool scheduleZonePickerMode = false;

// Running-screen default behavior.
// When water is active, the countdown/status screen is the default.
// If the user navigates away, keep their chosen screen until idle timeout.
static const uint32_t RUNNING_RETURN_IDLE_MS = 60000;
bool wasWateringActive = false;
bool userLeftRunningScreen = false;
uint32_t lastUserInteractionMs = 0;
uint8_t ambientSweepIndex = 0;
int displayedRunningZone = 0;   // 1-5 while zone status is displayed
bool displayedRunningSpigot = false;
int homeMenuIndex = 0;
static const int HOME_MENU_COUNT = 5;
static const int BACK_EDGE_W = 20;
static const int BACK_SWIPE_START_X = 36;
static const int BACK_SWIPE_MIN_DX = 36;
static const int BACK_TAP_HIT_W = 34;

// Low-flash rendering guard.
// Full-frame GC9A01 redraws clear to black first and can visibly flash.
ScreenId lastDrawnScreen = SCR_HOME;
int lastDrawnHomeMenuIndex = -1;
int lastDrawnSelectedZone = -1;
int lastDrawnScheduleZone = -1;
int lastDrawnScheduleSlot = -1;
String lastUiStateSignature = "";

bool isAnyZoneRunning() {
  for (int i = 0; i < 5; i++) {
    if (zoneRuns[i].active) return true;
  }
  return false;
}

int activeZoneCount() {
  int c = 0;
  for (int i = 0; i < 5; i++) if (zoneRuns[i].active) c++;
  return c;
}

long localRemaining(const RunItem &r) {
  if (!r.active) return 0;
  if (r.remainingSeconds < 0) return -1;
  long elapsed = (millis() - r.lastUpdateMs) / 1000;
  long rem = r.remainingSeconds - elapsed;
  if (rem < 0) rem = 0;
  return rem;
}

String fmtDuration(long seconds) {
  if (seconds < 0) return "--";
  long m = seconds / 60;
  long s = seconds % 60;
  char buf[20];
  if (m >= 60) {
    snprintf(buf, sizeof(buf), "%ldh %02ldm", m / 60, m % 60);
  } else {
    snprintf(buf, sizeof(buf), "%ldm %02lds", m, s);
  }
  return String(buf);
}

String fmtTime12(int h, int m) {
  int hh = h % 12;
  if (hh == 0) hh = 12;
  char buf[16];
  snprintf(buf, sizeof(buf), "%d:%02d%s", hh, m, h >= 12 ? "p" : "a");
  return String(buf);
}

uint32_t zoneLedColor(int zoneNumber) {
  if (zoneNumber >= 1 && zoneNumber <= 5) {
    ZoneRgb &c = relayZoneColors[zoneNumber - 1];
    return pixels.Color(c.r, c.g, c.b);
  }
  return pixels.Color(18, 18, 18);
}

uint32_t spigotLedColor() {
  return pixels.Color(0, 210, 210);             // Spigots/master: cyan, not a zone
}

uint32_t statusLedColor() {
  if (commandPending) return pixels.Color(255, 190, 0);
  if (!relayConnected) return pixels.Color(120, 0, 0);
  return pixels.Color(0, 28, 8);
}

void fillAllLeds(uint32_t color) {
  for (int i = 0; i < LED_NUM; i++) pixels.setPixelColor(i, color);
  pixels.show();
}

void updateZoneLeds(bool force) {
  uint32_t now = millis();
  if (!force && now - lastLedUpdateMs < 120) return;

  int activeZones[5];
  int count = 0;
  for (int i = 0; i < 5; i++) {
    if (zoneRuns[i].active) activeZones[count++] = i + 1;
  }

  if (count > 1 && (force || now - lastLedUpdateMs >= LED_CYCLE_MS)) {
    ledCycleIndex = (ledCycleIndex + 1) % count;
  } else if (count <= 1) {
    ledCycleIndex = 0;
  }

  if (count == 0) {
    fillAllLeds(spigotRun.active ? spigotLedColor() : statusLedColor());
    lastLedUpdateMs = now;
    return;
  }

  if (count == 1) {
    fillAllLeds(zoneLedColor(activeZones[0]));
    lastLedUpdateMs = now;
    return;
  }

  fillAllLeds(zoneLedColor(activeZones[ledCycleIndex % count]));
  lastLedUpdateMs = now;
}

int coerceZoneNumber(JsonVariantConst v, bool knownZeroBased) {
  if (v.isNull()) return 0;
  int raw = v.as<int>();
  if (knownZeroBased) return constrain(raw + 1, 1, 5);
  if (raw >= 1 && raw <= 5) return raw;
  if (raw == 0) return 1; // only used when caller has no better field; do not display Zone 0
  return 0;
}

void clearRuns() {
  for (int i = 0; i < 5; i++) zoneRuns[i] = RunItem();
  spigotRun = RunItem();
  spigotRun.spigot = true;
}

void applyRunItem(int zoneNumber, bool spigot, bool active, long remainingSec, long durationSec) {
  if (spigot) {
    spigotRun.active = active;
    spigotRun.spigot = true;
    spigotRun.zoneNumber = 0;
    spigotRun.remainingSeconds = remainingSec;
    spigotRun.durationSeconds = durationSec;
    spigotRun.lastUpdateMs = millis();
    return;
  }
  if (zoneNumber < 1 || zoneNumber > 5) return;
  RunItem &r = zoneRuns[zoneNumber - 1];
  r.active = active;
  r.spigot = false;
  r.zoneNumber = zoneNumber;
  r.remainingSeconds = remainingSec;
  r.durationSeconds = durationSec;
  r.lastUpdateMs = millis();
}

long readSeconds(JsonObjectConst obj) {
  if (obj["remainingSeconds"].is<long>()) return obj["remainingSeconds"].as<long>();
  if (obj["remainingSec"].is<long>()) return obj["remainingSec"].as<long>();
  if (obj["remainingMs"].is<long>()) return obj["remainingMs"].as<long>() / 1000;
  if (obj["durationSeconds"].is<long>() && obj["elapsedSeconds"].is<long>()) {
    return max(0L, obj["durationSeconds"].as<long>() - obj["elapsedSeconds"].as<long>());
  }
  if (obj["durationMs"].is<long>() && obj["elapsedMs"].is<long>()) {
    return max(0L, (obj["durationMs"].as<long>() - obj["elapsedMs"].as<long>()) / 1000);
  }
  if (obj["runMinutes"].is<long>()) return obj["runMinutes"].as<long>() * 60;
  if (obj["durationMinutes"].is<long>()) return obj["durationMinutes"].as<long>() * 60;
  return -1;
}

long readDurationSeconds(JsonObjectConst obj) {
  if (obj["durationSeconds"].is<long>()) return obj["durationSeconds"].as<long>();
  if (obj["durationMs"].is<long>()) return obj["durationMs"].as<long>() / 1000;
  if (obj["runMinutes"].is<long>()) return obj["runMinutes"].as<long>() * 60;
  if (obj["durationMinutes"].is<long>()) return obj["durationMinutes"].as<long>() * 60;
  return -1;
}


int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool parseHexColor(const String &value, ZoneRgb &out) {
  String s = value;
  s.trim();
  if (s.startsWith("#")) s = s.substring(1);
  if (s.length() != 6) return false;
  int vals[6];
  for (int i = 0; i < 6; i++) {
    vals[i] = hexNibble(s[i]);
    if (vals[i] < 0) return false;
  }
  out.r = (vals[0] << 4) | vals[1];
  out.g = (vals[2] << 4) | vals[3];
  out.b = (vals[4] << 4) | vals[5];
  return true;
}

bool readRgbFromObject(JsonObjectConst obj, ZoneRgb &out) {
  if (obj["r"].is<int>() && obj["g"].is<int>() && obj["b"].is<int>()) {
    out.r = constrain(obj["r"].as<int>(), 0, 255);
    out.g = constrain(obj["g"].as<int>(), 0, 255);
    out.b = constrain(obj["b"].as<int>(), 0, 255);
    return true;
  }
  if (obj["red"].is<int>() && obj["green"].is<int>() && obj["blue"].is<int>()) {
    out.r = constrain(obj["red"].as<int>(), 0, 255);
    out.g = constrain(obj["green"].as<int>(), 0, 255);
    out.b = constrain(obj["blue"].as<int>(), 0, 255);
    return true;
  }
  if (obj["color"].is<JsonObjectConst>()) {
    return readRgbFromObject(obj["color"].as<JsonObjectConst>(), out);
  }
  if (obj["rgb"].is<JsonArrayConst>()) {
    JsonArrayConst a = obj["rgb"].as<JsonArrayConst>();
    if (a.size() >= 3) {
      out.r = constrain(a[0].as<int>(), 0, 255);
      out.g = constrain(a[1].as<int>(), 0, 255);
      out.b = constrain(a[2].as<int>(), 0, 255);
      return true;
    }
  }
  if (obj["color"].is<const char*>()) return parseHexColor(String(obj["color"].as<const char*>()), out);
  if (obj["hex"].is<const char*>()) return parseHexColor(String(obj["hex"].as<const char*>()), out);
  if (obj["colorHex"].is<const char*>()) return parseHexColor(String(obj["colorHex"].as<const char*>()), out);
  return false;
}

void applyRelayZoneColorFromObject(JsonObjectConst obj) {
  int zoneNumber = 0;
  if (obj["zoneIndex"].is<int>()) zoneNumber = coerceZoneNumber(obj["zoneIndex"], true);
  else if (obj["zoneNumber"].is<int>()) zoneNumber = coerceZoneNumber(obj["zoneNumber"], false);
  else if (obj["displayZone"].is<int>()) zoneNumber = coerceZoneNumber(obj["displayZone"], false);
  else if (obj["zone"].is<int>()) zoneNumber = coerceZoneNumber(obj["zone"], false);
  else if (obj["channel"].is<int>() && obj["channel"].as<int>() >= 1 && obj["channel"].as<int>() <= 5) zoneNumber = obj["channel"].as<int>();

  if (zoneNumber < 1 || zoneNumber > 5) return;

  ZoneRgb c;
  if (readRgbFromObject(obj, c)) {
    relayZoneColors[zoneNumber - 1] = c;
    relayZoneColorsLoaded = true;
  }
}

void parseRelayZoneColors(JsonDocument &doc) {
  if (doc["zoneColors"].is<JsonArrayConst>()) {
    int implicitZone = 1;
    for (JsonVariantConst v : doc["zoneColors"].as<JsonArrayConst>()) {
      if (v.is<JsonObjectConst>()) {
        JsonObjectConst obj = v.as<JsonObjectConst>();
        ZoneRgb c;
        if (readRgbFromObject(obj, c)) {
          int zoneNumber = 0;
          if (obj["zoneIndex"].is<int>()) zoneNumber = coerceZoneNumber(obj["zoneIndex"], true);
          else if (obj["zoneNumber"].is<int>()) zoneNumber = coerceZoneNumber(obj["zoneNumber"], false);
          else if (obj["zone"].is<int>()) zoneNumber = coerceZoneNumber(obj["zone"], false);
          else zoneNumber = implicitZone;
          if (zoneNumber >= 1 && zoneNumber <= 5) {
            relayZoneColors[zoneNumber - 1] = c;
            relayZoneColorsLoaded = true;
          }
        }
      } else if (v.is<JsonArrayConst>()) {
        JsonArrayConst a = v.as<JsonArrayConst>();
        if (implicitZone >= 1 && implicitZone <= 5 && a.size() >= 3) {
          relayZoneColors[implicitZone - 1] = {
            (uint8_t)constrain(a[0].as<int>(), 0, 255),
            (uint8_t)constrain(a[1].as<int>(), 0, 255),
            (uint8_t)constrain(a[2].as<int>(), 0, 255)
          };
          relayZoneColorsLoaded = true;
        }
      }
      implicitZone++;
    }
  }

  if (doc["zones"].is<JsonArrayConst>()) {
    for (JsonObjectConst obj : doc["zones"].as<JsonArrayConst>()) applyRelayZoneColorFromObject(obj);
  }
  if (doc["zoneRuns"].is<JsonArrayConst>()) {
    for (JsonObjectConst obj : doc["zoneRuns"].as<JsonArrayConst>()) applyRelayZoneColorFromObject(obj);
  }
  if (doc["relays"].is<JsonArrayConst>()) {
    for (JsonObjectConst obj : doc["relays"].as<JsonArrayConst>()) applyRelayZoneColorFromObject(obj);
  }
}

void parseRunObject(JsonObjectConst obj, bool spigotHint = false) {
  bool active = true;
  if (obj["active"].is<bool>()) active = obj["active"].as<bool>();
  if (obj["running"].is<bool>()) active = obj["running"].as<bool>();
  if (obj["on"].is<bool>()) active = obj["on"].as<bool>();
  // Some relay firmware routes can turn a specific relay off without clearing
  // the timer object immediately. If relayOn is present and false, treat it as stopped.
  if (obj["relayOn"].is<bool>() && !obj["relayOn"].as<bool>()) active = false;

  bool spigot = spigotHint;
  if (obj["spigot"].is<bool>()) spigot = obj["spigot"].as<bool>();
  if (obj["master"].is<bool>()) spigot = obj["master"].as<bool>();
  if (obj["relay"].is<int>() && obj["relay"].as<int>() == 6) spigot = true;
  if (obj["relayNumber"].is<int>() && obj["relayNumber"].as<int>() == 6) spigot = true;

  int zoneNumber = 0;
  if (obj["zoneIndex"].is<int>()) zoneNumber = coerceZoneNumber(obj["zoneIndex"], true);
  else if (obj["zoneNumber"].is<int>()) zoneNumber = coerceZoneNumber(obj["zoneNumber"], false);
  else if (obj["displayZone"].is<int>()) zoneNumber = coerceZoneNumber(obj["displayZone"], false);
  else if (obj["zone"].is<int>()) zoneNumber = coerceZoneNumber(obj["zone"], false);

  long remaining = readSeconds(obj);
  long duration = readDurationSeconds(obj);

  if (spigot) {
    applyRunItem(0, true, active, remaining, duration);
  } else if (zoneNumber >= 1 && zoneNumber <= 5) {
    applyRunItem(zoneNumber, false, active, remaining, duration);
  }
}

void parseSchedulesArray(JsonArrayConst arr) {
  for (JsonObjectConst obj : arr) {
    int zoneNumber = 0;
    if (obj["zoneIndex"].is<int>()) zoneNumber = coerceZoneNumber(obj["zoneIndex"], true);
    else if (obj["zoneNumber"].is<int>()) zoneNumber = coerceZoneNumber(obj["zoneNumber"], false);
    else if (obj["displayZone"].is<int>()) zoneNumber = coerceZoneNumber(obj["displayZone"], false);
    else if (obj["zone"].is<int>()) zoneNumber = coerceZoneNumber(obj["zone"], false);
    if (zoneNumber < 1 || zoneNumber > 5) continue;

    ZoneSchedule &zs = zoneSchedules[zoneNumber - 1];
    zs.loaded = true;
    if (zs.count >= 6) continue;

    ScheduleItem &si = zs.slots[zs.count++];
    si.valid = true;
    si.zoneNumber = zoneNumber;
    si.enabled = obj["enabled"].is<bool>() ? obj["enabled"].as<bool>() : true;

    if (obj["startHour"].is<int>()) si.startHour = obj["startHour"].as<int>();
    else if (obj["hour"].is<int>()) si.startHour = obj["hour"].as<int>();

    if (obj["startMinute"].is<int>()) si.startMinute = obj["startMinute"].as<int>();
    else if (obj["minute"].is<int>()) si.startMinute = obj["minute"].as<int>();

    if (obj["runMinutes"].is<int>()) si.durationMinutes = obj["runMinutes"].as<int>();
    else if (obj["durationMinutes"].is<int>()) si.durationMinutes = obj["durationMinutes"].as<int>();
    else if (obj["minutes"].is<int>()) si.durationMinutes = obj["minutes"].as<int>();
  }
}

void parseSchedules(JsonDocument &doc) {
  for (int i = 0; i < 5; i++) {
    zoneSchedules[i] = ZoneSchedule();
    zoneSchedules[i].loaded = false;
  }

  if (doc["dailySchedules"].is<JsonArrayConst>()) parseSchedulesArray(doc["dailySchedules"].as<JsonArrayConst>());
  if (doc["schedules"].is<JsonArrayConst>()) parseSchedulesArray(doc["schedules"].as<JsonArrayConst>());

  if (doc["zones"].is<JsonArrayConst>()) {
    for (JsonObjectConst zobj : doc["zones"].as<JsonArrayConst>()) {
      int zoneNumber = 0;
      if (zobj["zoneIndex"].is<int>()) zoneNumber = coerceZoneNumber(zobj["zoneIndex"], true);
      else if (zobj["zoneNumber"].is<int>()) zoneNumber = coerceZoneNumber(zobj["zoneNumber"], false);
      else if (zobj["zone"].is<int>()) zoneNumber = coerceZoneNumber(zobj["zone"], false);
      if (zoneNumber < 1 || zoneNumber > 5) continue;

      zoneSchedules[zoneNumber - 1].loaded = true;
      if (zobj["schedules"].is<JsonArrayConst>()) parseSchedulesArray(zobj["schedules"].as<JsonArrayConst>());
      if (zobj["dailySchedules"].is<JsonArrayConst>()) parseSchedulesArray(zobj["dailySchedules"].as<JsonArrayConst>());
    }
  }

  scheduleLoaded = false;
  for (int i = 0; i < 5; i++) {
    if (zoneSchedules[i].loaded || zoneSchedules[i].count > 0) scheduleLoaded = true;
  }
}

void parseState(JsonDocument &doc) {
  clearRuns();
  parseRelayZoneColors(doc);

  if (doc["currentRun"].is<JsonObjectConst>()) parseRunObject(doc["currentRun"].as<JsonObjectConst>());
  if (doc["currentSpigotRun"].is<JsonObjectConst>()) parseRunObject(doc["currentSpigotRun"].as<JsonObjectConst>(), true);
  if (doc["spigotRun"].is<JsonObjectConst>()) parseRunObject(doc["spigotRun"].as<JsonObjectConst>(), true);

  if (doc["zoneRuns"].is<JsonArrayConst>()) {
    for (JsonObjectConst obj : doc["zoneRuns"].as<JsonArrayConst>()) parseRunObject(obj);
  }

  if (doc["relays"].is<JsonArrayConst>()) {
    int idx = 0;
    for (JsonVariantConst rv : doc["relays"].as<JsonArrayConst>()) {
      idx++;
      if (rv.is<bool>()) {
        bool on = rv.as<bool>();
        if (idx >= 1 && idx <= 5) applyRunItem(idx, false, on, -1, -1);
        if (idx == 6) applyRunItem(0, true, on, -1, -1);
      } else if (rv.is<JsonObjectConst>()) {
        JsonObjectConst obj = rv.as<JsonObjectConst>();
        bool on = obj["on"].is<bool>() ? obj["on"].as<bool>() : (obj["active"].is<bool>() ? obj["active"].as<bool>() : false);
        int relayNumber = obj["relay"].is<int>() ? obj["relay"].as<int>() : (obj["relayNumber"].is<int>() ? obj["relayNumber"].as<int>() : idx);
        if (relayNumber >= 1 && relayNumber <= 5) applyRunItem(relayNumber, false, on, readSeconds(obj), readDurationSeconds(obj));
        if (relayNumber == 6) applyRunItem(0, true, on, readSeconds(obj), readDurationSeconds(obj));
      }
    }
  }

  parseSchedules(doc);

  relayConnected = true;
  relayError = "";
  relayStatusText = "Connected";
  lastGoodStateMs = millis();
  updateZoneLeds(true);
}

// ----------------------------- HTTP Relay -----------------------------------

String joinUrl(const String &base, const String &path) {
  if (base.endsWith("/") && path.startsWith("/")) return base.substring(0, base.length() - 1) + path;
  if (!base.endsWith("/") && !path.startsWith("/")) return base + "/" + path;
  return base + path;
}

bool relayGetRaw(const String &path, String &body, int &httpCode, String &err) {
  if (cfg.relayBaseUrl.length() == 0) {
    err = "relay URL blank";
    httpCode = -1;
    return false;
  }

  WiFiClient client;
  HTTPClient http;
  String url = joinUrl(cfg.relayBaseUrl, path);
  if (!http.begin(client, url)) {
    err = "relay HTTP begin failed";
    httpCode = -1;
    return false;
  }
  http.setTimeout(3500);
  if (cfg.relayToken.length()) {
    http.addHeader("x-api-token", cfg.relayToken);
  }
  httpCode = http.GET();
  if (httpCode <= 0) {
    err = "relay request failed";
    http.end();
    return false;
  }
  body = http.getString();
  http.end();

  if (httpCode < 200 || httpCode >= 300) {
    err = "relay returned non-200";
    return false;
  }
  return true;
}

bool relayPostJson(const String &path, const String &payload, String &body, int &httpCode, String &err) {
  if (cfg.relayBaseUrl.length() == 0) {
    err = "relay URL blank";
    httpCode = -1;
    return false;
  }
  WiFiClient client;
  HTTPClient http;
  String url = joinUrl(cfg.relayBaseUrl, path);
  if (!http.begin(client, url)) {
    err = "relay HTTP begin failed";
    httpCode = -1;
    return false;
  }
  http.setTimeout(5000);
  http.addHeader("Content-Type", "application/json");
  if (cfg.relayToken.length()) {
    http.addHeader("x-api-token", cfg.relayToken);
  }
  httpCode = http.POST(payload);
  if (httpCode <= 0) {
    err = "relay request failed";
    http.end();
    return false;
  }
  body = http.getString();
  http.end();
  if (httpCode < 200 || httpCode >= 300) {
    err = "relay returned non-200";
    return false;
  }
  return true;
}

bool loadRelayState(bool fallbackOk = true) {
  String body, err;
  int code = 0;
  bool ok = relayGetRaw("/api/state", body, code, err);
  if (!ok && fallbackOk) ok = relayGetRaw("/status", body, code, err);

  if (!ok) {
    relayConnected = false;
    relayError = err + (code > 0 ? " HTTP " + String(code) : "");
    relayStatusText = relayError;
    return false;
  }

  DynamicJsonDocument doc(16384);
  DeserializationError jerr = deserializeJson(doc, body);
  if (jerr) {
    relayConnected = false;
    relayError = "relay JSON parse failed";
    relayStatusText = relayError;
    return false;
  }

  parseState(doc);
  if (scheduleLoaded) relayStatusText = "Connected";
  else relayStatusText = "state loaded, schedule not loaded";
  return true;
}

bool loadScheduleOnly() {
  bool ok = loadRelayState(true);
  if (!ok) return false;
  if (!scheduleLoaded) {
    relayStatusText = "state loaded but no schedules";
  }
  return ok;
}

bool pingRelay() {
  String body, err;
  int code;
  if (cfg.relayBaseUrl.length() == 0) {
    relayConnected = false;
    relayError = "relay URL blank";
    return false;
  }

  bool ok = relayGetRaw("/api/state", body, code, err);
  if (!ok) ok = relayGetRaw("/status", body, code, err);
  if (!ok) {
    relayConnected = false;
    relayError = err + (code > 0 ? " HTTP " + String(code) : "");
    return false;
  }
  relayConnected = true;
  relayError = "";
  return true;
}


void showCommandPending(const String &text) {
  commandPending = true;
  commandPendingText = text;
  // Do not force an immediate full-screen redraw here.
  // TFT redraw clears to black first; command animations caused visible flashing.
}

void clearCommandPending() {
  commandPending = false;
  commandPendingText = "";
  commandMessageUntil = millis() + 1200;
  needsRedraw = true;
}

bool sendManualRun(int zoneNumber, int minutes) {
  zoneNumber = constrain(zoneNumber, 1, 5);
  minutes = constrain(minutes, 1, 240);
  cfg.lastSelectedZone = zoneNumber;
  cfg.manualDefaultMinutes = minutes;
  saveConfig();
  showCommandPending("Sending...");
  String body, err;
  int code;
  bool ok = relayGetRaw("/api/manual-run?zone=" + String(zoneNumber) + "&minutes=" + String(minutes), body, code, err);
  lastCommandText = ok ? "Started Zone " + String(zoneNumber) : "Command Failed /api/manual-run " + err + (code > 0 ? " HTTP " + String(code) : "");
  clearCommandPending();
  loadRelayState(true);
  return ok;
}

bool sendSpigotRun(int minutes) {
  minutes = constrain(minutes, 1, 240);
  cfg.manualDefaultMinutes = minutes;
  saveConfig();
  showCommandPending("Sending...");
  String body, err;
  int code;
  bool ok = relayGetRaw("/api/spigots-run?minutes=" + String(minutes), body, code, err);
  lastCommandText = ok ? "Spigots On" : "Command Failed /api/spigots-run " + err + (code > 0 ? " HTTP " + String(code) : "");
  clearCommandPending();
  loadRelayState(true);
  return ok;
}


bool relayGetFirstOk(const String paths[], int pathCount, String &usedPath, String &lastErr, int &lastCode) {
  String body;
  for (int i = 0; i < pathCount; i++) {
    String err;
    int code = 0;
    bool ok = relayGetRaw(paths[i], body, code, err);
    if (ok) {
      usedPath = paths[i];
      lastCode = code;
      lastErr = "";
      return true;
    }
    usedPath = paths[i];
    lastErr = err;
    lastCode = code;
  }
  return false;
}

bool sendStopZone(int zoneNumber) {
  zoneNumber = constrain(zoneNumber, 1, 5);
  showCommandPending("Stopping Z" + String(zoneNumber));

  String paths[] = {
    "/api/zone-stop?zone=" + String(zoneNumber),
    "/api/stop-zone?zone=" + String(zoneNumber),
    "/api/manual-stop?zone=" + String(zoneNumber),
    "/api/relay?zone=" + String(zoneNumber) + "&state=0"
  };

  String usedPath, err;
  int code = 0;
  bool ok = relayGetFirstOk(paths, 4, usedPath, err, code);

  lastCommandText = ok ? "Stopped Zone " + String(zoneNumber)
                       : "Command Failed stop Z" + String(zoneNumber) + " " + err + (code > 0 ? " HTTP " + String(code) : "");
  clearCommandPending();
  loadRelayState(true);
  return ok;
}

bool sendStopSpigots() {
  showCommandPending("Stopping Spigots");

  String paths[] = {
    "/api/spigots-run?action=off",
    "/api/spigots-run?minutes=0&action=off",
    "/api/spigot-stop",
    "/api/stop-spigots"
  };

  String usedPath, err;
  int code = 0;
  bool ok = relayGetFirstOk(paths, 4, usedPath, err, code);

  lastCommandText = ok ? "Stopped Spigots"
                       : "Command Failed stop spigots " + err + (code > 0 ? " HTTP " + String(code) : "");
  clearCommandPending();
  loadRelayState(true);
  return ok;
}

bool sendAllOff() {
  showCommandPending("Stopping All");

  String body, err;
  int code = 0;
  bool zonesOk = relayGetRaw("/api/alloff", body, code, err);

  // Relay v25 alloff stops zone runs. Stop spigots/master explicitly as well.
  String body2, err2;
  int code2 = 0;
  bool spigotOk = relayGetRaw("/api/spigots-run?action=off", body2, code2, err2);

  bool ok = zonesOk || spigotOk;
  if (zonesOk && spigotOk) {
    lastCommandText = "Stopped All";
  } else if (zonesOk && !spigotOk) {
    lastCommandText = "Zones stopped";
  } else if (!zonesOk && spigotOk) {
    lastCommandText = "Spigots stopped";
  } else {
    lastCommandText = "Command Failed stop all " + err + (code > 0 ? " HTTP " + String(code) : "");
  }

  clearCommandPending();
  loadRelayState(true);
  return ok;
}

bool sendScheduleSlot(int zoneNumber, int hour, int minute, int durationMinutes, bool enabled) {
  DynamicJsonDocument doc(512);
  JsonObject slot = doc.createNestedObject("schedule");
  slot["zone"] = zoneNumber;
  slot["zoneNumber"] = zoneNumber;
  slot["startHour"] = hour;
  slot["startMinute"] = minute;
  slot["runMinutes"] = durationMinutes;
  slot["durationMinutes"] = durationMinutes;
  slot["enabled"] = enabled;
  String payload;
  serializeJson(doc, payload);

  showCommandPending("Sending...");
  String body, err;
  int code;
  bool ok = relayPostJson("/api/schedules", payload, body, code, err);
  if (!ok) {
    // Fallback for relay builds that only provide simple GET schedule add.
    String path = "/api/schedule/add?zone=" + String(zoneNumber) +
                  "&hour=" + String(hour) +
                  "&minute=" + String(minute) +
                  "&minutes=" + String(durationMinutes);
    ok = relayGetRaw(path, body, code, err);
  }
  lastCommandText = ok ? "Saved" : "Command Failed schedule save " + err + (code > 0 ? " HTTP " + String(code) : "");
  clearCommandPending();
  loadRelayState(true);
  return ok;
}

bool deleteScheduleSlot(int zoneNumber, int slotIndex) {
  showCommandPending("Sending...");
  String body, err;
  int code;
  String path = "/api/schedule/delete?zone=" + String(zoneNumber) + "&slot=" + String(slotIndex);
  bool ok = relayGetRaw(path, body, code, err);
  lastCommandText = ok ? "Deleted" : "Command Failed /api/schedule/delete " + err + (code > 0 ? " HTTP " + String(code) : "");
  clearCommandPending();
  loadRelayState(true);
  return ok;
}

// ----------------------------- Web UI ---------------------------------------

String htmlEsc(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  return s;
}

String adminPage() {
  String staStatus;
  if (cfg.staSsid.length() == 0) staStatus = "station Wi-Fi not configured";
  else if (WiFi.status() == WL_CONNECTED) staStatus = "connected to " + WiFi.SSID() + " / " + WiFi.localIP().toString();
  else staStatus = "station Wi-Fi failed or reconnecting";

  String relay = relayConnected ? "Connected" : ("Disconnected: " + relayError);
  String checked = cfg.emergencyDirectStop ? "checked" : "";

  String page = R"HTML(
<!doctype html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>GardenKnob Admin</title>
<style>
body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:#10130f;color:#f5fff2}
main{max-width:720px;margin:0 auto;padding:18px}
.card{background:#1d241b;border:1px solid #43503e;border-radius:18px;padding:16px;margin:14px 0;box-shadow:0 8px 30px #0008}
h1{font-size:26px;margin:4px 0 8px} h2{font-size:18px;margin:6px 0 12px}
label{display:block;margin:10px 0 4px;color:#cfe8c8}
input{width:100%;box-sizing:border-box;font-size:16px;padding:12px;border-radius:12px;border:1px solid #607058;background:#0b0f09;color:#fff}
input[type=checkbox]{width:auto;transform:scale(1.4);margin-right:8px}
button,a.btn{display:inline-block;margin:8px 8px 0 0;padding:12px 14px;border-radius:12px;border:0;background:#bfe86c;color:#10130f;font-weight:800;text-decoration:none}
button.warn,a.warn{background:#ffcf4a}.danger{background:#ff5c57;color:#fff}
.status{font-family:ui-monospace,Consolas,monospace;background:#0b0f09;padding:10px;border-radius:10px;white-space:pre-wrap}
.small{color:#aec7a8;font-size:13px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
@media(max-width:560px){.grid{grid-template-columns:1fr}}
</style></head><body><main>
<h1>GardenKnob Admin</h1>
<div class="small">Firmware )HTML" + String(FW_VERSION) + R"HTML(</div>
<div class="card"><h2>Status</h2><div class="status">AP: )HTML" + htmlEsc(WiFi.softAPSSID()) + " / " + WiFi.softAPIP().toString() + "\nSTA: " + htmlEsc(staStatus) + "\nRelay: " + htmlEsc(relay) + "\nDevice: " + htmlEsc(cfg.deviceName) + R"HTML(</div>
<a class="btn" href="/test-relay">Test Relay</a>
<a class="btn" href="/relay-state">View Relay State</a>
<div class="small" style="margin-top:10px">Device UI: Home is an action carousel. Dial/swipe to Schedule, then press/tap center.</div>
</div>
<div class="card"><h2>Configuration</h2>
<form method="post" action="/save" enctype="application/x-www-form-urlencoded" autocomplete="on">
<label>Device name</label><input name="deviceName" value=")HTML" + htmlEsc(cfg.deviceName) + R"HTML(">
<div class="grid"><div><label>AP SSID</label><input name="apSsid" value=")HTML" + htmlEsc(cfg.apSsid) + R"HTML("></div>
<div><label>AP password</label><input name="apPass" value=")HTML" + htmlEsc(cfg.apPass) + R"HTML("></div></div>
<label>Station SSID</label><input name="staSsid" value=")HTML" + htmlEsc(cfg.staSsid) + R"HTML(">
<label>Station password</label><input name="staPass" type="password" value=")HTML" + htmlEsc(cfg.staPass) + R"HTML(">
<label>Relay base URL</label><input name="relayUrl" value=")HTML" + htmlEsc(cfg.relayBaseUrl) + R"HTML(">
<label>Relay API token</label><input name="relayToken" value=")HTML" + htmlEsc(cfg.relayToken) + R"HTML(">
<div class="grid"><div><label>Brightness 0-255</label><input name="bright" type="number" min="20" max="255" value=")HTML" + String(cfg.brightness) + R"HTML("></div>
<div><label>Default manual run minutes</label><input name="defMin" type="number" min="1" max="240" value=")HTML" + String(cfg.manualDefaultMinutes) + R"HTML("></div></div>
<label><input name="emStop" type="checkbox" )HTML" + checked + R"HTML(> Very long encoder press immediately sends all-off</label>
<button type="submit">Save Settings</button>
<div class="small">Saving returns a confirmation page first. AP changes restart after the response is sent.</div>
</form></div>
<div class="card"><h2>Maintenance</h2>
<a class="btn warn" href="/restart">Restart</a>
<a class="btn danger" href="/factory-reset" onclick="return confirm('Factory reset GardenKnob settings?')">Factory Reset</a>
</div>
</main></body></html>
)HTML";
  return page;
}

String portalBaseUrl() {
  return String("http://") + WiFi.softAPIP().toString() + "/";
}

void sendCaptiveRedirect() {
  server.sendHeader("Location", portalBaseUrl(), true);
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.send(302, "text/plain", "");
}

void sendCaptiveHtml() {
  String page = "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<meta http-equiv='refresh' content='0;url=" + portalBaseUrl() + "'>";
  page += "<title>GardenKnob Setup</title></head><body>";
  page += "<h1>GardenKnob Setup</h1><p><a href='" + portalBaseUrl() + "'>Open setup portal</a></p>";
  page += "</body></html>";
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.send(200, "text/html", page);
}

void sendCaptiveText() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.send(200, "text/plain", "GardenKnob setup portal: " + portalBaseUrl());
}

void sendRedirectHome() {
  sendCaptiveRedirect();
}

void handleSave() {
  String oldApSsid = cfg.apSsid;
  String oldApPass = cfg.apPass;
  String oldStaSsid = cfg.staSsid;
  String oldStaPass = cfg.staPass;

  if (server.hasArg("deviceName")) cfg.deviceName = server.arg("deviceName");
  if (server.hasArg("apSsid")) cfg.apSsid = server.arg("apSsid");
  if (server.hasArg("apPass")) cfg.apPass = server.arg("apPass");
  if (server.hasArg("staSsid")) cfg.staSsid = server.arg("staSsid");
  if (server.hasArg("staPass")) cfg.staPass = server.arg("staPass");
  if (server.hasArg("relayUrl")) cfg.relayBaseUrl = server.arg("relayUrl");
  if (server.hasArg("relayToken")) cfg.relayToken = server.arg("relayToken");
  if (server.hasArg("bright")) cfg.brightness = constrain(server.arg("bright").toInt(), 20, 255);
  if (server.hasArg("defMin")) cfg.manualDefaultMinutes = constrain(server.arg("defMin").toInt(), 1, 240);
  cfg.emergencyDirectStop = server.hasArg("emStop");

  if (cfg.apSsid.length() == 0) cfg.apSsid = "GardenKnob";
  if (cfg.apPass.length() > 0 && cfg.apPass.length() < 8) cfg.apPass = "gardenknob";

  saveConfig();
  analogWrite(TFT_BLK, cfg.brightness);

  bool apChanged = (cfg.apSsid != oldApSsid) || (cfg.apPass != oldApPass);
  bool staChanged = (cfg.staSsid != oldStaSsid) || (cfg.staPass != oldStaPass);

  configSavedNotice = true;
  configSavedNoticeUntil = millis() + 6000;
  relayStatusText = "Settings saved";
  relayError = "Settings saved";

  String page = "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<title>GardenKnob Saved</title>";
  page += "<style>body{font-family:system-ui;background:#10130f;color:#f5fff2;margin:0;padding:20px}";
  page += ".card{max-width:680px;margin:0 auto;background:#1d241b;border:1px solid #43503e;border-radius:18px;padding:18px}";
  page += "a,button{display:inline-block;margin:8px 8px 0 0;padding:12px 14px;border-radius:12px;background:#bfe86c;color:#10130f;font-weight:800;text-decoration:none;border:0}";
  page += ".warn{background:#ffcf4a}.mono{font-family:ui-monospace,Consolas,monospace;white-space:pre-wrap;background:#0b0f09;padding:10px;border-radius:10px}</style></head><body><div class='card'>";
  page += "<h1>Settings saved</h1>";
  page += "<p>The settings were written to flash. The display should show <b>Settings saved</b>.</p>";
  page += "<div class='mono'>";
  page += "AP SSID: " + htmlEsc(cfg.apSsid) + "\\n";
  page += "AP IP: " + WiFi.softAPIP().toString() + "\\n";
  page += "Station SSID: " + htmlEsc(cfg.staSsid.length() ? cfg.staSsid : "(blank)") + "\\n";
  page += "Relay URL: " + htmlEsc(cfg.relayBaseUrl) + "\\n";
  page += "</div>";
  if (apChanged) {
    page += "<p><b>AP settings changed.</b> The device will restart in about 1 second. Reconnect to the new AP if your browser disconnects.</p>";
  } else if (staChanged) {
    page += "<p><b>Station Wi-Fi changed.</b> The device will reconnect after this page finishes loading. The setup AP stays available.</p>";
  } else {
    page += "<p>No AP restart required. Relay settings are active immediately.</p>";
  }
  page += "<a href='/'>Back to Admin</a><a class='warn' href='/test-relay'>Test Relay</a>";
  page += "</div></body></html>";

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Connection", "close");
  server.send(200, "text/html", page);
  delay(30); // let the TCP response leave before reconnect/restart is scheduled

  if (apChanged) {
    restartPending = true;
    restartAtMs = millis() + 1200;
  } else if (staChanged) {
    staReconnectPending = true;
    staReconnectAtMs = millis() + 1200;
  }
}

void handleRelayStatePage() {
  String body, err;
  int code;
  bool ok = relayGetRaw("/api/state", body, code, err);
  if (!ok) ok = relayGetRaw("/status", body, code, err);
  String out = "<meta name=viewport content='width=device-width,initial-scale=1'><pre style='white-space:pre-wrap;background:#111;color:#eee;padding:1rem;border-radius:1rem'>";
  out += ok ? htmlEsc(body) : htmlEsc(err + " HTTP " + String(code));
  out += "</pre><p><a href='/'>Back</a></p>";
  server.send(200, "text/html", out);
}

void setupWebServer() {
  server.on("/", HTTP_GET, [](){ server.send(200, "text/html", adminPage()); });
  server.on("/save", HTTP_POST, handleSave);
  server.on("/save", HTTP_GET, [](){ sendCaptiveRedirect(); });
  server.on("/test-relay", HTTP_GET, [](){
    bool ok = pingRelay();
    server.send(200, "text/html", String("<meta name=viewport content='width=device-width,initial-scale=1'><p>Relay test: ") +
      (ok ? "OK" : htmlEsc(relayError)) + "</p><p><a href='/'>Back</a></p>");
  });
  server.on("/relay-state", HTTP_GET, handleRelayStatePage);
  server.on("/portal-status", HTTP_GET, [](){
    DynamicJsonDocument doc(512);
    doc["apSsid"] = WiFi.softAPSSID();
    doc["apIp"] = WiFi.softAPIP().toString();
    doc["portalUrl"] = portalBaseUrl();
    doc["staConnected"] = WiFi.status() == WL_CONNECTED;
    doc["staIp"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
    String out;
    serializeJson(doc, out);
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", out);
  });
  server.on("/restart", HTTP_GET, [](){
    server.send(200, "text/html", "<p>Restarting...</p>");
    delay(300);
    ESP.restart();
  });
  server.on("/factory-reset", HTTP_GET, [](){
    server.send(200, "text/html", "<p>Factory reset...</p>");
    delay(300);
    factoryReset();
  });

  // Captive portal probe routes.
  // Android expects /generate_204 or /gen_204 to return 204 only when internet is available.
  // Returning a redirect forces the captive portal UI to open.
  server.on("/generate_204", HTTP_GET, sendCaptiveRedirect);
  server.on("/gen_204", HTTP_GET, sendCaptiveRedirect);

  // Apple CNA probes expect "Success" only when internet is available.
  // Returning HTML/redirect opens the portal sheet.
  server.on("/hotspot-detect.html", HTTP_GET, sendCaptiveHtml);
  server.on("/library/test/success.html", HTTP_GET, sendCaptiveHtml);
  server.on("/success.txt", HTTP_GET, sendCaptiveText);

  // Windows NCSI/connectivity probes.
  server.on("/connecttest.txt", HTTP_GET, sendCaptiveRedirect);
  server.on("/ncsi.txt", HTTP_GET, sendCaptiveRedirect);
  server.on("/fwlink", HTTP_GET, sendCaptiveRedirect);

  // Extra common captive routes used by phones/browsers/routers.
  server.on("/canonical.html", HTTP_GET, sendCaptiveHtml);
  server.on("/mobile/status.php", HTTP_GET, sendCaptiveRedirect);
  server.on("/redirect", HTTP_GET, sendCaptiveRedirect);
  server.on("/wpad.dat", HTTP_GET, sendCaptiveText);

  server.onNotFound(sendCaptiveRedirect);
  server.begin();
}

// ----------------------------- Wi-Fi ----------------------------------------

void setupWiFi() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(apIP, apGateway, apSubnet);

  String apSsid = cfg.apSsid.length() ? cfg.apSsid : "GardenKnob";
  String apPass = cfg.apPass.length() >= 8 ? cfg.apPass : "gardenknob";
  WiFi.softAP(apSsid.c_str(), apPass.c_str());

  dnsServer.start(53, "*", apIP);

  if (cfg.staSsid.length() > 0) {
    WiFi.begin(cfg.staSsid.c_str(), cfg.staPass.c_str());
  }
}

// ----------------------------- UI State -------------------------------------



ScreenId screen = SCR_HOME;
ScreenId previousScreen = SCR_HOME;
int selectedItem = 0; // Zone select: 0..4 zones, 5 spigot, 6 schedule
int selectedZone = 1;
DurationTarget durationTarget = DUR_ZONE;
int durationMinutes = cfg.manualDefaultMinutes;

int scheduleZone = 1;
int scheduleSlot = 0;     // existing slot index, 0-based; count position means Add Slot
bool scheduleAddMode = false;
int editHour = 6;
int editMinute = 0;
int editDuration = cfg.manualDefaultMinutes;
int editField = 0; // 0 hour, 1 minute, 2 duration, 3 save, 4 cancel, 5 delete

bool needsRedraw = true;
uint32_t lastDrawMs = 0;
uint32_t lastInteractionMs = 0;
uint32_t runningCycleMs = 0;
int runningDisplayIndex = 0;

// ----------------------------- Encoder Input --------------------------------

volatile int encoderDelta = 0;
volatile uint32_t lastEncoderIsrUs = 0;
volatile bool buttonEdge = false;
volatile bool buttonLevel = true;

// The physical rotary encoder reports multiple quadrature edges per tactile click.
// UI code consumes normalized detents so one physical click = one UI action.
// If a different encoder revision reports 4 edges per click, change this to 4.
static const int ENCODER_EDGES_PER_DETENT = 2;
int encoderDetentRemainder = 0;

bool buttonPressed = false;
uint32_t buttonDownMs = 0;
bool longPressSent = false;
bool emergencyPressSent = false;

void IRAM_ATTR encoderISR() {
  uint32_t now = micros();
  if (now - lastEncoderIsrUs < 800) return;
  lastEncoderIsrUs = now;
  int a = digitalRead(ENCODER_A_PIN);
  int b = digitalRead(ENCODER_B_PIN);
  if (a == b) encoderDelta++;
  else encoderDelta--;
}

void IRAM_ATTR buttonISR() {
  buttonLevel = digitalRead(ENCODER_BTN);
  buttonEdge = true;
}

void initEncoder() {
  pinMode(ENCODER_A_PIN, INPUT_PULLUP);
  pinMode(ENCODER_B_PIN, INPUT_PULLUP);
  pinMode(ENCODER_BTN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENCODER_A_PIN), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_BTN), buttonISR, CHANGE);
}

int takeEncoderDelta() {
  noInterrupts();
  int raw = encoderDelta;
  encoderDelta = 0;
  interrupts();

  encoderDetentRemainder += raw;

  int detents = 0;
  while (encoderDetentRemainder >= ENCODER_EDGES_PER_DETENT) {
    detents++;
    encoderDetentRemainder -= ENCODER_EDGES_PER_DETENT;
  }
  while (encoderDetentRemainder <= -ENCODER_EDGES_PER_DETENT) {
    detents--;
    encoderDetentRemainder += ENCODER_EDGES_PER_DETENT;
  }

  return detents;
}

// ----------------------------- Touch Input ----------------------------------



bool i2cReadRegs(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  uint8_t got = Wire.requestFrom(addr, len);
  if (got != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

TouchPoint readTouch() {
  TouchPoint tp;
  uint8_t buf[6];
  // CST816D commonly exposes touch count at 0x02 and x/y at 0x03..0x06.
  if (!i2cReadRegs(TOUCH_ADDR, 0x02, buf, 5)) return tp;
  uint8_t fingers = buf[0] & 0x0F;
  if (fingers == 0) return tp;
  int x = ((buf[1] & 0x0F) << 8) | buf[2];
  int y = ((buf[3] & 0x0F) << 8) | buf[4];

  // Rotate/remap here if your shipped panel reports swapped coordinates.
  tp.touched = true;
  tp.x = constrain(x, 0, 239);
  tp.y = constrain(y, 0, 239);
  return tp;
}

TouchPoint lastTouch;
TouchPoint touchStart;
uint32_t lastTouchMs = 0;
bool touchWasDown = false;

// Encoder button presses can physically deflect the touch panel.
// Any button press cancels the current touch and suppresses touch release/tap
// events for a short window so one physical press cannot become both a
// button press and a touchscreen tap.
uint32_t suppressTouchUntilMs = 0;
static const uint32_t TOUCH_SUPPRESS_AFTER_BUTTON_MS = 450;

void initTouch() {
  pinMode(TOUCH_INT, INPUT_PULLUP);
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);
  delay(5);
  digitalWrite(TOUCH_RST, HIGH);
  delay(50);
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
}

// ----------------------------- Drawing Helpers ------------------------------

void drawCenteredText(const String &txt, int y, int size, uint16_t color = C_TEXT) {
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds((char*)txt.c_str(), 0, y, &x1, &y1, &w, &h);
  gfx->setCursor(CX - w / 2, y);
  gfx->print(txt);
}

void drawSmallCentered(const String &txt, int y, uint16_t color = C_DIM) {
  drawCenteredText(txt, y, 1, color);
}

void drawRing(int radius, uint16_t color) {
  gfx->drawCircle(CX, CY, radius, color);
  gfx->drawCircle(CX, CY, radius - 1, color);
}

void fillCircularBackground() {
  gfx->fillScreen(C_BG);
  gfx->fillCircle(CX, CY, 119, C_BG);
  gfx->fillCircle(CX, CY, 108, C_BG);
}

uint16_t rgb565From888(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

uint16_t zoneUiColor(int zoneNumber) {
  if (zoneNumber >= 1 && zoneNumber <= 5) {
    ZoneRgb &c = relayZoneColors[zoneNumber - 1];
    return rgb565From888(c.r, c.g, c.b);
  }
  return C_DIM;
}

uint16_t dimColor(uint16_t c) {
  // Cheap RGB565 dimming: keep hue recognizable, reduce intensity.
  uint16_t r = (c >> 11) & 0x1F;
  uint16_t g = (c >> 5) & 0x3F;
  uint16_t b = c & 0x1F;
  r = r / 3;
  g = g / 3;
  b = b / 3;
  return (r << 11) | (g << 5) | b;
}

void drawConnectionDot() {
  uint16_t c = commandPending ? C_AMBER : (relayConnected ? C_GREEN : C_RED);
  gfx->fillCircle(120, 14, 4, c);
  gfx->drawCircle(120, 14, 7, C_PANEL);
}

void drawTopPipRow(int activeIndex, int total, uint16_t color) {
  if (total < 1) return;
  int spacing = 10;
  int start = CX - ((total - 1) * spacing) / 2;
  for (int i = 0; i < total; i++) {
    gfx->fillCircle(start + i * spacing, 24, i == activeIndex ? 3 : 2, i == activeIndex ? color : C_DIM);
  }
}

void drawFineOuterBezel(uint16_t accent = C_DIM) {
  gfx->drawCircle(CX, CY, 119, C_RING_DARK);
  gfx->drawCircle(CX, CY, 118, C_PANEL);
  gfx->drawCircle(CX, CY, 110, C_RING_DARK);
  for (int i = 0; i < 60; i++) {
    float a = (i * 6 - 90) * PI / 180.0f;
    int len = (i % 5 == 0) ? 7 : 3;
    uint16_t c = (i % 5 == 0) ? accent : C_PANEL;
    int x1 = CX + cosf(a) * (116 - len);
    int y1 = CY + sinf(a) * (116 - len);
    int x2 = CX + cosf(a) * 116;
    int y2 = CY + sinf(a) * 116;
    gfx->drawLine(x1, y1, x2, y2, c);
  }
}

void drawArcSegment(float startDeg, float endDeg, int rOuter, int rInner, uint16_t color) {
  if (endDeg < startDeg) {
    float t = startDeg;
    startDeg = endDeg;
    endDeg = t;
  }
  for (float d = startDeg; d <= endDeg; d += 2.0f) {
    float a = d * PI / 180.0f;
    int x1 = CX + cosf(a) * rInner;
    int y1 = CY + sinf(a) * rInner;
    int x2 = CX + cosf(a) * rOuter;
    int y2 = CY + sinf(a) * rOuter;
    gfx->drawLine(x1, y1, x2, y2, color);
  }
}

void drawMenuArc(int selected, int total, uint16_t accent) {
  if (total <= 0) return;
  drawFineOuterBezel(dimColor(accent));
  float slice = 300.0f / total;
  float start = -240.0f + selected * slice + 4.0f;
  float end = start + slice - 8.0f;
  drawArcSegment(start, end, 116, 108, accent);
  for (int i = 0; i < total; i++) {
    float mid = (-240.0f + i * slice + slice * 0.5f) * PI / 180.0f;
    int x = CX + cosf(mid) * 101;
    int y = CY + sinf(mid) * 101;
    gfx->fillCircle(x, y, i == selected ? 3 : 2, i == selected ? accent : C_PANEL);
  }
}

void drawSoftGlowCircle(int x, int y, int r, uint16_t color) {
  gfx->drawCircle(x, y, r + 4, dimColor(color));
  gfx->drawCircle(x, y, r + 2, dimColor(color));
  gfx->drawCircle(x, y, r, color);
}

uint8_t pulsePhase(uint16_t periodMs = 1200) {
  uint32_t t = millis() % periodMs;
  if (t > periodMs / 2) t = periodMs - t;
  return map(t, 0, periodMs / 2, 0, 255);
}

void drawPulseCircle(int x, int y, int baseR, uint16_t color) {
  int add = map(pulsePhase(), 0, 255, 0, 5);
  gfx->drawCircle(x, y, baseR + add, dimColor(color));
  gfx->drawCircle(x, y, baseR + add + 1, dimColor(color));
  gfx->drawCircle(x, y, baseR, color);
}

void drawSpinner(int cx, int cy, uint16_t color) {
  int active = (millis() / 120) % 12;
  for (int i = 0; i < 12; i++) {
    float a = (i * 30 - 90) * PI / 180.0f;
    int x = cx + cosf(a) * 16;
    int y = cy + sinf(a) * 16;
    uint16_t c = (i == active) ? color : (i == (active + 11) % 12 ? dimColor(color) : C_PANEL);
    gfx->fillCircle(x, y, i == active ? 3 : 2, c);
  }
}

void drawWaterWaveBand(int x, int y, int w, int h, uint16_t color, uint8_t phaseOffset = 0) {
  gfx->fillRoundRect(x, y, w, h, h / 2, C_DEEP_BLUE);
  gfx->drawRoundRect(x, y, w, h, h / 2, dimColor(color));
  int phase = ((millis() / 90) + phaseOffset) % 12;
  for (int xx = x + 4; xx < x + w - 4; xx++) {
    int rel = (xx - x + phase) % 24;
    int yy = y + h / 2 + (rel < 12 ? rel / 4 : (24 - rel) / 4) - 2;
    gfx->drawPixel(xx, yy, color);
    if (yy + 1 < y + h - 2) gfx->drawPixel(xx, yy + 1, dimColor(color));
  }
}

void drawTinyGridDisc(uint16_t accent) {
  gfx->drawCircle(CX, CY, 92, dimColor(accent));
  for (int x = 40; x <= 200; x += 20) {
    int dy = sqrt(max(0, 92 * 92 - (x - CX) * (x - CX)));
    gfx->drawLine(x, CY - dy, x, CY + dy, C_GRID);
  }
  for (int y = 40; y <= 200; y += 20) {
    int dx = sqrt(max(0, 92 * 92 - (y - CY) * (y - CY)));
    gfx->drawLine(CX - dx, y, CX + dx, y, C_GRID);
  }
}

void drawMetricRow(int y, const String &left, const String &right, uint16_t accent) {
  gfx->drawLine(48, y + 15, 192, y + 15, C_PANEL);
  gfx->setTextSize(1);
  gfx->setTextColor(C_DIM);
  gfx->setCursor(52, y);
  gfx->print(left);
  gfx->setTextColor(accent);
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds((char*)right.c_str(), 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor(188 - w, y);
  gfx->print(right);
}

void drawLargeZoneOrb(int zoneNumber, int x, int y, int r, bool active) {
  uint16_t c = zoneUiColor(zoneNumber);
  drawSoftGlowCircle(x, y, r, c);
  gfx->fillCircle(x, y, r - 3, active ? dimColor(c) : C_BG);
  gfx->drawCircle(x, y, r - 3, c);
  gfx->setTextSize(3);
  gfx->setTextColor(active ? C_TEXT : c);
  String s = String(zoneNumber);
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds((char*)s.c_str(), 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor(x - w / 2, y - h / 2);
  gfx->print(s);
}

void drawOrbitDots(uint16_t accent, int count = 5, int radius = 96) {
  int sweep = (millis() / 90) % 360;
  for (int i = 0; i < count; i++) {
    float a = (sweep + i * (360.0f / count) - 90) * PI / 180.0f;
    int x = CX + cosf(a) * radius;
    int y = CY + sinf(a) * radius;
    gfx->fillCircle(x, y, i == 0 ? 3 : 2, i == 0 ? accent : dimColor(accent));
  }
}

void drawSignalSweep(uint16_t accent) {
  int start = (millis() / 18) % 360;
  drawArcSegment(start - 18, start, 106, 101, dimColor(accent));
  drawArcSegment(start, start + 10, 106, 99, accent);
}

void drawMiniLedStrip(int x, int y, int activeZone = 0) {
  for (int i = 1; i <= 5; i++) {
    uint16_t c = zoneUiColor(i);
    bool on = activeZone == i || zoneIsActive(i);
    gfx->fillRoundRect(x + (i - 1) * 22, y, 16, 8, 4, on ? c : dimColor(c));
    gfx->drawRoundRect(x + (i - 1) * 22, y, 16, 8, 4, c);
  }
}

void drawActiveZoneOrbs(int y, int selectedZoneNumber) {
  int startX = 48;
  for (int i = 1; i <= 5; i++) {
    uint16_t c = zoneUiColor(i);
    bool active = zoneIsActive(i);
    bool selected = i == selectedZoneNumber;
    int x = startX + (i - 1) * 36;
    if (selected) {
      gfx->drawCircle(x, y, 13, C_AMBER);
    }
    gfx->fillCircle(x, y, active ? 10 : 7, active ? c : C_BG);
    gfx->drawCircle(x, y, active ? 10 : 7, c);
    gfx->setTextSize(1);
    gfx->setTextColor(active ? C_BG : c);
    String s = String(i);
    int16_t x1, y1;
    uint16_t w, h;
    gfx->getTextBounds((char*)s.c_str(), 0, 0, &x1, &y1, &w, &h);
    gfx->setCursor(x - w / 2, y - h / 2);
    gfx->print(s);
  }
}

void drawSegmentedMeter(int x, int y, int w, int h, float frac, uint16_t accent) {
  frac = constrain(frac, 0.0f, 1.0f);
  int segments = 12;
  int gap = 2;
  int sw = (w - gap * (segments - 1)) / segments;
  int lit = roundf(frac * segments);
  for (int i = 0; i < segments; i++) {
    uint16_t c = i < lit ? accent : C_PANEL;
    gfx->fillRoundRect(x + i * (sw + gap), y, sw, h, 2, c);
  }
}


void drawHomeConstellation(uint16_t accent) {
  int pts[5][2] = {{78,70},{118,54},{164,76},{146,128},{92,130}};
  for (int i = 0; i < 5; i++) {
    int j = (i + 1) % 5;
    gfx->drawLine(pts[i][0], pts[i][1], pts[j][0], pts[j][1], dimColor(accent));
    gfx->fillCircle(pts[i][0], pts[i][1], 2, accent);
  }
}

void drawReticle(int cx, int cy, int r, uint16_t accent) {
  gfx->drawCircle(cx, cy, r, dimColor(accent));
  gfx->drawCircle(cx, cy, r + 6, C_PANEL);
  gfx->drawLine(cx - r - 8, cy, cx - r + 4, cy, accent);
  gfx->drawLine(cx + r - 4, cy, cx + r + 8, cy, accent);
  gfx->drawLine(cx, cy - r - 8, cx, cy - r + 4, accent);
  gfx->drawLine(cx, cy + r - 4, cx, cy + r + 8, accent);
  gfx->fillCircle(cx, cy, 2, accent);
}

void drawHexBadge(int cx, int cy, int r, const String &label, uint16_t accent, uint16_t textColor = C_TEXT) {
  int px[6];
  int py[6];
  for (int i = 0; i < 6; i++) {
    float a = (60 * i - 30) * PI / 180.0f;
    px[i] = cx + cosf(a) * r;
    py[i] = cy + sinf(a) * r;
  }
  for (int i = 0; i < 6; i++) {
    int j = (i + 1) % 6;
    gfx->drawLine(px[i], py[i], px[j], py[j], accent);
    gfx->drawLine(px[i], py[i] + 1, px[j], py[j] + 1, dimColor(accent));
  }
  gfx->setTextSize(1);
  gfx->setTextColor(textColor);
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds((char*)label.c_str(), 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor(cx - w / 2, cy - h / 2);
  gfx->print(label);
}


String homeMenuLabel(int idx) {
  switch ((idx + HOME_MENU_COUNT) % HOME_MENU_COUNT) {
    case 0: return "Run Zones";
    case 1: return "Schedule";
    case 2: return "Spigots";
    case 3: return "Status";
    case 4: return "Stop All";
  }
  return "Run Zone";
}

uint16_t homeMenuAccent(int idx) {
  switch ((idx + HOME_MENU_COUNT) % HOME_MENU_COUNT) {
    case 0: return zoneUiColor(selectedZone);
    case 1: return C_ZONE3;
    case 2: return C_CYAN;
    case 3: return relayConnected ? C_GREEN : C_RED;
    case 4: return C_RED;
  }
  return C_DIM;
}

void drawMenuSideLabel(const String &label, int x, int y, uint16_t color) {
  gfx->setTextSize(1);
  gfx->setTextColor(color);
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds((char*)label.c_str(), 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor(x - w / 2, y);
  gfx->print(label);
}

void drawHomeActionIcon(int idx, uint16_t accent) {
  switch ((idx + HOME_MENU_COUNT) % HOME_MENU_COUNT) {
    case 0:
      drawDropletIcon(120, 78, accent);
      drawLargeZoneOrb(selectedZone, 120, 118, 25, zoneIsActive(selectedZone));
      break;
    case 1:
      drawClockIcon(120, 88, accent);
      drawHexBadge(120, 126, 22, "CAL", accent);
      break;
    case 2:
      drawFaucetIcon(120, 92, accent);
      drawWaterWaveBand(70, 124, 100, 16, accent);
      break;
    case 3:
      drawWifiIcon(120, 78, relayConnected);
      drawHexBadge(120, 126, 22, relayConnected ? "OK" : "ERR", accent);
      break;
    case 4:
      drawWarningTriangle(120, 86, 22, accent);
      drawHexBadge(120, 130, 24, "STOP", accent);
      break;
  }
}

void drawHomeMenuCarousel() {
  uint16_t accent = homeMenuAccent(homeMenuIndex);
  String title = homeMenuLabel(homeMenuIndex);
  String subtitle = (isAnyZoneRunning() || spigotRun.active) ? "Watering Active" : "Garden Controller";

  drawHomeHeader(title, subtitle, homeMenuIndex, accent);
  drawMainCarouselIcon(homeMenuIndex, CX, 118, accent);

  String body = "Rotate to choose";
  String body2 = "Press to open";
  switch ((homeMenuIndex + HOME_MENU_COUNT) % HOME_MENU_COUNT) {
    case 0:
      body = "Run a zone manually";
      body2 = zoneIsActive(selectedZone) ? ("Zone " + String(selectedZone) + " is active") : ("Zone " + String(selectedZone) + " selected");
      break;
    case 1:
      body = "Edit watering schedules";
      body2 = "Choose a zone";
      break;
    case 2:
      body = "Run manual spigots";
      body2 = spigotRun.active ? "Spigots are active" : "Start manual water";
      break;
    case 3:
      body = relayConnected ? "Relay connected" : "Relay disconnected";
      body2 = (isAnyZoneRunning() || spigotRun.active) ? "View active watering" : "View device status";
      break;
    case 4:
      body = (isAnyZoneRunning() || spigotRun.active) ? "Stop all water now" : "Nothing is running";
      body2 = (isAnyZoneRunning() || spigotRun.active) ? "Immediate stop" : "Water is off";
      break;
  }

  drawCenteredText(body, 165, 2, C_TEXT);
  drawSmallCentered(body2, 188, C_SOFT_TEXT);
  drawHomeCarouselEdgeIndicator(homeMenuIndex, HOME_MENU_COUNT, accent);
}

void activateHomeMenuAction() {
  switch ((homeMenuIndex + HOME_MENU_COUNT) % HOME_MENU_COUNT) {
    case 0:
      selectedItem = selectedZone - 1;
      go(SCR_ZONE_SELECT);
      break;
    case 1:
      scheduleZone = selectedZone;
      scheduleSlot = 0;
      scheduleZonePickerMode = true;
      go(SCR_SCHEDULE);
      break;
    case 2:
      selectedItem = 5;
      durationTarget = DUR_SPIGOT;
      durationMinutes = cfg.manualDefaultMinutes;
      go(SCR_DURATION);
      break;
    case 3:
      if (isAnyZoneRunning() || spigotRun.active) go(SCR_RUNNING);
      else go(SCR_DIAGNOSTICS);
      break;
    case 4:
      if (isAnyZoneRunning() || spigotRun.active) {
        sendAllOff();
        go(SCR_HOME);
      } else {
        lastCommandText = "Water is off";
        commandMessageUntil = millis() + 2000;
        needsRedraw = true;
      }
      break;
  }
}



void drawPlainPageDots(int active, int total, uint16_t accent) {
  int spacing = 12;
  int start = CX - ((total - 1) * spacing) / 2;
  for (int i = 0; i < total; i++) {
    gfx->fillCircle(start + i * spacing, 184, i == active ? 4 : 3, i == active ? accent : C_PANEL);
  }
}

void drawPlainTitle(const String &title, const String &subtitle, uint16_t accent) {
  gfx->setTextSize(1);
  gfx->setTextColor(C_SOFT_TEXT);
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds((char*)subtitle.c_str(), 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor(CX - w / 2, 30);
  gfx->print(subtitle);

  gfx->setTextSize(2);
  gfx->setTextColor(accent);
  gfx->getTextBounds((char*)title.c_str(), 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor(CX - w / 2, 52);
  gfx->print(title);
}

void drawPlainCard(int x, int y, int w, int h, uint16_t outline) {
  if (y + h > SAFE_BOTTOM) y = SAFE_BOTTOM - h;
  if (x < SAFE_LEFT) x = SAFE_LEFT;
  if (x + w > SAFE_RIGHT) x = SAFE_RIGHT - w;
  gfx->fillRoundRect(x, y, w, h, 18, C_CARD);
  gfx->drawRoundRect(x, y, w, h, 18, outline);
}

void drawCleanCard(int x, int y, int w, int h, uint16_t fill, uint16_t outline) {
  if (y + h > SAFE_BOTTOM) y = SAFE_BOTTOM - h;
  if (x < SAFE_LEFT) x = SAFE_LEFT;
  if (x + w > SAFE_RIGHT) x = SAFE_RIGHT - w;
  gfx->fillRoundRect(x + 1, y + 2, w, h, 18, C_SHADOW);
  gfx->fillRoundRect(x, y, w, h, 18, fill);
  gfx->drawRoundRect(x, y, w, h, 18, outline);
}

void drawCleanTitle(const String &title, const String &subtitle, uint16_t accent) {
  gfx->setTextSize(1);
  gfx->setTextColor(C_SOFT_TEXT);
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds((char*)subtitle.c_str(), 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor(CX - w / 2, 30);
  gfx->print(subtitle);

  gfx->setTextSize(2);
  gfx->setTextColor(C_TEXT);
  gfx->getTextBounds((char*)title.c_str(), 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor(CX - w / 2, 48);
  gfx->print(title);

  gfx->fillRoundRect(CX - 18, 74, 36, 4, 2, accent);
}

void drawMenuDotsClean(int active, int total, uint16_t accent) {
  int spacing = 12;
  int start = CX - ((total - 1) * spacing) / 2;
  for (int i = 0; i < total; i++) {
    int r = i == active ? 4 : 3;
    gfx->fillCircle(start + i * spacing, 184, r, i == active ? accent : C_PANEL);
  }
}

void drawCleanWaterIcon(int cx, int cy, uint16_t accent) {
  gfx->fillCircle(cx, cy + 10, 18, dimColor(accent));
  gfx->drawCircle(cx, cy + 10, 18, accent);
  gfx->drawLine(cx, cy - 24, cx - 16, cy + 5, accent);
  gfx->drawLine(cx, cy - 24, cx + 16, cy + 5, accent);
  gfx->drawLine(cx - 16, cy + 5, cx - 12, cy + 19, accent);
  gfx->drawLine(cx + 16, cy + 5, cx + 12, cy + 19, accent);
  gfx->fillCircle(cx - 6, cy + 5, 3, C_TEXT);
}

void drawCleanCalendarIcon(int cx, int cy, uint16_t accent) {
  gfx->fillRoundRect(cx - 28, cy - 25, 56, 50, 8, C_BG);
  gfx->drawRoundRect(cx - 28, cy - 25, 56, 50, 8, accent);
  gfx->fillRoundRect(cx - 28, cy - 25, 56, 13, 7, accent);
  gfx->drawLine(cx - 16, cy - 4, cx + 16, cy - 4, C_PANEL);
  gfx->drawLine(cx - 16, cy + 10, cx + 16, cy + 10, C_PANEL);
  gfx->fillCircle(cx - 10, cy + 4, 3, accent);
  gfx->fillCircle(cx + 10, cy + 4, 3, accent);
}

void drawCleanFaucetIcon(int cx, int cy, uint16_t accent) {
  gfx->drawRoundRect(cx - 30, cy - 14, 42, 12, 5, accent);
  gfx->drawLine(cx - 8, cy - 26, cx - 8, cy - 14, accent);
  gfx->drawRoundRect(cx - 20, cy - 30, 24, 8, 4, accent);
  gfx->drawLine(cx + 12, cy - 8, cx + 12, cy + 18, accent);
  gfx->drawLine(cx + 12, cy + 18, cx + 24, cy + 18, accent);
  gfx->drawCircle(cx + 24, cy + 30, 6, accent);
  gfx->drawPixel(cx + 24, cy + 28, C_TEXT);
}

void drawCleanStatusIcon(int cx, int cy, uint16_t accent) {
  gfx->drawCircle(cx, cy, 30, accent);
  gfx->drawCircle(cx, cy, 22, dimColor(accent));
  gfx->fillCircle(cx, cy, relayConnected ? 8 : 6, relayConnected ? C_GREEN : C_RED);
  gfx->drawLine(cx, cy - 18, cx, cy - 8, accent);
  gfx->drawLine(cx + 18, cy, cx + 8, cy, accent);
}

void drawCleanStopIcon(int cx, int cy, uint16_t accent) {
  gfx->fillCircle(cx, cy, 32, dimColor(accent));
  gfx->drawCircle(cx, cy, 32, accent);
  gfx->fillRoundRect(cx - 16, cy - 16, 32, 32, 6, accent);
  gfx->setTextSize(1);
  gfx->setTextColor(C_TEXT);
  gfx->setCursor(cx - 12, cy - 3);
  gfx->print("STOP");
}


const uint16_t * homeMenuIconData(int idx) {
  switch ((idx + HOME_MENU_COUNT) % HOME_MENU_COUNT) {
    case 0: return ICON_RUN_24;
    case 1: return ICON_SCHED_24;
    case 2: return ICON_SPIGOT_24;
    case 3: return ICON_STATUS_24;
    case 4: return ICON_STOP_24;
  }
  return ICON_RUN_24;
}

void drawSprite565(int x, int y, int w, int h, const uint16_t *bmp, uint16_t transparent) {
  for (int yy = 0; yy < h; yy++) {
    for (int xx = 0; xx < w; xx++) {
      uint16_t c = pgm_read_word(&bmp[yy * w + xx]);
      if (c != transparent) gfx->drawPixel(x + xx, y + yy, c);
    }
  }
}

void drawSprite565Scaled(int x, int y, int w, int h, const uint16_t *bmp, int scale, uint16_t transparent) {
  for (int yy = 0; yy < h; yy++) {
    for (int xx = 0; xx < w; xx++) {
      uint16_t c = pgm_read_word(&bmp[yy * w + xx]);
      if (c == transparent) continue;
      if (scale <= 1) gfx->drawPixel(x + xx, y + yy, c);
      else gfx->fillRect(x + xx * scale, y + yy * scale, scale, scale, c);
    }
  }
}

void drawHomeCarouselEdgeIndicator(int active, int total, uint16_t accent) {
  if (total <= 0) return;
  float seg = 360.0f / (float)total;
  float gap = 2.0f;
  int outerR = 118;

  for (int band = 0; band < 3; band++) {
    int r = outerR - band;
    for (int deg = 0; deg < 360; deg++) {
      float a = (deg - 90) * PI / 180.0f;
      int px = CX + (int)roundf(cosf(a) * r);
      int py = CY + (int)roundf(sinf(a) * r);
      gfx->drawPixel(px, py, C_PANEL);
    }
  }

  float startDeg = active * seg;
  float endDeg = startDeg + seg;
  for (int band = 0; band < 3; band++) {
    int r = outerR - band;
    for (float deg = startDeg + gap; deg <= endDeg - gap; deg += 0.5f) {
      float a = (deg - 90) * PI / 180.0f;
      int px = CX + (int)roundf(cosf(a) * r);
      int py = CY + (int)roundf(sinf(a) * r);
      gfx->drawPixel(px, py, accent);
    }
  }
}

void drawHomeHeader(const String &title, const String &subtitle, int idx, uint16_t accent) {
  const uint16_t *icon = homeMenuIconData(idx);
  drawSprite565(46, 22, HOME_ICON_W, HOME_ICON_H, icon, SPRITE_TRANSPARENT);

  gfx->setTextSize(1);
  gfx->setTextColor(C_SOFT_TEXT);
  gfx->setCursor(78, 28);
  gfx->print(subtitle);

  gfx->setTextSize(2);
  gfx->setTextColor(accent);
  gfx->setCursor(78, 42);
  gfx->print(title);
}

void drawMainCarouselIcon(int idx, int cx, int cy, uint16_t accent) {
  const uint16_t *icon = homeMenuIconData(idx);
  drawSprite565Scaled(cx - (HOME_ICON_W), cy - (HOME_ICON_H), HOME_ICON_W, HOME_ICON_H, icon, 2);
}

void drawRadialValue(const String &mainText, const String &subText, uint16_t accent) {
  drawReticle(CX, CY, 58, accent);
  drawCenteredText(mainText, 94, 3, C_TEXT);
  drawCenteredText(subText, 128, 1, accent);
}

void drawToastOverlay(const String &msg, uint16_t accent) {
  if (!msg.length()) return;
  drawCleanCard(34, 82, 172, 56, C_CARD, accent);
  drawCenteredText(msg.substring(0, 18), 100, 2, accent);
}

void drawZoneScanLines(uint16_t accent) {
  int phase = (millis() / 80) % 16;
  for (int y = 42 + phase; y < 198; y += 16) {
    int dx = sqrt(max(0, 92 * 92 - (y - CY) * (y - CY)));
    gfx->drawLine(CX - dx, y, CX + dx, y, dimColor(accent));
  }
}

void drawScheduleTimeline(int x, int y, int w, int h, ZoneSchedule &zs, uint16_t accent) {
  gfx->drawRoundRect(x, y, w, h, 8, dimColor(accent));
  gfx->drawLine(x + 8, y + h / 2, x + w - 8, y + h / 2, C_PANEL);
  if (!zs.loaded || zs.count == 0) return;
  for (int i = 0; i < zs.count && i < 6; i++) {
    ScheduleItem &s = zs.slots[i];
    int minutes = s.startHour * 60 + s.startMinute;
    int px = x + 8 + (minutes * (w - 16)) / 1440;
    uint16_t c = i == scheduleSlot ? C_AMBER : accent;
    gfx->fillCircle(px, y + h / 2, i == scheduleSlot ? 4 : 3, c);
  }
}

void drawHeaderChip(const String &label, uint16_t accent) {
  gfx->fillRoundRect(54, 20, 132, 22, 11, C_BG);
  gfx->drawRoundRect(54, 20, 132, 22, 11, dimColor(accent));
  gfx->setTextSize(1);
  gfx->setTextColor(accent);
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds((char*)label.c_str(), 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor(CX - w / 2, 27);
  gfx->print(label);
}

void drawStatusRibbon(const String &label, uint16_t accent) {
  gfx->fillRoundRect(34, 182, 172, 18, 8, C_BG);
  gfx->drawRoundRect(34, 182, 172, 18, 8, dimColor(accent));
  gfx->setTextSize(1);
  gfx->setTextColor(accent);
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds((char*)label.c_str(), 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor(CX - w / 2, 188);
  gfx->print(label);
}

void drawDropletIcon(int cx, int cy, uint16_t color) {
  gfx->fillCircle(cx, cy + 5, 9, dimColor(color));
  gfx->drawCircle(cx, cy + 5, 9, color);
  gfx->drawLine(cx, cy - 13, cx - 8, cy + 1, color);
  gfx->drawLine(cx, cy - 13, cx + 8, cy + 1, color);
  gfx->drawLine(cx - 8, cy + 1, cx - 9, cy + 5, color);
  gfx->drawLine(cx + 8, cy + 1, cx + 9, cy + 5, color);
}

void drawClockIcon(int cx, int cy, uint16_t color) {
  gfx->drawCircle(cx, cy, 12, color);
  gfx->drawCircle(cx, cy, 11, dimColor(color));
  gfx->drawLine(cx, cy, cx, cy - 7, color);
  gfx->drawLine(cx, cy, cx + 6, cy + 4, color);
  gfx->fillCircle(cx, cy, 2, color);
}

void drawWifiIcon(int cx, int cy, bool ok) {
  uint16_t c = ok ? C_GREEN : C_RED;
  gfx->fillCircle(cx, cy + 10, 2, c);
  gfx->drawCircle(cx, cy + 9, 8, c);
  gfx->drawCircle(cx, cy + 9, 15, dimColor(c));
  gfx->fillRect(cx - 18, cy + 9, 36, 18, C_BG);
  if (!ok) {
    gfx->drawLine(cx - 11, cy - 2, cx + 11, cy + 18, C_RED);
    gfx->drawLine(cx + 11, cy - 2, cx - 11, cy + 18, C_RED);
  }
}

void drawRoundButton(int x, int y, int w, int h, const String &label, uint16_t fill, uint16_t outline, uint16_t textColor = C_TEXT) {
  if (y + h > SAFE_BOTTOM) y = SAFE_BOTTOM - h;
  if (x < SAFE_LEFT) x = SAFE_LEFT;
  if (x + w > SAFE_RIGHT) x = SAFE_RIGHT - w;
  gfx->fillRoundRect(x + 2, y + 2, w, h, h / 2, C_SHADOW);
  gfx->fillRoundRect(x, y, w, h, h / 2, fill);
  gfx->drawRoundRect(x, y, w, h, h / 2, outline);
  gfx->setTextSize(1);
  gfx->setTextColor(textColor);
  int16_t x1, y1;
  uint16_t tw, th;
  gfx->getTextBounds((char*)label.c_str(), 0, 0, &x1, &y1, &tw, &th);
  gfx->setCursor(x + (w - tw) / 2, y + (h - th) / 2);
  gfx->print(label);
}

void drawWarningTriangle(int cx, int cy, int r, uint16_t color) {
  int x1 = cx, y1 = cy - r;
  int x2 = cx - r, y2 = cy + r;
  int x3 = cx + r, y3 = cy + r;
  gfx->drawLine(x1, y1, x2, y2, color);
  gfx->drawLine(x2, y2, x3, y3, color);
  gfx->drawLine(x3, y3, x1, y1, color);
  gfx->drawLine(x1, y1 + 2, x2 + 2, y2 - 1, color);
  gfx->drawLine(x2 + 2, y2 - 1, x3 - 2, y3 - 1, color);
  gfx->drawLine(x3 - 2, y3 - 1, x1, y1 + 2, color);
  gfx->setTextSize(2);
  gfx->setTextColor(color);
  gfx->setCursor(cx - 3, cy - 3);
  gfx->print("!");
}

void drawFaucetIcon(int cx, int cy, uint16_t color) {
  gfx->drawLine(cx - 15, cy - 4, cx + 8, cy - 4, color);
  gfx->drawLine(cx - 15, cy - 3, cx + 8, cy - 3, color);
  gfx->drawLine(cx - 5, cy - 12, cx - 5, cy - 4, color);
  gfx->drawLine(cx - 12, cy - 12, cx + 2, cy - 12, color);
  gfx->drawLine(cx + 8, cy - 4, cx + 8, cy + 7, color);
  gfx->drawLine(cx + 7, cy + 7, cx + 12, cy + 7, color);
  gfx->drawCircle(cx + 12, cy + 15, 3, color);
  gfx->drawPixel(cx + 12, cy + 14, color);
}

void drawOuterNavTicks() {
  for (int i = 0; i < 8; i++) {
    float a = (i * 45 - 90) * PI / 180.0f;
    int x1 = CX + cosf(a) * 111;
    int y1 = CY + sinf(a) * 111;
    int x2 = CX + cosf(a) * 116;
    int y2 = CY + sinf(a) * 116;
    gfx->drawLine(x1, y1, x2, y2, C_PANEL);
  }
}

void drawPolyFilled(uint8_t polyIndex, uint16_t fill, uint16_t outline) {
  const Poly &poly = polys[polyIndex];
  int16_t xs[8], ys[8];
  for (int i = 0; i < poly.n; i++) {
    ScreenPt s = toScreen(poly.pts[i]);
    xs[i] = s.x;
    ys[i] = s.y;
  }

  // Simple scanline fill for small convex/mostly convex polygons.
  int minY = 239, maxY = 0;
  for (int i = 0; i < poly.n; i++) {
    minY = min(minY, (int)ys[i]);
    maxY = max(maxY, (int)ys[i]);
  }

  for (int y = minY; y <= maxY; y++) {
    int nodes[10];
    int nodeCount = 0;
    int j = poly.n - 1;
    for (int i = 0; i < poly.n; i++) {
      if ((ys[i] < y && ys[j] >= y) || (ys[j] < y && ys[i] >= y)) {
        nodes[nodeCount++] = xs[i] + (y - ys[i]) * (xs[j] - xs[i]) / ((ys[j] - ys[i]) == 0 ? 1 : (ys[j] - ys[i]));
      }
      j = i;
    }
    for (int i = 0; i < nodeCount - 1; i++) {
      for (int k = i + 1; k < nodeCount; k++) {
        if (nodes[i] > nodes[k]) {
          int t = nodes[i]; nodes[i] = nodes[k]; nodes[k] = t;
        }
      }
    }
    for (int i = 0; i < nodeCount; i += 2) {
      if (i + 1 >= nodeCount) break;
      gfx->drawLine(nodes[i], y, nodes[i + 1], y, fill);
    }
  }

  for (int i = 0, j = poly.n - 1; i < poly.n; j = i++) {
    gfx->drawLine(xs[j], ys[j], xs[i], ys[i], outline);
    gfx->drawLine(xs[j] + 1, ys[j], xs[i] + 1, ys[i], outline);
  }
}

bool zoneIsActive(int zoneNumber) {
  if (zoneNumber < 1 || zoneNumber > 5) return false;
  return zoneRuns[zoneNumber - 1].active;
}

bool zoneContainsPoly(int zoneNumber, uint8_t polyIndex) {
  if (zoneNumber < 1 || zoneNumber > 5) return false;
  return zoneToPolys[zoneNumber - 1][0] == polyIndex || zoneToPolys[zoneNumber - 1][1] == polyIndex;
}

void drawBadge(int x, int y, int zoneNumber, bool active, bool selected) {
  uint16_t zc = zoneUiColor(zoneNumber);
  uint16_t fill = active ? zc : C_PANEL;
  uint16_t text = active ? C_BG : C_TEXT;
  if (selected) {
    gfx->drawCircle(x, y, 16, C_AMBER);
    gfx->drawCircle(x, y, 17, C_AMBER);
  }
  gfx->fillCircle(x + 1, y + 1, 13, C_SHADOW);
  gfx->fillCircle(x, y, 12, fill);
  gfx->drawCircle(x, y, 12, active ? C_TEXT : zc);
  gfx->setTextSize(2);
  gfx->setTextColor(text);
  String n = String(zoneNumber);
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds((char*)n.c_str(), 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor(x - w / 2, y - h / 2);
  gfx->print(n);
}


ScreenPt toZoneScreen(const MapPt &p, float minX, float maxX, float minY, float maxY) {
  float w = maxX - minX;
  float h = maxY - minY;
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  float target = 116.0f;
  float s = min(target / w, target / h);
  ScreenPt out;
  out.x = (int16_t)roundf(CX + (p.x - (minX + maxX) / 2.0f) * s);
  out.y = (int16_t)roundf(104 + (p.y - (minY + maxY) / 2.0f) * s);
  return out;
}

void drawZonePolyFilledRecentered(uint8_t polyIndex, float minX, float maxX, float minY, float maxY, uint16_t fill, uint16_t outline) {
  const Poly &poly = polys[polyIndex];
  int16_t xs[8], ys[8];
  for (int i = 0; i < poly.n; i++) {
    ScreenPt s = toZoneScreen(poly.pts[i], minX, maxX, minY, maxY);
    xs[i] = s.x;
    ys[i] = s.y;
  }

  int yMin = 239, yMax = 0;
  for (int i = 0; i < poly.n; i++) {
    yMin = min(yMin, (int)ys[i]);
    yMax = max(yMax, (int)ys[i]);
  }

  for (int y = yMin; y <= yMax; y++) {
    int nodes[10];
    int nodeCount = 0;
    int j = poly.n - 1;
    for (int i = 0; i < poly.n; i++) {
      if ((ys[i] < y && ys[j] >= y) || (ys[j] < y && ys[i] >= y)) {
        nodes[nodeCount++] = xs[i] + (y - ys[i]) * (xs[j] - xs[i]) / ((ys[j] - ys[i]) == 0 ? 1 : (ys[j] - ys[i]));
      }
      j = i;
    }
    for (int i = 0; i < nodeCount - 1; i++) {
      for (int k = i + 1; k < nodeCount; k++) {
        if (nodes[i] > nodes[k]) {
          int t = nodes[i]; nodes[i] = nodes[k]; nodes[k] = t;
        }
      }
    }
    for (int i = 0; i < nodeCount; i += 2) {
      if (i + 1 >= nodeCount) break;
      gfx->drawLine(nodes[i], y, nodes[i + 1], y, fill);
    }
  }

  for (int i = 0, j = poly.n - 1; i < poly.n; j = i++) {
    gfx->drawLine(xs[j], ys[j], xs[i], ys[i], outline);
    gfx->drawLine(xs[j] + 1, ys[j], xs[i] + 1, ys[i], outline);
  }
}

void drawSelectedZoneOnly(int zoneNumber) {
  zoneNumber = constrain(zoneNumber, 1, 5);
  uint16_t accent = zoneUiColor(zoneNumber);
  bool active = zoneIsActive(zoneNumber);

  float minX = 999, maxX = -999, minY = 999, maxY = -999;
  for (int k = 0; k < 2; k++) {
    uint8_t pidx = zoneToPolys[zoneNumber - 1][k];
    if (pidx == 255) continue;
    const Poly &poly = polys[pidx];
    for (int i = 0; i < poly.n; i++) {
      minX = min(minX, poly.pts[i].x);
      maxX = max(maxX, poly.pts[i].x);
      minY = min(minY, poly.pts[i].y);
      maxY = max(maxY, poly.pts[i].y);
    }
  }

  for (int k = 0; k < 2; k++) {
    uint8_t pidx = zoneToPolys[zoneNumber - 1][k];
    if (pidx == 255) continue;
    uint16_t fill = active ? accent : dimColor(accent);
    drawZonePolyFilledRecentered(pidx, minX, maxX, minY, maxY, fill, active ? C_TEXT : accent);
  }

  if (zoneNumber == 4) {
    ScreenPt a = toZoneScreen({polys[0].cx, polys[0].cy}, minX, maxX, minY, maxY);
    ScreenPt b = toZoneScreen({polys[1].cx, polys[1].cy}, minX, maxX, minY, maxY);
    gfx->drawLine(a.x, a.y, b.x, b.y, C_AMBER);
  }

  gfx->fillCircle(CX, 112, 17, C_BG);
  gfx->drawCircle(CX, 112, 18, accent);
  gfx->setTextSize(2);
  gfx->setTextColor(C_TEXT);
  String label = String(zoneNumber);
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds((char*)label.c_str(), 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor(CX - w / 2, 112 - h / 2);
  gfx->print(label);
}


void drawGardenMap(int selectedZoneNumber, bool mini = false) {
  for (uint8_t p = 0; p < 6; p++) {
    int zn = polygonToZoneNumber(p);
    bool active = zoneIsActive(zn);
    bool selected = (zn == selectedZoneNumber);
    uint16_t zc = zoneUiColor(zn);
    uint16_t fill = active ? zc : (selected ? dimColor(zc) : C_BG);
    uint16_t outline = selected ? C_AMBER : (active ? zc : dimColor(zc));
    drawPolyFilled(p, fill, outline);
  }

  // Linked-zone cue for Zone 4: draw under badges so both badge numbers stay readable.
  ScreenPt z4a = toScreen({polys[0].cx, polys[0].cy});
  ScreenPt z4b = toScreen({polys[1].cx, polys[1].cy});
  uint16_t z4c = selectedZoneNumber == 4 || zoneIsActive(4) ? zoneUiColor(4) : dimColor(zoneUiColor(4));
  gfx->drawLine(z4a.x, z4a.y, z4b.x, z4b.y, z4c);
  gfx->drawLine(z4a.x, z4a.y + 1, z4b.x, z4b.y + 1, z4c);

  for (uint8_t p = 0; p < 6; p++) {
    int zn = polygonToZoneNumber(p);
    ScreenPt c = toScreen({polys[p].cx, polys[p].cy});
    drawBadge(c.x, c.y, zn, zoneIsActive(zn), zn == selectedZoneNumber);
  }
}

void drawBottomPrompt(const String &txt, uint16_t color = C_TEXT) {
  gfx->fillRoundRect(42, 202, 156, 24, 10, C_PANEL);
  drawCenteredText(txt, 209, 1, color);
}

void drawProgressRing(float frac, uint16_t color) {
  frac = constrain(frac, 0.0f, 1.0f);
  int r = 108;
  gfx->drawCircle(CX, CY, r, C_PANEL);
  gfx->drawCircle(CX, CY, r - 1, C_PANEL);
  gfx->drawCircle(CX, CY, r - 2, C_PANEL);
  int segments = (int)(frac * 96);
  for (int i = 0; i < segments; i++) {
    float a = (-90 + i * 3.75f) * PI / 180.0f;
    int x1 = CX + cosf(a) * (r - 7);
    int y1 = CY + sinf(a) * (r - 7);
    int x2 = CX + cosf(a) * r;
    int y2 = CY + sinf(a) * r;
    gfx->drawLine(x1, y1, x2, y2, color);
    if (i % 2 == 0) gfx->drawPixel(x2, y2, C_TEXT);
  }
}

// ----------------------------- UI Drawing -----------------------------------

void drawHome() {
  fillCircularBackground();
  drawConnectionDot();

  if (configSavedNotice && millis() < configSavedNoticeUntil) {
    drawToastOverlay("Settings saved", C_GREEN);
    return;
  }

  if (commandPending && commandPendingText.length()) {
    drawSignalSweep(C_AMBER);
    drawOrbitDots(C_AMBER, 6, 72);
    drawSpinner(120, 78, C_AMBER);
    drawCenteredText(commandPendingText, 106, 2, C_AMBER);
    return;
  }

  if (millis() < commandMessageUntil && lastCommandText.length()) {
    bool fail = lastCommandText.indexOf("Failed") >= 0 || lastCommandText.indexOf("failed") >= 0;
    drawToastOverlay(lastCommandText, fail ? C_RED : C_GREEN);
    commandMessageUntil = 0;  // toast consumed; do not keep redrawing it on an idle TFT
    return;
  }

  drawHomeMenuCarousel();
}

void drawZoneSelect() {
  fillCircularBackground();

  if (selectedItem > 4) selectedItem = selectedZone - 1;
  selectedZone = constrain(selectedZone, 1, 5);
  uint16_t accent = zoneUiColor(selectedZone);

  drawConnectionDot();
  drawPlainTitle("Zone " + String(selectedZone), zoneIsActive(selectedZone) ? "Running" : "Select Zone", accent);

  drawPlainCard(50, 82, 140, 86, dimColor(accent));
  drawSelectedZoneOnly(selectedZone);

  int prevZone = selectedZone == 1 ? 5 : selectedZone - 1;
  int nextZone = selectedZone == 5 ? 1 : selectedZone + 1;
  drawMenuSideLabel("‹", 38, 126, C_SOFT_TEXT);
  drawMenuSideLabel("›", 202, 126, C_SOFT_TEXT);

  drawPlainPageDots(selectedZone - 1, 5, accent);
  drawSmallCentered("Press to run", SAFE_BOTTOM_HINT_Y, C_SOFT_TEXT);
}

void drawDuration() {
  fillCircularBackground();
  uint16_t accent = durationTarget == DUR_ZONE ? zoneUiColor(selectedZone) : C_CYAN;
  drawConnectionDot();

  String title = durationTarget == DUR_ZONE ? ("Zone " + String(selectedZone)) : "Spigots";
  drawPlainTitle(title, "Run Time", accent);

  drawCenteredText(String(durationMinutes), 92, 5, C_TEXT);
  drawCenteredText("minutes", 142, 2, C_SOFT_TEXT);

  gfx->drawCircle(52, 116, 17, accent);
  gfx->setTextSize(3);
  gfx->setTextColor(accent);
  gfx->setCursor(44, 105);
  gfx->print("-");
  gfx->drawCircle(188, 116, 17, accent);
  gfx->setCursor(179, 105);
  gfx->print("+");

  drawRoundButton(44, SAFE_BOTTOM_BUTTON_Y, 72, 26, "Cancel", C_BUTTON, C_DIM);
  drawRoundButton(124, SAFE_BOTTOM_BUTTON_Y, 72, 26, "Start", accent, accent, C_BG);
}

void drawRunning() {
  fillCircularBackground();
  drawConnectionDot();

  RunItem *r = nullptr;
  int count = activeZoneCount();
  if (count > 0) {
    if (millis() - runningCycleMs > 12000) {
      runningCycleMs = millis();
      runningDisplayIndex++;
    }
    int seen = 0;
    int target = runningDisplayIndex % count;
    for (int i = 0; i < 5; i++) {
      if (zoneRuns[i].active) {
        if (seen == target) {
          r = &zoneRuns[i];
          selectedZone = i + 1;
          break;
        }
        seen++;
      }
    }
  }

  if (r) {
    displayedRunningZone = r->zoneNumber;
    displayedRunningSpigot = false;
    long rem = localRemaining(*r);
    uint16_t accent = zoneUiColor(r->zoneNumber);
    float frac = (r->durationSeconds > 0) ? (float)rem / (float)r->durationSeconds : 0.0f;
    drawProgressRing(frac, accent);
    drawPlainTitle("Zone " + String(r->zoneNumber), "Running", accent);
    drawCenteredText(fmtDuration(rem), 94, 4, C_TEXT);
    drawCenteredText("remaining", 136, 2, C_SOFT_TEXT);
  } else if (spigotRun.active) {
    displayedRunningZone = 0;
    displayedRunningSpigot = true;
    long rem = localRemaining(spigotRun);
    float frac = (spigotRun.durationSeconds > 0) ? (float)rem / (float)spigotRun.durationSeconds : 0.0f;
    drawProgressRing(frac, C_CYAN);
    drawPlainTitle("Spigots", "Running", C_CYAN);
    drawCenteredText(fmtDuration(rem), 94, 4, C_TEXT);
    drawCenteredText("remaining", 136, 2, C_SOFT_TEXT);
  } else {
    displayedRunningZone = 0;
    displayedRunningSpigot = false;
    drawPlainTitle("Water Off", "Status", C_GREEN);
    drawCenteredText("No active run", 108, 2, C_TEXT);
  }

  String stopLabel = displayedRunningSpigot ? "Stop Spigot" : (displayedRunningZone > 0 ? "Stop Zone" : "Back");
  drawRoundButton(54, SAFE_BOTTOM_BUTTON_Y, 132, 28, stopLabel, C_RED, C_RED);
}

void drawSchedule() {
  fillCircularBackground();
  drawConnectionDot();

  if (scheduleZonePickerMode) {
    uint16_t accent = zoneUiColor(scheduleZone);
    drawPlainTitle("Zone " + String(scheduleZone), "Choose Schedule", accent);
    drawPlainCard(50, 82, 140, 86, dimColor(accent));
    drawSelectedZoneOnly(scheduleZone);

    drawMenuSideLabel("‹", 38, 126, C_SOFT_TEXT);
    drawMenuSideLabel("›", 202, 126, C_SOFT_TEXT);
    drawHomeCarouselEdgeIndicator(scheduleZone - 1, 5, accent);
    drawSmallCentered("Press to view slots", SAFE_BOTTOM_HINT_Y, C_SOFT_TEXT);
    return;
  }

  drawPlainTitle("Zone " + String(scheduleZone), "Schedule", C_ZONE3);

  ZoneSchedule &zs = zoneSchedules[scheduleZone - 1];

  gfx->drawLine(74, 78, 166, 78, C_PANEL);
  if (!scheduleLoaded || !zs.loaded) {
    drawCenteredText("Not Loaded", 112, 2, C_AMBER);
    drawSmallCentered("Retrying...", 140, C_SOFT_TEXT);
  } else if (zs.count == 0) {
    bool selectedAdd = (scheduleSlot == 0);
    drawCenteredText("No Schedule", 110, selectedAdd ? 2 : 1, selectedAdd ? C_TEXT : C_SOFT_TEXT);
    drawCenteredText("+ Add Slot", 146, selectedAdd ? 2 : 1, C_ZONE1);
    if (selectedAdd) gfx->drawLine(70, 166, 170, 166, C_ZONE1);
  } else {
    for (int i = 0; i < zs.count && i < 3; i++) {
      ScheduleItem &s = zs.slots[i];
      int y = 94 + i * 30;
      String line = fmtTime12(s.startHour, s.startMinute) + "   " + String(s.durationMinutes) + "m";
      bool selected = (i == scheduleSlot);
      drawCenteredText(line, y, selected ? 2 : 1, selected ? C_TEXT : (s.enabled ? C_SOFT_TEXT : C_DIM));
      if (selected) gfx->drawLine(56, y + 21, 184, y + 21, C_ZONE3);
    }

    if (zs.count < 6) {
      int addIndex = zs.count;
      int y = 94 + min((int)zs.count, 3) * 30;
      bool selected = (scheduleSlot == addIndex);
      drawCenteredText("+ Add Slot", y, selected ? 2 : 1, selected ? C_ZONE1 : C_SOFT_TEXT);
      if (selected) gfx->drawLine(64, y + 21, 176, y + 21, C_ZONE1);
    }
  }

  drawSmallCentered("Rotate slot", 188, C_SOFT_TEXT);
  drawSmallCentered("Press edit", SAFE_BOTTOM_HINT_Y, C_SOFT_TEXT);
}

void drawScheduleEdit() {
  fillCircularBackground();
  drawConnectionDot();
  drawPlainTitle("Zone " + String(scheduleZone), scheduleAddMode ? "Add Schedule" : "Edit Schedule", C_ZONE3);

  String line = fmtTime12(editHour, editMinute);

  gfx->drawRoundRect(48, 82, 144, 38, 8, editField <= 1 ? C_AMBER : C_PANEL);
  drawCenteredText(line, 91, 3, editField <= 1 ? C_AMBER : C_TEXT);

  gfx->drawRoundRect(48, 130, 144, 30, 8, editField == 2 ? C_AMBER : C_PANEL);
  drawCenteredText(String(editDuration) + " min", 137, 2, editField == 2 ? C_AMBER : C_TEXT);

  if (!scheduleAddMode) {
    drawRoundButton(34, SAFE_BOTTOM_BUTTON_Y, 78, 26, "Delete", editField == 5 ? C_RED : C_BUTTON, editField == 5 ? C_RED : C_DIM, C_TEXT);
  }
  drawRoundButton(128, SAFE_BOTTOM_BUTTON_Y, 78, 26, "Save", editField == 3 ? C_GREEN : C_BUTTON, editField == 3 ? C_GREEN : C_DIM, editField == 3 ? C_BG : C_TEXT);
}

void drawStopConfirm() {
  fillCircularBackground();
  drawCleanTitle("Stop All", "Watering", C_RED);
  drawCleanCard(46, 90, 148, 62, C_CARD, dimColor(C_RED));
  drawCenteredText("Stop all water?", 112, 2, C_TEXT);
  drawRoundButton(34, SAFE_BOTTOM_BUTTON_Y, 78, 28, "Cancel", C_BUTTON, C_DIM);
  drawRoundButton(126, SAFE_BOTTOM_BUTTON_Y, 84, 28, "Stop All", C_RED, C_RED);
}

void drawSetupError() {
  fillCircularBackground();
  drawFineOuterBezel(C_AMBER);
  drawConnectionDot();
  drawCenteredText("Setup", 38, 3, C_AMBER);
  drawWifiIcon(120, 62, WiFi.status() == WL_CONNECTED);
  gfx->drawCircle(CX, CY, 70, C_PANEL);
  drawCleanCard(42, 84, 156, 92, C_CARD, dimColor(C_AMBER));
  drawCenteredText("Connect Wi-Fi:", 92, 1, C_TEXT);
  drawCenteredText(cfg.apSsid, 104, 2, C_TEXT);
  drawCenteredText("Open:", 140, 1, C_DIM);
  drawCenteredText("192.168.6.1", 158, 2, C_TEXT);
  drawStatusRibbon(configSavedNotice && millis() < configSavedNoticeUntil ? "settings saved" : "setup portal open", configSavedNotice && millis() < configSavedNoticeUntil ? C_GREEN : C_AMBER);
  drawSmallCentered(relayError.substring(0, 28), SAFE_BOTTOM_HINT_Y, configSavedNotice && millis() < configSavedNoticeUntil ? C_GREEN : C_RED);
}


void drawDiagnostics() {
  fillCircularBackground();
  drawFineOuterBezel(C_MINT);
  
  drawHeaderChip("DIAGNOSTICS", C_MINT);
  drawWifiIcon(120, 55, WiFi.status() == WL_CONNECTED);
  
  drawCleanCard(42, 78, 156, 92, C_CARD, dimColor(C_MINT));

  String sta = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "not connected";
  String ap = WiFi.softAPIP().toString();
  String relay = relayConnected ? "OK" : "FAIL";
  String heap = String(ESP.getFreeHeap());

  if (diagnosticsPage == 0) {
    drawMetricRow(86, "STA", sta.substring(0, 18), C_MINT);
    drawMetricRow(108, "AP", ap, C_MINT);
    drawMetricRow(130, "Relay", relay, relayConnected ? C_GREEN : C_RED);
    drawMetricRow(152, "Heap", heap, C_MINT);
  } else {
    drawMetricRow(86, "Touch", touchWasDown ? "down" : "idle", C_MINT);
    drawMetricRow(108, "Zone", String(selectedZone), zoneUiColor(selectedZone));
    drawMetricRow(130, "Sched", scheduleLoaded ? "loaded" : "not loaded", scheduleLoaded ? C_GREEN : C_AMBER);
    drawMetricRow(152, "Weather", weatherLoaded ? "loaded" : "unavailable", weatherLoaded ? C_GREEN : C_DIM);
  }

  drawSmallCentered("Rotate page  Edge back", SAFE_BOTTOM_HINT_Y, C_DIM);
  drawTopPipRow(diagnosticsPage, 2, C_MINT);
}


bool screenAllowsBack() {
  return screen != SCR_HOME;
}

void performBackGesture() {
  switch (screen) {
    case SCR_DURATION:
      if (durationTarget == DUR_ZONE && previousScreen == SCR_ZONE_SELECT) go(SCR_ZONE_SELECT);
      else go(SCR_HOME);
      break;
    case SCR_ZONE_SELECT:
      go(SCR_HOME);
      break;
    case SCR_SCHEDULE:
      if (scheduleZonePickerMode) go(SCR_HOME);
      else {
        scheduleZonePickerMode = true;
        needsRedraw = true;
      }
      break;
    case SCR_SCHEDULE_EDIT:
      lastCommandText = "Canceled";
      commandMessageUntil = millis() + 1200;
      go(SCR_SCHEDULE);
      break;
    case SCR_RUNNING:
      go(SCR_HOME);
      break;
    case SCR_STOP_CONFIRM:
      go(SCR_HOME);
      break;
    case SCR_SETUP_ERROR:
      go(SCR_HOME);
      break;
    case SCR_DIAGNOSTICS:
      go(SCR_HOME);
      break;
    default:
      go(SCR_HOME);
      break;
  }
}

void drawBackEdge() {
  if (!screenAllowsBack()) return;

  gfx->fillRoundRect(0, 40, BACK_EDGE_W, 160, 8, C_RED);

  // Black back arrow centered inside the red back button.
  int cy = 120;
  int tipX = 5;
  int shaftStartX = 8;
  int shaftEndX = 15;

  // Arrow shaft (3 px thick)
  gfx->drawLine(shaftStartX, cy - 1, shaftEndX, cy - 1, C_BG);
  gfx->drawLine(shaftStartX, cy,     shaftEndX, cy,     C_BG);
  gfx->drawLine(shaftStartX, cy + 1, shaftEndX, cy + 1, C_BG);

  // Upper diagonal
  gfx->drawLine(shaftStartX, cy - 1, tipX, cy - 6, C_BG);
  gfx->drawLine(shaftStartX, cy,     tipX, cy - 5, C_BG);
  gfx->drawLine(shaftStartX, cy + 1, tipX, cy - 4, C_BG);

  // Lower diagonal
  gfx->drawLine(shaftStartX, cy - 1, tipX, cy + 4, C_BG);
  gfx->drawLine(shaftStartX, cy,     tipX, cy + 5, C_BG);
  gfx->drawLine(shaftStartX, cy + 1, tipX, cy + 6, C_BG);
}

void drawUI() {
  needsRedraw = false;
  lastDrawMs = millis();
  lastDrawnScreen = screen;
  lastDrawnHomeMenuIndex = homeMenuIndex;
  lastDrawnSelectedZone = selectedZone;
  lastDrawnScheduleZone = scheduleZone;
  lastDrawnScheduleSlot = scheduleSlot;
  lastUiStateSignature = buildRedrawStateSignature();
  switch (screen) {
    case SCR_HOME: drawHome(); break;
    case SCR_ZONE_SELECT: drawZoneSelect(); break;
    case SCR_DURATION: drawDuration(); break;
    case SCR_RUNNING: drawRunning(); break;
    case SCR_SCHEDULE: drawSchedule(); break;
    case SCR_SCHEDULE_EDIT: drawScheduleEdit(); break;
    case SCR_STOP_CONFIRM: drawStopConfirm(); break;
    case SCR_SETUP_ERROR: drawSetupError(); break;
    case SCR_DIAGNOSTICS: drawDiagnostics(); break;
  }
  drawBackEdge();
}

// ----------------------------- UI Actions -----------------------------------


void stopDisplayedRunContext() {
  if (displayedRunningSpigot) {
    sendStopSpigots();
    go(SCR_HOME);
    return;
  }
  if (displayedRunningZone >= 1 && displayedRunningZone <= 5) {
    sendStopZone(displayedRunningZone);
    go(SCR_HOME);
    return;
  }
  lastCommandText = "Nothing to stop";
  commandMessageUntil = millis() + 1500;
  go(SCR_HOME);
}


bool waterIsActive() {
  return isAnyZoneRunning() || spigotRun.active;
}

void noteUserInteraction() {
  lastUserInteractionMs = millis();
}

void setScreen(ScreenId s, bool userNavigation) {
  bool watering = waterIsActive();

  if (watering && screen == SCR_RUNNING && s != SCR_RUNNING && userNavigation) {
    userLeftRunningScreen = true;
    lastUserInteractionMs = millis();
  }

  if (s == SCR_RUNNING) {
    userLeftRunningScreen = false;
  }

  if (screen != s) {
    previousScreen = screen;
    screen = s;
    needsRedraw = true;
  }
}

void go(ScreenId s) {
  setScreen(s, true);
}

void acceptedFeedback() {
  // Brief input acknowledgement only. Persistent zone/status LED state is restored immediately after.
  uint32_t c = commandPending ? pixels.Color(255, 190, 0) : (relayConnected ? pixels.Color(20, 80, 20) : pixels.Color(90, 0, 0));
  fillAllLeds(c);
  lastLedUpdateMs = 0;
}

void selectCurrentZoneItem() {
  if (selectedItem <= 4) {
    selectedZone = selectedItem + 1;
    durationTarget = DUR_ZONE;
    durationMinutes = cfg.manualDefaultMinutes;
    go(SCR_DURATION);
  } else if (selectedItem == 5) {
    durationTarget = DUR_SPIGOT;
    durationMinutes = cfg.manualDefaultMinutes;
    go(SCR_DURATION);
  } else {
    scheduleZone = selectedZone;
    go(SCR_SCHEDULE);
  }
}


void openScheduleEditor() {
  ZoneSchedule &zs = zoneSchedules[scheduleZone - 1];

  scheduleAddMode = false;
  editField = 0;

  if (!scheduleLoaded || !zs.loaded) {
    lastCommandText = "Schedule not loaded";
    commandMessageUntil = millis() + 1600;
    needsRedraw = true;
    return;
  }

  if (zs.count == 0 || scheduleSlot >= zs.count) {
    scheduleAddMode = true;
    editHour = 6;
    editMinute = 0;
    editDuration = cfg.manualDefaultMinutes;
  } else {
    ScheduleItem &slot = zs.slots[scheduleSlot];
    editHour = constrain(slot.startHour, 0, 23);
    editMinute = constrain(slot.startMinute, 0, 59);
    editDuration = constrain(slot.durationMinutes, 1, 240);
  }

  go(SCR_SCHEDULE_EDIT);
}

void shortPress() {
  acceptedFeedback();
  switch (screen) {
    case SCR_HOME:
      activateHomeMenuAction();
      break;
    case SCR_ZONE_SELECT:
      selectCurrentZoneItem();
      break;
    case SCR_DURATION:
      if (durationTarget == DUR_ZONE) sendManualRun(selectedZone, durationMinutes);
      else sendSpigotRun(durationMinutes);
      go(SCR_RUNNING);
      break;
    case SCR_RUNNING:
      stopDisplayedRunContext();
      break;
        case SCR_SCHEDULE:
      if (scheduleZonePickerMode) {
        scheduleZonePickerMode = false;
        scheduleSlot = 0;
        needsRedraw = true;
      } else {
        openScheduleEditor();
      }
      break;
    case SCR_SCHEDULE_EDIT:
      if (editField < 2) {
        // Accept this input and move to the next input.
        editField++;
      } else if (editField == 2) {
        // Duration is the last form input. Press saves the edit/new schedule.
        if (!scheduleAddMode) deleteScheduleSlot(scheduleZone, scheduleSlot);
        sendScheduleSlot(scheduleZone, editHour, editMinute, editDuration, true);
        scheduleZonePickerMode = false;
        go(SCR_SCHEDULE);
      } else if (editField == 5) {
        // Delete only happens if the user deliberately rotates to the Delete action.
        if (!scheduleAddMode) deleteScheduleSlot(scheduleZone, scheduleSlot);
        go(SCR_SCHEDULE);
      } else {
        // Save action if the user deliberately rotated to it.
        if (!scheduleAddMode) deleteScheduleSlot(scheduleZone, scheduleSlot);
        sendScheduleSlot(scheduleZone, editHour, editMinute, editDuration, true);
        scheduleZonePickerMode = false;
        go(SCR_SCHEDULE);
      }
      needsRedraw = true;
      break;
    case SCR_STOP_CONFIRM:
      sendAllOff();
      go(SCR_HOME);
      break;
    case SCR_SETUP_ERROR:
      go(SCR_HOME);
      break;
    case SCR_DIAGNOSTICS:
      go(SCR_HOME);
      break;
  }
}

void longPress() {
  acceptedFeedback();
  switch (screen) {
    case SCR_RUNNING:
      stopDisplayedRunContext();
      break;
    case SCR_STOP_CONFIRM:
      go(SCR_RUNNING);
      break;
    case SCR_HOME:
      go(SCR_DIAGNOSTICS);
      break;
    case SCR_ZONE_SELECT:
      go(SCR_HOME);
      break;
    case SCR_SCHEDULE_EDIT:
      // Long press cancels editing without save/delete.
      lastCommandText = "Canceled";
      commandMessageUntil = millis() + 1500;
      go(SCR_SCHEDULE);
      break;
    case SCR_DIAGNOSTICS:
      go(SCR_HOME);
      break;
    default:
      go(SCR_HOME);
      break;
  }
}

void emergencyPress() {
  if (cfg.emergencyDirectStop || screen == SCR_RUNNING) {
    sendAllOff();
    go(SCR_HOME);
  } else {
    go(SCR_STOP_CONFIRM);
  }
}

void rotateUI(int delta) {
  if (delta == 0) return;
  noteUserInteraction();
  acceptedFeedback();

  // Encoder hardware on this board reports the physical direction opposite of
  // the intuitive value-editing direction. Normalize it here:
  // clockwise/right turn = +1 / increase, counterclockwise/left turn = -1 / decrease.
  int valueStep = delta > 0 ? -1 : 1;
  int navStep = valueStep;
  int mag = abs(delta);
  bool fast = mag >= 3;

  switch (screen) {
    case SCR_HOME:
      homeMenuIndex = (homeMenuIndex + navStep + HOME_MENU_COUNT) % HOME_MENU_COUNT;
      needsRedraw = true;
      break;
    case SCR_ZONE_SELECT:
      selectedZone += navStep;
      if (selectedZone < 1) selectedZone = 5;
      if (selectedZone > 5) selectedZone = 1;
      selectedItem = selectedZone - 1;
      cfg.lastSelectedZone = selectedZone;
      needsRedraw = true;
      break;
    case SCR_DURATION:
      durationMinutes += valueStep * (fast ? 5 : 1);
      durationMinutes = constrain(durationMinutes, 1, 240);
      needsRedraw = true;
      break;
    case SCR_RUNNING:
      // While watering is active, rotation should not trap the user on status.
      // Move directly into the action carousel so more actions can be selected.
      homeMenuIndex = (homeMenuIndex + navStep + HOME_MENU_COUNT) % HOME_MENU_COUNT;
      go(SCR_HOME);
      break;
        case SCR_SCHEDULE:
      if (scheduleZonePickerMode) {
        scheduleZone += navStep;
        if (scheduleZone < 1) scheduleZone = 5;
        if (scheduleZone > 5) scheduleZone = 1;
        scheduleSlot = 0;
      } else if (fast) {
        scheduleZone += navStep;
        if (scheduleZone < 1) scheduleZone = 5;
        if (scheduleZone > 5) scheduleZone = 1;
        scheduleSlot = 0;
      } else {
        ZoneSchedule &zs = zoneSchedules[scheduleZone - 1];
        int maxSlot = zs.loaded ? min((int)zs.count, 5) : 0;
        if (zs.loaded && zs.count < 6) maxSlot = zs.count;
        scheduleSlot += navStep;
        if (scheduleSlot < 0) scheduleSlot = maxSlot;
        if (scheduleSlot > maxSlot) scheduleSlot = 0;
      }
      needsRedraw = true;
      break;
        case SCR_SCHEDULE_EDIT:
      if (editField == 0) {
        editHour = (editHour + valueStep + 24) % 24;
      } else if (editField == 1) {
        editMinute += valueStep * (fast ? 5 : 1);
        while (editMinute < 0) editMinute += 60;
        while (editMinute >= 60) editMinute -= 60;
      } else if (editField == 2) {
        editDuration += valueStep * (fast ? 5 : 1);
        editDuration = constrain(editDuration, 1, 240);
      } else {
        editField += navStep;
        int maxActionField = scheduleAddMode ? 3 : 5;
        if (editField == 4) editField += navStep;
        if (editField < 3) editField = maxActionField;
        if (editField > maxActionField) editField = 3;
      }
      needsRedraw = true;
      break;
    case SCR_STOP_CONFIRM:
      needsRedraw = true;
      break;
    case SCR_DIAGNOSTICS:
      diagnosticsPage += navStep;
      if (diagnosticsPage < 0) diagnosticsPage = 1;
      if (diagnosticsPage > 1) diagnosticsPage = 0;
      needsRedraw = true;
      break;
    default:
      needsRedraw = true;
      break;
  }
}

void handleTouchTap(int x, int y) {
  noteUserInteraction();
  acceptedFeedback();

  if (screenAllowsBack() && x <= BACK_TAP_HIT_W && y >= 30 && y <= 210) {
    performBackGesture();
    return;
  }

  if (screen == SCR_ZONE_SELECT) {
    if (x < 80) {
      selectedZone = selectedZone == 1 ? 5 : selectedZone - 1;
      selectedItem = selectedZone - 1;
      cfg.lastSelectedZone = selectedZone;
      needsRedraw = true;
    } else if (x > 160) {
      selectedZone = selectedZone == 5 ? 1 : selectedZone + 1;
      selectedItem = selectedZone - 1;
      cfg.lastSelectedZone = selectedZone;
      needsRedraw = true;
    } else {
      durationTarget = DUR_ZONE;
      durationMinutes = cfg.manualDefaultMinutes;
      go(SCR_DURATION);
    }
    return;
  }

  if (screen == SCR_HOME) {
    if (x < 80) {
      homeMenuIndex = (homeMenuIndex + HOME_MENU_COUNT - 1) % HOME_MENU_COUNT;
      needsRedraw = true;
    } else if (x > 160) {
      homeMenuIndex = (homeMenuIndex + 1) % HOME_MENU_COUNT;
      needsRedraw = true;
    } else {
      activateHomeMenuAction();
    }
    return;
  }

  if (screen == SCR_DURATION) {
    if (x >= 44 && x <= 116 && y >= 172 && y <= 206) {
      go(SCR_ZONE_SELECT);
    } else if (x >= 124 && x <= 196 && y >= 172 && y <= 206) {
      if (durationTarget == DUR_ZONE) sendManualRun(selectedZone, durationMinutes);
      else sendSpigotRun(durationMinutes);
      go(SCR_RUNNING);
    } else if (y < 120) {
      durationMinutes = constrain(durationMinutes + 1, 1, 240);
      needsRedraw = true;
    } else {
      durationMinutes = constrain(durationMinutes - 1, 1, 240);
      needsRedraw = true;
    }
    return;
  }

  if (screen == SCR_RUNNING) {
    if (x >= 54 && x <= 186 && y >= 172 && y <= 208) {
      stopDisplayedRunContext();
    } else {
      go(SCR_HOME);
    }
    return;
  }

  if (screen == SCR_STOP_CONFIRM) {
    if (x >= 30 && x <= 110 && y >= 152 && y <= 186) go(SCR_RUNNING);
    else if (x >= 122 && x <= 212 && y >= 152 && y <= 186) {
      sendAllOff();
      go(SCR_HOME);
    }
    return;
  }

  if (screen == SCR_SCHEDULE) {
    if (scheduleZonePickerMode) {
      if (x < 80) {
        scheduleZone = scheduleZone == 1 ? 5 : scheduleZone - 1;
        scheduleSlot = 0;
        needsRedraw = true;
      } else if (x > 160) {
        scheduleZone = scheduleZone == 5 ? 1 : scheduleZone + 1;
        scheduleSlot = 0;
        needsRedraw = true;
      } else {
        scheduleZonePickerMode = false;
        scheduleSlot = 0;
        needsRedraw = true;
      }
    } else {
      openScheduleEditor();
    }
    return;
  }

  if (screen == SCR_SCHEDULE_EDIT) {
    if (!scheduleAddMode && x >= 30 && x <= 116 && y >= 170 && y <= 208) {
      deleteScheduleSlot(scheduleZone, scheduleSlot);
      go(SCR_SCHEDULE);
    } else if (x >= 124 && x <= 214 && y >= 170 && y <= 208) {
      if (!scheduleAddMode) deleteScheduleSlot(scheduleZone, scheduleSlot);
      sendScheduleSlot(scheduleZone, editHour, editMinute, editDuration, true);
      go(SCR_SCHEDULE);
    } else if (y < 105) {
      editField = 0;
      needsRedraw = true;
    } else if (y < 155) {
      editField = 2;
      needsRedraw = true;
    }
    return;
  }

  if (screen == SCR_SETUP_ERROR) {
    go(SCR_HOME);
    return;
  }

  if (screen == SCR_DIAGNOSTICS) {
    if (y > 180) go(SCR_HOME);
    else {
      diagnosticsPage = (diagnosticsPage + 1) % 2;
      needsRedraw = true;
    }
    return;
  }
}


void cancelTouchForButtonPress() {
  suppressTouchUntilMs = millis() + TOUCH_SUPPRESS_AFTER_BUTTON_MS;
  touchWasDown = false;
  lastTouch.touched = false;
  touchStart.touched = false;
}

bool touchSuppressed() {
  return millis() < suppressTouchUntilMs;
}

void processInput() {
  int delta = takeEncoderDelta();
  if (delta != 0) {
    rotateUI(delta);
  }

  bool level = digitalRead(ENCODER_BTN);
  uint32_t now = millis();

  if (level == LOW && !buttonPressed) {
    noteUserInteraction();
    buttonPressed = true;
    buttonDownMs = now;
    emergencyPressSent = false;
    longPressSent = false;
    cancelTouchForButtonPress();
  }

  if (buttonPressed && level == LOW) {
    uint32_t held = now - buttonDownMs;

    if (!emergencyPressSent && held >= 2500) {
      emergencyPressSent = true;
      longPressSent = true;
      cancelTouchForButtonPress();
      emergencyPress();
    } else if (!longPressSent && held >= 800) {
      longPressSent = true;
      cancelTouchForButtonPress();
      longPress();
    } else if (!emergencyPressSent && held >= 500) {
      // Keep suppressing touch while the button is being held against the touch surface.
      cancelTouchForButtonPress();
    }
  }

  if (level == HIGH && buttonPressed) {
    uint32_t held = now - buttonDownMs;
    buttonPressed = false;
    cancelTouchForButtonPress();

    if (!emergencyPressSent && !longPressSent) {
      shortPress();
    }

    emergencyPressSent = false;
    longPressSent = false;
  }

  TouchPoint tp = readTouch();

  if (touchSuppressed()) {
    // Drain and ignore any touch state caused by pressing/releasing the encoder.
    touchWasDown = false;
    lastTouch.touched = false;
    touchStart.touched = false;
    return;
  }

  if (tp.touched) {
    lastTouch = tp;
    if (!touchWasDown) {
      touchWasDown = true;
      touchStart = tp;
      lastTouchMs = now;
    }
  } else {
    if (touchWasDown && now - lastTouchMs > 25) {
      touchWasDown = false;

      if (touchSuppressed()) {
        lastTouch.touched = false;
        touchStart.touched = false;
        return;
      }

      int dx = lastTouch.x - touchStart.x;
      int dy = lastTouch.y - touchStart.y;

      if (screenAllowsBack() && touchStart.x <= BACK_SWIPE_START_X && dx >= BACK_SWIPE_MIN_DX && abs(dx) > abs(dy)) {
        performBackGesture();
      } else if (screen == SCR_HOME && abs(dx) > 45 && abs(dx) > abs(dy)) {
        if (dx < 0) homeMenuIndex = (homeMenuIndex + 1) % HOME_MENU_COUNT;
        else homeMenuIndex = (homeMenuIndex + HOME_MENU_COUNT - 1) % HOME_MENU_COUNT;
        needsRedraw = true;
      } else if (screen == SCR_ZONE_SELECT && abs(dx) > 45 && abs(dx) > abs(dy)) {
        if (dx < 0) selectedZone = selectedZone == 5 ? 1 : selectedZone + 1;
        else selectedZone = selectedZone == 1 ? 5 : selectedZone - 1;
        selectedItem = selectedZone - 1;
        cfg.lastSelectedZone = selectedZone;
        needsRedraw = true;
      } else if (screen == SCR_SCHEDULE && scheduleZonePickerMode && abs(dx) > 45 && abs(dx) > abs(dy)) {
        if (dx < 0) scheduleZone = scheduleZone == 5 ? 1 : scheduleZone + 1;
        else scheduleZone = scheduleZone == 1 ? 5 : scheduleZone - 1;
        scheduleSlot = 0;
        needsRedraw = true;
      } else {
        handleTouchTap(lastTouch.x, lastTouch.y);
      }
    }
  }
}

// ----------------------------- Polling --------------------------------------


String buildUiStateSignature() {
  String s;
  s.reserve(128);
  s += "scr=" + String((int)screen);
  s += "|menu=" + String(homeMenuIndex);
  s += "|sel=" + String(selectedZone);
  s += "|schedZone=" + String(scheduleZone);
  s += "|schedSlot=" + String(scheduleSlot);
  s += "|relay=" + String(relayConnected ? 1 : 0);
  s += "|sched=" + String(scheduleLoaded ? 1 : 0);
  s += "|weather=" + String(weatherLoaded ? 1 : 0);
  s += "|spigot=" + String(spigotRun.active ? 1 : 0);
  s += "|spRem=" + String(localRemaining(spigotRun));
  for (int i = 0; i < 5; i++) {
    s += "|z" + String(i + 1) + "=" + String(zoneRuns[i].active ? 1 : 0);
    s += "," + String(localRemaining(zoneRuns[i]));
  }
  return s;
}


String buildRedrawStateSignature() {
  String s;
  s.reserve(128);
  s += "scr=" + String((int)screen);
  s += "|menu=" + String(homeMenuIndex);
  s += "|sel=" + String(selectedZone);
  s += "|schedZone=" + String(scheduleZone);
  s += "|schedSlot=" + String(scheduleSlot);
  s += "|schedPicker=" + String(scheduleZonePickerMode ? 1 : 0);
  s += "|editField=" + String(editField);
  s += "|relay=" + String(relayConnected ? 1 : 0);
  s += "|schedLoaded=" + String(scheduleLoaded ? 1 : 0);
  s += "|water=" + String(waterIsActive() ? 1 : 0);
  s += "|spigotActive=" + String(spigotRun.active ? 1 : 0);

  for (int i = 0; i < 5; i++) {
    s += "|z" + String(i + 1) + "=" + String(zoneRuns[i].active ? 1 : 0);
  }

  // Only the running meter needs a redraw because seconds are changing.
  // Other carousel/list screens should remain perfectly still while idle.
  if (screen == SCR_RUNNING) {
    s += "|spRem=" + String(localRemaining(spigotRun));
    for (int i = 0; i < 5; i++) {
      s += "|zr" + String(i + 1) + "=" + String(localRemaining(zoneRuns[i]));
    }
  }

  return s;
}

void requestRedrawIfUiStateChanged() {
  String sig = buildRedrawStateSignature();
  if (sig != lastUiStateSignature) {
    lastUiStateSignature = sig;
    needsRedraw = true;
  }
}

void pollRelayTasks() {
  uint32_t now = millis();
  uint32_t stateInterval = (isAnyZoneRunning() || spigotRun.active || screen == SCR_RUNNING) ? 1000 : 2000;
  if (now - lastStatePollMs > stateInterval) {
    lastStatePollMs = now;
    loadRelayState(true);
    requestRedrawIfUiStateChanged();
  }

  if (now - lastSchedulePollMs > 30000) {
    lastSchedulePollMs = now;
    loadScheduleOnly();
    requestRedrawIfUiStateChanged();
  }

  if (now - lastTimePollMs > 60000) {
    lastTimePollMs = now;
    String body, err;
    int code;
    relayGetRaw("/time", body, code, err);
  }

  if (now - lastWeatherPollMs > 300000) {
    lastWeatherPollMs = now;
    String body, err;
    int code;
    weatherLoaded = relayGetRaw("/weather", body, code, err);
  }
}

void updateScreenAutoMode() {
  bool watering = waterIsActive();
  uint32_t now = millis();

  if (watering && !wasWateringActive) {
    // A watering cycle just started. The countdown screen is the default.
    userLeftRunningScreen = false;
    setScreen(SCR_RUNNING, false);
  }

  if (watering) {
    if (screen != SCR_RUNNING && userLeftRunningScreen && (now - lastUserInteractionMs >= RUNNING_RETURN_IDLE_MS)) {
      // User navigated away, then went idle. Return to the live meter.
      userLeftRunningScreen = false;
      setScreen(SCR_RUNNING, false);
    } else if (screen == SCR_HOME && !userLeftRunningScreen) {
      // While watering is active, Home is not the default unless the user explicitly navigated there.
      setScreen(SCR_RUNNING, false);
    }
  } else {
    userLeftRunningScreen = false;
    if (screen == SCR_RUNNING) {
      setScreen(SCR_HOME, false);
    }
  }

  wasWateringActive = watering;

  if (!relayConnected && cfg.staSsid.length() == 0 && screen == SCR_HOME) {
    setScreen(SCR_SETUP_ERROR, false);
  }
}

// ----------------------------- Setup / Loop ---------------------------------

void initPowerAndDisplay() {
  pinMode(LCD_PWR_EN1, OUTPUT);
  pinMode(LCD_PWR_EN2, OUTPUT);
  digitalWrite(LCD_PWR_EN1, HIGH);
  digitalWrite(LCD_PWR_EN2, HIGH);

  pinMode(TFT_BLK, OUTPUT);
  analogWrite(TFT_BLK, cfg.brightness);

  pinMode(POWER_LIGHT_PIN, OUTPUT);
  digitalWrite(POWER_LIGHT_PIN, HIGH);

  gfx->begin();
  gfx->fillScreen(C_BG);
  drawCenteredText("GardenKnob", 96, 2, C_TEXT);
  drawCenteredText("Booting...", 124, 1, C_DIM);
}

void initPixels() {
  pixels.begin();
  pixels.setBrightness(25);
  pixels.clear();
  fillAllLeds(pixels.Color(0, 12, 4));
}

void setup() {
  Serial.begin(115200);
  delay(100);
  loadConfig();
  computePolyCentroids();
  setupMapTransform();
  initPixels();
  initPowerAndDisplay();
  initTouch();
  initEncoder();
  setupWiFi();
  setupWebServer();
  clearRuns();

  lastStatePollMs = millis() - 2500;
  lastSchedulePollMs = millis() - 30000;
  lastTimePollMs = millis() - 60000;

  selectedZone = cfg.lastSelectedZone;
  selectedItem = selectedZone - 1;
  durationMinutes = cfg.manualDefaultMinutes;
  screen = cfg.staSsid.length() == 0 ? SCR_SETUP_ERROR : SCR_HOME;
  updateZoneLeds(true);
  needsRedraw = true;
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();

  if (restartPending && millis() >= restartAtMs) {
    delay(50);
    ESP.restart();
  }

  if (staReconnectPending && millis() >= staReconnectAtMs) {
    staReconnectPending = false;
    WiFi.disconnect(false, false);
    if (cfg.staSsid.length() > 0) {
      WiFi.begin(cfg.staSsid.c_str(), cfg.staPass.c_str());
    }
  }

  if (configSavedNotice && millis() > configSavedNoticeUntil) {
    configSavedNotice = false;
    needsRedraw = true;
  }

  if (!staReconnectPending && WiFi.status() != WL_CONNECTED && cfg.staSsid.length() > 0) {
    static uint32_t lastReconnect = 0;
    if (millis() - lastReconnect > 10000) {
      lastReconnect = millis();
      WiFi.disconnect();
      WiFi.begin(cfg.staSsid.c_str(), cfg.staPass.c_str());
    }
  }

  processInput();
  pollRelayTasks();
  updateScreenAutoMode();

  if (millis() < commandMessageUntil) needsRedraw = true;

  updateZoneLeds(false);

  // Avoid high-frequency full-screen redraws on the TFT.
  // The GC9A01 redraw is visible line-by-line; redrawing animated screens every ~180 ms
  // caused severe flashing. Only command-pending and diagnostics keep moderate animation.
  // Avoid full-screen redraws unless visible state changed.
  // Clearing the GC9A01 to black and repainting the whole frame is visibly flashy.
  bool animatedScreen = screen == SCR_DIAGNOSTICS;
  uint32_t redrawInterval = commandPending ? 500 : 1000;
  bool slowRunningRefresh = (screen == SCR_RUNNING && millis() - lastDrawMs > 1000);
  bool animatedRefresh = (animatedScreen && millis() - lastDrawMs > redrawInterval);

  if (needsRedraw || slowRunningRefresh || animatedRefresh) {
    drawUI();
  }

  delay(8);
}

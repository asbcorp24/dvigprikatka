#pragma once

#include <Arduino.h>
#include <Wire.h>

#define U8G2_R0 0
#define U8X8_PIN_NONE 255
#define u8g2_font_6x12_tf ((const uint8_t*)1)
#define u8g2_font_logisoso24_tf ((const uint8_t*)2)

class U8G2_SH1106_128X64_NONAME_F_HW_I2C {
public:
    U8G2_SH1106_128X64_NONAME_F_HW_I2C(uint8_t rotation = U8G2_R0, uint8_t reset = U8X8_PIN_NONE);

    void begin();
    void setContrast(uint8_t value);
    void clearBuffer();
    void sendBuffer();
    void setFont(const uint8_t *font);
    void setDrawColor(uint8_t color);
    void drawBox(int16_t x, int16_t y, int16_t w, int16_t h);
    void drawUTF8(int16_t x, int16_t y, const char *text);
    int16_t getUTF8Width(const char *text) const;

private:
    static constexpr uint8_t WIDTH = 128;
    static constexpr uint8_t HEIGHT = 64;
    static constexpr uint8_t ADDRESS = 0x3C;

    uint8_t buffer[WIDTH * HEIGHT / 8] = {0};
    uint8_t drawColor = 1;
    uint8_t scale = 1;

    void command(uint8_t c);
    void pixel(int16_t x, int16_t y, bool on);
    void drawChar(int16_t x, int16_t baselineY, char c);
    const uint8_t *glyph(char c) const;
};

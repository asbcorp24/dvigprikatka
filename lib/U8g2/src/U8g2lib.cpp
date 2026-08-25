#include "U8g2lib.h"
#include <cstring>

U8G2_SH1106_128X64_NONAME_F_HW_I2C::U8G2_SH1106_128X64_NONAME_F_HW_I2C(uint8_t, uint8_t) {}

void U8G2_SH1106_128X64_NONAME_F_HW_I2C::command(uint8_t c) {
    Wire.beginTransmission(ADDRESS);
    Wire.write(0x00);
    Wire.write(c);
    Wire.endTransmission();
}

void U8G2_SH1106_128X64_NONAME_F_HW_I2C::begin() {
    command(0xAE);
    command(0xD5); command(0x80);
    command(0xA8); command(0x3F);
    command(0xD3); command(0x00);
    command(0x40);
    command(0xAD); command(0x8B);
    command(0xA1);
    command(0xC8);
    command(0xDA); command(0x12);
    command(0x81); command(0xFF);
    command(0xD9); command(0x1F);
    command(0xDB); command(0x40);
    command(0xA4);
    command(0xA6);
    command(0xAF);
    clearBuffer();
    sendBuffer();
}

void U8G2_SH1106_128X64_NONAME_F_HW_I2C::setContrast(uint8_t value) {
    command(0x81);
    command(value);
}

void U8G2_SH1106_128X64_NONAME_F_HW_I2C::clearBuffer() {
    memset(buffer, 0, sizeof(buffer));
}

void U8G2_SH1106_128X64_NONAME_F_HW_I2C::sendBuffer() {
    for (uint8_t page = 0; page < 8; ++page) {
        command(0xB0 + page);
        command(0x02);
        command(0x10);

        for (uint8_t x = 0; x < WIDTH; x += 16) {
            Wire.beginTransmission(ADDRESS);
            Wire.write(0x40);
            for (uint8_t i = 0; i < 16; ++i) {
                Wire.write(buffer[page * WIDTH + x + i]);
            }
            Wire.endTransmission();
        }
    }
}

void U8G2_SH1106_128X64_NONAME_F_HW_I2C::setFont(const uint8_t *font) {
    scale = (font == u8g2_font_logisoso24_tf) ? 3 : 1;
}

void U8G2_SH1106_128X64_NONAME_F_HW_I2C::setDrawColor(uint8_t color) {
    drawColor = color ? 1 : 0;
}

void U8G2_SH1106_128X64_NONAME_F_HW_I2C::pixel(int16_t x, int16_t y, bool on) {
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
    uint16_t index = x + (y / 8) * WIDTH;
    uint8_t mask = 1 << (y & 7);
    if (on) buffer[index] |= mask;
    else buffer[index] &= ~mask;
}

void U8G2_SH1106_128X64_NONAME_F_HW_I2C::drawBox(int16_t x, int16_t y, int16_t w, int16_t h) {
    for (int16_t yy = y; yy < y + h; ++yy)
        for (int16_t xx = x; xx < x + w; ++xx)
            pixel(xx, yy, drawColor != 0);
}

const uint8_t *U8G2_SH1106_128X64_NONAME_F_HW_I2C::glyph(char c) const {
    static const uint8_t blank[5] = {0,0,0,0,0};
    static const uint8_t table[][5] = {
        {0x3E,0x51,0x49,0x45,0x3E}, // 0
        {0x00,0x42,0x7F,0x40,0x00}, // 1
        {0x42,0x61,0x51,0x49,0x46}, // 2
        {0x21,0x41,0x45,0x4B,0x31}, // 3
        {0x18,0x14,0x12,0x7F,0x10}, // 4
        {0x27,0x45,0x45,0x45,0x39}, // 5
        {0x3C,0x4A,0x49,0x49,0x30}, // 6
        {0x01,0x71,0x09,0x05,0x03}, // 7
        {0x36,0x49,0x49,0x49,0x36}, // 8
        {0x06,0x49,0x49,0x29,0x1E}, // 9
        {0x7E,0x11,0x11,0x11,0x7E}, // A
        {0x7F,0x49,0x49,0x49,0x36}, // B
        {0x3E,0x41,0x41,0x41,0x22}, // C
        {0x7F,0x41,0x41,0x22,0x1C}, // D
        {0x7F,0x49,0x49,0x49,0x41}, // E
        {0x7F,0x09,0x09,0x09,0x01}, // F
        {0x3E,0x41,0x49,0x49,0x7A}, // G
        {0x7F,0x08,0x08,0x08,0x7F}, // H
        {0x00,0x41,0x7F,0x41,0x00}, // I
        {0x20,0x40,0x41,0x3F,0x01}, // J
        {0x7F,0x08,0x14,0x22,0x41}, // K
        {0x7F,0x40,0x40,0x40,0x40}, // L
        {0x7F,0x02,0x0C,0x02,0x7F}, // M
        {0x7F,0x04,0x08,0x10,0x7F}, // N
        {0x3E,0x41,0x41,0x41,0x3E}, // O
        {0x7F,0x09,0x09,0x09,0x06}, // P
        {0x3E,0x41,0x51,0x21,0x5E}, // Q
        {0x7F,0x09,0x19,0x29,0x46}, // R
        {0x46,0x49,0x49,0x49,0x31}, // S
        {0x01,0x01,0x7F,0x01,0x01}, // T
        {0x3F,0x40,0x40,0x40,0x3F}, // U
        {0x1F,0x20,0x40,0x20,0x1F}, // V
        {0x7F,0x20,0x18,0x20,0x7F}, // W
        {0x63,0x14,0x08,0x14,0x63}, // X
        {0x03,0x04,0x78,0x04,0x03}, // Y
        {0x61,0x51,0x49,0x45,0x43}  // Z
    };
    static const uint8_t colon[5] = {0,0x36,0x36,0,0};
    static const uint8_t dash[5] = {0x08,0x08,0x08,0x08,0x08};
    static const uint8_t slash[5] = {0x20,0x10,0x08,0x04,0x02};
    static const uint8_t equal[5] = {0x14,0x14,0x14,0x14,0x14};
    static const uint8_t gt[5] = {0x00,0x41,0x22,0x14,0x08};
    static const uint8_t dot[5] = {0,0x60,0x60,0,0};

    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    if (c >= '0' && c <= '9') return table[c - '0'];
    if (c >= 'A' && c <= 'Z') return table[10 + c - 'A'];
    if (c == ':') return colon;
    if (c == '-') return dash;
    if (c == '/') return slash;
    if (c == '=') return equal;
    if (c == '>') return gt;
    if (c == '.') return dot;
    return blank;
}

void U8G2_SH1106_128X64_NONAME_F_HW_I2C::drawChar(int16_t x, int16_t baselineY, char c) {
    const uint8_t *g = glyph(c);
    int16_t top = baselineY - 7 * scale;

    for (uint8_t col = 0; col < 5; ++col) {
        uint8_t bits = g[col];
        for (uint8_t row = 0; row < 7; ++row) {
            if (bits & (1 << row)) {
                for (uint8_t sx = 0; sx < scale; ++sx)
                    for (uint8_t sy = 0; sy < scale; ++sy)
                        pixel(x + col * scale + sx, top + row * scale + sy, drawColor != 0);
            }
        }
    }
}

void U8G2_SH1106_128X64_NONAME_F_HW_I2C::drawUTF8(int16_t x, int16_t y, const char *text) {
    if (!text) return;
    while (*text) {
        unsigned char c = (unsigned char)*text++;
        if (c < 0x80) {
            drawChar(x, y, (char)c);
            x += 6 * scale;
        }
    }
}

int16_t U8G2_SH1106_128X64_NONAME_F_HW_I2C::getUTF8Width(const char *text) const {
    if (!text) return 0;
    int16_t count = 0;
    while (*text) {
        unsigned char c = (unsigned char)*text++;
        if (c < 0x80) ++count;
    }
    return count * 6 * scale;
}

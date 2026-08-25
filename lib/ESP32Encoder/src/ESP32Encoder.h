#pragma once

#include <Arduino.h>
#include <driver/pcnt.h>

enum class puType : uint8_t {
    up,
    down,
    none
};

class ESP32Encoder {
public:
    ESP32Encoder() = default;

    void attachHalfQuad(int aPinNumber, int bPinNumber);
    int64_t getCount();
    int64_t clearCount();
    void setCount(int64_t value);
    void detach();

    static puType useInternalWeakPullResistors;

private:
    pcnt_unit_t unit = PCNT_UNIT_0;
    bool attached = false;
    int64_t offset = 0;
};

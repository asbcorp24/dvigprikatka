#include "ESP32Encoder.h"
#include <driver/gpio.h>

puType ESP32Encoder::useInternalWeakPullResistors = puType::up;

void ESP32Encoder::attachHalfQuad(int aPinNumber, int bPinNumber) {
    if (attached) detach();

    if (useInternalWeakPullResistors == puType::up) {
        gpio_set_pull_mode((gpio_num_t)aPinNumber, GPIO_PULLUP_ONLY);
        gpio_set_pull_mode((gpio_num_t)bPinNumber, GPIO_PULLUP_ONLY);
    } else if (useInternalWeakPullResistors == puType::down) {
        gpio_set_pull_mode((gpio_num_t)aPinNumber, GPIO_PULLDOWN_ONLY);
        gpio_set_pull_mode((gpio_num_t)bPinNumber, GPIO_PULLDOWN_ONLY);
    } else {
        gpio_set_pull_mode((gpio_num_t)aPinNumber, GPIO_FLOATING);
        gpio_set_pull_mode((gpio_num_t)bPinNumber, GPIO_FLOATING);
    }

    pcnt_config_t cfg = {};
    cfg.pulse_gpio_num = aPinNumber;
    cfg.ctrl_gpio_num = bPinNumber;
    cfg.channel = PCNT_CHANNEL_0;
    cfg.unit = unit;
    cfg.pos_mode = PCNT_COUNT_DEC;
    cfg.neg_mode = PCNT_COUNT_INC;
    cfg.lctrl_mode = PCNT_MODE_REVERSE;
    cfg.hctrl_mode = PCNT_MODE_KEEP;
    cfg.counter_h_lim = 32767;
    cfg.counter_l_lim = -32768;

    pcnt_unit_config(&cfg);
    pcnt_set_filter_value(unit, 250);
    pcnt_filter_enable(unit);
    pcnt_counter_pause(unit);
    pcnt_counter_clear(unit);
    pcnt_counter_resume(unit);

    offset = 0;
    attached = true;
}

int64_t ESP32Encoder::getCount() {
    int16_t value = 0;
    if (attached) pcnt_get_counter_value(unit, &value);
    return offset + value;
}

int64_t ESP32Encoder::clearCount() {
    int64_t old = getCount();
    if (attached) pcnt_counter_clear(unit);
    offset = 0;
    return old;
}

void ESP32Encoder::setCount(int64_t value) {
    if (attached) pcnt_counter_clear(unit);
    offset = value;
}

void ESP32Encoder::detach() {
    if (!attached) return;
    pcnt_counter_pause(unit);
    pcnt_counter_clear(unit);
    attached = false;
    offset = 0;
}

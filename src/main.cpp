#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Preferences.h>
#include <ESP32Encoder.h>

static constexpr uint8_t PIN_SDA = 21;
static constexpr uint8_t PIN_SCL = 22;
static constexpr uint8_t PIN_ENC_A = 32;
static constexpr uint8_t PIN_ENC_B = 33;
static constexpr uint8_t PIN_ENC_PUSH = 25;
static constexpr uint8_t PIN_BACK = 26;
static constexpr uint8_t PIN_CONFIRM = 27;
static constexpr uint8_t PIN_RELAY_M1_RIGHT = 16;
static constexpr uint8_t PIN_RELAY_M1_LEFT = 17;
static constexpr uint8_t PIN_RELAY_M2_RIGHT = 18;
static constexpr uint8_t PIN_RELAY_M2_LEFT = 19;

static constexpr bool RELAY_ACTIVE_LOW = true;
static constexpr uint32_t DIRECTION_DEAD_TIME_MS = 800;
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 35;
static constexpr uint32_t DISPLAY_UPDATE_MS = 150;

static constexpr uint16_t MIN_DIRECTION_MINUTES = 1;
static constexpr uint16_t MAX_TOTAL_MINUTES = 120;

U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);
Preferences preferences;
ESP32Encoder encoder;

uint16_t rightMinutes = 1;
uint16_t leftMinutes = 2;
uint16_t totalMinutes = 30;

uint16_t cycleMinutes() {
    return rightMinutes + leftMinutes;
}

uint16_t maxRightMinutes() {
    return MAX_TOTAL_MINUTES - leftMinutes;
}

uint16_t maxLeftMinutes() {
    return MAX_TOTAL_MINUTES - rightMinutes;
}

void ensureTotalAtLeastCycle() {
    const uint16_t minTotal = cycleMinutes();
    if (totalMinutes < minTotal) totalMinutes = minTotal;
    if (totalMinutes > MAX_TOTAL_MINUTES) totalMinutes = MAX_TOTAL_MINUTES;
}

void normalizeSettings() {
    if (rightMinutes < MIN_DIRECTION_MINUTES) rightMinutes = MIN_DIRECTION_MINUTES;
    if (leftMinutes < MIN_DIRECTION_MINUTES) leftMinutes = MIN_DIRECTION_MINUTES;

    if (rightMinutes >= MAX_TOTAL_MINUTES) {
        rightMinutes = MAX_TOTAL_MINUTES - MIN_DIRECTION_MINUTES;
    }
    if (leftMinutes >= MAX_TOTAL_MINUTES) {
        leftMinutes = MAX_TOTAL_MINUTES - MIN_DIRECTION_MINUTES;
    }

    if (cycleMinutes() > MAX_TOTAL_MINUTES) {
        leftMinutes = MAX_TOTAL_MINUTES - rightMinutes;
        if (leftMinutes < MIN_DIRECTION_MINUTES) {
            leftMinutes = MIN_DIRECTION_MINUTES;
            rightMinutes = MAX_TOTAL_MINUTES - MIN_DIRECTION_MINUTES;
        }
    }

    if (totalMinutes > MAX_TOTAL_MINUTES) totalMinutes = MAX_TOTAL_MINUTES;
    ensureTotalAtLeastCycle();
}

void loadSettings() {
    preferences.begin("dvigprikatka", false);
    rightMinutes = preferences.getUShort("right", 1);
    leftMinutes = preferences.getUShort("left", 2);
    totalMinutes = preferences.getUShort("total", 30);
    normalizeSettings();
}

void saveSettings() {
    normalizeSettings();
    preferences.putUShort("right", rightMinutes);
    preferences.putUShort("left", leftMinutes);
    preferences.putUShort("total", totalMinutes);
}

void writeRelay(uint8_t pin, bool on) {
    const uint8_t active = RELAY_ACTIVE_LOW ? LOW : HIGH;
    const uint8_t inactive = RELAY_ACTIVE_LOW ? HIGH : LOW;
    digitalWrite(pin, on ? active : inactive);
}

void allRelaysOff() {
    writeRelay(PIN_RELAY_M1_RIGHT, false);
    writeRelay(PIN_RELAY_M1_LEFT, false);
    writeRelay(PIN_RELAY_M2_RIGHT, false);
    writeRelay(PIN_RELAY_M2_LEFT, false);
}

enum class MotorPhase : uint8_t {
    STOPPED,
    RIGHT,
    PAUSE_TO_LEFT,
    LEFT,
    PAUSE_TO_RIGHT
};

struct MotorController {
    uint8_t rightPin;
    uint8_t leftPin;
    MotorPhase phase = MotorPhase::STOPPED;
    uint32_t phaseStartedAt = 0;
    uint32_t cycles = 0;

    void off() {
        writeRelay(rightPin, false);
        writeRelay(leftPin, false);
    }

    void setRight() {
        off();
        writeRelay(rightPin, true);
    }

    void setLeft() {
        off();
        writeRelay(leftPin, true);
    }

    void start(uint32_t now) {
        cycles = 0;
        phaseStartedAt = now;
        phase = MotorPhase::RIGHT;
        setRight();
    }

    void stop() {
        off();
        phase = MotorPhase::STOPPED;
    }

    void update(uint32_t now) {
        const uint32_t elapsed = now - phaseStartedAt;

        switch (phase) {
            case MotorPhase::RIGHT:
                if (elapsed >= static_cast<uint32_t>(rightMinutes) * 60000UL) {
                    off();
                    phase = MotorPhase::PAUSE_TO_LEFT;
                    phaseStartedAt = now;
                }
                break;

            case MotorPhase::PAUSE_TO_LEFT:
                if (elapsed >= DIRECTION_DEAD_TIME_MS) {
                    setLeft();
                    phase = MotorPhase::LEFT;
                    phaseStartedAt = now;
                }
                break;

            case MotorPhase::LEFT:
                if (elapsed >= static_cast<uint32_t>(leftMinutes) * 60000UL) {
                    off();
                    cycles++;
                    phase = MotorPhase::PAUSE_TO_RIGHT;
                    phaseStartedAt = now;
                }
                break;

            case MotorPhase::PAUSE_TO_RIGHT:
                if (elapsed >= DIRECTION_DEAD_TIME_MS) {
                    setRight();
                    phase = MotorPhase::RIGHT;
                    phaseStartedAt = now;
                }
                break;

            default:
                break;
        }
    }

    uint32_t remaining(uint32_t now) const {
        uint32_t duration = 0;

        switch (phase) {
            case MotorPhase::RIGHT:
                duration = static_cast<uint32_t>(rightMinutes) * 60000UL;
                break;
            case MotorPhase::LEFT:
                duration = static_cast<uint32_t>(leftMinutes) * 60000UL;
                break;
            case MotorPhase::PAUSE_TO_LEFT:
            case MotorPhase::PAUSE_TO_RIGHT:
                duration = DIRECTION_DEAD_TIME_MS;
                break;
            default:
                return 0;
        }

        const uint32_t elapsed = now - phaseStartedAt;
        return elapsed >= duration ? 0 : duration - elapsed;
    }

    const char *name() const {
        switch (phase) {
            case MotorPhase::RIGHT: return "RIGHT";
            case MotorPhase::LEFT: return "LEFT";
            case MotorPhase::PAUSE_TO_LEFT:
            case MotorPhase::PAUSE_TO_RIGHT: return "PAUSE";
            default: return "STOP";
        }
    }
};

MotorController motor1{PIN_RELAY_M1_RIGHT, PIN_RELAY_M1_LEFT};
MotorController motor2{PIN_RELAY_M2_RIGHT, PIN_RELAY_M2_LEFT};

enum class UiState : uint8_t {
    MENU,
    EDIT_RIGHT,
    EDIT_LEFT,
    EDIT_TOTAL,
    RUNNING,
    FINISHED
};

UiState uiState = UiState::MENU;
uint8_t menuIndex = 0;
uint32_t runStartedAt = 0;
uint32_t lastDisplayAt = 0;

struct AsyncButton {
    uint8_t pin;
    bool rawState = HIGH;
    bool stableState = HIGH;
    uint32_t changedAt = 0;
    bool event = false;

    void begin() {
        pinMode(pin, INPUT_PULLUP);
        rawState = stableState = digitalRead(pin);
        changedAt = millis();
    }

    void update(uint32_t now) {
        const bool raw = digitalRead(pin);
        if (raw != rawState) {
            rawState = raw;
            changedAt = now;
        }
        if (raw != stableState && (now - changedAt) >= BUTTON_DEBOUNCE_MS) {
            stableState = raw;
            if (stableState == LOW) event = true;
        }
    }

    bool pressed() {
        if (!event) return false;
        event = false;
        return true;
    }
};

AsyncButton btnEncoder{PIN_ENC_PUSH};
AsyncButton btnBack{PIN_BACK};
AsyncButton btnConfirm{PIN_CONFIRM};

String mmss(uint32_t ms) {
    const uint32_t sec = ms / 1000UL;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02lu:%02lu",
             static_cast<unsigned long>(sec / 60UL),
             static_cast<unsigned long>(sec % 60UL));
    return String(buf);
}

void text(uint8_t x, uint8_t y, const String &s) {
    display.drawUTF8(x, y, s.c_str());
}

void drawMenu() {
    display.clearBuffer();
    display.setFont(u8g2_font_6x12_tf);

    const char *labels[4] = {"RIGHT", "LEFT", "TOTAL", "START"};
    String values[4] = {
        String(rightMinutes) + " min",
        String(leftMinutes) + " min",
        String(totalMinutes) + " min",
        ""
    };

    for (uint8_t i = 0; i < 4; i++) {
        const uint8_t y = 13 + i * 16;
        if (i == menuIndex) {
            display.drawBox(0, y - 11, 128, 14);
            display.setDrawColor(0);
        }

        text(3, y, String(i == menuIndex ? "> " : "  ") + labels[i]);

        if (i < 3) {
            const int16_t w = display.getUTF8Width(values[i].c_str());
            text(125 - w, y, values[i]);
        }

        if (i == menuIndex) display.setDrawColor(1);
    }

    display.sendBuffer();
}

void drawEdit(const char *title, uint16_t value, uint16_t minValue, uint16_t maxValue) {
    display.clearBuffer();
    display.setFont(u8g2_font_6x12_tf);
    text(0, 12, String("SET ") + title);

    display.setFont(u8g2_font_logisoso24_tf);
    const String v = String(value);
    const int16_t w = display.getUTF8Width(v.c_str());
    text((128 - w) / 2, 43, v);

    display.setFont(u8g2_font_6x12_tf);
    text(0, 62, String(minValue) + "-" + String(maxValue) + " min PUSH=OK");
    display.sendBuffer();
}

void drawRunning(uint32_t now) {
    const uint32_t totalDuration = static_cast<uint32_t>(totalMinutes) * 60000UL;
    const uint32_t elapsed = now - runStartedAt;
    const uint32_t remain = elapsed >= totalDuration ? 0 : totalDuration - elapsed;

    display.clearBuffer();
    display.setFont(u8g2_font_6x12_tf);
    text(0, 12, String("TOTAL: ") + mmss(remain));
    text(0, 27, String("M1: ") + motor1.name() + " " + mmss(motor1.remaining(now)));
    text(0, 42, String("M2: ") + motor2.name() + " " + mmss(motor2.remaining(now)));
    text(0, 57, String("CYCLES: ") + motor1.cycles + " / " + motor2.cycles);
    display.sendBuffer();
}

void drawFinished() {
    display.clearBuffer();
    display.setFont(u8g2_font_6x12_tf);
    text(18, 14, "WORK FINISHED");
    text(5, 31, String("M1 cycles: ") + motor1.cycles);
    text(5, 45, String("M2 cycles: ") + motor2.cycles);
    text(1, 62, "PUSH=MENU CON=START");
    display.sendBuffer();
}

void startRun() {
    normalizeSettings();
    saveSettings();

    const uint32_t now = millis();
    runStartedAt = now;
    lastDisplayAt = 0;

    motor1.start(now);
    motor2.start(now);
    uiState = UiState::RUNNING;
    drawRunning(now);
}

void stopRun(bool finished) {
    motor1.stop();
    motor2.stop();
    allRelaysOff();

    if (finished) {
        uiState = UiState::FINISHED;
        drawFinished();
    } else {
        uiState = UiState::MENU;
        drawMenu();
    }
}

void updateRun(uint32_t now) {
    if (uiState != UiState::RUNNING) return;

    const uint32_t totalDuration = static_cast<uint32_t>(totalMinutes) * 60000UL;
    if ((now - runStartedAt) >= totalDuration) {
        stopRun(true);
        return;
    }

    motor1.update(now);
    motor2.update(now);

    if ((now - lastDisplayAt) >= DISPLAY_UPDATE_MS) {
        lastDisplayAt = now;
        drawRunning(now);
    }
}

void handleEncoderTurn(int8_t step) {
    if (step == 0) return;

    Serial.printf("ENC step=%d raw=%lld\n", step, static_cast<long long>(encoder.getCount()));

    switch (uiState) {
        case UiState::MENU:
            menuIndex = step > 0 ? (menuIndex + 1) % 4 : (menuIndex + 3) % 4;
            drawMenu();
            break;

        case UiState::EDIT_RIGHT:
            if (step > 0 && rightMinutes < maxRightMinutes()) {
                rightMinutes++;
                ensureTotalAtLeastCycle();
            }
            if (step < 0 && rightMinutes > MIN_DIRECTION_MINUTES) {
                rightMinutes--;
            }
            drawEdit("RIGHT", rightMinutes, MIN_DIRECTION_MINUTES, maxRightMinutes());
            break;

        case UiState::EDIT_LEFT:
            if (step > 0 && leftMinutes < maxLeftMinutes()) {
                leftMinutes++;
                ensureTotalAtLeastCycle();
            }
            if (step < 0 && leftMinutes > MIN_DIRECTION_MINUTES) {
                leftMinutes--;
            }
            drawEdit("LEFT", leftMinutes, MIN_DIRECTION_MINUTES, maxLeftMinutes());
            break;

        case UiState::EDIT_TOTAL: {
            const uint16_t minTotal = cycleMinutes();
            if (step > 0 && totalMinutes < MAX_TOTAL_MINUTES) totalMinutes++;
            if (step < 0 && totalMinutes > minTotal) totalMinutes--;
            drawEdit("TOTAL", totalMinutes, cycleMinutes(), MAX_TOTAL_MINUTES);
            break;
        }

        default:
            break;
    }
}

void handleEncoderPush() {
    switch (uiState) {
        case UiState::MENU:
            if (menuIndex == 0) {
                uiState = UiState::EDIT_RIGHT;
                drawEdit("RIGHT", rightMinutes, MIN_DIRECTION_MINUTES, maxRightMinutes());
            } else if (menuIndex == 1) {
                uiState = UiState::EDIT_LEFT;
                drawEdit("LEFT", leftMinutes, MIN_DIRECTION_MINUTES, maxLeftMinutes());
            } else if (menuIndex == 2) {
                uiState = UiState::EDIT_TOTAL;
                drawEdit("TOTAL", totalMinutes, cycleMinutes(), MAX_TOTAL_MINUTES);
            } else {
                startRun();
            }
            break;

        case UiState::EDIT_RIGHT:
        case UiState::EDIT_LEFT:
        case UiState::EDIT_TOTAL:
            saveSettings();
            uiState = UiState::MENU;
            drawMenu();
            break;

        case UiState::RUNNING:
            stopRun(false);
            break;

        case UiState::FINISHED:
            uiState = UiState::MENU;
            drawMenu();
            break;
    }
}

int64_t lastEncoderPosition = 0;

void updateEncoder() {
    const int64_t raw = encoder.getCount();
    const int64_t position = raw / 2;

    while (lastEncoderPosition < position) {
        lastEncoderPosition++;
        handleEncoderTurn(+1);
    }
    while (lastEncoderPosition > position) {
        lastEncoderPosition--;
        handleEncoderTurn(-1);
    }
}

void setup() {
    Serial.begin(115200);

    pinMode(PIN_RELAY_M1_RIGHT, OUTPUT);
    pinMode(PIN_RELAY_M1_LEFT, OUTPUT);
    pinMode(PIN_RELAY_M2_RIGHT, OUTPUT);
    pinMode(PIN_RELAY_M2_LEFT, OUTPUT);
    allRelaysOff();

    btnEncoder.begin();
    btnBack.begin();
    btnConfirm.begin();

    ESP32Encoder::useInternalWeakPullResistors = puType::up;
    encoder.attachHalfQuad(PIN_ENC_A, PIN_ENC_B);
    encoder.clearCount();
    lastEncoderPosition = 0;

    Wire.begin(PIN_SDA, PIN_SCL);
    Wire.setClock(400000);
    display.begin();
    display.setContrast(255);

    loadSettings();
    drawMenu();

    Serial.printf("Settings RIGHT=%u LEFT=%u TOTAL=%u MIN_TOTAL=%u\n",
                  rightMinutes, leftMinutes, totalMinutes, cycleMinutes());
}

void loop() {
    const uint32_t now = millis();

    updateEncoder();

    btnEncoder.update(now);
    btnBack.update(now);
    btnConfirm.update(now);

    if (btnEncoder.pressed()) handleEncoderPush();
    if (btnBack.pressed() && uiState == UiState::RUNNING) stopRun(false);
    if (btnConfirm.pressed() && uiState != UiState::RUNNING) startRun();

    updateRun(now);
}

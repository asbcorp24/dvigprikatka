#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Preferences.h>

// ============================================================
// ESP32 + OLED SH1106 128x64 + EC11 + 4 relay
// Полностью неблокирующая логика: НЕТ delay().
// Все таймеры работают через millis().
// ============================================================

// OLED
static constexpr uint8_t PIN_SDA = 21;
static constexpr uint8_t PIN_SCL = 22;

// Энкодер / кнопки панели
static constexpr uint8_t PIN_ENC_A    = 32; // TRA
static constexpr uint8_t PIN_ENC_B    = 33; // TRB
static constexpr uint8_t PIN_ENC_PUSH = 25; // PSH
static constexpr uint8_t PIN_BACK     = 26; // BAK = STOP
static constexpr uint8_t PIN_CONFIRM  = 27; // CON = START

// Реле
static constexpr uint8_t PIN_RELAY_M1_RIGHT = 16;
static constexpr uint8_t PIN_RELAY_M1_LEFT  = 17;
static constexpr uint8_t PIN_RELAY_M2_RIGHT = 18;
static constexpr uint8_t PIN_RELAY_M2_LEFT  = 19;

static constexpr bool RELAY_ACTIVE_LOW = true;
static constexpr uint32_t DIRECTION_DEAD_TIME_MS = 800;
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 35;
static constexpr uint32_t DISPLAY_UPDATE_MS = 150;

U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);
Preferences preferences;

// ============================================================
// СОХРАНЯЕМЫЕ НАСТРОЙКИ
// ============================================================
uint8_t rightMinutes = 1;     // 1..3
uint8_t leftMinutes = 2;      // 1..3
uint16_t totalMinutes = 30;   // 1..120

void loadSettings() {
    preferences.begin("dvigprikatka", false);
    rightMinutes = preferences.getUChar("right", 1);
    leftMinutes = preferences.getUChar("left", 2);
    totalMinutes = preferences.getUShort("total", 30);

    rightMinutes = constrain(rightMinutes, 1, 3);
    leftMinutes = constrain(leftMinutes, 1, 3);
    totalMinutes = constrain(totalMinutes, 1, 120);
}

void saveSettings() {
    preferences.putUChar("right", rightMinutes);
    preferences.putUChar("left", leftMinutes);
    preferences.putUShort("total", totalMinutes);
}

// ============================================================
// РЕЛЕ
// ============================================================
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

// ============================================================
// АСИНХРОННЫЙ АВТОМАТ ОДНОГО ДВИГАТЕЛЯ
// Каждый двигатель имеет собственный таймер и состояние.
// ============================================================
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

    void relaysOff() {
        writeRelay(rightPin, false);
        writeRelay(leftPin, false);
    }

    void setRight() {
        relaysOff();
        writeRelay(rightPin, true);
    }

    void setLeft() {
        relaysOff();
        writeRelay(leftPin, true);
    }

    void start(uint32_t now) {
        cycles = 0;
        relaysOff();
        phase = MotorPhase::RIGHT;
        phaseStartedAt = now;
        setRight();
    }

    void stop() {
        relaysOff();
        phase = MotorPhase::STOPPED;
    }

    void update(uint32_t now) {
        const uint32_t elapsed = now - phaseStartedAt;

        switch (phase) {
            case MotorPhase::RIGHT:
                if (elapsed >= (uint32_t)rightMinutes * 60000UL) {
                    relaysOff();
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
                if (elapsed >= (uint32_t)leftMinutes * 60000UL) {
                    relaysOff();
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

            case MotorPhase::STOPPED:
            default:
                break;
        }
    }

    uint32_t phaseRemaining(uint32_t now) const {
        uint32_t duration = 0;
        switch (phase) {
            case MotorPhase::RIGHT:
                duration = (uint32_t)rightMinutes * 60000UL;
                break;
            case MotorPhase::LEFT:
                duration = (uint32_t)leftMinutes * 60000UL;
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

    const char *phaseName() const {
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

// ============================================================
// СОСТОЯНИЕ ПРОГРАММЫ
// ============================================================
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

// ============================================================
// АСИНХРОННАЯ ОБРАБОТКА КНОПОК
// ============================================================
struct AsyncButton {
    uint8_t pin;
    bool rawState = HIGH;
    bool stableState = HIGH;
    uint32_t rawChangedAt = 0;
    bool pressEvent = false;

    void begin() {
        pinMode(pin, INPUT_PULLUP);
        rawState = digitalRead(pin);
        stableState = rawState;
        rawChangedAt = millis();
    }

    void update(uint32_t now) {
        const bool raw = digitalRead(pin);

        if (raw != rawState) {
            rawState = raw;
            rawChangedAt = now;
        }

        if (raw != stableState && (now - rawChangedAt) >= BUTTON_DEBOUNCE_MS) {
            stableState = raw;
            if (stableState == LOW) {
                pressEvent = true;
            }
        }
    }

    bool pressed() {
        if (!pressEvent) return false;
        pressEvent = false;
        return true;
    }
};

AsyncButton btnEncoder{PIN_ENC_PUSH};
AsyncButton btnBack{PIN_BACK};
AsyncButton btnConfirm{PIN_CONFIRM};

// ============================================================
// ЭНКОДЕР - НЕБЛОКИРУЮЩИЙ ОПРОС
// ============================================================
uint8_t lastEncoderState = 0;
int8_t encoderAccumulator = 0;

int8_t pollEncoder() {
    static const int8_t table[16] = {
         0, -1,  1,  0,
         1,  0,  0, -1,
        -1,  0,  0,  1,
         0,  1, -1,  0
    };

    const uint8_t a = digitalRead(PIN_ENC_A) ? 1 : 0;
    const uint8_t b = digitalRead(PIN_ENC_B) ? 1 : 0;
    const uint8_t current = (a << 1) | b;

    encoderAccumulator += table[(lastEncoderState << 2) | current];
    lastEncoderState = current;

    if (encoderAccumulator >= 4) {
        encoderAccumulator = 0;
        return 1;
    }
    if (encoderAccumulator <= -4) {
        encoderAccumulator = 0;
        return -1;
    }
    return 0;
}

// ============================================================
// ЭКРАН
// ============================================================
String mmss(uint32_t ms) {
    const uint32_t sec = ms / 1000UL;
    const uint32_t min = sec / 60UL;
    const uint32_t rem = sec % 60UL;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02lu:%02lu",
             (unsigned long)min, (unsigned long)rem);
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

void drawEdit(const char *title, uint16_t value, uint16_t maxValue) {
    display.clearBuffer();
    display.setFont(u8g2_font_6x12_tf);
    text(0, 12, String("SET ") + title);

    display.setFont(u8g2_font_logisoso24_tf);
    const String valueText = String(value);
    const int16_t w = display.getUTF8Width(valueText.c_str());
    text((128 - w) / 2, 43, valueText);

    display.setFont(u8g2_font_6x12_tf);
    text(0, 62, String("1-") + maxValue + " min PUSH=OK");
    display.sendBuffer();
}

void drawRunning(uint32_t now) {
    const uint32_t totalDuration = (uint32_t)totalMinutes * 60000UL;
    const uint32_t elapsed = now - runStartedAt;
    const uint32_t remain = elapsed >= totalDuration ? 0 : totalDuration - elapsed;

    display.clearBuffer();
    display.setFont(u8g2_font_6x12_tf);

    text(0, 11, String("TOTAL ") + mmss(remain));
    text(0, 25, String("M1 ") + motor1.phaseName() + " C:" + motor1.cycles);
    text(0, 39, String("   left ") + mmss(motor1.phaseRemaining(now)));
    text(0, 53, String("M2 ") + motor2.phaseName() + " C:" + motor2.cycles);
    text(0, 64, "PUSH/BACK = STOP");

    display.sendBuffer();
}

void drawFinished() {
    display.clearBuffer();
    display.setFont(u8g2_font_6x12_tf);
    text(19, 14, "WORK FINISHED");
    text(5, 31, String("M1 cycles: ") + motor1.cycles);
    text(5, 45, String("M2 cycles: ") + motor2.cycles);
    text(1, 62, "PUSH=MENU CON=START");
    display.sendBuffer();
}

// ============================================================
// ЗАПУСК / ОСТАНОВ
// ============================================================
void startRun() {
    saveSettings();

    const uint32_t now = millis();
    runStartedAt = now;
    lastDisplayAt = 0;

    // Оба двигателя имеют независимые автоматы состояний.
    // Они запускаются одновременно, но дальше обслуживаются независимо.
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

// ============================================================
// НЕБЛОКИРУЮЩЕЕ ОБСЛУЖИВАНИЕ РАБОТЫ
// ============================================================
void updateRun(uint32_t now) {
    if (uiState != UiState::RUNNING) return;

    const uint32_t totalDuration = (uint32_t)totalMinutes * 60000UL;

    // Общий таймер имеет наивысший приоритет.
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

// ============================================================
// UI EVENTS
// ============================================================
void handleEncoderTurn(int8_t step) {
    if (step == 0) return;

    switch (uiState) {
        case UiState::MENU:
            if (step > 0) menuIndex = (menuIndex + 1) % 4;
            else menuIndex = (menuIndex + 3) % 4;
            drawMenu();
            break;

        case UiState::EDIT_RIGHT:
            if (step > 0 && rightMinutes < 3) rightMinutes++;
            if (step < 0 && rightMinutes > 1) rightMinutes--;
            drawEdit("RIGHT", rightMinutes, 3);
            break;

        case UiState::EDIT_LEFT:
            if (step > 0 && leftMinutes < 3) leftMinutes++;
            if (step < 0 && leftMinutes > 1) leftMinutes--;
            drawEdit("LEFT", leftMinutes, 3);
            break;

        case UiState::EDIT_TOTAL:
            if (step > 0 && totalMinutes < 120) totalMinutes++;
            if (step < 0 && totalMinutes > 1) totalMinutes--;
            drawEdit("TOTAL", totalMinutes, 120);
            break;

        case UiState::RUNNING:
        case UiState::FINISHED:
            break;
    }
}

void handleEncoderPush() {
    switch (uiState) {
        case UiState::MENU:
            switch (menuIndex) {
                case 0:
                    uiState = UiState::EDIT_RIGHT;
                    drawEdit("RIGHT", rightMinutes, 3);
                    break;
                case 1:
                    uiState = UiState::EDIT_LEFT;
                    drawEdit("LEFT", leftMinutes, 3);
                    break;
                case 2:
                    uiState = UiState::EDIT_TOTAL;
                    drawEdit("TOTAL", totalMinutes, 120);
                    break;
                case 3:
                    startRun();
                    break;
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

// ============================================================
// SETUP
// ============================================================
void setup() {
    Serial.begin(115200);

    pinMode(PIN_RELAY_M1_RIGHT, OUTPUT);
    pinMode(PIN_RELAY_M1_LEFT, OUTPUT);
    pinMode(PIN_RELAY_M2_RIGHT, OUTPUT);
    pinMode(PIN_RELAY_M2_LEFT, OUTPUT);
    allRelaysOff();

    pinMode(PIN_ENC_A, INPUT_PULLUP);
    pinMode(PIN_ENC_B, INPUT_PULLUP);
    btnEncoder.begin();
    btnBack.begin();
    btnConfirm.begin();

    const uint8_t a = digitalRead(PIN_ENC_A) ? 1 : 0;
    const uint8_t b = digitalRead(PIN_ENC_B) ? 1 : 0;
    lastEncoderState = (a << 1) | b;

    Wire.begin(PIN_SDA, PIN_SCL);
    display.begin();
    display.setContrast(255);

    loadSettings();
    drawMenu();
}

// ============================================================
// LOOP: НИКАКИХ delay(), ВСЕ ЗАДАЧИ ВЫПОЛНЯЮТСЯ ПАРАЛЛЕЛЬНО
// ============================================================
void loop() {
    const uint32_t now = millis();

    // 1. Энкодер постоянно опрашивается независимо от работы двигателей.
    handleEncoderTurn(pollEncoder());

    // 2. Все кнопки обновляются независимо.
    btnEncoder.update(now);
    btnBack.update(now);
    btnConfirm.update(now);

    // 3. Встроенная кнопка энкодера управляет меню / START / STOP.
    if (btnEncoder.pressed()) {
        handleEncoderPush();
    }

    // 4. BACK всегда является быстрым STOP во время работы.
    if (btnBack.pressed()) {
        if (uiState == UiState::RUNNING) {
            stopRun(false);
        }
    }

    // 5. CONFIRM дублирует START.
    if (btnConfirm.pressed()) {
        if (uiState != UiState::RUNNING) {
            startRun();
        }
    }

    // 6. Оба двигателя и общий таймер обновляются независимо.
    updateRun(now);
}

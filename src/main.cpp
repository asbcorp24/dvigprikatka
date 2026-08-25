#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Preferences.h>

// ============================================================
// OLED 1.3" SH1106 + EC11 + BACK/CONFIRM
// ============================================================
static constexpr uint8_t PIN_SDA = 21;
static constexpr uint8_t PIN_SCL = 22;

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
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 40;

U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);
Preferences preferences;

// ============================================================
// НАСТРОЙКИ
// ============================================================
uint8_t rightMinutes = 1;   // 1..3
uint8_t leftMinutes  = 2;   // 1..3
uint16_t totalMinutes = 30; // 1..120

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

void motorsRight() {
    allRelaysOff();
    writeRelay(PIN_RELAY_M1_RIGHT, true);
    writeRelay(PIN_RELAY_M2_RIGHT, true);
}

void motorsLeft() {
    allRelaysOff();
    writeRelay(PIN_RELAY_M1_LEFT, true);
    writeRelay(PIN_RELAY_M2_LEFT, true);
}

// ============================================================
// UI
// ============================================================
enum class UiState : uint8_t {
    MENU,
    EDIT_RIGHT,
    EDIT_LEFT,
    EDIT_TOTAL,
    RUNNING,
    FINISHED
};

enum class RunPhase : uint8_t {
    RIGHT,
    PAUSE_TO_LEFT,
    LEFT,
    PAUSE_TO_RIGHT,
    IDLE
};

UiState uiState = UiState::MENU;
RunPhase runPhase = RunPhase::IDLE;
uint8_t menuIndex = 0;

uint32_t runStartedAt = 0;
uint32_t phaseStartedAt = 0;
uint32_t lastScreenUpdate = 0;
uint32_t completedCycles = 0;

String mmss(uint32_t ms) {
    const uint32_t sec = ms / 1000UL;
    const uint32_t min = sec / 60UL;
    const uint32_t rem = sec % 60UL;
    char b[16];
    snprintf(b, sizeof(b), "%02lu:%02lu", (unsigned long)min, (unsigned long)rem);
    return String(b);
}

void drawText(uint8_t x, uint8_t y, const String &s) {
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
        drawText(3, y, String(i == menuIndex ? "> " : "  ") + labels[i]);
        if (i < 3) {
            const int16_t w = display.getUTF8Width(values[i].c_str());
            drawText(125 - w, y, values[i]);
        }
        if (i == menuIndex) display.setDrawColor(1);
    }

    display.sendBuffer();
}

void drawEdit(const char *title, uint16_t value, uint16_t maxValue) {
    display.clearBuffer();
    display.setFont(u8g2_font_6x12_tf);
    drawText(0, 12, String("SET ") + title);

    display.setFont(u8g2_font_logisoso24_tf);
    String v = String(value);
    int16_t w = display.getUTF8Width(v.c_str());
    drawText((128 - w) / 2, 43, v);

    display.setFont(u8g2_font_6x12_tf);
    drawText(0, 62, String("1-") + maxValue + " min  PUSH=OK");
    display.sendBuffer();
}

void drawRunning() {
    const uint32_t now = millis();
    const uint32_t totalDuration = (uint32_t)totalMinutes * 60000UL;
    const uint32_t totalElapsed = now - runStartedAt;
    const uint32_t totalRemain = totalElapsed >= totalDuration ? 0 : totalDuration - totalElapsed;

    String phaseName = "STOP";
    uint32_t phaseDuration = 0;

    switch (runPhase) {
        case RunPhase::RIGHT:
            phaseName = "RIGHT";
            phaseDuration = (uint32_t)rightMinutes * 60000UL;
            break;
        case RunPhase::LEFT:
            phaseName = "LEFT";
            phaseDuration = (uint32_t)leftMinutes * 60000UL;
            break;
        case RunPhase::PAUSE_TO_LEFT:
        case RunPhase::PAUSE_TO_RIGHT:
            phaseName = "PAUSE";
            phaseDuration = DIRECTION_DEAD_TIME_MS;
            break;
        default:
            break;
    }

    const uint32_t phaseElapsed = now - phaseStartedAt;
    const uint32_t phaseRemain = phaseElapsed >= phaseDuration ? 0 : phaseDuration - phaseElapsed;

    display.clearBuffer();
    display.setFont(u8g2_font_6x12_tf);
    drawText(0, 12, String("CYCLE: ") + (completedCycles + 1));
    drawText(0, 27, String("MODE : ") + phaseName);
    drawText(0, 42, String("PHASE: ") + mmss(phaseRemain));
    drawText(0, 57, String("TOTAL: ") + mmss(totalRemain));
    display.sendBuffer();
}

void drawFinished() {
    display.clearBuffer();
    display.setFont(u8g2_font_6x12_tf);
    drawText(18, 16, "WORK FINISHED");
    drawText(12, 34, String("CYCLES: ") + completedCycles);
    drawText(3, 54, "PUSH=MENU CON=START");
    display.sendBuffer();
}

// ============================================================
// ЭНКОДЕР И КНОПКИ
// ============================================================
uint8_t lastEncoderState = 0;
int8_t encoderAccumulator = 0;

int8_t readEncoderStep() {
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

struct Button {
    uint8_t pin;
    bool lastRaw = HIGH;
    bool stable = HIGH;
    uint32_t changedAt = 0;

    bool pressed() {
        const bool raw = digitalRead(pin);
        const uint32_t now = millis();
        if (raw != lastRaw) {
            lastRaw = raw;
            changedAt = now;
        }
        if ((now - changedAt) >= BUTTON_DEBOUNCE_MS && raw != stable) {
            stable = raw;
            if (stable == LOW) return true;
        }
        return false;
    }
};

Button btnEncoder{PIN_ENC_PUSH};
Button btnBack{PIN_BACK};
Button btnConfirm{PIN_CONFIRM};

// ============================================================
// РАБОТА ДВИГАТЕЛЕЙ
// ============================================================
void startRun() {
    saveSettings();
    completedCycles = 0;
    runStartedAt = millis();
    phaseStartedAt = runStartedAt;
    runPhase = RunPhase::RIGHT;
    uiState = UiState::RUNNING;
    motorsRight();
    drawRunning();
}

void stopRun(bool finished) {
    allRelaysOff();
    runPhase = RunPhase::IDLE;

    if (finished) {
        uiState = UiState::FINISHED;
        drawFinished();
    } else {
        uiState = UiState::MENU;
        drawMenu();
    }
}

void updateRun() {
    if (uiState != UiState::RUNNING) return;

    const uint32_t now = millis();
    const uint32_t totalDuration = (uint32_t)totalMinutes * 60000UL;

    // Общее время имеет приоритет: останавливаемся точно по заданному времени,
    // даже если текущая фаза ещё не закончилась.
    if ((now - runStartedAt) >= totalDuration) {
        stopRun(true);
        return;
    }

    const uint32_t phaseElapsed = now - phaseStartedAt;

    switch (runPhase) {
        case RunPhase::RIGHT:
            if (phaseElapsed >= (uint32_t)rightMinutes * 60000UL) {
                allRelaysOff();
                runPhase = RunPhase::PAUSE_TO_LEFT;
                phaseStartedAt = now;
            }
            break;

        case RunPhase::PAUSE_TO_LEFT:
            if (phaseElapsed >= DIRECTION_DEAD_TIME_MS) {
                motorsLeft();
                runPhase = RunPhase::LEFT;
                phaseStartedAt = now;
            }
            break;

        case RunPhase::LEFT:
            if (phaseElapsed >= (uint32_t)leftMinutes * 60000UL) {
                allRelaysOff();
                completedCycles++;
                runPhase = RunPhase::PAUSE_TO_RIGHT;
                phaseStartedAt = now;
            }
            break;

        case RunPhase::PAUSE_TO_RIGHT:
            if (phaseElapsed >= DIRECTION_DEAD_TIME_MS) {
                motorsRight();
                runPhase = RunPhase::RIGHT;
                phaseStartedAt = now;
            }
            break;

        default:
            break;
    }

    if ((now - lastScreenUpdate) >= 250) {
        lastScreenUpdate = now;
        drawRunning();
    }
}

// ============================================================
// УПРАВЛЕНИЕ МЕНЮ
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

        default:
            break;
    }
}

void handleEncoderPush() {
    switch (uiState) {
        case UiState::MENU:
            if (menuIndex == 0) {
                uiState = UiState::EDIT_RIGHT;
                drawEdit("RIGHT", rightMinutes, 3);
            } else if (menuIndex == 1) {
                uiState = UiState::EDIT_LEFT;
                drawEdit("LEFT", leftMinutes, 3);
            } else if (menuIndex == 2) {
                uiState = UiState::EDIT_TOTAL;
                drawEdit("TOTAL", totalMinutes, 120);
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
            // Нажатие встроенной кнопки энкодера во время работы = STOP.
            stopRun(false);
            break;

        case UiState::FINISHED:
            uiState = UiState::MENU;
            drawMenu();
            break;
    }
}

void setup() {
    Serial.begin(115200);

    pinMode(PIN_RELAY_M1_RIGHT, OUTPUT);
    pinMode(PIN_RELAY_M1_LEFT, OUTPUT);
    pinMode(PIN_RELAY_M2_RIGHT, OUTPUT);
    pinMode(PIN_RELAY_M2_LEFT, OUTPUT);
    allRelaysOff();

    pinMode(PIN_ENC_A, INPUT_PULLUP);
    pinMode(PIN_ENC_B, INPUT_PULLUP);
    pinMode(PIN_ENC_PUSH, INPUT_PULLUP);
    pinMode(PIN_BACK, INPUT_PULLUP);
    pinMode(PIN_CONFIRM, INPUT_PULLUP);

    Wire.begin(PIN_SDA, PIN_SCL);
    display.begin();
    display.setContrast(255);

    loadSettings();

    lastEncoderState = ((digitalRead(PIN_ENC_A) ? 1 : 0) << 1) |
                       (digitalRead(PIN_ENC_B) ? 1 : 0);

    drawMenu();
}

void loop() {
    handleEncoderTurn(readEncoderStep());

    // Основное управление одной встроенной кнопкой энкодера.
    if (btnEncoder.pressed()) {
        handleEncoderPush();
    }

    // CONFIRM дублирует START.
    if (btnConfirm.pressed()) {
        if (uiState != UiState::RUNNING) {
            startRun();
        }
    }

    // BACK дублирует STOP.
    if (btnBack.pressed()) {
        if (uiState == UiState::RUNNING) {
            stopRun(false);
        }
    }

    updateRun();
    delay(1);
}

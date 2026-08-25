#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>

// ===================== HARDWARE =====================
static constexpr uint8_t LCD_ADDRESS = 0x27;
static constexpr uint8_t LCD_COLS = 20;
static constexpr uint8_t LCD_ROWS = 4;
static constexpr uint8_t PIN_I2C_SDA = 21;
static constexpr uint8_t PIN_I2C_SCL = 22;

static constexpr uint8_t PIN_ENC_A   = 32;
static constexpr uint8_t PIN_ENC_B   = 33;
static constexpr uint8_t PIN_ENC_BTN = 25;

static constexpr uint8_t PIN_RELAY_M1_RIGHT = 16;
static constexpr uint8_t PIN_RELAY_M1_LEFT  = 17;
static constexpr uint8_t PIN_RELAY_M2_RIGHT = 18;
static constexpr uint8_t PIN_RELAY_M2_LEFT  = 19;

static constexpr bool RELAY_ACTIVE_LOW = true;
static constexpr uint32_t DIRECTION_DEAD_TIME_MS = 800;
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 35;

LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLS, LCD_ROWS);
Preferences prefs;

enum class UiState : uint8_t {
    MENU,
    EDIT_RIGHT_TIME,
    EDIT_LEFT_TIME,
    EDIT_TOTAL_TIME,
    RUNNING,
    FINISHED
};

enum class RunPhase : uint8_t {
    RIGHT,
    DEAD_RIGHT_TO_LEFT,
    LEFT,
    DEAD_LEFT_TO_NEXT,
    IDLE
};

UiState uiState = UiState::MENU;
RunPhase runPhase = RunPhase::IDLE;

// Настройки. Ограничения:
// вправо 1..3 мин, влево 1..3 мин, общее время 1..120 мин.
uint8_t rightMinutes = 1;
uint8_t leftMinutes = 2;
uint8_t totalMinutes = 30;

uint8_t menuIndex = 0;
static constexpr uint8_t MENU_ITEMS = 4;

uint32_t runStartedAt = 0;
uint32_t phaseStartedAt = 0;
uint32_t lastScreenUpdate = 0;
uint32_t completedCycles = 0;
uint32_t motor1Cycles = 0;
uint32_t motor2Cycles = 0;
uint32_t currentCycle = 1;

uint8_t lastEncoderState = 0;
int8_t encoderAccumulator = 0;

bool lastButtonRaw = HIGH;
bool buttonStable = HIGH;
uint32_t buttonChangedAt = 0;

// ===================== SETTINGS =====================
void loadSettings() {
    prefs.begin("dvigprikatka", true);
    rightMinutes = prefs.getUChar("right", 1);
    leftMinutes  = prefs.getUChar("left", 2);
    totalMinutes = prefs.getUChar("total", 30);
    prefs.end();

    rightMinutes = constrain(rightMinutes, 1, 3);
    leftMinutes  = constrain(leftMinutes, 1, 3);
    totalMinutes = constrain(totalMinutes, 1, 120);
}

void saveSettings() {
    prefs.begin("dvigprikatka", false);
    prefs.putUChar("right", rightMinutes);
    prefs.putUChar("left", leftMinutes);
    prefs.putUChar("total", totalMinutes);
    prefs.end();

    Serial.printf("Settings saved: R=%u L=%u TOTAL=%u min\n",
                  rightMinutes, leftMinutes, totalMinutes);
}

// ===================== RELAYS =====================
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

// ===================== LCD =====================
void printLine(uint8_t row, const String &text) {
    lcd.setCursor(0, row);
    String out = text;
    if (out.length() > LCD_COLS) out = out.substring(0, LCD_COLS);
    while (out.length() < LCD_COLS) out += ' ';
    lcd.print(out);
}

String mmss(uint32_t milliseconds) {
    uint32_t totalSeconds = milliseconds / 1000UL;
    uint32_t minutes = totalSeconds / 60UL;
    uint32_t seconds = totalSeconds % 60UL;
    char buf[12];
    snprintf(buf, sizeof(buf), "%02lu:%02lu",
             (unsigned long)minutes, (unsigned long)seconds);
    return String(buf);
}

void drawMenu() {
    printLine(0, String(menuIndex == 0 ? ">" : " ") + " Right: " + rightMinutes + " min");
    printLine(1, String(menuIndex == 1 ? ">" : " ") + " Left : " + leftMinutes + " min");
    printLine(2, String(menuIndex == 2 ? ">" : " ") + " Total: " + totalMinutes + " min");
    printLine(3, String(menuIndex == 3 ? ">" : " ") + " START");
}

void drawEditScreen(const char *title, uint16_t value, const char *suffix) {
    printLine(0, String("SET ") + title);
    printLine(1, "Rotate encoder");
    printLine(2, String("> ") + value + suffix);
    printLine(3, "Press = SAVE/OK");
}

void drawRunning() {
    uint32_t now = millis();
    uint32_t totalDuration = (uint32_t)totalMinutes * 60000UL;
    uint32_t totalElapsed = now - runStartedAt;
    if (totalElapsed > totalDuration) totalElapsed = totalDuration;
    uint32_t totalRemain = totalDuration - totalElapsed;

    uint32_t phaseDuration = 0;
    String direction = "STOP";

    switch (runPhase) {
        case RunPhase::RIGHT:
            phaseDuration = (uint32_t)rightMinutes * 60000UL;
            direction = "RIGHT";
            break;
        case RunPhase::LEFT:
            phaseDuration = (uint32_t)leftMinutes * 60000UL;
            direction = "LEFT";
            break;
        case RunPhase::DEAD_RIGHT_TO_LEFT:
        case RunPhase::DEAD_LEFT_TO_NEXT:
            phaseDuration = DIRECTION_DEAD_TIME_MS;
            direction = "PAUSE";
            break;
        default:
            break;
    }

    uint32_t phaseElapsed = now - phaseStartedAt;
    if (phaseElapsed > phaseDuration) phaseElapsed = phaseDuration;
    uint32_t phaseRemain = phaseDuration > phaseElapsed ? phaseDuration - phaseElapsed : 0;

    printLine(0, "Cycle " + String(currentCycle) + " " + direction);
    printLine(1, "Total left " + mmss(totalRemain));
    printLine(2, "Phase left " + mmss(phaseRemain));
    printLine(3, "M1:" + String(motor1Cycles) + " M2:" + String(motor2Cycles));
}

void drawFinished() {
    printLine(0, "WORK FINISHED");
    printLine(1, "Time: " + String(totalMinutes) + " min");
    printLine(2, "Cycles: " + String(completedCycles));
    printLine(3, "Press = MENU");
}

// ===================== ENCODER =====================
int8_t readEncoderStep() {
    static const int8_t transitionTable[16] = {
         0, -1,  1,  0,
         1,  0,  0, -1,
        -1,  0,  0,  1,
         0,  1, -1,  0
    };

    uint8_t a = digitalRead(PIN_ENC_A) ? 1 : 0;
    uint8_t b = digitalRead(PIN_ENC_B) ? 1 : 0;
    uint8_t current = (a << 1) | b;
    uint8_t index = (lastEncoderState << 2) | current;
    lastEncoderState = current;
    encoderAccumulator += transitionTable[index];

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

bool buttonPressed() {
    bool raw = digitalRead(PIN_ENC_BTN);
    uint32_t now = millis();

    if (raw != lastButtonRaw) {
        lastButtonRaw = raw;
        buttonChangedAt = now;
    }

    if ((now - buttonChangedAt) >= BUTTON_DEBOUNCE_MS && raw != buttonStable) {
        buttonStable = raw;
        return buttonStable == LOW;
    }
    return false;
}

// ===================== RUN =====================
void startRun() {
    allRelaysOff();
    completedCycles = 0;
    motor1Cycles = 0;
    motor2Cycles = 0;
    currentCycle = 1;

    runStartedAt = millis();
    phaseStartedAt = runStartedAt;
    runPhase = RunPhase::RIGHT;
    motorsRight();
    uiState = UiState::RUNNING;

    lcd.clear();
    drawRunning();
    Serial.println("RUN START");
}

void stopRun(bool finished) {
    allRelaysOff();
    runPhase = RunPhase::IDLE;

    if (finished) {
        uiState = UiState::FINISHED;
        lcd.clear();
        drawFinished();
        Serial.println("RUN FINISHED BY TOTAL TIME");
    } else {
        uiState = UiState::MENU;
        lcd.clear();
        drawMenu();
        Serial.println("RUN STOPPED BY USER");
    }
}

void updateRun() {
    uint32_t now = millis();
    uint32_t totalDuration = (uint32_t)totalMinutes * 60000UL;

    // Главное условие завершения: выбранное общее время 1..120 минут.
    if ((uint32_t)(now - runStartedAt) >= totalDuration) {
        stopRun(true);
        return;
    }

    uint32_t elapsed = now - phaseStartedAt;

    switch (runPhase) {
        case RunPhase::RIGHT:
            if (elapsed >= (uint32_t)rightMinutes * 60000UL) {
                allRelaysOff();
                runPhase = RunPhase::DEAD_RIGHT_TO_LEFT;
                phaseStartedAt = now;
            }
            break;

        case RunPhase::DEAD_RIGHT_TO_LEFT:
            if (elapsed >= DIRECTION_DEAD_TIME_MS) {
                motorsLeft();
                runPhase = RunPhase::LEFT;
                phaseStartedAt = now;
            }
            break;

        case RunPhase::LEFT:
            if (elapsed >= (uint32_t)leftMinutes * 60000UL) {
                allRelaysOff();
                completedCycles++;
                motor1Cycles++;
                motor2Cycles++;
                runPhase = RunPhase::DEAD_LEFT_TO_NEXT;
                phaseStartedAt = now;
            }
            break;

        case RunPhase::DEAD_LEFT_TO_NEXT:
            if (elapsed >= DIRECTION_DEAD_TIME_MS) {
                currentCycle = completedCycles + 1;
                motorsRight();
                runPhase = RunPhase::RIGHT;
                phaseStartedAt = now;
            }
            break;

        case RunPhase::IDLE:
            break;
    }

    if (now - lastScreenUpdate >= 250) {
        lastScreenUpdate = now;
        drawRunning();
    }
}

// ===================== UI =====================
void handleEncoder(int8_t step) {
    if (step == 0) return;

    switch (uiState) {
        case UiState::MENU:
            if (step > 0) menuIndex = (menuIndex + 1) % MENU_ITEMS;
            else menuIndex = (menuIndex + MENU_ITEMS - 1) % MENU_ITEMS;
            drawMenu();
            break;

        case UiState::EDIT_RIGHT_TIME:
            if (step > 0 && rightMinutes < 3) rightMinutes++;
            if (step < 0 && rightMinutes > 1) rightMinutes--;
            drawEditScreen("RIGHT TIME", rightMinutes, " min");
            break;

        case UiState::EDIT_LEFT_TIME:
            if (step > 0 && leftMinutes < 3) leftMinutes++;
            if (step < 0 && leftMinutes > 1) leftMinutes--;
            drawEditScreen("LEFT TIME", leftMinutes, " min");
            break;

        case UiState::EDIT_TOTAL_TIME:
            if (step > 0 && totalMinutes < 120) totalMinutes++;
            if (step < 0 && totalMinutes > 1) totalMinutes--;
            drawEditScreen("TOTAL TIME", totalMinutes, " min");
            break;

        case UiState::RUNNING:
        case UiState::FINISHED:
            break;
    }
}

void handleButton() {
    switch (uiState) {
        case UiState::MENU:
            if (menuIndex == 0) {
                uiState = UiState::EDIT_RIGHT_TIME;
                lcd.clear();
                drawEditScreen("RIGHT TIME", rightMinutes, " min");
            } else if (menuIndex == 1) {
                uiState = UiState::EDIT_LEFT_TIME;
                lcd.clear();
                drawEditScreen("LEFT TIME", leftMinutes, " min");
            } else if (menuIndex == 2) {
                uiState = UiState::EDIT_TOTAL_TIME;
                lcd.clear();
                drawEditScreen("TOTAL TIME", totalMinutes, " min");
            } else {
                startRun();
            }
            break;

        case UiState::EDIT_RIGHT_TIME:
        case UiState::EDIT_LEFT_TIME:
        case UiState::EDIT_TOTAL_TIME:
            // Сохраняем только после подтверждения, а не при каждом щелчке.
            saveSettings();
            uiState = UiState::MENU;
            lcd.clear();
            drawMenu();
            break;

        case UiState::RUNNING:
            stopRun(false);
            break;

        case UiState::FINISHED:
            uiState = UiState::MENU;
            lcd.clear();
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
    pinMode(PIN_ENC_BTN, INPUT_PULLUP);

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    lcd.init();
    lcd.backlight();

    loadSettings();

    lastEncoderState = ((digitalRead(PIN_ENC_A) ? 1 : 0) << 1) |
                       (digitalRead(PIN_ENC_B) ? 1 : 0);

    lcd.clear();
    drawMenu();

    Serial.printf("Loaded: right=%u left=%u total=%u min\n",
                  rightMinutes, leftMinutes, totalMinutes);
}

void loop() {
    int8_t step = readEncoderStep();
    if (step != 0) handleEncoder(step);

    if (buttonPressed()) handleButton();

    if (uiState == UiState::RUNNING) updateRun();

    delay(1);
}

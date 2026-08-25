#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ============================================================
// НАСТРОЙКИ ЖЕЛЕЗА
// ============================================================

// LCD 20x4 по I2C. Если адрес другой (часто 0x3F) - поменять здесь.
static constexpr uint8_t LCD_ADDRESS = 0x27;
static constexpr uint8_t LCD_COLS = 20;
static constexpr uint8_t LCD_ROWS = 4;

// Стандартные I2C выводы ESP32 DevKit
static constexpr uint8_t PIN_I2C_SDA = 21;
static constexpr uint8_t PIN_I2C_SCL = 22;

// Энкодер EC11 / KY-040
static constexpr uint8_t PIN_ENC_A   = 32;
static constexpr uint8_t PIN_ENC_B   = 33;
static constexpr uint8_t PIN_ENC_BTN = 25;

// Реле:
// 1 - двигатель 1 вправо
// 2 - двигатель 1 влево
// 3 - двигатель 2 вправо
// 4 - двигатель 2 влево
static constexpr uint8_t PIN_RELAY_M1_RIGHT = 16;
static constexpr uint8_t PIN_RELAY_M1_LEFT  = 17;
static constexpr uint8_t PIN_RELAY_M2_RIGHT = 18;
static constexpr uint8_t PIN_RELAY_M2_LEFT  = 19;

// Большинство 4-канальных модулей реле активны LOW.
// Если Ваши реле включаются уровнем HIGH - поменяйте на false.
static constexpr bool RELAY_ACTIVE_LOW = true;

// Пауза между сменой направления. Нужна, чтобы реле противоположных
// направлений гарантированно не включились одновременно.
static constexpr uint32_t DIRECTION_DEAD_TIME_MS = 800;

// ============================================================

LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLS, LCD_ROWS);

enum class UiState : uint8_t {
    MENU,
    EDIT_RIGHT_TIME,
    EDIT_LEFT_TIME,
    EDIT_CYCLES,
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

// Значения по умолчанию соответствуют примеру:
// 1 минута вправо, 2 минуты влево.
uint8_t rightMinutes = 1;
uint8_t leftMinutes = 2;
uint16_t totalCycles = 10;

uint8_t menuIndex = 0;
static constexpr uint8_t MENU_ITEMS = 4;

uint16_t completedOverallCycles = 0;
uint16_t motor1Cycles = 0;
uint16_t motor2Cycles = 0;
uint16_t currentCycle = 0;

uint32_t phaseStartedAt = 0;
uint32_t lastScreenUpdate = 0;

// Энкодер
uint8_t lastEncoderState = 0;
int8_t encoderAccumulator = 0;

// Кнопка
bool lastButtonRaw = HIGH;
bool buttonStable = HIGH;
uint32_t buttonChangedAt = 0;
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 35;

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
    // Сначала гарантированно всё выключаем.
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
// LCD helpers
// ============================================================

void printLine(uint8_t row, const String &text) {
    lcd.setCursor(0, row);
    String out = text;
    if (out.length() > LCD_COLS) {
        out = out.substring(0, LCD_COLS);
    }
    while (out.length() < LCD_COLS) {
        out += ' ';
    }
    lcd.print(out);
}

String mmss(uint32_t milliseconds) {
    uint32_t totalSeconds = milliseconds / 1000UL;
    uint32_t minutes = totalSeconds / 60UL;
    uint32_t seconds = totalSeconds % 60UL;
    char buf[12];
    snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)minutes, (unsigned long)seconds);
    return String(buf);
}

void drawMenu() {
    String mark0 = (menuIndex == 0) ? ">" : " ";
    String mark1 = (menuIndex == 1) ? ">" : " ";
    String mark2 = (menuIndex == 2) ? ">" : " ";
    String mark3 = (menuIndex == 3) ? ">" : " ";

    printLine(0, mark0 + " Right: " + String(rightMinutes) + " min");
    printLine(1, mark1 + " Left : " + String(leftMinutes) + " min");
    printLine(2, mark2 + " Cycles: " + String(totalCycles));
    printLine(3, mark3 + " START");
}

void drawEditScreen(const char *title, uint16_t value, const char *suffix) {
    printLine(0, String("SET ") + title);
    printLine(1, "");
    printLine(2, String("> ") + String(value) + suffix);
    printLine(3, "Turn / press=OK");
}

void drawRunning() {
    const uint32_t now = millis();
    uint32_t phaseDuration = 0;
    String direction;

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
            direction = "STOP";
            break;
    }

    uint32_t elapsed = now - phaseStartedAt;
    if (elapsed > phaseDuration && phaseDuration > 0) {
        elapsed = phaseDuration;
    }
    uint32_t remain = (phaseDuration > elapsed) ? (phaseDuration - elapsed) : 0;

    printLine(0, "Cycle " + String(currentCycle) + "/" + String(totalCycles) + " " + direction);
    printLine(1, "M1:" + String(motor1Cycles) + " M2:" + String(motor2Cycles));

    if (runPhase == RunPhase::RIGHT || runPhase == RunPhase::LEFT) {
        printLine(2, "Remain " + mmss(remain));
    } else {
        printLine(2, "Direction pause");
    }

    printLine(3, "Press = STOP");
}

void drawFinished() {
    printLine(0, "WORK FINISHED");
    printLine(1, "Total: " + String(completedOverallCycles));
    printLine(2, "M1:" + String(motor1Cycles) + " M2:" + String(motor2Cycles));
    printLine(3, "Press = MENU");
}

// ============================================================
// ЭНКОДЕР / КНОПКА
// ============================================================

int8_t readEncoderStep() {
    // Таблица переходов квадратурного энкодера.
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

    // Один механический щелчок EC11 обычно даёт 4 перехода.
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
        if (buttonStable == LOW) {
            return true;
        }
    }
    return false;
}

// ============================================================
// УПРАВЛЕНИЕ ЦИКЛОМ
// ============================================================

void startRun() {
    allRelaysOff();

    completedOverallCycles = 0;
    motor1Cycles = 0;
    motor2Cycles = 0;
    currentCycle = 1;

    runPhase = RunPhase::RIGHT;
    phaseStartedAt = millis();
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
        Serial.println("RUN FINISHED");
    } else {
        uiState = UiState::MENU;
        lcd.clear();
        drawMenu();
        Serial.println("RUN STOPPED BY USER");
    }
}

void updateRun() {
    uint32_t now = millis();
    uint32_t elapsed = now - phaseStartedAt;

    switch (runPhase) {
        case RunPhase::RIGHT: {
            const uint32_t duration = (uint32_t)rightMinutes * 60000UL;
            if (elapsed >= duration) {
                allRelaysOff();
                runPhase = RunPhase::DEAD_RIGHT_TO_LEFT;
                phaseStartedAt = now;
                Serial.println("RIGHT DONE -> PAUSE");
            }
            break;
        }

        case RunPhase::DEAD_RIGHT_TO_LEFT:
            if (elapsed >= DIRECTION_DEAD_TIME_MS) {
                motorsLeft();
                runPhase = RunPhase::LEFT;
                phaseStartedAt = now;
                Serial.println("LEFT START");
            }
            break;

        case RunPhase::LEFT: {
            const uint32_t duration = (uint32_t)leftMinutes * 60000UL;
            if (elapsed >= duration) {
                allRelaysOff();

                // Полный цикл двигателя = вправо + влево.
                motor1Cycles++;
                motor2Cycles++;
                completedOverallCycles++;

                if (completedOverallCycles >= totalCycles) {
                    stopRun(true);
                    return;
                }

                runPhase = RunPhase::DEAD_LEFT_TO_NEXT;
                phaseStartedAt = now;
                Serial.printf("CYCLE %u DONE\n", completedOverallCycles);
            }
            break;
        }

        case RunPhase::DEAD_LEFT_TO_NEXT:
            if (elapsed >= DIRECTION_DEAD_TIME_MS) {
                currentCycle = completedOverallCycles + 1;
                motorsRight();
                runPhase = RunPhase::RIGHT;
                phaseStartedAt = now;
                Serial.printf("CYCLE %u RIGHT START\n", currentCycle);
            }
            break;

        case RunPhase::IDLE:
            break;
    }

    if (uiState == UiState::RUNNING && now - lastScreenUpdate >= 250) {
        lastScreenUpdate = now;
        drawRunning();
    }
}

// ============================================================
// UI
// ============================================================

void handleEncoder(int8_t step) {
    if (step == 0) return;

    switch (uiState) {
        case UiState::MENU:
            if (step > 0) {
                menuIndex = (menuIndex + 1) % MENU_ITEMS;
            } else {
                menuIndex = (menuIndex + MENU_ITEMS - 1) % MENU_ITEMS;
            }
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

        case UiState::EDIT_CYCLES:
            if (step > 0 && totalCycles < 999) totalCycles++;
            if (step < 0 && totalCycles > 1) totalCycles--;
            drawEditScreen("CYCLES", totalCycles, "");
            break;

        case UiState::RUNNING:
        case UiState::FINISHED:
            break;
    }
}

void handleButton() {
    switch (uiState) {
        case UiState::MENU:
            switch (menuIndex) {
                case 0:
                    uiState = UiState::EDIT_RIGHT_TIME;
                    lcd.clear();
                    drawEditScreen("RIGHT TIME", rightMinutes, " min");
                    break;
                case 1:
                    uiState = UiState::EDIT_LEFT_TIME;
                    lcd.clear();
                    drawEditScreen("LEFT TIME", leftMinutes, " min");
                    break;
                case 2:
                    uiState = UiState::EDIT_CYCLES;
                    lcd.clear();
                    drawEditScreen("CYCLES", totalCycles, "");
                    break;
                case 3:
                    startRun();
                    break;
            }
            break;

        case UiState::EDIT_RIGHT_TIME:
        case UiState::EDIT_LEFT_TIME:
        case UiState::EDIT_CYCLES:
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

// ============================================================

void setup() {
    Serial.begin(115200);

    pinMode(PIN_ENC_A, INPUT_PULLUP);
    pinMode(PIN_ENC_B, INPUT_PULLUP);
    pinMode(PIN_ENC_BTN, INPUT_PULLUP);

    pinMode(PIN_RELAY_M1_RIGHT, OUTPUT);
    pinMode(PIN_RELAY_M1_LEFT, OUTPUT);
    pinMode(PIN_RELAY_M2_RIGHT, OUTPUT);
    pinMode(PIN_RELAY_M2_LEFT, OUTPUT);
    allRelaysOff();

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    lcd.init();
    lcd.backlight();
    lcd.clear();

    lastEncoderState = ((digitalRead(PIN_ENC_A) ? 1 : 0) << 1) |
                       (digitalRead(PIN_ENC_B) ? 1 : 0);

    printLine(0, "DVIG PRIKATKA");
    printLine(1, "ESP32 controller");
    printLine(2, "4 relay / 2 motor");
    printLine(3, "Starting...");
    delay(1000);

    lcd.clear();
    drawMenu();

    Serial.println("READY");
}

void loop() {
    int8_t encoderStep = readEncoderStep();
    if (encoderStep != 0) {
        handleEncoder(encoderStep);
    }

    if (buttonPressed()) {
        handleButton();
    }

    if (uiState == UiState::RUNNING) {
        updateRun();
    }

    delay(1);
}

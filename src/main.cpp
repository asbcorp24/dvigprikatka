#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Preferences.h>

// ============================================================
// ESP32 + OLED SH1106 128x64 + EC11 + 4 relay
// Полностью неблокирующая логика: НЕТ delay().
// ============================================================

static constexpr uint8_t PIN_SDA = 21;
static constexpr uint8_t PIN_SCL = 22;

static constexpr uint8_t PIN_ENC_A    = 32; // TRA
static constexpr uint8_t PIN_ENC_B    = 33; // TRB
static constexpr uint8_t PIN_ENC_PUSH = 25; // PSH
static constexpr uint8_t PIN_BACK     = 26; // BAK = STOP
static constexpr uint8_t PIN_CONFIRM  = 27; // CON = START

static constexpr uint8_t PIN_RELAY_M1_RIGHT = 16;
static constexpr uint8_t PIN_RELAY_M1_LEFT  = 17;
static constexpr uint8_t PIN_RELAY_M2_RIGHT = 18;
static constexpr uint8_t PIN_RELAY_M2_LEFT  = 19;

static constexpr bool RELAY_ACTIVE_LOW = true;
static constexpr uint32_t DIRECTION_DEAD_TIME_MS = 800;
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 35;
static constexpr uint32_t DISPLAY_UPDATE_MS = 150;
static constexpr uint32_t ENCODER_DEBOUNCE_US = 1800;

U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);
Preferences preferences;

uint8_t rightMinutes = 1;
uint8_t leftMinutes = 2;
uint16_t totalMinutes = 30;

void loadSettings() {
    preferences.begin("dvigprikatka", false);
    rightMinutes = constrain(preferences.getUChar("right", 1), 1, 3);
    leftMinutes = constrain(preferences.getUChar("left", 2), 1, 3);
    totalMinutes = constrain(preferences.getUShort("total", 30), 1, 120);
}

void saveSettings() {
    preferences.putUChar("right", rightMinutes);
    preferences.putUChar("left", leftMinutes);
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

enum class MotorPhase : uint8_t { STOPPED, RIGHT, PAUSE_TO_LEFT, LEFT, PAUSE_TO_RIGHT };

struct MotorController {
    uint8_t rightPin;
    uint8_t leftPin;
    MotorPhase phase = MotorPhase::STOPPED;
    uint32_t phaseStartedAt = 0;
    uint32_t cycles = 0;

    void off() { writeRelay(rightPin, false); writeRelay(leftPin, false); }
    void setRight() { off(); writeRelay(rightPin, true); }
    void setLeft() { off(); writeRelay(leftPin, true); }

    void start(uint32_t now) {
        cycles = 0;
        off();
        phase = MotorPhase::RIGHT;
        phaseStartedAt = now;
        setRight();
    }

    void stop() { off(); phase = MotorPhase::STOPPED; }

    void update(uint32_t now) {
        const uint32_t elapsed = now - phaseStartedAt;
        switch (phase) {
            case MotorPhase::RIGHT:
                if (elapsed >= (uint32_t)rightMinutes * 60000UL) {
                    off(); phase = MotorPhase::PAUSE_TO_LEFT; phaseStartedAt = now;
                }
                break;
            case MotorPhase::PAUSE_TO_LEFT:
                if (elapsed >= DIRECTION_DEAD_TIME_MS) {
                    setLeft(); phase = MotorPhase::LEFT; phaseStartedAt = now;
                }
                break;
            case MotorPhase::LEFT:
                if (elapsed >= (uint32_t)leftMinutes * 60000UL) {
                    off(); cycles++; phase = MotorPhase::PAUSE_TO_RIGHT; phaseStartedAt = now;
                }
                break;
            case MotorPhase::PAUSE_TO_RIGHT:
                if (elapsed >= DIRECTION_DEAD_TIME_MS) {
                    setRight(); phase = MotorPhase::RIGHT; phaseStartedAt = now;
                }
                break;
            default: break;
        }
    }

    uint32_t remaining(uint32_t now) const {
        uint32_t duration = 0;
        switch (phase) {
            case MotorPhase::RIGHT: duration = (uint32_t)rightMinutes * 60000UL; break;
            case MotorPhase::LEFT: duration = (uint32_t)leftMinutes * 60000UL; break;
            case MotorPhase::PAUSE_TO_LEFT:
            case MotorPhase::PAUSE_TO_RIGHT: duration = DIRECTION_DEAD_TIME_MS; break;
            default: return 0;
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

enum class UiState : uint8_t { MENU, EDIT_RIGHT, EDIT_LEFT, EDIT_TOTAL, RUNNING, FINISHED };
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
        bool raw = digitalRead(pin);
        if (raw != rawState) { rawState = raw; changedAt = now; }
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

// ============================================================
// ЭНКОДЕР
// Один импульс TRA = один шаг. TRB определяет направление.
// Это надежнее для данной панели, чем ожидание 4 переходов EC11.
// ============================================================
volatile int16_t encoderSteps = 0;
volatile uint32_t encoderLastUs = 0;
portMUX_TYPE encoderMux = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR encoderISR() {
    // Считаем только момент перехода TRA в LOW.
    if (digitalRead(PIN_ENC_A) != LOW) return;

    const uint32_t nowUs = micros();
    if ((uint32_t)(nowUs - encoderLastUs) < ENCODER_DEBOUNCE_US) return;
    encoderLastUs = nowUs;

    const bool b = digitalRead(PIN_ENC_B);
    portENTER_CRITICAL_ISR(&encoderMux);
    encoderSteps += b ? 1 : -1;
    portEXIT_CRITICAL_ISR(&encoderMux);
}

int8_t getEncoderStep() {
    int8_t result = 0;
    portENTER_CRITICAL(&encoderMux);
    if (encoderSteps > 0) { encoderSteps--; result = 1; }
    else if (encoderSteps < 0) { encoderSteps++; result = -1; }
    portEXIT_CRITICAL(&encoderMux);
    return result;
}

String mmss(uint32_t ms) {
    const uint32_t sec = ms / 1000UL;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)(sec / 60UL), (unsigned long)(sec % 60UL));
    return String(buf);
}

void text(uint8_t x, uint8_t y, const String &s) { display.drawUTF8(x, y, s.c_str()); }

void drawMenu() {
    display.clearBuffer();
    display.setFont(u8g2_font_6x12_tf);
    const char *labels[4] = {"RIGHT", "LEFT", "TOTAL", "START"};
    String values[4] = { String(rightMinutes)+" min", String(leftMinutes)+" min", String(totalMinutes)+" min", "" };
    for (uint8_t i=0; i<4; i++) {
        uint8_t y = 13 + i*16;
        if (i == menuIndex) { display.drawBox(0, y-11, 128, 14); display.setDrawColor(0); }
        text(3, y, String(i==menuIndex ? "> " : "  ") + labels[i]);
        if (i < 3) { int16_t w=display.getUTF8Width(values[i].c_str()); text(125-w, y, values[i]); }
        if (i == menuIndex) display.setDrawColor(1);
    }
    display.sendBuffer();
}

void drawEdit(const char *title, uint16_t value, uint16_t maxValue) {
    display.clearBuffer(); display.setFont(u8g2_font_6x12_tf);
    text(0,12,String("SET ")+title);
    display.setFont(u8g2_font_logisoso24_tf);
    String v=String(value); int16_t w=display.getUTF8Width(v.c_str()); text((128-w)/2,43,v);
    display.setFont(u8g2_font_6x12_tf); text(0,62,String("1-")+maxValue+" min PUSH=OK");
    display.sendBuffer();
}

void drawRunning(uint32_t now) {
    uint32_t totalDuration=(uint32_t)totalMinutes*60000UL;
    uint32_t elapsed=now-runStartedAt;
    uint32_t remain=elapsed>=totalDuration?0:totalDuration-elapsed;
    display.clearBuffer(); display.setFont(u8g2_font_6x12_tf);
    text(0,11,String("TOTAL ")+mmss(remain));
    text(0,25,String("M1 ")+motor1.name()+" C:"+motor1.cycles);
    text(0,39,String("M1 LEFT ")+mmss(motor1.remaining(now)));
    text(0,53,String("M2 ")+motor2.name()+" C:"+motor2.cycles);
    text(0,64,"PUSH/BACK=STOP");
    display.sendBuffer();
}

void drawFinished() {
    display.clearBuffer(); display.setFont(u8g2_font_6x12_tf);
    text(18,14,"WORK FINISHED");
    text(5,31,String("M1 cycles: ")+motor1.cycles);
    text(5,45,String("M2 cycles: ")+motor2.cycles);
    text(1,62,"PUSH=MENU CON=START");
    display.sendBuffer();
}

void startRun() {
    saveSettings();
    uint32_t now=millis(); runStartedAt=now; lastDisplayAt=0;
    motor1.start(now); motor2.start(now); uiState=UiState::RUNNING; drawRunning(now);
}

void stopRun(bool finished) {
    motor1.stop(); motor2.stop(); allRelaysOff();
    if (finished) { uiState=UiState::FINISHED; drawFinished(); }
    else { uiState=UiState::MENU; drawMenu(); }
}

void updateRun(uint32_t now) {
    if (uiState != UiState::RUNNING) return;
    uint32_t totalDuration=(uint32_t)totalMinutes*60000UL;
    if ((now-runStartedAt)>=totalDuration) { stopRun(true); return; }
    motor1.update(now); motor2.update(now);
    if ((now-lastDisplayAt)>=DISPLAY_UPDATE_MS) { lastDisplayAt=now; drawRunning(now); }
}

void handleEncoderTurn(int8_t step) {
    if (!step) return;
    Serial.printf("ENC step=%d A=%d B=%d\n", step, digitalRead(PIN_ENC_A), digitalRead(PIN_ENC_B));
    switch (uiState) {
        case UiState::MENU:
            menuIndex = step > 0 ? (menuIndex+1)%4 : (menuIndex+3)%4; drawMenu(); break;
        case UiState::EDIT_RIGHT:
            if (step>0 && rightMinutes<3) rightMinutes++; if (step<0 && rightMinutes>1) rightMinutes--;
            drawEdit("RIGHT",rightMinutes,3); break;
        case UiState::EDIT_LEFT:
            if (step>0 && leftMinutes<3) leftMinutes++; if (step<0 && leftMinutes>1) leftMinutes--;
            drawEdit("LEFT",leftMinutes,3); break;
        case UiState::EDIT_TOTAL:
            if (step>0 && totalMinutes<120) totalMinutes++; if (step<0 && totalMinutes>1) totalMinutes--;
            drawEdit("TOTAL",totalMinutes,120); break;
        default: break;
    }
}

void handleEncoderPush() {
    switch (uiState) {
        case UiState::MENU:
            if (menuIndex==0) { uiState=UiState::EDIT_RIGHT; drawEdit("RIGHT",rightMinutes,3); }
            else if (menuIndex==1) { uiState=UiState::EDIT_LEFT; drawEdit("LEFT",leftMinutes,3); }
            else if (menuIndex==2) { uiState=UiState::EDIT_TOTAL; drawEdit("TOTAL",totalMinutes,120); }
            else startRun();
            break;
        case UiState::EDIT_RIGHT:
        case UiState::EDIT_LEFT:
        case UiState::EDIT_TOTAL:
            saveSettings(); uiState=UiState::MENU; drawMenu(); break;
        case UiState::RUNNING: stopRun(false); break;
        case UiState::FINISHED: uiState=UiState::MENU; drawMenu(); break;
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
    btnEncoder.begin(); btnBack.begin(); btnConfirm.begin();

    // Только TRA вызывает ISR. На FALLING получаем один шаг на щелчок.
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encoderISR, CHANGE);

    Wire.begin(PIN_SDA, PIN_SCL);
    Wire.setClock(400000);
    display.begin(); display.setContrast(255);

    loadSettings(); drawMenu();

    Serial.printf("Encoder init: TRA GPIO%u=%d TRB GPIO%u=%d PUSH GPIO%u=%d\n",
                  PIN_ENC_A,digitalRead(PIN_ENC_A),PIN_ENC_B,digitalRead(PIN_ENC_B),PIN_ENC_PUSH,digitalRead(PIN_ENC_PUSH));
}

void loop() {
    uint32_t now=millis();

    for (uint8_t i=0; i<8; i++) {
        int8_t step=getEncoderStep();
        if (!step) break;
        handleEncoderTurn(step);
    }

    btnEncoder.update(now); btnBack.update(now); btnConfirm.update(now);
    if (btnEncoder.pressed()) handleEncoderPush();
    if (btnBack.pressed() && uiState==UiState::RUNNING) stopRun(false);
    if (btnConfirm.pressed() && uiState!=UiState::RUNNING) startRun();

    updateRun(now);
}

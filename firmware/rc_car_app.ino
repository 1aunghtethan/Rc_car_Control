/* ============================================================================
 *  RC CAR  —  FULL FUNCTION FIRMWARE  (v3)
 *  Works with the "RC Car Remote" phone app (Web Bluetooth / Android).
 *  ----------------------------------------------------------------------------
 *  Board  : Arduino UNO R3
 *  Driver : L298N   (ENA/ENB = PWM speed, IN1..IN4 = direction)
 *  Radio  : HC-05 (Bluetooth Classic)  or  HM-10 / AT-09 / ESP32 (BLE)
 *
 *  *** PIN CONFIGURATION IS UNCHANGED FROM  forCode/(4).txt  ***
 *      ENA=5  ENB=6  IN1=2  IN2=4  IN3=7  IN4=8
 *      SERVO=3  TRIG=11  ECHO=12
 *  No wire has to move. Nothing new is required.
 *
 *  ---------------------------------------------------------------------------
 *  WHAT IS NEW vs (4).txt  (all added functions use the SAME pins)
 *  ---------------------------------------------------------------------------
 *   1. FAILSAFE      - if the phone goes out of range / dies while the car is
 *                      moving, the car stops by itself (no more runaways).
 *   2. SMOOTH RAMP   - speed eases up/down instead of jumping, so the gearbox
 *                      and battery are not slammed on every press.
 *   3. TURN MODE     - pivot (spin in place) or gentle (arc) steering.
 *   4. ADJUSTABLE    - obstacle distance and failsafe timeout are settable.
 *   5. LIVE TELEMETRY- the car can push distance readings on its own.
 *   6. CLEAR PATH    - erase a recorded path without recording over it.
 *   7. NON-BLOCKING  - the obstacle scan only runs while driving forward, so
 *                      the car stays responsive (the old loop() stalled on
 *                      every delay()).
 *
 *  ---------------------------------------------------------------------------
 *  PROTOCOL  (phone -> car, every command ends with '\n')
 *  ---------------------------------------------------------------------------
 *   Drive (single letters, low latency, sent while a button is held):
 *      F forward    B backward    L left    R right    S stop
 *   Multi-char commands (start with '!' or '?' so they never clash with FBLRS):
 *      V:<0-255>      set speed                 V:200
 *      G:<0-180>      set servo angle           G:135
 *      ?D             ask distance    ->  DIST:<cm>
 *      ?S             ask status      ->  STAT:<mode>,<spd>,<ang>,<dist>,<obsCm>,<failsafe>
 *      !R:1 / !R:0    start / stop recording
 *      !P:1 / !P:0    start / stop playback
 *      !O:1 / !O:0    obstacle avoidance on / off
 *      !T             servo sweep self-test
 *      !H:1 / !H:0    horn on / off        (only if HORN_ENABLED = 1)
 *      !A:<cm>        set obstacle distance        !A:20
 *      !M:<0|1>       turn mode 0=pivot 1=gentle   !M:1
 *      !F:<ms>        failsafe timeout, 0 = off    !F:2000
 *      !L:<0|1>       live telemetry push on/off   !L:1
 *      !K             clear the recorded path
 *   Legacy: a bare digit '1'..'9' on its own line still sets speed (n * 28).
 *
 *  Car -> phone:
 *      READY · DIST:<cm> · STAT:<...> · EVT:OBSTACLE · EVT:FAILSAFE
 *      EVT:REC_START · EVT:REC_STOP steps=N · EVT:PLAY_START · EVT:PLAY_STOP
 *      EVT:NO_PATH · EVT:PATH_CLEARED
 *
 *  NOTE: unplug the HC-05 / BLE module from D0/D1 while uploading, or the
 *  upload fails (it shares the USB pins). Reconnect it afterwards.
 * ==========================================================================*/

#include <Servo.h>

/* ------------------------- PIN MAP (UNCHANGED) --------------------------- */
const int ENA = 5;          // left  motor speed (PWM)
const int ENB = 6;          // right motor speed (PWM)
const int IN1 = 2;          // left  motor dir A
const int IN2 = 4;          // left  motor dir B
const int IN3 = 7;          // right motor dir A
const int IN4 = 8;          // right motor dir B

const int SERVO_PIN = 3;
const int TRIG_PIN  = 11;
const int ECHO_PIN  = 12;

/* Horn is OPTIONAL and OFF by default, so your wiring does not change.
   If you later add a buzzer on pin 13, set HORN_ENABLED to 1.          */
#define HORN_ENABLED 0
const int HORN_PIN = 13;

/* ------------------------------ TUNING ---------------------------------- */
const int    DEFAULT_SPEED   = 200;   // 0-255
const int    DEFAULT_OBST_CM = 15;    // avoid anything closer than this
const int    TURN_MS         = 800;   // how long to swing when avoiding
const long   RECORD_MAX_MS   = 60000; // auto-stop recording after 60 s
const int    RAMP_STEP       = 15;    // PWM change per ramp tick
const unsigned long RAMP_MS  = 5;     // ramp tick interval

/* --------------------------- GLOBAL STATE -------------------------------- */
Servo myServo;

int  carSpeed   = DEFAULT_SPEED;
int  servoAngle = 90;

const byte MODE_NORMAL = 0;
const byte MODE_RECORD = 1;
const byte MODE_PLAY   = 2;
byte currentMode = MODE_NORMAL;

bool obstacleEnabled = true;
int  obstacleCm      = DEFAULT_OBST_CM;
bool gentleTurn      = false;

bool          liveTelemetry = false;
unsigned long lastLive      = 0;

unsigned long failsafeMs  = 2000;     // 0 disables
unsigned long lastRxTime  = 0;

/* Smooth speed ramp */
int           motorTarget = 0;        // 0 = stopped
int           appliedPwm  = 0;
unsigned long lastRamp    = 0;

/* Path memory (record & playback) */
#define MAX_STEPS 200
char          pathActions[MAX_STEPS];
unsigned long pathDurations[MAX_STEPS];
int           totalSteps      = 0;
int           currentPlayStep = 0;

unsigned long lastActionTime    = 0;
unsigned long recordStartTime   = 0;
unsigned long lastDistanceCheck = 0;

char currentAction = 'S';

/* Serial line buffer */
char lineBuf[32];
byte lineLen = 0;

/* ================================ SETUP ================================= */
void setup() {
  Serial.begin(9600);

  pinMode(ENA, OUTPUT);  pinMode(ENB, OUTPUT);
  pinMode(IN1, OUTPUT);  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);  pinMode(IN4, OUTPUT);

#if HORN_ENABLED
  pinMode(HORN_PIN, OUTPUT);
  digitalWrite(HORN_PIN, LOW);
#endif

  myServo.attach(SERVO_PIN);
  myServo.write(servoAngle);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  carStop();
  lastRxTime = millis();
  Serial.println("READY");
}

/* ================================= LOOP ================================= */
void loop() {
  readSerial();              // phone commands (+ failsafe heartbeat)
  updateRamp();              // ease PWM toward the target speed
  handlePlayback();          // advance a recorded path
  checkRecordTimeout();      // stop a runaway recording
  handleObstacleAvoidance(); // auto-brake while driving forward
  handleFailsafe();          // stop if the phone goes silent
  handleLiveTelemetry();     // optional distance push
}

/* ============================ SERIAL PARSER ============================= */
void readSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    lastRxTime = millis();               // any byte = the phone is alive

    if (c == '\n' || c == '\r') {
      if (lineLen > 0) {
        lineBuf[lineLen] = '\0';
        processLine(lineBuf);
        lineLen = 0;
      }
      continue;
    }

    // Legacy single-char drive (old apps send a bare 'F' with no newline).
    if (lineLen == 0 && (c == 'F' || c == 'B' || c == 'L' || c == 'R' || c == 'S')) {
      drive(c);
      continue;
    }

    if (lineLen < sizeof(lineBuf) - 1) lineBuf[lineLen++] = c;
    else                               lineLen = 0;   // overflow -> drop it
  }
}

void processLine(char *s) {
  /* ---- V:<speed> -------------------------------------------------------- */
  if (s[0] == 'V' && s[1] == ':') {
    carSpeed = constrain(atoi(s + 2), 0, 255);
    if (motorTarget > 0) motorTarget = carSpeed;
    Serial.print("STAT:"); sendStatusBody();
    return;
  }

  /* ---- G:<angle> -------------------------------------------------------- */
  if (s[0] == 'G' && s[1] == ':') {
    servoAngle = constrain(atoi(s + 2), 0, 180);
    myServo.write(servoAngle);
    Serial.print("STAT:"); sendStatusBody();
    return;
  }

  /* ---- ?D  ask distance ------------------------------------------------- */
  if (s[0] == '?' && (s[1] == 'D' || s[1] == 'd')) { sendDistance(); return; }

  /* ---- ?S  ask status --------------------------------------------------- */
  if (s[0] == '?' && (s[1] == 'S' || s[1] == 's')) {
    Serial.print("STAT:"); sendStatusBody(); return;
  }

  /* ---- !x commands ------------------------------------------------------ */
  if (s[0] == '!') {
    char k = s[1];
    bool on = (s[3] == '1');

    if (k == 'R') { on ? startRecording() : stopRecording(); return; }
    if (k == 'P') { on ? startPlayback()  : stopPlayback();  return; }
    if (k == 'O') { obstacleEnabled = on; Serial.print("EVT:OBSTACLE="); Serial.println(on ? 1 : 0); return; }
    if (k == 'T') { sweepServo(); return; }
    if (k == 'H') { setHorn(on); return; }
    if (k == 'A') { obstacleCm = constrain(atoi(s + 3), 3, 200); Serial.print("EVT:OBST_CM="); Serial.println(obstacleCm); return; }
    if (k == 'M') { gentleTurn = on; Serial.print("EVT:TURN="); Serial.println(gentleTurn ? "GENTLE" : "PIVOT"); return; }
    if (k == 'F') { failsafeMs = (unsigned long)constrain(atoi(s + 3), 0, 60000); Serial.print("EVT:FAILSAFE_MS="); Serial.println(failsafeMs); return; }
    if (k == 'L') { liveTelemetry = on; Serial.print("EVT:LIVE="); Serial.println(on ? 1 : 0); return; }
    if (k == 'K') { totalSteps = 0; currentPlayStep = 0; Serial.println("EVT:PATH_CLEARED"); return; }
    return;
  }

  /* ---- legacy speed: a lone digit '1'..'9' on its own line -------------- */
  if (s[0] >= '1' && s[0] <= '9' && s[1] == '\0') {
    carSpeed = (s[0] - '0') * 28;
    if (motorTarget > 0) motorTarget = carSpeed;
    Serial.print("STAT:"); sendStatusBody();
    return;
  }

  /* ---- a bare drive letter that arrived as a full line ("F\n") ---------- */
  if (s[0] == 'F' || s[0] == 'B' || s[0] == 'L' || s[0] == 'R' || s[0] == 'S') {
    drive(s[0]);
  }
}

/* ========================= TELEMETRY HELPERS ============================ */
void sendDistance() {
  Serial.print("DIST:");
  Serial.println(getDistance());
}

void sendStatusBody() {
  Serial.print(currentMode);      Serial.print(',');
  Serial.print(carSpeed);         Serial.print(',');
  Serial.print(servoAngle);       Serial.print(',');
  Serial.print(getDistance());    Serial.print(',');
  Serial.print(obstacleCm);       Serial.print(',');
  Serial.println(failsafeMs);
}

/* ============================ DRIVE LOGIC =============================== */
void drive(char cmd) {
  if (currentMode == MODE_PLAY) return;   // manual input is ignored during playback

  if (currentMode == MODE_RECORD && cmd != currentAction) {
    if (totalSteps < MAX_STEPS) {
      pathDurations[totalSteps] = millis() - lastActionTime;
      pathActions[totalSteps]   = currentAction;
      totalSteps++;
      currentAction  = cmd;
      lastActionTime = millis();
    } else {
      stopRecording();
    }
  }

  executeAction(cmd);
  if (currentMode == MODE_NORMAL) currentAction = cmd;
}

void executeAction(char act) {
  if      (act == 'F') carForward();
  else if (act == 'B') carBackward();
  else if (act == 'L') carTurnLeft();
  else if (act == 'R') carTurnRight();
  else if (act == 'S') carStop();
}

/* ====================== OBSTACLE AVOIDANCE ============================== */
void handleObstacleAvoidance() {
  if (!obstacleEnabled) return;
  if (currentMode != MODE_NORMAL) return;
  if (currentAction != 'F') return;       // only brake while going forward

  if (millis() - lastDistanceCheck >= 60) {
    lastDistanceCheck = millis();
    int d = getDistance();
    if (d <= obstacleCm && d > 0) scanAndAvoid();
  }
}

void scanAndAvoid() {
  Serial.println("EVT:OBSTACLE");
  carStop();
  delay(150);

  myServo.write(135); delay(350);
  int leftDistance = getDistance();

  myServo.write(45);  delay(350);
  int rightDistance = getDistance();

  myServo.write(servoAngle); delay(150);

  if (leftDistance > rightDistance && leftDistance > obstacleCm) {
    carTurnLeft();  applyImmediate(); delay(TURN_MS);
  } else if (rightDistance > leftDistance && rightDistance > obstacleCm) {
    carTurnRight(); applyImmediate(); delay(TURN_MS);
  } else {
    carBackward();  applyImmediate(); delay(500);
  }
  carStop();
  currentAction = 'S';
}

/* ========================= FAILSAFE / RAMP ============================== */
void handleFailsafe() {
  if (failsafeMs == 0) return;
  if (currentMode == MODE_PLAY) return;   // playback keeps running on purpose
  if (motorTarget == 0) return;           // already stopped

  if (millis() - lastRxTime > failsafeMs) {
    carStop();
    currentAction = 'S';
    Serial.println("EVT:FAILSAFE");
  }
}

void updateRamp() {
  if (millis() - lastRamp < RAMP_MS) return;
  lastRamp = millis();

  int target = (motorTarget > 0) ? carSpeed : 0;

  if (appliedPwm < target)      appliedPwm = min(target, appliedPwm + RAMP_STEP);
  else if (appliedPwm > target) appliedPwm = max(target, appliedPwm - RAMP_STEP);

  analogWrite(ENA, appliedPwm);
  analogWrite(ENB, appliedPwm);
}

/* Apply full speed at once. Used inside the blocking obstacle sequence,
   where the normal 5 ms ramp tick in loop() does not get a chance to run. */
void applyImmediate() {
  appliedPwm = carSpeed;
  analogWrite(ENA, appliedPwm);
  analogWrite(ENB, appliedPwm);
}

void handleLiveTelemetry() {
  if (!liveTelemetry) return;
  if (millis() - lastLive >= 500) { lastLive = millis(); sendDistance(); }
}

/* ======================== SENSOR / SERVO ================================ */
int getDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 25000);  // 25 ms timeout
  if (duration == 0) return 999;
  return (int)(duration * 0.034 / 2);
}

void sweepServo() {
  myServo.write(0);   delay(400);
  myServo.write(90);  delay(400);
  myServo.write(180); delay(400);
  myServo.write(servoAngle); delay(200);
}

void setHorn(bool on) {
#if HORN_ENABLED
  digitalWrite(HORN_PIN, on ? HIGH : LOW);
#else
  (void)on;
#endif
}

/* ======================= RECORD / PLAYBACK ============================== */
void checkRecordTimeout() {
  if (currentMode == MODE_RECORD && millis() - recordStartTime >= RECORD_MAX_MS) {
    stopRecording();
  }
}

void startRecording() {
  currentMode    = MODE_RECORD;
  totalSteps     = 0;
  currentAction  = 'S';
  executeAction('S');
  lastActionTime  = millis();
  recordStartTime = millis();
  Serial.println("EVT:REC_START");
}

void stopRecording() {
  if (currentMode == MODE_RECORD) {
    if (totalSteps < MAX_STEPS) {
      pathDurations[totalSteps] = millis() - lastActionTime;
      pathActions[totalSteps]   = currentAction;
      totalSteps++;
    }
    currentMode   = MODE_NORMAL;
    currentAction = 'S';
    executeAction('S');
    Serial.print("EVT:REC_STOP steps=");
    Serial.println(totalSteps);
  }
}

void handlePlayback() {
  if (currentMode != MODE_PLAY) return;
  if (millis() - lastActionTime >= pathDurations[currentPlayStep]) {
    currentPlayStep++;
    if (currentPlayStep >= totalSteps) {
      stopPlayback();
    } else {
      executeAction(pathActions[currentPlayStep]);
      lastActionTime = millis();
    }
  }
}

void startPlayback() {
  if (totalSteps == 0) { Serial.println("EVT:NO_PATH"); return; }
  currentMode     = MODE_PLAY;
  currentPlayStep = 0;

  while (currentPlayStep < totalSteps && pathActions[currentPlayStep] == 'S') {
    currentPlayStep++;
  }
  if (currentPlayStep >= totalSteps) { stopPlayback(); return; }

  executeAction(pathActions[currentPlayStep]);
  lastActionTime = millis();
  Serial.println("EVT:PLAY_START");
}

void stopPlayback() {
  currentMode   = MODE_NORMAL;
  currentAction = 'S';
  executeAction('S');
  Serial.println("EVT:PLAY_STOP");
}

/* ========================= MOTOR DRIVER ================================= */
/* Direction pins are set here; speed is applied by updateRamp().           */

void carForward() {                       // both wheels forward
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
  motorTarget = carSpeed;
}

void carBackward() {                      // both wheels reverse
  digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH);
  motorTarget = carSpeed;
}

/* Left turn.
   Pivot  : left wheel reverses, right wheel forward  -> spins in place.
   Gentle : left wheel stops,     right wheel forward  -> arcs forward.  */
void carTurnLeft() {
  if (gentleTurn) {
    digitalWrite(IN1, LOW);  digitalWrite(IN2, LOW);
    digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
  } else {
    digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);
    digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
  }
  motorTarget = carSpeed;
}

/* Right turn. */
void carTurnRight() {
  if (gentleTurn) {
    digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
    digitalWrite(IN3, LOW);  digitalWrite(IN4, LOW);
  } else {
    digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
    digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH);
  }
  motorTarget = carSpeed;
}

void carStop() {                          // immediate, for safety
  motorTarget = 0;
  appliedPwm  = 0;
  analogWrite(ENA, 0); analogWrite(ENB, 0);
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
}

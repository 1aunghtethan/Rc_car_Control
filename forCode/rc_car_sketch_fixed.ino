
#include <Servo.h>

// ============================================================
// PIN CONFIGURATION
// ============================================================

// Motor Speed Control (PWM)
const int ENA = 5;
const int ENB = 6;

// Motor Direction
const int IN1 = 2;
const int IN2 = 4;
const int IN3 = 7;
const int IN4 = 8;

// Servo
const int SERVO_PIN = 3;

// Ultrasonic Sensor HC-SR04
const int TRIG_PIN = 11;
const int ECHO_PIN = 12;


// ============================================================
// GLOBAL OBJECTS
// ============================================================

Servo myServo;


// ============================================================
// MOTOR SPEED
// ============================================================

int carSpeed = 200;

// ============================================================
// SERIAL LINE BUFFER  (added)
// ============================================================
// The phone app sends LINE commands such as "V:200\n". Reading them one
// character at a time made 'V' and ':' fall through to nothing and turned
// "200" into the single digit '2' -> carSpeed = 56. Buffering a whole line
// first, then parsing it, fixes that. Bare single letters (F/B/L/R/S) still
// work exactly as before.

#define LINE_MAX 32
char lineBuf[LINE_MAX];
byte lineLen = 0;


// ============================================================
// PATH RECORDING / PLAYBACK
// ============================================================

#define MAX_STEPS 150

// RAM saving: durations are stored as uint16_t counts of 50 ms ticks
// instead of 4-byte unsigned long (milliseconds).
// Saves 300 bytes of SRAM. Range: 65535 ticks * 50 ms = ~54.6 minutes
// per step, which is far more than any realistic path step needs.
#define TICK_MS 50

char pathActions[MAX_STEPS];          // 150 bytes
uint16_t pathDurations[MAX_STEPS];    // 300 bytes (was 600)

// Convert a millisecond interval into 50 ms ticks, saturating at the
// uint16_t ceiling. A step longer than 54.6 minutes is clamped rather
// than wrapping around to a tiny value, which would replay instantly.
uint16_t ticksFromMs(unsigned long ms) {

  unsigned long ticks = (ms + (TICK_MS / 2)) / TICK_MS;   // rounded

  if (ticks > 65535UL) {
    return 65535;
  }

  return (uint16_t)ticks;
}

int totalSteps = 0;
int currentPlayStep = 0;

unsigned long lastActionTime = 0;
unsigned long recordStartTime = 0;
unsigned long lastDistanceCheck = 0;

char currentAction = 'S';


// ============================================================
// SYSTEM MODES
// ============================================================

const byte MODE_NORMAL = 0;
const byte MODE_RECORD = 1;
const byte MODE_PLAY   = 2;

byte currentMode = MODE_NORMAL;


// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(9600);

  // ----------------------------------------------------------
  // Motor Driver Pins
  // ----------------------------------------------------------

  pinMode(ENA, OUTPUT);
  pinMode(ENB, OUTPUT);

  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);


  // ----------------------------------------------------------
  // Servo Setup
  // ----------------------------------------------------------

  myServo.attach(SERVO_PIN);

  // Center servo
  myServo.write(90);

  delay(500);


  // ----------------------------------------------------------
  // Ultrasonic Sensor Setup
  // ----------------------------------------------------------

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  // Make sure trigger starts LOW
  digitalWrite(TRIG_PIN, LOW);


  // ----------------------------------------------------------
  // Stop Motors
  // ----------------------------------------------------------

  carStop();


  // ----------------------------------------------------------
  // Startup Message
  // ----------------------------------------------------------

  Serial.println();
  Serial.println("================================");
  Serial.println("Robot System Ready!");
  Serial.println("================================");
  Serial.println("F = Forward");
  Serial.println("B = Backward");
  Serial.println("L = Left");
  Serial.println("R = Right");
  Serial.println("S = Stop");
  Serial.println("U = Ultrasonic Test");
  Serial.println("V:<0-255> = Set speed (from app)");
  Serial.println("W = Servo Test");
  Serial.println("P = Record");
  Serial.println("X = Playback");
  Serial.println("================================");
}


// ============================================================
// MAIN LOOP
// ============================================================

void loop() {

  checkRecordTimeout();

  handlePlayback();

  handleObstacleAvoidance();

  handleBluetoothCommands();
}


// ============================================================
// RECORD MODE TIMEOUT
// ============================================================

void checkRecordTimeout() {

  if (currentMode == MODE_RECORD) {

    if (millis() - recordStartTime >= 60000UL) {

      stopRecording();
    }
  }
}


// ============================================================
// PLAYBACK MANAGEMENT
// ============================================================

void handlePlayback() {

  if (currentMode != MODE_PLAY) {
    return;
  }

  if (currentPlayStep >= totalSteps) {
    stopPlayback();
    return;
  }

  if (millis() - lastActionTime >= (unsigned long)pathDurations[currentPlayStep] * TICK_MS) {

    currentPlayStep++;

    if (currentPlayStep >= totalSteps) {

      stopPlayback();

    } else {

      executeAction(pathActions[currentPlayStep]);

      lastActionTime = millis();
    }
  }
}


// ============================================================
// AUTOMATIC OBSTACLE DETECTION
// ============================================================

void handleObstacleAvoidance() {

  // Only automatic obstacle avoidance in normal mode
  // and when the robot is moving forward.

  if (currentMode != MODE_NORMAL) {
    return;
  }

  if (currentAction != 'F') {
    return;
  }


  // Check sensor every 100 ms

  if (millis() - lastDistanceCheck >= 100UL) {

    lastDistanceCheck = millis();

    int frontDistance = getDistance();


    // --------------------------------------------------------
    // Sensor failed / no echo
    // --------------------------------------------------------

    if (frontDistance == -1) {

      Serial.println("Ultrasonic: No echo");

      return;
    }


    // --------------------------------------------------------
    // Obstacle detected
    // --------------------------------------------------------

    if (frontDistance <= 15) {

      Serial.print("Obstacle detected: ");
      Serial.print(frontDistance);
      Serial.println(" cm");

      scanAndAvoid();
    }
  }
}


// ============================================================
// BLUETOOTH / SERIAL COMMANDS
// ============================================================

void handleBluetoothCommands() {

  while (Serial.available()) {

    char c = Serial.read();

    // --------------------------------------------------------
    // End of a line -> parse what we have
    // --------------------------------------------------------

    if (c == '\n' || c == '\r') {

      if (lineLen > 0) {

        lineBuf[lineLen] = '\0';

        processLine(lineBuf);

        lineLen = 0;
      }

      continue;
    }

    // --------------------------------------------------------
    // Legacy bare drive letter, sent with no newline
    // --------------------------------------------------------

    if (lineLen == 0 &&
        (c == 'F' || c == 'B' || c == 'L' || c == 'R' || c == 'S')) {

      executeAction(c);

      if (currentMode == MODE_NORMAL) {
        currentAction = c;
      }

      continue;
    }

    // --------------------------------------------------------
    // Accumulate
    // --------------------------------------------------------

    if (lineLen < LINE_MAX - 1) {

      lineBuf[lineLen++] = c;

    } else {

      lineLen = 0;   // overflow -> drop the line
    }
  }
}


// ============================================================
// LINE PARSER  (added)
// ============================================================

void processLine(char *s) {

  if (s[0] == '\0') {
    return;
  }


  // ==========================================================
  // V:<0-255>   speed, sent by the phone app's gear buttons
  // ==========================================================

  if (s[0] == 'V' && s[1] == ':') {

    carSpeed = atoi(s + 2);

    if (carSpeed < 0)   carSpeed = 0;
    if (carSpeed > 255) carSpeed = 255;

    Serial.print("Speed set to: ");
    Serial.println(carSpeed);

    return;
  }


  // ==========================================================
  // G:<0-180>   servo angle
  // ==========================================================

  if (s[0] == 'G' && s[1] == ':') {

    int a = atoi(s + 2);

    if (a < 0)   a = 0;
    if (a > 180) a = 180;

    myServo.write(a);

    return;
  }


  // ==========================================================
  // Speed hotkeys on their own line:  H = 50%,  Q = halve
  // ==========================================================

  if (s[0] == 'H' || s[0] == 'h') {

    carSpeed = 128;

    Serial.println("Speed set to 50% (128)");

    return;
  }

  if (s[0] == 'Q' || s[0] == 'q') {

    carSpeed = carSpeed / 2;

    if (carSpeed < 60) {
      carSpeed = 60;
    }

    Serial.print("Speed reduced to: ");
    Serial.println(carSpeed);

    return;
  }


  // ==========================================================
  // Single digit on its own line: 1-9  ->  n * 28
  // (kept for the old app; the new app sends V: instead)
  // ==========================================================

  if (s[0] >= '1' && s[0] <= '9' && s[1] == '\0') {

    carSpeed = (s[0] - '0') * 28;

    Serial.print("Speed set to: ");
    Serial.println(carSpeed);

    return;
  }


  // ==========================================================
  // RECORD / PLAYBACK
  // ==========================================================

  if (s[0] == 'P' || s[0] == 'p') {

    if (currentMode == MODE_NORMAL) {

      startRecording();

    } else if (currentMode == MODE_RECORD) {

      stopRecording();
    }

    return;
  }

  if (s[0] == 'X' || s[0] == 'x') {

    if (currentMode == MODE_NORMAL && totalSteps > 0) {

      startPlayback();

    } else if (currentMode == MODE_PLAY) {

      stopPlayback();
    }

    return;
  }


  // ==========================================================
  // ULTRASONIC TEST
  // ==========================================================

  if (s[0] == 'U' || s[0] == 'u') {

    int distance = getDistance();

    if (distance == -1) {

      Serial.println("No echo - check ultrasonic sensor");

    } else {

      Serial.print("Distance: ");
      Serial.print(distance);
      Serial.println(" cm");
    }

    return;
  }


  // ==========================================================
  // SERVO TEST
  // ==========================================================

  if (s[0] == 'W' || s[0] == 'w') {

    sweepServo();

    return;
  }


  // ==========================================================
  // ?D  -> one-shot distance, for the app's "Read distance" test
  // ==========================================================

  if (s[0] == '?' && (s[1] == 'D' || s[1] == 'd')) {

    Serial.print("DIST:");
    Serial.println(getDistance());

    return;
  }


  // ==========================================================
  // MANUAL DRIVE  (F B L R S), whether bare or as a full line
  // ==========================================================

  if (s[0] == 'F' || s[0] == 'B' ||
      s[0] == 'L' || s[0] == 'R' || s[0] == 'S') {

    if (currentMode != MODE_PLAY) {

      // ------------------------------------------------
      // RECORD MODE
      // ------------------------------------------------

      if (currentMode == MODE_RECORD && s[0] != currentAction) {

        if (totalSteps < MAX_STEPS) {

          pathDurations[totalSteps] = ticksFromMs(millis() - lastActionTime);

          pathActions[totalSteps] = currentAction;

          totalSteps++;

          currentAction = s[0];

          lastActionTime = millis();

        } else {

          stopRecording();
        }
      }

      executeAction(s[0]);

      if (currentMode == MODE_NORMAL) {
        currentAction = s[0];
      }
    }
  }
}


// ============================================================
// OBSTACLE AVOIDANCE SCANNER
// ============================================================

void scanAndAvoid() {

  // Stop robot before scanning

  carStop();

  delay(200);


  // ==========================================================
  // SCAN LEFT
  // ==========================================================

  Serial.println("Scanning LEFT...");

  myServo.write(135);

  delay(400);

  int leftDistance = getDistance();


  if (leftDistance == -1) {
    leftDistance = 0;
  }


  Serial.print("Left: ");
  Serial.print(leftDistance);
  Serial.println(" cm");


  // ==========================================================
  // SCAN RIGHT
  // ==========================================================

  Serial.println("Scanning RIGHT...");

  myServo.write(45);

  delay(400);

  int rightDistance = getDistance();


  if (rightDistance == -1) {
    rightDistance = 0;
  }


  Serial.print("Right: ");
  Serial.print(rightDistance);
  Serial.println(" cm");


  // ==========================================================
  // CENTER SERVO
  // ==========================================================

  myServo.write(90);

  delay(200);


  // ==========================================================
  // DECIDE WHICH DIRECTION TO TAKE
  // ==========================================================

  if (leftDistance > rightDistance &&
      leftDistance > 15) {

    Serial.println("Turning LEFT");

    carTurnLeft();

    delay(800);
  }


  else if (rightDistance > leftDistance &&
           rightDistance > 15) {

    Serial.println("Turning RIGHT");

    carTurnRight();

    delay(800);
  }


  else {

    Serial.println("Both sides blocked - reversing");

    carBackward();

    delay(500);
  }


  carStop();

  // Reset current action so obstacle detection
  // does not immediately trigger again.

  currentAction = 'S';
}


// ============================================================
// ULTRASONIC SENSOR
// ============================================================

int getDistance() {

  // ----------------------------------------------------------
  // This is based on your WORKING test code.
  // ----------------------------------------------------------

  digitalWrite(TRIG_PIN, LOW);

  delayMicroseconds(3);


  digitalWrite(TRIG_PIN, HIGH);

  delayMicroseconds(10);

  digitalWrite(TRIG_PIN, LOW);


  // Wait for echo

  unsigned long duration =
      pulseIn(ECHO_PIN, HIGH, 25000UL);


  // No echo

  if (duration == 0) {

    return -1;
  }


  // Convert microseconds to centimeters

  float distance =
      duration * 0.0343 / 2.0;


  return (int)distance;
}


// ============================================================
// SERVO TEST
// ============================================================

void sweepServo() {

  Serial.println("Servo test started");


  myServo.write(0);

  delay(700);


  myServo.write(90);

  delay(700);


  myServo.write(180);

  delay(700);


  myServo.write(90);

  delay(700);


  Serial.println("Servo test finished");
}


// ============================================================
// START RECORDING
// ============================================================

void startRecording() {

  currentMode = MODE_RECORD;

  totalSteps = 0;

  currentAction = 'S';

  executeAction('S');

  lastActionTime = millis();

  recordStartTime = millis();

  Serial.println("Recording started");
}


// ============================================================
// STOP RECORDING
// ============================================================

void stopRecording() {

  if (currentMode == MODE_RECORD) {

    if (totalSteps < MAX_STEPS) {

      pathDurations[totalSteps] =
          ticksFromMs(millis() - lastActionTime);

      pathActions[totalSteps] =
          currentAction;

      totalSteps++;
    }


    currentMode = MODE_NORMAL;

    executeAction('S');

    currentAction = 'S';

    Serial.print("Recording stopped. Steps: ");

    Serial.println(totalSteps);
  }
}


// ============================================================
// START PLAYBACK
// ============================================================

void startPlayback() {

  if (totalSteps <= 0) {

    Serial.println("No recorded path");

    return;
  }


  currentMode = MODE_PLAY;

  currentPlayStep = 0;


  // Skip initial STOP commands

  while (currentPlayStep < totalSteps &&
         pathActions[currentPlayStep] == 'S') {

    currentPlayStep++;
  }


  if (currentPlayStep >= totalSteps) {

    stopPlayback();

    return;
  }


  executeAction(pathActions[currentPlayStep]);

  lastActionTime = millis();


  Serial.println("Playback started");
}


// ============================================================
// STOP PLAYBACK
// ============================================================

void stopPlayback() {

  currentMode = MODE_NORMAL;

  executeAction('S');

  currentAction = 'S';

  Serial.println("Playback stopped");
}


// ============================================================
// COMMAND MAPPER
// ============================================================

void executeAction(char act) {

  if (act == 'F') {

    carForward();

  } else if (act == 'B') {

    carBackward();

  } else if (act == 'L') {

    carTurnLeft();

  } else if (act == 'R') {

    carTurnRight();

  } else if (act == 'S') {

    carStop();
  }
}


// ============================================================
// MOTOR CONTROL
// ============================================================

// ------------------------------------------------------------
// FORWARD
// ------------------------------------------------------------

void carForward() {

  analogWrite(ENA, carSpeed);
  analogWrite(ENB, carSpeed);

  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);

  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
}


// ------------------------------------------------------------
// BACKWARD
// ------------------------------------------------------------

void carBackward() {

  analogWrite(ENA, carSpeed);
  analogWrite(ENB, carSpeed);

  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);

  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);
}


// ------------------------------------------------------------
// TURN LEFT
// ------------------------------------------------------------

void carTurnLeft() {

  analogWrite(ENA, carSpeed);
  analogWrite(ENB, carSpeed);

  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);

  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);
}


// ------------------------------------------------------------
// TURN RIGHT
// ------------------------------------------------------------

void carTurnRight() {

  analogWrite(ENA, carSpeed);
  analogWrite(ENB, carSpeed);

  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);

  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
}


// ------------------------------------------------------------
// STOP
// ------------------------------------------------------------

void carStop() {

  analogWrite(ENA, 0);
  analogWrite(ENB, 0);

  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);

  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
}

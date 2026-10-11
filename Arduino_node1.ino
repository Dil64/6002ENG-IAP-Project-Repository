// ================= SENSOR + MOSFET + DS18B20 + UART TO T-BEAM =================
// Board: Arduino UNO R4 WiFi
//
// FLOW:
// 0) Wait for START signal from T-Beam (only once)
// 1) Pump ON for 6 seconds
// 2) Pressure + temperature ON, measure for 5 seconds
// 3) pH + turbidity ON, measure for 10 seconds
// 4) Valve ON for 75 seconds, wait 10 seconds
// 5) Send payload to T-Beam, wait for LoRaWAN confirmation
// 6) Everything OFF until the next cycle is due
// 7) Repeat forever - cycles START every CYCLE_PERIOD_MS (15 minutes),
//    measured start-to-start, so the period does not drift
//
// Payload:
// temp,24.31,ph,7.15,level,12.4,turb,330.0
//
// T-Beam replies:
// UART_RX_OK
// LORA_CLOUD_OK
// LORA_CLOUD_FAIL
// BUSY
// LORA_NOT_JOINED
// ==============================================================================

#include <OneWire.h>
#include <DallasTemperature.h>

// =======================================================================
// START SIGNAL
// =======================================================================

bool cycleStarted = false;
unsigned long cycleStartMs = 0;

// =======================================================================
// DS18B20
// =======================================================================

const int ONE_WIRE_BUS = 10;
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensor(&oneWire);

// =======================================================================
// ANALOG SENSOR PINS
// =======================================================================

const int PH_PIN = A0;
const int TURBIDITY_PIN = A1;
const int PRESSURE_PIN = A2;

// =======================================================================
// MOSFET CONTROL PINS
// =======================================================================

const int MOSFET_PH_TURB_PIN = 2;
const int MOSFET_PRESSURE_PIN = 3;

const bool MOSFET_ACTIVE_HIGH = true;

void mosfetOn(int pin) {
  digitalWrite(pin, MOSFET_ACTIVE_HIGH ? HIGH : LOW);
}

void mosfetOff(int pin) {
  digitalWrite(pin, MOSFET_ACTIVE_HIGH ? LOW : HIGH);
}

void allMosfetsOff() {
  mosfetOff(MOSFET_PH_TURB_PIN);
  mosfetOff(MOSFET_PRESSURE_PIN);
}

// =======================================================================
// RELAY CONTROL PINS
// =======================================================================

const int RELAY_PUMP_PIN = 7;
const int RELAY_VALVE_PIN = 8;

const bool RELAY_ACTIVE_HIGH = true;

void relayOn(int pin) {
  digitalWrite(pin, RELAY_ACTIVE_HIGH ? HIGH : LOW);
}

void relayOff(int pin) {
  digitalWrite(pin, RELAY_ACTIVE_HIGH ? LOW : HIGH);
}

void allRelaysOff() {
  relayOff(RELAY_PUMP_PIN);
  relayOff(RELAY_VALVE_PIN);
}

// =======================================================================
// TIMING
// =======================================================================

const unsigned long CYCLE_PERIOD_MS = 900000UL;         // 15 minutes, start of one cycle to start of next
const unsigned long PRESS_TURB_MEASURE_MS = 5000UL;     // 5 seconds
const unsigned long PUMP_ON_MS = 6000UL;                // 6 seconds
const unsigned long PH_TEMP_MEASURE_MS = 10000UL;       // 10 seconds
const unsigned long VALVE_OPEN_MS = 75000UL;            // 75 seconds

// =======================================================================
// UART / LORA TIMING
// =======================================================================

const unsigned long UART_BAUD = 115200;
const unsigned long WAIT_FOR_LORA_CONFIRM_MS = 120000UL;
const unsigned long UART_RETRY_DELAY_MS = 5000UL;
const unsigned long WAIT_AFTER_LORA_CONFIRM_MS = 0UL;
const int MAX_UART_SEND_ATTEMPTS = 3;

// =======================================================================
// BASIC ADC HELPER
// =======================================================================

int readADC_Avg(int pin, int n) {
  long acc = 0;

  for (int i = 0; i < n; i++) {
    acc += analogRead(pin);
    delay(5);
  }

  return (int)(acc / n);
}

// =======================================================================
// pH SENSOR
// =======================================================================

const float VREF_PH = 5.0f;
const int ADC_RES_PH = 1023;
const int N_SAMPLES_PH = 40;
const float CAL_V7 = 3.8612f;
const float CAL_V10 = 4.3206f;

// Updated calibration: pH = -6.613V + 34.864
const float PH_SLOPE = -6.613f;
const float PH_INTERCEPT = 34.864f;

float readPH_and_debug() {
  int adc = readADC_Avg(PH_PIN, N_SAMPLES_PH);

  float volt = (adc * VREF_PH) / ADC_RES_PH;
  float pH = (PH_SLOPE * volt) + PH_INTERCEPT;

  if (pH < 0) pH = 0;
  if (pH > 14) pH = 14;

  Serial.print("pH A1 ADC = ");
  Serial.print(adc);
  Serial.print("  V = ");
  Serial.print(volt, 4);
  Serial.print(" V  pH = ");
  Serial.println(pH, 2);

  return pH;
}

// =======================================================================
// DS18B20 TEMPERATURE SENSOR
// =======================================================================

float readTemperatureC() {
  tempSensor.requestTemperatures();

  float tempC = tempSensor.getTempCByIndex(0);

  Serial.print("Temperature = ");

  if (tempC == DEVICE_DISCONNECTED_C) {
    Serial.println("DS18B20 disconnected");
  } else {
    Serial.print(tempC, 2);
    Serial.println(" C");
  }

  return tempC;
}

// =======================================================================
// PRESSURE / WATER LEVEL SENSOR
// =======================================================================

const float VREF_PRESS = 5.0f;
const int ADC_RES_10 = 1023;
const float VOLT_MIN = 0.50f;
const float VOLT_MAX = 4.50f;
const float FS_KPA = 250.0f;
const bool INVERT_MAPPING = false;
const float KPA_TO_CM = 10.197f;

float ZERO_KPA = 0.0f;

// -----------------------------------------------------------------------
// OPTIONAL PRESSURE ZERO
// -----------------------------------------------------------------------

void zeroAtAir() {
  int adc = readADC_Avg(PRESSURE_PIN, 50);

  float v = (adc * VREF_PRESS) / ADC_RES_10;

  float p0 = (v - VOLT_MIN) * (FS_KPA / (VOLT_MAX - VOLT_MIN));

  if (INVERT_MAPPING) {
    p0 = FS_KPA - p0;
  }

  if (p0 < 0) p0 = 0;

  ZERO_KPA = p0;

  Serial.print("[Zero] ZERO_KPA set to ");
  Serial.print(ZERO_KPA, 3);
  Serial.println(" kPa");
}

// -----------------------------------------------------------------------
// READ WATER LEVEL
// -----------------------------------------------------------------------
// Node 1 calibration: H1 = 1000 * (V1 - 0.524)
float readWaterLevel_cm() {
  long totalRaw = 0;

  for (int i = 0; i < 30; i++) {
    totalRaw += analogRead(PRESSURE_PIN);
    delay(10);
  }

  float pressureRaw = totalRaw / 30.0;

  float pressureVoltage =
      pressureRaw * (5.0 / 1023.0);

  // H1 = 1000(V1 - 0.524)
  float pressureValue =
      1000.0 * (pressureVoltage - 0.524);

  // Prevent negative readings
  if (pressureValue < 0) {
    pressureValue = 0;
  }

  Serial.print("Pressure A2 Raw = ");
  Serial.print(pressureRaw, 1);
  Serial.print("  V = ");
  Serial.print(pressureVoltage, 4);
  Serial.print(" V  H1 = ");
  Serial.println(pressureValue, 3);

  return pressureValue;
}



// =======================================================================
// TURBIDITY SENSOR
// =======================================================================

// Updated calibration: T1 = -851.064V1 + 3506.383
float readTurbidity() {
  const int N_SAMPLES_TURB = 50;

  int adc = readADC_Avg(TURBIDITY_PIN, N_SAMPLES_TURB);

  float volt = (adc * VREF_PH) / ADC_RES_PH;
  float NTU = (-851.064f * volt) + 3506.383f;

  if (NTU < 0) NTU = 0;

  Serial.print("Turbidity A0 ADC = ");
  Serial.print(adc);
  Serial.print("  V = ");
  Serial.print(volt, 4);
  Serial.print("  Turbidity = ");
  Serial.print(NTU, 1);
  Serial.println(" NTU");

  return NTU;
}

// =======================================================================
// CREATE PAYLOAD
// =======================================================================

String makeSensorPayload(float tempC, float pH, float level_cm, float turbidity) {
  String line = "";

  line += "temp,";
  line += String(tempC, 2);
  line += ",ph,";
  line += String(pH, 2);
  line += ",level,";
  line += String(level_cm, 1);
  line += ",turb,";
  line += String(turbidity, 1);

  return line;
}

// =======================================================================
// CLEAR T-BEAM SERIAL BUFFER
// =======================================================================

void clearTBeamSerialBuffer() {
  while (Serial1.available()) {
    Serial1.read();
  }
}

// =======================================================================
// SEND PAYLOAD AND WAIT FOR LORA ACK
// =======================================================================

bool sendPayloadAndWaitForCloudAck(const String &payload) {
  for (int attempt = 1; attempt <= MAX_UART_SEND_ATTEMPTS; attempt++) {
    Serial.print("Sending payload to T-Beam, attempt ");
    Serial.print(attempt);
    Serial.print(": ");
    Serial.println(payload);

    clearTBeamSerialBuffer();

    Serial1.println(payload);

    unsigned long startWait = millis();
    bool uartReceivedByTBeam = false;

    while (millis() - startWait < WAIT_FOR_LORA_CONFIRM_MS) {
      if (Serial1.available()) {
        String reply = Serial1.readStringUntil('\n');
        reply.trim();

        if (reply.length() == 0) {
          continue;
        }

        Serial.print("Reply from T-Beam: ");
        Serial.println(reply);

        if (reply == "UART_RX_OK") {
          uartReceivedByTBeam = true;
        }
        else if (reply == "LORA_CLOUD_OK") {
          Serial.println("Confirmed: LoRaWAN cloud/network received the data.");
          return true;
        }
        else if (reply == "LORA_CLOUD_FAIL") {
          Serial.println("T-Beam says LoRaWAN cloud send failed.");
          break;
        }
        else if (reply == "BUSY") {
          Serial.println("T-Beam is busy. Will retry after delay.");
          break;
        }
        else if (reply == "LORA_NOT_JOINED") {
          Serial.println("T-Beam not joined yet. Waiting/retrying.");
          break;
        }
      }

      delay(10);
    }

    if (!uartReceivedByTBeam) {
      Serial.println("No UART_RX_OK from T-Beam.");
    }

    if (attempt < MAX_UART_SEND_ATTEMPTS) {
      Serial.println("Delay before retry...");
      delay(UART_RETRY_DELAY_MS);
    }
  }

  return false;
}

// =======================================================================
// WAIT FOR START SIGNAL FROM T-BEAM
// =======================================================================

void waitForStartSignal() {
  Serial.println();
  Serial.println("====================================");
  Serial.println("WAITING FOR START SIGNAL FROM T-BEAM");
  Serial.println("====================================");

  while (!cycleStarted) {
    if (Serial1.available()) {
      String line = Serial1.readStringUntil('\n');
      line.trim();

      if (line.length() == 0) {
        continue;
      }

      Serial.print("Received from T-Beam: ");
      Serial.println(line);

      if (line == "START") {
        Serial.println("START signal received - beginning autonomous cycle");
        cycleStarted = true;
      }
    }

    delay(50);
  }
}

// =======================================================================
// SETUP
// =======================================================================

void setup() {
  Serial.begin(115200);

  // UART link to T-Beam
  Serial1.begin(UART_BAUD);
  Serial1.setTimeout(100);

  delay(1000);

  // MOSFET outputs
  pinMode(MOSFET_PH_TURB_PIN, OUTPUT);
  pinMode(MOSFET_PRESSURE_PIN, OUTPUT);

  // Relay outputs
  pinMode(RELAY_PUMP_PIN, OUTPUT);
  pinMode(RELAY_VALVE_PIN, OUTPUT);

  // Everything OFF
  allMosfetsOff();
  allRelaysOff();

  // Sensor inputs
  pinMode(TURBIDITY_PIN, INPUT);
  pinMode(PH_PIN, INPUT);
  pinMode(PRESSURE_PIN, INPUT);

  // Start temperature sensor
  tempSensor.begin();

  Serial.println("Sensor + MOSFET + DS18B20 + T-Beam UART code started.");
  Serial.println("Serial1 UART to T-Beam started at 115200 baud.");
  Serial.println("Payload format: temp,data,ph,data,level,data,turb,data");

  // Optional pressure tare:
  // zeroAtAir();

  // Wait for START signal
  waitForStartSignal();

  Serial.println("Waiting 10 seconds after START before beginning...");
  delay(10000);
}

// =======================================================================
// MAIN LOOP
// =======================================================================

void loop() {
  Serial.println();
  Serial.println("====================================");
  Serial.println("========== NEW CYCLE ==========");
  Serial.println("====================================");

  // ============================================================
  // STEP 1 - START OF CYCLE (everything OFF)
  // ============================================================

  allMosfetsOff();
  allRelaysOff();
  cycleStartMs = millis();

  // ============================================================
  // VARIABLES FOR THIS CYCLE
  // ============================================================

  float tempC = DEVICE_DISCONNECTED_C;
  float pH = NAN;
  float level_cm = 0.0;
  float turbidity = 0.0;

  // ============================================================
  // STEP 2 - PUMP ON FOR 6 SECONDS
  // ============================================================

  Serial.println();
  Serial.println("[STEP 2] PUMP ON FOR 6 SECONDS");

  allRelaysOff();
  relayOn(RELAY_PUMP_PIN);

  Serial.println("PUMP ON");

  delay(PUMP_ON_MS);

  relayOff(RELAY_PUMP_PIN);

  Serial.println("PUMP OFF");

  // ============================================================
  // STEP 3 - PRESSURE + TEMPERATURE FOR 5 SECONDS
  // (matches Zaki's reference: pressure MOSFET on, temperature
  // read via D10 at the same time - temperature does not need
  // a MOSFET since the DS18B20 uses the OneWire protocol)
  // ============================================================

  Serial.println();
  Serial.println("[STEP 3] PRESSURE + TEMPERATURE ON");

  allMosfetsOff();
  mosfetOn(MOSFET_PRESSURE_PIN);

  Serial.println("Pressure sensor ON");

  unsigned long pressTempStart = millis();

  float totalLevel = 0.0;
  float totalTemp = 0.0;
  int levelCount = 0;
  int tempCount = 0;

  while (millis() - pressTempStart < PRESS_TURB_MEASURE_MS) {
    float currentLevel = readWaterLevel_cm();
    float currentTemp = readTemperatureC();

    totalLevel += currentLevel;
    levelCount++;

    if (currentTemp != DEVICE_DISCONNECTED_C) {
      totalTemp += currentTemp;
      tempCount++;
    }

    delay(300);
  }

  if (levelCount > 0) {
    level_cm = totalLevel / levelCount;
  }

  if (tempCount > 0) {
    tempC = totalTemp / tempCount;
  }

  mosfetOff(MOSFET_PRESSURE_PIN);

  Serial.println("Pressure sensor OFF");

  Serial.print("FINAL WATER LEVEL = ");
  Serial.print(level_cm, 1);
  Serial.println(" cm");

  Serial.print("FINAL TEMPERATURE = ");
  Serial.print(tempC, 2);
  Serial.println(" C");

  // ============================================================
  // STEP 4 - pH + TURBIDITY FOR 10 SECONDS
  // (matches Zaki's reference: both share one MOSFET)
  // ============================================================

  Serial.println();
  Serial.println("[STEP 4] pH + TURBIDITY ON");

  allMosfetsOff();
  mosfetOn(MOSFET_PH_TURB_PIN);

  Serial.println("pH + turbidity sensors ON");

  unsigned long phTurbStart = millis();

  float totalPH = 0.0;
  float totalTurbidity = 0.0;
  int phCount = 0;
  int turbCount = 0;

  while (millis() - phTurbStart < PH_TEMP_MEASURE_MS) {
    float currentPH = readPH_and_debug();
    float currentTurbidity = readTurbidity();

    if (!isnan(currentPH)) {
      totalPH += currentPH;
      phCount++;
    }

    totalTurbidity += currentTurbidity;
    turbCount++;

    delay(300);
  }

  if (phCount > 0) {
    pH = totalPH / phCount;
  }

  if (turbCount > 0) {
    turbidity = totalTurbidity / turbCount;
  }

  mosfetOff(MOSFET_PH_TURB_PIN);

  Serial.println("pH + turbidity sensors OFF");

  Serial.print("FINAL pH = ");
  Serial.println(pH, 2);

  Serial.print("FINAL TURBIDITY = ");
  Serial.println(turbidity, 1);

  // ============================================================
  // STEP 5 - VALVE ON
  // ============================================================

  Serial.println();
  Serial.println("[STEP 5] VALVE ON");

  relayOff(RELAY_PUMP_PIN);
  relayOn(RELAY_VALVE_PIN);

  Serial.println("VALVE ON");

  delay(VALVE_OPEN_MS);

  relayOff(RELAY_VALVE_PIN);

  Serial.println("VALVE OFF");

  Serial.println("Waiting 10 seconds before sending...");
  delay(10000);

  // ============================================================
  // SENSOR SUMMARY
  // ============================================================

  Serial.println();
  Serial.println("----- SENSOR SUMMARY -----");

  Serial.print("Temperature C  = ");

  if (tempC == DEVICE_DISCONNECTED_C) {
    Serial.println("DS18B20 disconnected");
  }
  else {
    Serial.println(tempC, 2);
  }

  Serial.print("pH             = ");
  Serial.println(pH, 2);

  Serial.print("Water level cm = ");
  Serial.println(level_cm, 1);

  Serial.print("Turbidity NTU  = ");
  Serial.println(turbidity, 1);

  // ============================================================
  // STEP 5 - SEND DATA TO T-BEAM
  // ============================================================

  Serial.println();
  Serial.println("[STEP 6] SENDING DATA TO T-BEAM");

  String payload = makeSensorPayload(
    tempC,
    pH,
    level_cm,
    turbidity
  );

  bool cloudOk = sendPayloadAndWaitForCloudAck(payload);

  if (cloudOk) {
    Serial.println("Data confirmed by LoRaWAN network/cloud.");
  }
  else {
    Serial.println("Data was NOT confirmed after retries.");
  }

  if (WAIT_AFTER_LORA_CONFIRM_MS > 0) {
    delay(WAIT_AFTER_LORA_CONFIRM_MS);
  }

  // ============================================================
  // CYCLE COMPLETE
  // ============================================================

  allMosfetsOff();
  allRelaysOff();

  Serial.println();
  Serial.println("====================================");
  Serial.println("CYCLE COMPLETE");
  Serial.println("EVERYTHING OFF");
  Serial.println("NEXT CYCLE 15 MINUTES AFTER THIS ONE STARTED");
  Serial.println("====================================");

  // Wait until CYCLE_PERIOD_MS has passed since this cycle started.
  // (unsigned subtraction is safe across millis() rollover)
  while ((unsigned long)(millis() - cycleStartMs) < CYCLE_PERIOD_MS) {
    delay(100);
  }
}
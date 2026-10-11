#include <RadioLib.h>

// ============================================================
// NODE INFORMATION - CHANGE THIS FOR EACH PHYSICAL T-BEAM
// Lower NODE_ID wins if two nodes claim the channel at the
// exact same instant (tie-break rule).
// ============================================================

const char* NODE_NAME = "t-farmer-02";   
const int NODE_ID = 2;                   

// ============================================================
// T-BEAM SX1262 PIN MAPPING
// ============================================================

#define PIN_LORA_NSS    18
#define PIN_LORA_RESET  23
#define PIN_LORA_SCLK   5
#define PIN_LORA_MISO   19
#define PIN_LORA_MOSI   27
#define PIN_LORA_BUSY   32
#define PIN_LORA_DIO1   33

SX1262 radio = new Module(
  PIN_LORA_NSS,
  PIN_LORA_DIO1,
  PIN_LORA_RESET,
  PIN_LORA_BUSY
);

// ============================================================
// UART TO OWN ARDUINO
// ============================================================

#define UNO_UART_RX_PIN 13
#define UNO_UART_TX_PIN 14
#define UNO_UART_BAUD   115200

HardwareSerial UnoSerial(2);

// ============================================================
// LORAWAN OTAA - CHANGE THESE PER DEVICE
// (slave A credentials)
// ============================================================

uint64_t joinEUI = 0x0000000000000000;
uint64_t devEUI  = 0x70B3D57ED0078BAB;

uint8_t appKey[16] = {
  0x37, 0x70, 0xEE, 0x4C,
  0x4D, 0x6D, 0x53, 0x8A,
  0x7F, 0x5D, 0x4F, 0x71,
  0x0F, 0x03, 0x1C, 0x6E
};

uint8_t nwkKey[16] = {
  0x37, 0x70, 0xEE, 0x4C,
  0x4D, 0x6D, 0x53, 0x8A,
  0x7F, 0x5D, 0x4F, 0x71,
  0x0F, 0x03, 0x1C, 0x6E
};

LoRaWANNode node(&radio, &AU915, 2);

bool lorawanJoined = false;

// ============================================================
// COORDINATION STATE
// ============================================================

enum State {
  STATE_WAITING_FOR_GO,      // slave-only: listening for the master's GO
  STATE_WAITING_START_DELAY, // slave-only: GO heard, waiting START_DELAY_AFTER_GO_MS
  STATE_P2P_IDLE,       // listening, nothing to send right now
  STATE_CLAIMING,       // just broadcast BUSY, briefly checking for collision
  STATE_SENDING         // actively sending to the gateway
};

State currentState = STATE_WAITING_FOR_GO;

const bool IS_MASTER = false; // only true on the master node

// Slave-only: delay after hearing GO before starting this node's Arduino,
// so the slaves' cycles are staggered (slave A: 60 s, slave B: 120 s).
const unsigned long START_DELAY_AFTER_GO_MS = 60000UL;
unsigned long goReceivedMs = 0;

volatile bool receivedFlag = false;
void setFlag(void) { receivedFlag = true; }

// ============================================================
// BUSY HOLD - EASY TO CHANGE
// When another node announces BUSY (it is about to send to the gateway),
// this node waits BUSY_HOLD_MS before it tries to send itself. This must
// be longer than the longest a send can take (first-time join + two
// messages + the 10 s gap between them, roughly 30 s).
// ============================================================

const unsigned long BUSY_HOLD_MS = 45000;   
unsigned long busyUntilMs = 0;

void markChannelBusy()
{
  busyUntilMs = millis() + BUSY_HOLD_MS;
  if (busyUntilMs == 0) busyUntilMs = 1;   // 0 is reserved for "not busy"
}

bool channelIsBusy()
{
  return busyUntilMs != 0 && (long)(busyUntilMs - millis()) > 0;
}
bool pendingDataWaiting = false;   // true if our own Arduino has data queued to send
String pendingLine = "";

unsigned long claimStartMs = 0;
const unsigned long CLAIM_LISTEN_WINDOW_MS = 1500; // brief window to detect a collision
bool collisionDetected = false;
int collidingNodeId = -1;

String rxLine = "";

// ============================================================
// P2P: SWITCH RADIO INTO P2P MODE
// ============================================================

bool switchToP2PMode()
{
  radio.standby();

  int state = radio.begin(916.8, 125.0, 10, 5, 0x12, 14, 8, 1.8, false);

  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("P2P radio init failed: ");
    Serial.println(state);
    return false;
  }

  radio.setDio1Action(setFlag);
  receivedFlag = false;

  state = radio.startReceive();

  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("P2P startReceive failed: ");
    Serial.println(state);
    return false;
  }

  return true;
}

// ============================================================
// P2P: SEND A SHORT BROADCAST A FEW TIMES FOR RELIABILITY
// ============================================================

void broadcastMessage(const char* text)
{
  for (int i = 0; i < 3; i++) {
    radio.transmit(text);
    Serial.print("Broadcast: ");
    Serial.println(text);
    delay(300);
    radio.startReceive(); // resume listening between sends
  }
}

// ============================================================
// MASTER ONLY: BROADCAST GO REPEATEDLY FOR A WINDOW
// A single quick burst is easy for a slave to miss if it is still
// booting, so GO is repeated for GO_BROADCAST_DURATION_MS.
// ============================================================

const unsigned long GO_BROADCAST_DURATION_MS = 10000;
const unsigned long GO_SEND_INTERVAL_MS = 500;

void broadcastGoWindow()
{
  unsigned long start = millis();
  int count = 0;

  while (millis() - start < GO_BROADCAST_DURATION_MS) {
    int txState = radio.transmit("GO");
    count++;

    if (txState != RADIOLIB_ERR_NONE) {
      Serial.print("GO send failed, code: ");
      Serial.println(txState);
    }

    delay(GO_SEND_INTERVAL_MS);
  }

  Serial.print("GO broadcast window complete (");
  Serial.print(count);
  Serial.println(" transmissions).");

  radio.startReceive(); // resume listening
}

// ============================================================
// P2P: CHECK FOR AN INCOMING BUSY/FREE MESSAGE
// Returns true if a message was processed this call.
// ============================================================

bool checkP2PMessage()
{
  if (!receivedFlag) return false;
  receivedFlag = false;

  size_t packetLength = radio.getPacketLength();
  if (packetLength > 32) packetLength = 32;

  uint8_t receivedData[33] = {0};
  int state = radio.readData(receivedData, packetLength);

  radio.startReceive(); // keep listening

  if (state != RADIOLIB_ERR_NONE) return false;

  String msg = "";
  for (size_t i = 0; i < packetLength; i++) msg += (char)receivedData[i];

  Serial.print("[P2P] Received: ");
  Serial.println(msg);

  // Expected formats: "GO" or "BUSY:<id>"
  if (msg == "GO") {
    if (currentState == STATE_WAITING_FOR_GO) {
      Serial.print(">>> GO received - starting own Arduino in ");
      Serial.print(START_DELAY_AFTER_GO_MS / 1000);
      Serial.println(" s");
      goReceivedMs = millis();
      currentState = STATE_WAITING_START_DELAY;
    }
    return true;
  }

  int colonIdx = msg.indexOf(':');
  if (colonIdx < 0) return true;

  String type = msg.substring(0, colonIdx);
  int senderId = msg.substring(colonIdx + 1).toInt();

  if (senderId == NODE_ID) return true; // ignore our own echoes, if any

  if (type == "BUSY") {
    if (currentState == STATE_CLAIMING) {
      // Someone else claimed at the same time as us - collision
      collisionDetected = true;
      collidingNodeId = senderId;
    }
    markChannelBusy();   // wait BUSY_HOLD_MS so that node can finish sending
  }
  // (FREE messages are no longer used - the hold time replaces them)

  return true;
}

// ============================================================
// LORAWAN: INITIALISE + JOIN (ONCE)
// ============================================================

bool initialiseLoRaWAN()
{
  radio.standby();

  int state = radio.begin(915.0, 125.0, 7, 5, 0x34, 14, 8, 1.8, false);

  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("LoRaWAN radio init failed: ");
    Serial.println(state);
    return false;
  }

  if (lorawanJoined) return true;

  node.beginOTAA(joinEUI, devEUI, nwkKey, appKey);

  Serial.println("Joining TTN...");
  int joinState = node.activateOTAA();

  if (joinState == RADIOLIB_LORAWAN_NEW_SESSION || joinState == RADIOLIB_ERR_NONE) {
    Serial.println(">>> Joined LoRaWAN network!");
    lorawanJoined = true;
    delay(2000);
    return true;
  }

  Serial.print("Join failed, code: ");
  Serial.println(joinState);
  return false;
}

// ============================================================
// LORAWAN: SEND ONE 6-BYTE MESSAGE
// ============================================================

bool sendLoRaWANPart(uint8_t slot, int16_t value1, int16_t value2)
{
  uint8_t payload[6];
  payload[0] = 0xFF;
  payload[1] = slot;
  payload[2] = (uint8_t)(value1 >> 8);
  payload[3] = (uint8_t)(value1 & 0xFF);
  payload[4] = (uint8_t)(value2 >> 8);
  payload[5] = (uint8_t)(value2 & 0xFF);

  Serial.print("Sending payload: ");
  for (int i = 0; i < 6; i++) {
    if (payload[i] < 0x10) Serial.print("0");
    Serial.print(payload[i], HEX);
    Serial.print(" ");
  }
  Serial.println();

  // Confirmed uplink, with retries if no acknowledgement comes back.
  // RadioLib's RX1/RX2 windows are fixed by the LoRaWAN spec (not
  // adjustable the way the Arduino-side retry timing is), so retrying
  // the whole send is how extra chances are given to the network.
  const int MAX_UPLINK_ATTEMPTS = 3;
  const unsigned long UPLINK_RETRY_DELAY_MS = 10000;

  for (int attempt = 1; attempt <= MAX_UPLINK_ATTEMPTS; attempt++) {
    Serial.print("Confirmed uplink attempt ");
    Serial.print(attempt);
    Serial.print(" of ");
    Serial.println(MAX_UPLINK_ATTEMPTS);

    uint8_t downlinkPayload[256];
    size_t downlinkLen = 0;

    int state = node.sendReceive(
      payload, sizeof(payload),
      2, downlinkPayload, &downlinkLen,
      true // isConfirmed
    );

    if (state >= RADIOLIB_ERR_NONE) {
      Serial.println(">>> Confirmed uplink acknowledged - sent successfully.");
      return true;
    }

    Serial.print("Attempt failed, code: ");
    Serial.println(state);

    if (attempt < MAX_UPLINK_ATTEMPTS) {
      Serial.println("Retrying in 10 seconds...");
      delay(UPLINK_RETRY_DELAY_MS);
    }
  }

  Serial.println("Uplink failed after all attempts.");
  return false;
}

// ============================================================
// CYCLE BATCHING - EASY TO CHANGE
// Instead of sending every single Arduino reading, this many
// readings are collected first, outliers are removed, and the
// remaining values are averaged before sending one result.
// ============================================================

const int CYCLES_PER_SEND = 1;          
const float OUTLIER_THRESHOLD_STDDEV = 1.5; 
const int FIRST_SEND_CYCLES = 1;     

float tempBuffer[CYCLES_PER_SEND];
float phBuffer[CYCLES_PER_SEND];
float levelBuffer[CYCLES_PER_SEND];
float turbBuffer[CYCLES_PER_SEND];
int cyclesCollected = 0;
bool firstBatchDone = false;

// ============================================================
// AVERAGE A SET OF VALUES, EXCLUDING OUTLIERS
// Outliers are values further than OUTLIER_THRESHOLD_STDDEV
// standard deviations from the mean. If every value gets
// excluded (e.g. all identical, stddev = 0), falls back to a
// plain average of everything so a result is always produced.
// ============================================================

float averageWithOutlierRemoval(float values[], int count)
{
  float sum = 0;
  for (int i = 0; i < count; i++) sum += values[i];
  float mean = sum / count;

  float variance = 0;
  for (int i = 0; i < count; i++) {
    float diff = values[i] - mean;
    variance += diff * diff;
  }
  variance /= count;
  float stddev = sqrt(variance);

  float cleanSum = 0;
  int cleanCount = 0;

  for (int i = 0; i < count; i++) {
    if (stddev == 0 || fabs(values[i] - mean) <= OUTLIER_THRESHOLD_STDDEV * stddev) {
      cleanSum += values[i];
      cleanCount++;
    } else {
      Serial.print("  Outlier removed: ");
      Serial.println(values[i], 2);
    }
  }

  if (cleanCount == 0) {
    return mean;
  }

  return cleanSum / cleanCount;
}

// ============================================================
// HANDLE ONE INCOMING ARDUINO READING - BUFFER IT, AND ONLY
// TRIGGER A SEND ONCE CYCLES_PER_SEND READINGS ARE COLLECTED
// ============================================================

// ============================================================
// CLAMP BAD INPUT
// None of these four readings should ever legitimately be negative
// in this system (pH, pressure/level and turbidity are all defined
// as 0 or above, and this deployment does not expect sub-zero
// temperatures). A negative value here means a disconnected or
// faulty sensor rather than a real reading, so it is replaced with
// 0.0 instead of being sent as-is.
// ============================================================

float clampNegative(float value, const char* name)
{
  if (value < 0) {
    Serial.print("WARNING: ");
    Serial.print(name);
    Serial.print(" reading was negative (");
    Serial.print(value, 2);
    Serial.println(") - sending 0.0 instead.");
    return 0.0;
  }
  return value;
}

bool handleArduinoLine(const String &line, String &outAveragedLine)
{
  int tempIdx  = line.indexOf("temp,");
  int phIdx    = line.indexOf(",ph,");
  int levelIdx = line.indexOf(",level,");
  int turbIdx  = line.indexOf(",turb,");

  if (tempIdx < 0 || phIdx < 0 || levelIdx < 0 || turbIdx < 0) {
    Serial.println("Could not parse Arduino data line.");
    UnoSerial.println("LORA_CLOUD_FAIL");
    return false;
  }

  float tempVal  = clampNegative(line.substring(tempIdx + 5, phIdx).toFloat(), "temperature");
  float phVal    = clampNegative(line.substring(phIdx + 4, levelIdx).toFloat(), "pH");
  float levelVal = clampNegative(line.substring(levelIdx + 7, turbIdx).toFloat(), "level");
  float turbVal  = clampNegative(line.substring(turbIdx + 6).toFloat(), "turbidity");

  tempBuffer[cyclesCollected]  = tempVal;
  phBuffer[cyclesCollected]    = phVal;
  levelBuffer[cyclesCollected] = levelVal;
  turbBuffer[cyclesCollected]  = turbVal;
  cyclesCollected++;

  // The first batch can be a different size (FIRST_SEND_CYCLES) so the
  // network comes up sooner; every batch after that uses CYCLES_PER_SEND.
  int target = firstBatchDone ? CYCLES_PER_SEND
             : (FIRST_SEND_CYCLES < CYCLES_PER_SEND ? FIRST_SEND_CYCLES : CYCLES_PER_SEND);

  Serial.print("Collected reading ");
  Serial.print(cyclesCollected);
  Serial.print(" of ");
  Serial.print(target);
  Serial.print(": temp=");
  Serial.print(tempVal, 2);
  Serial.print(" ph=");
  Serial.print(phVal, 2);
  Serial.print(" level=");
  Serial.print(levelVal, 1);
  Serial.print(" turb=");
  Serial.println(turbVal, 1);

  UnoSerial.println("UART_RX_OK");

  if (cyclesCollected < target) {
    // Not enough readings yet - tell the Arduino it was buffered so it
    // does not wait for a cloud confirmation and resend the same reading.
    UnoSerial.println("BATCHED");
    return false;
  }

  // We have enough readings - clean and average each value, then
  // reset the buffer for the next batch.
  Serial.println();
  Serial.println(">>> Batch complete - removing outliers and averaging");

  float avgTemp  = averageWithOutlierRemoval(tempBuffer, cyclesCollected);
  float avgPh    = averageWithOutlierRemoval(phBuffer, cyclesCollected);
  float avgLevel = averageWithOutlierRemoval(levelBuffer, cyclesCollected);
  float avgTurb  = averageWithOutlierRemoval(turbBuffer, cyclesCollected);

  cyclesCollected = 0; // reset for the next batch
  firstBatchDone = true;

  Serial.print("Averaged result: temp=");
  Serial.print(avgTemp, 2);
  Serial.print(" ph=");
  Serial.print(avgPh, 2);
  Serial.print(" level=");
  Serial.print(avgLevel, 1);
  Serial.print(" turb=");
  Serial.println(avgTurb, 1);

  // Rebuild a line in the same format parseAndSendToGateway expects,
  // so the rest of the send pipeline doesn't need to change at all.
  outAveragedLine = "temp," + String(avgTemp, 2) +
                     ",ph," + String(avgPh, 2) +
                     ",level," + String(avgLevel, 1) +
                     ",turb," + String(avgTurb, 1);

  return true; // a batch is ready to send
}

// ============================================================
// PAYLOAD SCALING
// Values are sent as signed 16-bit integers (-32768..32767), so a
// scale of 100 can only carry up to 327.67 and a scale of 10 up to
// 3276.7. Converting a larger value to int16_t is undefined in C++ and
// gives a wrong reading, so anything larger is clamped instead and a
// warning is printed.
// These scales MUST match SLOT_SCALES in the backend.
// ============================================================

const float TEMP_SCALE  = 100.0;
const float PH_SCALE    = 100.0;
const float LEVEL_SCALE = 100.0;   // pressure / water level (slot 3)
const float TURB_SCALE  = 10.0;    // turbidity (slot 4)

int16_t packScaled(float value, float scale, const char* name)
{
  float scaled = value * scale;

  if (scaled > 32767.0f) {
    Serial.print("WARNING: ");
    Serial.print(name);
    Serial.print(" value ");
    Serial.print(value, 2);
    Serial.println(" exceeds payload range - clamped.");
    scaled = 32767.0f;
  } else if (scaled < -32768.0f) {
    Serial.print("WARNING: ");
    Serial.print(name);
    Serial.print(" value ");
    Serial.print(value, 2);
    Serial.println(" below payload range - clamped.");
    scaled = -32768.0f;
  }

  // round to nearest instead of truncating
  return (int16_t)(scaled >= 0 ? scaled + 0.5f : scaled - 0.5f);
}

// ============================================================
// PARSE ARDUINO LINE AND SEND BOTH MESSAGES
// Expected: temp,24.31,ph,7.15,level,12.4,turb,330.0
// ============================================================

bool parseAndSendToGateway(const String &line)
{
  int tempIdx  = line.indexOf("temp,");
  int phIdx    = line.indexOf(",ph,");
  int levelIdx = line.indexOf(",level,");
  int turbIdx  = line.indexOf(",turb,");

  if (tempIdx < 0 || phIdx < 0 || levelIdx < 0 || turbIdx < 0) {
    Serial.println("Could not parse Arduino data line.");
    return false;
  }

  // Defence in depth: the averaged line passed in here is built only
  // from values already clamped above, so these should never actually
  // be negative - but it is cheap to check again in case this function
  // is ever called with a line from somewhere else in future.
  float tempVal  = clampNegative(line.substring(tempIdx + 5, phIdx).toFloat(), "temperature");
  float phVal    = clampNegative(line.substring(phIdx + 4, levelIdx).toFloat(), "pH");
  float levelVal = clampNegative(line.substring(levelIdx + 7, turbIdx).toFloat(), "level");
  float turbVal  = clampNegative(line.substring(turbIdx + 6).toFloat(), "turbidity");

  Serial.println();
  Serial.println("Sensor values (averaged batch):");
  Serial.print("Temperature: "); Serial.println(tempVal, 2);
  Serial.print("pH: ");          Serial.println(phVal, 2);
  Serial.print("Level: ");       Serial.println(levelVal, 1);
  Serial.print("Turbidity: ");   Serial.println(turbVal, 1);

  if (!initialiseLoRaWAN()) {
    UnoSerial.println("LORA_CLOUD_FAIL");
    return false;
  }

  int16_t tempPacked  = packScaled(tempVal,  TEMP_SCALE,  "temperature");
  int16_t phPacked    = packScaled(phVal,    PH_SCALE,    "pH");
  int16_t levelPacked = packScaled(levelVal, LEVEL_SCALE, "pressure");
  int16_t turbPacked  = packScaled(turbVal,  TURB_SCALE,  "turbidity");

  bool part1OK = sendLoRaWANPart(1, tempPacked, phPacked);

  if (!part1OK) {
    UnoSerial.println("LORA_CLOUD_FAIL");
    return false;
  }

  delay(10000); // gap between the two messages

  bool part2OK = sendLoRaWANPart(3, levelPacked, turbPacked);

  if (!part2OK) {
    UnoSerial.println("LORA_CLOUD_FAIL");
    return false;
  }

  UnoSerial.println("LORA_CLOUD_OK");
  Serial.println(">>> Complete sensor reading transmitted.");
  return true;
}

// ============================================================
// TRY TO CLAIM THE CHANNEL AND SEND PENDING DATA
// ============================================================

void attemptClaim()
{
  Serial.println();
  Serial.print(">>> Attempting to claim channel (Node ");
  Serial.print(NODE_ID);
  Serial.println(")");

  currentState = STATE_CLAIMING;
  collisionDetected = false;
  collidingNodeId = -1;

  String busyMsg = "BUSY:" + String(NODE_ID);
  broadcastMessage(busyMsg.c_str());

  claimStartMs = millis();
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("=====================================");
  Serial.print(NODE_NAME);
  Serial.print(" (Node ID ");
  Serial.print(NODE_ID);
  Serial.println(") - SYMMETRIC PEER MODE");
  Serial.println("=====================================");

  UnoSerial.begin(UNO_UART_BAUD, SERIAL_8N1, UNO_UART_RX_PIN, UNO_UART_TX_PIN);
  SPI.begin(PIN_LORA_SCLK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);

  switchToP2PMode();
  currentState = STATE_WAITING_FOR_GO;

  Serial.println("Waiting for GO signal from master before starting...");
}

// ============================================================
// LOOP
// ============================================================

void loop()
{
  // --------------------------------------------------------
  // Always check for new data from our own Arduino
  // --------------------------------------------------------
  while (UnoSerial.available()) {
    char c = (char)UnoSerial.read();

    if (c == '\r') continue;

    if (c == '\n') {
      rxLine.trim();

      if (rxLine.length() > 0 && !pendingDataWaiting) {
        Serial.print("Received from own Arduino: ");
        Serial.println(rxLine);

        String averagedLine;
        bool batchReady = handleArduinoLine(rxLine, averagedLine);

        if (batchReady) {
          pendingLine = averagedLine;
          pendingDataWaiting = true;
        }
      }

      rxLine = "";
    } else {
      if (rxLine.length() < 128) rxLine += c;
    }
  }

  // --------------------------------------------------------
  // State machine
  // --------------------------------------------------------

  if (currentState == STATE_WAITING_FOR_GO) {

    checkP2PMessage();
  }

  else if (currentState == STATE_WAITING_START_DELAY) {

    checkP2PMessage(); // keep listening for BUSY from the other nodes

    if (millis() - goReceivedMs >= START_DELAY_AFTER_GO_MS) {
      Serial.println(">>> Start delay complete - triggering own Arduino now");
      UnoSerial.println("START");
      currentState = STATE_P2P_IDLE;
    }
  }

  else if (currentState == STATE_P2P_IDLE) {

    checkP2PMessage();

    if (pendingDataWaiting && !channelIsBusy()) {
      attemptClaim();
    }
  }

  else if (currentState == STATE_CLAIMING) {

    checkP2PMessage();

    if (collisionDetected) {

      // Tie-break: lower ID wins
      if (NODE_ID < collidingNodeId) {

        Serial.println(">>> Collision detected, but we have priority - proceeding.");
        // fall through to sending below (do not reset state)

      } else {

        Serial.println(">>> Collision detected, other node has priority - backing off.");
        currentState = STATE_P2P_IDLE;
        markChannelBusy(); // wait for their send to finish (BUSY_HOLD_MS)
        return;
      }
    }

    if (millis() - claimStartMs >= CLAIM_LISTEN_WINDOW_MS) {

      Serial.println(">>> Claim window passed, channel is ours.");
      currentState = STATE_SENDING;

      bool sent = parseAndSendToGateway(pendingLine);

      pendingDataWaiting = false;
      pendingLine = "";

      Serial.println();
      Serial.print(">>> Send finished (Node ");
      Serial.print(NODE_ID);
      Serial.println(")");

      // The radio is still in LoRaWAN mode after sending. Switch back to
      // P2P (916.8 MHz, sync 0x12) so this node can hear BUSY messages from
      // the other nodes again. No FREE message is sent - the other nodes
      // simply wait BUSY_HOLD_MS after hearing this node's BUSY.
      switchToP2PMode();

      (void)sent;

      currentState = STATE_P2P_IDLE;
    }
  }

  delay(10);
}

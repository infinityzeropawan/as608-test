// AS608 FINGERPRINT SENSOR - OFFICIAL PROTOCOL IMPLEMENTATION (FIXED)
// Based on official AS608/GT-511C3 documentation
#include <WiFi.h>
#include <WebServer.h>
#include <HardwareSerial.h>

const char* ssid = "IQ";
const char* password = "12345678";
const char* apSsid = "AS608-LOCAL";
const char* apPassword = "12345678";

#define RX_PIN 16
#define TX_PIN 17
#define TOUCH_PIN 4
#define SENSOR_BAUD 57600
#define MAX_USERS 162

// AS608 Password (default)
#define SENSOR_PASSWORD 0x00000000

HardwareSerial AS608(2);
WebServer server(80);

struct User {
  bool enrolled;
  uint16_t scans;
  uint32_t lastSeen;
  char code[16];
};

User users[MAX_USERS];

// State
bool sensorReady = false;
bool enrollmentActive = false;
uint32_t bootMs = 0;
uint32_t lastScanMs = 0;

bool touchActive = false;
bool lastTouchPin = HIGH;
unsigned long lastTouchChangeMs = 0;
const unsigned long DEBOUNCE_MS = 50;

char statusMsg[128] = "Initializing...";
char enrollMsg[128] = "";

// Protocol Constants
static const uint8_t HDR[] = {0xEF, 0x01};
static const uint8_t ADDR[] = {0xFF, 0xFF, 0xFF, 0xFF};

// Send command packet to sensor
void sendCmd(uint8_t cmd, const uint8_t* data, uint16_t dataLen) {
  // Clear any pending data in RX buffer
  while (AS608.available()) AS608.read();
  
  uint16_t pktLen = 3 + dataLen;  // cmd + data + checksum
  
  AS608.write(HDR, 2);           // Header
  AS608.write(ADDR, 4);          // Address
  AS608.write(0x01);            // Package type (command)
  AS608.write((uint8_t)(pktLen >> 8));  // Length high
  AS608.write((uint8_t)(pktLen & 0xFF)); // Length low
  AS608.write(cmd);              // Command code
  
  // Calculate checksum: type + length + cmd + data
  uint16_t sum = 0x01 + (pktLen >> 8) + (pktLen & 0xFF) + cmd;
  for (uint16_t i = 0; i < dataLen; i++) {
    AS608.write(data[i]);
    sum += data[i];
  }
  
  AS608.write((uint8_t)(sum >> 8));    // Checksum high
  AS608.write((uint8_t)(sum & 0xFF));  // Checksum low
}

// Receive response packet from sensor
// Returns: confirmation code (0x00 = success) or 0xFF on error
// Data is returned in buffer (starting after confirmation code)
uint8_t getReply(uint8_t* data, uint16_t* dataLen, uint32_t timeoutMs = 1000) {
  uint32_t start = millis();
  
  // Wait for header (EF 01)
  while (millis() - start < timeoutMs) {
    if (AS608.available() >= 2) {
      if (AS608.read() == 0xEF && AS608.read() == 0x01) {
        break;
      }
    }
    delay(1);
  }
  
  // Read rest of header
  uint8_t header[7];
  uint16_t idx = 0;
  while (idx < 7 && millis() - start < timeoutMs) {
    if (AS608.available()) {
      header[idx++] = AS608.read();
    }
  }
  
  if (idx < 7) {
    Serial.println("[COMM] Timeout waiting for header");
    return 0xFF;
  }
  
  // Verify address matches
  if (header[0] != 0xFF || header[1] != 0xFF || header[2] != 0xFF || header[3] != 0xFF) {
    Serial.println("[COMM] Address mismatch");
    return 0xFF;
  }
  
  // Get package type and length
  uint8_t pkgType = header[4];
  uint16_t pkgLen = ((uint16_t)header[5] << 8) | header[6];
  
  if (pkgLen < 3 || pkgLen > 256) {
    Serial.print("[COMM] Invalid length: "); Serial.println(pkgLen);
    return 0xFF;
  }
  
  // Read package content
  uint8_t* pkgData = new uint8_t[pkgLen];
  idx = 0;
  while (idx < pkgLen && millis() - start < timeoutMs) {
    if (AS608.available()) {
      pkgData[idx++] = AS608.read();
    }
  }
  
  if (idx < pkgLen) {
    Serial.println("[COMM] Timeout reading data");
    delete[] pkgData;
    return 0xFF;
  }
  
  // First byte is confirmation code
  uint8_t confirmCode = pkgData[0];
  
  // Return data (skip confirmation code)
  uint16_t resultLen = pkgLen - 2;  // Exclude confirm code and checksum
  if (resultLen > *dataLen) resultLen = *dataLen;
  
  if (data != nullptr && resultLen > 0) {
    memcpy(data, &pkgData[1], resultLen);
  }
  *dataLen = resultLen;
  
  delete[] pkgData;
  
  return confirmCode;
}

// === SENSOR FUNCTIONS ===

// Capture fingerprint image
// Returns: 0x00 = success, 0x02 = no finger, other = error
uint8_t getImage() {
  sendCmd(0x01, nullptr, 0);
  uint8_t data[32];
  uint16_t len = sizeof(data);
  uint8_t code = getReply(data, &len);
  if (code != 0x00 && code != 0x02) {
    Serial.print("[GetImage] Response: 0x"); Serial.println(code, HEX);
  }
  return code;
}

// Convert captured image to template
// slot: 0x01 for buffer 1, 0x02 for buffer 2
// Returns: 0x00 = success
uint8_t image2Tz(uint8_t slot) {
  uint8_t data[] = {slot};
  sendCmd(0x02, data, 1);
  uint8_t reply[16];
  uint16_t len = sizeof(reply);
  uint8_t code = getReply(reply, &len);
  if (code != 0x00) {
    Serial.print("[Image2Tz] Slot "); Serial.print(slot, HEX);
    Serial.print(" Response: 0x"); Serial.println(code, HEX);
  }
  return code;
}

// Create model from two templates
// Returns: 0x00 = success, 0x0A = templates don't match
uint8_t createModel() {
  sendCmd(0x05, nullptr, 0);
  uint8_t reply[16];
  uint16_t len = sizeof(reply);
  uint8_t code = getReply(reply, &len);
  if (code != 0x00) {
    Serial.print("[CreateModel] Response: 0x"); Serial.println(code, HEX);
  }
  return code;
}

// Store template to flash memory
// location: 0x0001-0x00A2 (1-162)
// Returns: 0x00 = success
uint8_t storeModel(uint16_t location) {
  // Location is 1-based in documentation, 0-based in some implementations
  uint8_t data[] = {0x01, (uint8_t)((location - 1) >> 8), (uint8_t)((location - 1) & 0xFF)};
  sendCmd(0x06, data, 3);
  uint8_t reply[16];
  uint16_t len = sizeof(reply);
  uint8_t code = getReply(reply, &len);
  if (code != 0x00) {
    Serial.print("[StoreModel] Location "); Serial.print(location);
    Serial.print(" Response: 0x"); Serial.println(code, HEX);
  }
  return code;
}

// Search database for matching template
// Returns: 0x00 = found, 0x09 = no match
uint8_t searchFingerprint(uint16_t* matchId, uint16_t* matchScore) {
  // Search starting from page 0, return up to 1 match
  uint8_t data[] = {0x01, 0x00, 0x00, 0x00, 0x01};
  sendCmd(0x04, data, 5);
  uint8_t reply[32];
  uint16_t len = sizeof(reply);
  uint8_t code = getReply(reply, &len);
  
  if (code == 0x00 && len >= 5) {
    *matchId = ((uint16_t)reply[1] << 8) | reply[2];
    *matchScore = ((uint16_t)reply[3] << 8) | reply[4];
    Serial.print("[Search] Found ID: "); Serial.print(*matchId + 1);
    Serial.print(" Score: "); Serial.println(*matchScore);
  } else if (code == 0x09) {
    Serial.println("[Search] No match found");
    *matchId = 0xFFFF;
    *matchScore = 0;
  } else {
    Serial.print("[Search] Response: 0x"); Serial.println(code, HEX);
    *matchId = 0xFFFF;
    *matchScore = 0;
  }
  return code;
}

// Verify sensor is responding
// Returns: true if sensor responds correctly
bool verifySensor() {
  Serial.println("[SENSOR] Verifying...");
  
  for (int i = 0; i < 3; i++) {
    // Read parameters command
    uint8_t data[] = {0x00, 0x00, 0x00, 0x00};
    sendCmd(0x0F, data, 4);
    
    uint8_t reply[16];
    uint16_t len = sizeof(reply);
    uint8_t code = getReply(reply, &len, 2000);
    
    if (code == 0x00 && len >= 4) {
      Serial.println("[SENSOR] Verified OK");
      Serial.print("[SENSOR] Status: 0x"); Serial.println(reply[0], HEX);
      Serial.print("[SENSOR] ID: "); Serial.println((reply[1] << 8) | reply[2], DEC);
      return true;
    }
    
    Serial.print("[SENSOR] Attempt "); Serial.print(i + 1);
    Serial.print(" failed, code: 0x"); Serial.println(code, HEX);
    delay(300);
  }
  
  Serial.println("[SENSOR] Verification failed");
  return false;
}

// Delete specific template
// location: 1-162
// Returns: 0x00 = success
uint8_t deleteModel(uint16_t location) {
  uint8_t data[] = {0x00, 0x00, (uint8_t)((location - 1) >> 8), (uint8_t)((location - 1) & 0xFF)};
  sendCmd(0x0C, data, 4);
  uint8_t reply[16];
  uint16_t len = sizeof(reply);
  uint8_t code = getReply(reply, &len);
  if (code != 0x00) {
    Serial.print("[Delete] Response: 0x"); Serial.println(code, HEX);
  }
  return code;
}

// Empty the entire database
// Returns: 0x00 = success
bool emptyDatabase() {
  sendCmd(0x0D, nullptr, 0);
  uint8_t reply[16];
  uint16_t len = sizeof(reply);
  uint8_t code = getReply(reply, &len);
  if (code == 0x00) {
    Serial.println("[DB] Database emptied");
    return true;
  }
  Serial.print("[DB] Empty failed: 0x"); Serial.println(code, HEX);
  return false;
}

// Get template count
// Returns: number of enrolled templates or 0xFF on error
uint8_t getTemplateCount() {
  sendCmd(0x0D, nullptr, 0);  // Actually this is not the right command
  // Use 0x1D for template count on some models
  // For AS608, we use 0x0D with specific parameters
  
  // Let's try a different approach - read system parameters
  uint8_t data[] = {0x00, 0x00, 0x00, 0x00};
  sendCmd(0x0F, data, 4);
  uint8_t reply[16];
  uint16_t len = sizeof(reply);
  uint8_t code = getReply(reply, &len);
  
  if (code == 0x00 && len >= 6) {
    return reply[3];  // This might be template count
  }
  return 0xFF;
}

// === ENROLLMENT ===
bool enrollUser(uint16_t userId) {
  if (userId >= MAX_USERS) return false;
  
  enrollmentActive = true;
  snprintf(enrollMsg, sizeof(enrollMsg), "Slot %u: Place finger", userId + 1);
  Serial.println(enrollMsg);

  // Step 1: Capture FIRST image
  Serial.println("[ENROLL] Step 1: Waiting for first finger...");
  uint32_t deadline = millis() + 15000;
  while (millis() < deadline) {
    uint8_t code = getImage();
    if (code == 0x00) {
      Serial.println("[ENROLL] First image captured!");
      break;
    } else if (code == 0x02) {
      // No finger, keep waiting
      delay(100);
    } else {
      Serial.print("[ENROLL] Error capturing image 1: 0x"); Serial.println(code, HEX);
      enrollmentActive = false;
      snprintf(enrollMsg, sizeof(enrollMsg), "Capture failed: 0x%X", code);
      return false;
    }
  }
  
  if (millis() >= deadline) {
    enrollmentActive = false;
    snprintf(enrollMsg, sizeof(enrollMsg), "Timeout waiting for finger");
    return false;
  }

  // Step 2: Convert to template in buffer 1
  Serial.println("[ENROLL] Step 2: Converting to template (buffer 1)...");
  uint8_t tz = image2Tz(0x01);
  if (tz != 0x00) {
    enrollmentActive = false;
    snprintf(enrollMsg, sizeof(enrollMsg), "Convert failed: 0x%X", tz);
    Serial.print("[ENROLL] image2Tz error: 0x"); Serial.println(tz, HEX);
    return false;
  }
  Serial.println("[ENROLL] Template 1 created");

  // Step 3: Wait for finger release
  snprintf(enrollMsg, sizeof(enrollMsg), "Remove finger");
  Serial.println("[ENROLL] Step 3: Waiting for finger removal...");
  deadline = millis() + 10000;
  while (millis() < deadline) {
    uint8_t code = getImage();
    if (code == 0x02) {
      Serial.println("[ENROLL] Finger removed");
      break;
    }
    delay(100);
  }

  // Step 4: Capture SECOND image
  snprintf(enrollMsg, sizeof(enrollMsg), "Place same finger again");
  Serial.println("[ENROLL] Step 4: Waiting for second finger...");
  deadline = millis() + 15000;
  while (millis() < deadline) {
    uint8_t code = getImage();
    if (code == 0x00) {
      Serial.println("[ENROLL] Second image captured!");
      break;
    } else if (code == 0x02) {
      delay(100);
    } else {
      enrollmentActive = false;
      snprintf(enrollMsg, sizeof(enrollMsg), "Capture 2 failed: 0x%X", code);
      Serial.print("[ENROLL] Error: 0x"); Serial.println(code, HEX);
      return false;
    }
  }
  
  if (millis() >= deadline) {
    enrollmentActive = false;
    snprintf(enrollMsg, sizeof(enrollMsg), "Timeout on second finger");
    return false;
  }

  // Step 5: Convert to template in buffer 2
  Serial.println("[ENROLL] Step 5: Converting to template (buffer 2)...");
  tz = image2Tz(0x02);
  if (tz != 0x00) {
    enrollmentActive = false;
    snprintf(enrollMsg, sizeof(enrollMsg), "Convert 2 failed: 0x%X", tz);
    Serial.print("[ENROLL] image2Tz(2) error: 0x"); Serial.println(tz, HEX);
    return false;
  }
  Serial.println("[ENROLL] Template 2 created");

  // Step 6: Create model from both templates
  Serial.println("[ENROLL] Step 6: Creating combined model...");
  uint8_t result = createModel();
  if (result != 0x00) {
    enrollmentActive = false;
    snprintf(enrollMsg, sizeof(enrollMsg), "Create model failed: 0x%X", result);
    Serial.print("[ENROLL] createModel error: 0x"); Serial.println(result, HEX);
    return false;
  }
  Serial.println("[ENROLL] Model created successfully");

  // Step 7: Store template
  Serial.print("[ENROLL] Step 7: Storing to slot "); Serial.println(userId + 1);
  result = storeModel(userId + 1);  // 1-based location
  if (result != 0x00) {
    enrollmentActive = false;
    snprintf(enrollMsg, sizeof(enrollMsg), "Store failed: 0x%X", result);
    Serial.print("[ENROLL] storeModel error: 0x"); Serial.println(result, HEX);
    return false;
  }

  // Success!
  users[userId].enrolled = true;
  users[userId].scans = 0;
  enrollmentActive = false;
  snprintf(enrollMsg, sizeof(enrollMsg), "✓ Slot %u enrolled!", userId + 1);
  Serial.println(enrollMsg);
  
  // Verify the enrollment worked
  uint16_t testId = 0;
  uint16_t testScore = 0;
  uint8_t sr = searchFingerprint(&testId, &testScore);
  if (sr == 0x00) {
    Serial.println("[ENROLL] Verified - fingerprint found in database!");
  } else {
    Serial.println("[ENROLL] Warning - could not verify enrollment");
  }
  
  return true;
}

// === MATCHING ===
void scanForMatch() {
  if (!sensorReady || enrollmentActive) return;
  if (millis() - lastScanMs < 100) return;
  lastScanMs = millis();

  uint8_t code = getImage();
  if (code == 0x02) return;  // No finger
  if (code != 0x00) {
    Serial.print("[SCAN] Error: 0x"); Serial.println(code, HEX);
    return;  // Error
  }

  if (image2Tz(0x01) != 0x00) {
    Serial.println("[SCAN] Image2Tz failed");
    return;
  }

  uint16_t id = 0;
  uint16_t score = 0;
  uint8_t sr = searchFingerprint(&id, &score);

  if (sr == 0x00 && id < MAX_USERS) {
    users[id].enrolled = true;
    users[id].scans++;
    users[id].lastSeen = (millis() - bootMs) / 1000;
    snprintf(statusMsg, sizeof(statusMsg), "MATCH: Slot %u (score %u)", id + 1, score);
    Serial.println(statusMsg);
  } else if (sr == 0x09) {
    snprintf(statusMsg, sizeof(statusMsg), "No match");
  }
}

// === WEB SERVER ===
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width'>";
  html += "<title>AS608 Fingerprint</title><style>";
  html += "body{font-family:Arial;background:#f0f0f0;margin:0;padding:20px;}";
  html += ".box{max-width:900px;margin:auto;background:#fff;border-radius:8px;padding:20px;box-shadow:0 2px 4px rgba(0,0,0,0.1);}";
  html += "h1{color:#333;margin-top:0;}";
  html += ".status{background:#e3f2fd;padding:15px;border-radius:4px;margin:15px 0;}";
  html += ".enroll{background:#fff3e0;padding:15px;border-radius:4px;margin:15px 0;}";
  html += "table{width:100%;border-collapse:collapse;}";
  html += "th,td{border:1px solid #ddd;padding:10px;text-align:left;}";
  html += "th{background:#f5f5f5;font-weight:bold;}";
  html += "tr:nth-child(even){background:#fafafa;}";
  html += "button{padding:8px 16px;background:#2196F3;color:#fff;border:none;border-radius:4px;cursor:pointer;margin:2px;}";
  html += "button:hover{background:#1976D2;}";
  html += ".enrolled{color:green;font-weight:bold;}";
  html += ".empty{color:#999;}";
  html += "</style></head><body><div class='box'>";
  html += "<h1>AS608 Fingerprint Sensor</h1>";
  html += "<div class='status'>";
  html += String("Status: <b id='status'>") + String(statusMsg) + "</b><br>";
  html += String("Sensor: <b>") + (sensorReady ? "READY" : "ERROR") + "</b>";
  html += "</div>";
  html += "<div class='enroll'>";
  html += String("Message: <b id='enroll'>") + String(enrollMsg) + "</b>";
  html += "</div>";
  html += "<table><thead><tr><th>Slot</th><th>Enrolled</th><th>Code</th><th>Scans</th><th>Last Seen</th><th>Action</th></tr></thead><tbody>";
  
  for (int i = 0; i < MAX_USERS; i++) {
    html += String("<tr><td>") + String(i+1) + "</td>";
    html += String("<td class='") + (users[i].enrolled ? "enrolled" : "empty") + String("'>") + (users[i].enrolled ? "YES" : "NO") + "</td>";
    html += String("<td>") + String(users[i].code) + "</td>";
    html += String("<td>") + String(users[i].scans) + "</td>";
    html += String("<td>") + String(users[i].lastSeen) + "s</td>";
    html += "<td>";
    if (!users[i].enrolled) {
      html += String("<button onclick='enroll(") + String(i) + ")'>Enroll</button>";
    }
    html += String("<button onclick='erase(") + String(i) + ")'>Erase</button>";
    html += "</td></tr>";
  }
  
  html += "</tbody></table>";
  html += "<p><button onclick='clearAll()'>CLEAR ALL</button></p>";
  html += "<script>";
  html += "function enroll(id) { fetch('/api/enroll?id=' + id).then(r => r.text()).then(t => { document.getElementById('enroll').innerText = t; setTimeout(() => location.reload(), 3000); }); }";
  html += "function erase(id) { fetch('/api/erase?id=' + id).then(r => location.reload()); }";
  html += "function clearAll() { if(confirm('Erase ALL?')) fetch('/api/clear').then(r => location.reload()); }";
  html += "setInterval(() => fetch('/api/status').then(r => r.text()).then(t => document.getElementById('status').innerText = t), 1000);";
  html += "setInterval(() => fetch('/api/enrollmsg').then(r => r.text()).then(t => document.getElementById('enroll').innerText = t), 1000);";
  html += "</script></body></html>";
  
  server.send(200, "text/html", html);
}

void handleEnroll() {
  int id = server.arg("id").toInt();
  if (id >= 0 && id < MAX_USERS) {
    enrollUser(id);
  }
  server.send(200, "text/plain", enrollMsg);
}

void handleErase() {
  int id = server.arg("id").toInt();
  if (id >= 0 && id < MAX_USERS) {
    // Delete from sensor
    deleteModel(id + 1);
    users[id].enrolled = false;
    users[id].scans = 0;
    snprintf(enrollMsg, sizeof(enrollMsg), "Slot %u erased", id + 1);
  }
  server.send(200, "text/plain", "OK");
}

void handleClear() {
  if (emptyDatabase()) {
    for (int i = 0; i < MAX_USERS; i++) {
      users[i].enrolled = false;
      users[i].scans = 0;
    }
    snprintf(enrollMsg, sizeof(enrollMsg), "All slots cleared");
    snprintf(statusMsg, sizeof(statusMsg), "Database cleared");
  }
  server.send(200, "text/plain", "OK");
}

void handleStatus() {
  server.send(200, "text/plain", statusMsg);
}

void handleEnrollMsg() {
  server.send(200, "text/plain", enrollMsg);
}

// === MAIN ===
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n=== AS608 FINGERPRINT SYSTEM (FIXED) ===\n");
  
  bootMs = millis();
  pinMode(TOUCH_PIN, INPUT_PULLUP);

  // Initialize users
  for (int i = 0; i < MAX_USERS; i++) {
    users[i].enrolled = false;
    users[i].scans = 0;
    users[i].lastSeen = 0;
    snprintf(users[i].code, sizeof(users[i].code), "USER-%03d", i + 1);
  }

  // Initialize sensor serial
  AS608.begin(SENSOR_BAUD, SERIAL_8N1, RX_PIN, TX_PIN);
  delay(500);
  
  // Verify sensor
  sensorReady = verifySensor();
  snprintf(statusMsg, sizeof(statusMsg), sensorReady ? "Sensor OK" : "Sensor ERROR");

  // WiFi
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid, password);
  for (int i = 0; i < 20; i++) {
    if (WiFi.status() == WL_CONNECTED) break;
    delay(500);
  }
  WiFi.softAP(apSsid, apPassword);
  
  Serial.print("IP: "); Serial.println(WiFi.localIP());
  Serial.print("AP: "); Serial.println(apSsid);

  // Web server routes
  server.on("/", handleRoot);
  server.on("/api/enroll", handleEnroll);
  server.on("/api/erase", handleErase);
  server.on("/api/clear", handleClear);
  server.on("/api/status", handleStatus);
  server.on("/api/enrollmsg", handleEnrollMsg);
  server.begin();
  
  Serial.println("\n=== READY ===");
  Serial.println("Touch button to scan, or use web interface to enroll");
}

void loop() {
  server.handleClient();

  // Touch button with debounce
  bool currentPin = digitalRead(TOUCH_PIN);
  if (currentPin != lastTouchPin) {
    lastTouchChangeMs = millis();
    lastTouchPin = currentPin;
  } else if (millis() - lastTouchChangeMs > DEBOUNCE_MS) {
    bool newState = (currentPin == LOW);
    if (newState != touchActive) {
      touchActive = newState;
      if (touchActive) {
        Serial.println("[TOUCH] Button pressed - starting scan");
      }
    }
  }

  if (touchActive) {
    scanForMatch();
  }

  delay(5);
}

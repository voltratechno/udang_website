#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <HX711.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>

// =====================
// WIFI CONFIG
// =====================
const char* ssid = "Shopeefood";
const char* password = "samasamaaaa";

// =====================
// MQTT CONFIG (EMQX Public Broker)
// =====================
const char* mqtt_server = "broker.emqx.io";
const int mqtt_port = 1883;
const char* mqtt_client_id = "esp32_shrimp_feeder_001";
const char* topic_cmd = "shrimp/feeder/cmd";      // Subscribe: perintah dari website
const char* topic_state = "shrimp/feeder/state";   // Publish: status hardware
const char* topic_weight = "shrimp/feeder/weight"; // Publish: berat loadcell
const char* topic_encoder = "shrimp/feeder/encoder"; // Publish: posisi encoder

WiFiClient espClient;
PubSubClient client(espClient);

// =====================
// HX711 + LCD
// =====================
HX711 scale;
LiquidCrystal_I2C lcd(0x27, 16, 2);

uint8_t dataPin = 15;
uint8_t clockPin = 5;

float berat;
float threshold = 50.0;  // Diubah dari 10.0 menjadi 50.0

// MOTOR + ENCODER
#define RPWM 26
#define LPWM 25
#define ENCODER_A 34
#define ENCODER_B 35
#define POSISI_BUKA   600
#define POSISI_TUTUP -100
#define PWM_MOTOR 100
#define TOLERANSI 5

volatile long encoderCount = 0;
bool posisiBuka = false;
bool motorRunning = false;
long targetEncoder = POSISI_TUTUP;

// =====================
// SSR (BLOWER)
// =====================
#define SSR_PIN 14
bool ssrState = false;

// =====================
// SERVO
// =====================
#define SERVO_PIN 23
Servo myservo;
bool servoState = false;  // false = 0°, true = 50°

// =====================
// PUSH BUTTON
// =====================
#define PB1 32  // Motor Katup
#define PB2 33  // SSR Blower
#define PB3 27  // Servo

// =====================
// VARIABEL SISTEM
// =====================
unsigned long lastPublishTime = 0;
unsigned long lastLCDUpdate = 0;
const unsigned long publishInterval = 500;  // Publish MQTT setiap 500ms
bool wifiConnected = false;
bool mqttConnected = false;

// Mode operasi
enum Mode { MODE_MANUAL, MODE_REMOTE };
Mode currentMode = MODE_MANUAL;  // Default manual
bool remoteLock = false;         // Lock saat ada perintah remote

// =====================
// FUNGSI UNTUK MENDAPATKAN BERAT YANG DIVALIDASI
// =====================
float getValidatedWeight() {
  float rawWeight = scale.get_units(5);
  
  // Jika nilai absolut < 50 gram atau nilai negatif, return 0
  if (rawWeight < 50.0 || rawWeight < 0) {
    return 0.0;
  }
  
  return rawWeight;
}

// =====================
// ENCODER ISR
// =====================
void IRAM_ATTR encoderISR() {
  if (digitalRead(ENCODER_A) == digitalRead(ENCODER_B))
    encoderCount++;
  else
    encoderCount--;
}

// =====================
// MOTOR CONTROL
// =====================
void motorMaju() {
  analogWrite(RPWM, PWM_MOTOR);
  analogWrite(LPWM, 0);
}

void motorMundur() {
  analogWrite(RPWM, 0);
  analogWrite(LPWM, PWM_MOTOR);
}

void stopMotor() {
  analogWrite(RPWM, 0);
  analogWrite(LPWM, 0);
  motorRunning = false;
}

void setMotorPosition(bool buka) {
  if (motorRunning) return;  // Jangan ganggu jika sedang bergerak
  
  posisiBuka = buka;
  
  if (buka) {
    targetEncoder = POSISI_BUKA;
    Serial.println("➡️ Menuju POSISI BUKA");
    if (encoderCount < POSISI_BUKA)
      motorMaju();
    else
      motorMundur();
  } else {
    targetEncoder = POSISI_TUTUP;
    Serial.println("⬅️ Menuju POSISI TUTUP");
    if (encoderCount > POSISI_TUTUP)
      motorMundur();
    else
      motorMaju();
  }
  
  motorRunning = true;
}

// =====================
// SSR CONTROL
// =====================
void setSSR(bool on) {
  ssrState = on;
  digitalWrite(SSR_PIN, on ? HIGH : LOW);
  Serial.println(on ? "💨 SSR ON" : "💨 SSR OFF");
}

// =====================
// SERVO CONTROL
// =====================
void setServo(bool open) {
  servoState = open;
  
  if (open) {
    for (int i = myservo.read(); i <= 50; i++) {
      myservo.write(i);
      delay(15);
    }
    Serial.println("🔓 Servo 50° (Buka)");
  } else {
    for (int i = myservo.read(); i >= 0; i--) {
      myservo.write(i);
      delay(15);
    }
    Serial.println("🔒 Servo 0° (Tutup)");
  }
}

// =====================
// MQTT CALLBACK
// =====================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  Serial.print("📥 MQTT Received [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(message);
  
  // Parse JSON
  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, message);
  
  if (error) {
    Serial.print("❌ JSON Parse Error: ");
    Serial.println(error.c_str());
    return;
  }
  
  const char* action = doc["action"];
  int value = doc["value"] | 0;
  
  // Switch ke mode remote saat ada perintah MQTT
  currentMode = MODE_REMOTE;
  remoteLock = true;
  
  if (strcmp(action, "motor") == 0) {
    setMotorPosition(value == 1);
  }
  else if (strcmp(action, "ssr") == 0) {
    setSSR(value == 1);
  }
  else if (strcmp(action, "servo") == 0) {
    setServo(value == 50);
  }
  else if (strcmp(action, "get_status") == 0) {
    publishFullStatus();
  }
  else if (strcmp(action, "release") == 0) {
    // Kembalikan ke mode manual
    currentMode = MODE_MANUAL;
    remoteLock = false;
    Serial.println("🔓 Mode dikembalikan ke MANUAL");
  }
  
  remoteLock = false;
}

// =====================
// MQTT RECONNECT
// =====================
void reconnectMQTT() {
  while (!client.connected()) {
    Serial.print("🔄 Menghubungkan MQTT...");
    
    if (client.connect(mqtt_client_id)) {
      Serial.println(" ✅ Terhubung!");
      client.subscribe(topic_cmd);
      Serial.print("📡 Subscribe: ");
      Serial.println(topic_cmd);
      
      // Publish status awal
      publishFullStatus();
      mqttConnected = true;
    } else {
      Serial.print(" ❌ Gagal, rc=");
      Serial.print(client.state());
      Serial.println(" Coba lagi 5 detik...");
      mqttConnected = false;
      delay(5000);
    }
  }
}

// =====================
// PUBLISH FUNCTIONS
// =====================
void publishWeight() {
  if (!client.connected()) return;
  
  StaticJsonDocument<100> doc;
  doc["weight"] = berat;  // berat sudah divalidasi di loop
  doc["unit"] = "gram";
  
  char buffer[100];
  serializeJson(doc, buffer);
  client.publish(topic_weight, buffer);
}

void publishEncoder() {
  if (!client.connected()) return;
  
  StaticJsonDocument<100> doc;
  doc["count"] = encoderCount;
  doc["position"] = posisiBuka ? "buka" : "tutup";
  
  char buffer[100];
  serializeJson(doc, buffer);
  client.publish(topic_encoder, buffer);
}

void publishFullStatus() {
  if (!client.connected()) return;
  
  StaticJsonDocument<256> doc;
  doc["weight"] = berat;  // berat sudah divalidasi di loop
  doc["encoder"] = encoderCount;
  doc["motor"] = posisiBuka ? 1 : 0;
  doc["ssr"] = ssrState ? 1 : 0;
  doc["servo"] = servoState ? 50 : 0;
  doc["mode"] = currentMode == MODE_REMOTE ? "remote" : "manual";
  
  char buffer[256];
  serializeJson(doc, buffer);
  client.publish(topic_state, buffer);
  
  Serial.print("📤 Publish Status: ");
  Serial.println(buffer);
}

// =====================
// LCD DISPLAY UPDATE
// =====================
void updateLCD() {
  lcd.setCursor(0, 0);
  lcd.print("B:");
  lcd.print(berat, 1);  // berat sudah 0 jika < 50g atau negatif
  lcd.print("g M:");
  lcd.print(posisiBuka ? "ON" : "OF");
  lcd.print("    ");
  
  lcd.setCursor(0, 1);
  lcd.print("S:");
  lcd.print(ssrState ? "ON" : "OF");
  lcd.print(" V:");
  lcd.print(servoState ? "50" : "0 ");
  lcd.print(" ");
  lcd.print(currentMode == MODE_REMOTE ? "RMT" : "MAN");
  lcd.print("  ");
}

// =====================
// WIFI SETUP
// =====================
void setupWiFi() {
  Serial.print("📶 Menghubungkan WiFi");
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("WiFi Connecting");
  
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("\n✅ WiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP().toString().substring(0, 8));
  } else {
    wifiConnected = false;
    Serial.println("\n⚠️ WiFi Gagal - Mode Offline");
    lcd.setCursor(0, 1);
    lcd.print("OFFLINE MODE");
  }
  delay(1000);
}

// =====================
// SETUP
// =====================
void setup() {
  Serial.begin(115200);
  Serial.println("\n\n╔══════════════════════════════╗");
  Serial.println("║  SHRIMP FEEDER ESP32 v2.0  ║");
  Serial.println("║  Dual Mode: Manual + MQTT  ║");
  Serial.println("╚══════════════════════════════╝\n");
  
  // Init Push Buttons
  pinMode(PB1, INPUT_PULLUP);
  pinMode(PB2, INPUT_PULLUP);
  pinMode(PB3, INPUT_PULLUP);
  
  // Init SSR
  pinMode(SSR_PIN, OUTPUT);
  digitalWrite(SSR_PIN, LOW);
  
  // Init LCD
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Inisialisasi...");
  lcd.setCursor(0, 1);
  lcd.print("Shrimp Feeder");
  delay(1500);
  
  // Init Loadcell
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Init Loadcell");
  lcd.setCursor(0, 1);
  lcd.print("Please Wait...");
  
  scale.begin(dataPin, clockPin);
  scale.set_offset(193344);
  scale.set_scale(104.434151);
  scale.tare();
  delay(1000);
  
  // Init Motor
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Init Motor");
  lcd.setCursor(0, 1);
  lcd.print("Encoder Setup");
  
  pinMode(RPWM, OUTPUT);
  pinMode(LPWM, OUTPUT);
  pinMode(ENCODER_A, INPUT);
  pinMode(ENCODER_B, INPUT);
  
  attachInterrupt(digitalPinToInterrupt(ENCODER_A), encoderISR, CHANGE);
  
  analogWrite(RPWM, 0);
  analogWrite(LPWM, 0);
  delay(1000);
  
  // Init Servo
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Init Servo");
  lcd.setCursor(0, 1);
  lcd.print("Please Wait...");
  
  myservo.setPeriodHertz(50);
  myservo.attach(SERVO_PIN, 500, 2400);
  myservo.write(0);
  delay(1000);
  
  // Setup WiFi
  setupWiFi();
  
  // Setup MQTT
  if (wifiConnected) {
    client.setServer(mqtt_server, mqtt_port);
    client.setCallback(mqttCallback);
  }
  
  // Ready
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("SYSTEM READY");
  lcd.setCursor(0, 1);
  lcd.print(wifiConnected ? "WIFI + MQTT" : "OFFLINE MODE");
  delay(2000);
  
  Serial.println("\n✅ SYSTEM READY");
  Serial.println("Mode: " + String(wifiConnected ? "ONLINE (MQTT)" : "OFFLINE (Manual)"));
  Serial.println("Push Button: PB1=Motor, PB2=SSR, PB3=Servo\n");
}

// =====================
// LOOP
// =====================
void loop() {
  static bool lastPB1 = HIGH;
  static bool lastPB2 = HIGH;
  static bool lastPB3 = HIGH;
  
  // =====================
  // MQTT LOOP
  // =====================
  if (wifiConnected) {
    if (!client.connected()) {
      reconnectMQTT();
    }
    client.loop();
  }
  
  // =====================
  // BACA LOADCELL DENGAN VALIDASI
  // =====================
  berat = getValidatedWeight();  // Menggunakan fungsi validasi
  
  // =====================
  // PUSH BUTTON HANDLING (Mode Manual)
  // =====================
  bool pb1State = digitalRead(PB1);
  bool pb2State = digitalRead(PB2);
  bool pb3State = digitalRead(PB3);
  
  // Hanya proses push button jika mode manual
  if (currentMode == MODE_MANUAL) {
    
    // PB1 - Motor Katup
    if (lastPB1 == HIGH && pb1State == LOW && !motorRunning) {
      Serial.println("🔘 PB1: Toggle Motor");
      setMotorPosition(!posisiBuka);
      delay(50);
    }
    
    // PB2 - SSR Blower
    if (lastPB2 == HIGH && pb2State == LOW) {
      Serial.println("🔘 PB2: Toggle SSR");
      setSSR(!ssrState);
      delay(50);
    }
    
    // PB3 - Servo
    if (lastPB3 == HIGH && pb3State == LOW) {
      Serial.println("🔘 PB3: Toggle Servo");
      setServo(!servoState);
      delay(50);
    }
  }
  
  lastPB1 = pb1State;
  lastPB2 = pb2State;
  lastPB3 = pb3State;
  
  // =====================
  // KONTROL POSISI MOTOR
  // =====================
  if (motorRunning) {
    if (targetEncoder == POSISI_BUKA) {
      if (encoderCount >= (POSISI_BUKA - TOLERANSI)) {
        stopMotor();
        Serial.print("✅ BUKA TERCAPAI: ");
        Serial.println(encoderCount);
      }
    }
    
    if (targetEncoder == POSISI_TUTUP) {
      if (encoderCount <= (POSISI_TUTUP + TOLERANSI)) {
        stopMotor();
        Serial.print("✅ TUTUP TERCAPAI: ");
        Serial.println(encoderCount);
      }
    }
  }
  
  // =====================
  // PUBLISH MQTT (Periodik)
  // =====================
  unsigned long now = millis();
  if (now - lastPublishTime >= publishInterval) {
    lastPublishTime = now;
    
    if (client.connected()) {
      publishWeight();
      publishEncoder();
    }
  }
  
  // =====================
  // PUBLISH FULL STATUS (Setiap 5 detik)
  // =====================
  static unsigned long lastFullPublish = 0;
  if (now - lastFullPublish >= 5000) {
    lastFullPublish = now;
    if (client.connected()) {
      publishFullStatus();
    }
  }
  
  // =====================
  // LCD DISPLAY UPDATE
  // =====================
  if (now - lastLCDUpdate >= 500) {
    lastLCDUpdate = now;
    updateLCD();
  }
  
  // =====================
  // SERIAL MONITOR (Debug)
  // =====================
  static unsigned long lastSerialPrint = 0;
  if (now - lastSerialPrint >= 1000) {
    lastSerialPrint = now;
    Serial.print("Berat: ");
    Serial.print(berat, 1);  // berat sudah 0 jika < 50g atau negatif
    Serial.print("g | Encoder: ");
    Serial.print(encoderCount);
    Serial.print(" | Motor: ");
    Serial.print(posisiBuka ? "BUKA" : "TUTUP");
    Serial.print(" | SSR: ");
    Serial.print(ssrState ? "ON" : "OFF");
    Serial.print(" | Servo: ");
    Serial.print(servoState ? "50°" : "0°");
    Serial.print(" | Mode: ");
    Serial.println(currentMode == MODE_REMOTE ? "REMOTE" : "MANUAL");
  }
  
  delay(50);
}
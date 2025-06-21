/**************************************************************************************************
 * Improved Fingerprint Attendance System for ESP32
 * Version: 5.3 (Fully Commented)
 *
 * Description:
 * A comprehensive fingerprint attendance system using an ESP32. It is designed to be
 * robust and user-friendly, with features for online and offline operation.
 *
 * Key Features:
 * - On-Demand WiFi Setup: Uses WiFiManager to create a web portal for WiFi configuration
 * only when a dedicated button is held down, allowing for fast boot-up in normal operation.
 * - Offline Logging: If no WiFi is available, attendance logs are saved to an SD card.
 * - Automatic Sync: Saved offline logs are automatically sent to the server once an
 * internet connection is established.
 * - Accurate Timestamps: Uses a DS3231 Real-Time Clock (RTC) with a backup battery
 * to ensure timestamps are always accurate, even after power loss.
 * - Full Data Logging: Logs include the User ID and a Unix timestamp for complete record-keeping.
 * - Secure Server Communication: Uses HTTPS for secure data transmission.
 * - User-Friendly Interface: A 16x2 LCD provides clear instructions and feedback.
 * - low-power mode
 *
 **************************************************************************************************/

// --- LIBRARY INCLUSIONS ---
#include <Adafruit_Fingerprint.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LiquidCrystal.h>
#include "RTClib.h"
#include <SD.h>
#include <SPI.h>

// --- HARDWARE PIN DEFINITIONS ---
// LCD Pins (rs, en, d4, d5, d6, d7)
const int rs = 27, en = 26, d4 = 25, d5 = 33, d6 = 32, d7 = 14;
// Fingerprint Sensor RX/TX (connected to ESP32's Serial Port 2)
const int FINGERPRINT_RX = 16;
const int FINGERPRINT_TX = 17;
// Main operational button (for enrolling, menus, etc.)
const int BUTTON_PIN1 = 34;
// Button to trigger WiFiManager setup portal by long press (connect to GPIO 35 and GND)
const int BUTTON_PIN2 = 35;
// SD Card Chip Select (CS) Pin
const int SD_CS_PIN = 5;
const int MAX_FINGERS_PER_USER = 3;
// --- SERVER AND TIME CONFIGURATION ---
#define SERVER_HOST "https://192.168.1.12:7069" // The base URL of your backend server
#define NTP_SERVER "pool.ntp.org"             // Network Time Protocol server for initial time sync
#define GMT_OFFSET_SEC 3600 * 2               // GMT offset for your timezone (e.g., UTC+2 for Egypt Standard Time)
#define DAYLIGHT_OFFSET_SEC 0                 // Daylight saving offset (0 if not applicable)

// --- STATE MANAGEMENT ---
// Enum to track the current menu state for the main button
enum class MenuState {
  MAIN_MENU,
  OPTIONS_MENU
};

MenuState currentMenuState = MenuState::MAIN_MENU;

// --- OBJECT INITIALIZATIONS ---
LiquidCrystal lcd(rs, en, d4, d5, d6, d7);
HardwareSerial mySerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);
RTC_DS3231 rtc;

// --- GLOBAL VARIABLES ---
unsigned long button1PressTime = 0;
unsigned long button2PressTime = 0;
const int longPressDuration = 2000; // 2 seconds for a long press
bool wifiManagerActive = false;
unsigned long lastActivityTime = 0;
const unsigned long idleTimeout = 30000; // 30 seconds to enter low power mode

// --- FUNCTION PROTOTYPES ---
// Core System & UI
void displayMessage(String line1, String line2 = "", int delayMs = 0);
void handleButton1();
void handleButton2();
void enterLowPowerMode();

// WiFi & Server
void setupWiFi();
void startWifiManagerPortal();
bool logAttendanceToServer(uint16_t id, time_t timestamp);
bool clearAllFingerprintsFromServer();
// Fingerprint Operations
void scanForFingerprint();
void enrollNewFingerprint();
int getFingerprintImage(int step);
int createAndStoreModel(uint16_t id);

// RTC, SD Card, and Logging
void setupSDCard();
void setupRTC();
void syncRtcWithNtp();
void logAttendanceOffline(uint16_t id, time_t timestamp); 
void syncOfflineLogs();

// Menu Actions
void runMainMenuAction();
void runOptionsMenuAction();
void showOptionsMenu();
void attemptToClearAllData();

//
uint16_t findNextAvailableID();
uint16_t getNextAvailableIDFromServer();
void sendEnrollmentCompleteToServer(uint16_t primaryId, uint16_t* fingerIds, int count);
uint16_t getPrimaryUserIDFromServer(uint16_t fingerId);
uint16_t getPrimaryIdFromLocalMap(uint16_t sensorId);
void addIdMapping(uint16_t sId, uint16_t pId);
void saveIdMapToSD();
void loadIdMapFromSD();
void fetchAndStoreIdMapFromServer();

struct FingerIdMapping {
    uint16_t sensorId;
    uint16_t primaryUserId;
};

// مصفوفة لتخزين الخريطة في ذاكرة الـ ESP32 (حجم أقصى 128 ID على الحساس)
FingerIdMapping idMap[128];
int currentMapSize = 0;
/**************************************************************************************************
 * SETUP FUNCTION
 **************************************************************************************************/
void setup() {
  Serial.begin(115200);
  while (!Serial); // Wait for serial connection

  // --- Initialize LCD ---
  lcd.begin(16, 2);
  displayMessage("System Starting", "Please wait...");

  // --- Initialize Buttons ---
  pinMode(BUTTON_PIN1, INPUT_PULLUP);
  pinMode(BUTTON_PIN2, INPUT_PULLUP);

  // --- Initialize Fingerprint Sensor ---
  mySerial.begin(57600, SERIAL_8N1, FINGERPRINT_RX, FINGERPRINT_TX);
  finger.begin(57600);
  if (finger.verifyPassword()) {
    Serial.println("Found fingerprint sensor!");
  } else {
    Serial.println("Did not find fingerprint sensor :(");
    displayMessage("Sensor Error", "Check connection");
    while (1) { delay(1); }
  }

  // --- Initialize SD Card ---
  setupSDCard();

  // --- Initialize RTC ---
  setupRTC();

  // --- Setup WiFi ---
  setupWiFi();

  // --- Initial Sync of Offline Logs ---
  syncOfflineLogs();

  loadIdMapFromSD();

  if (WiFi.status() == WL_CONNECTED) {
        fetchAndStoreIdMapFromServer();
    }

  // --- Display Main Menu ---
  displayMessage("btn1:add finger", "btn2:manage wifi ");
  lastActivityTime = millis();
}

/**************************************************************************************************
 * MAIN LOOP
 **************************************************************************************************/
void loop() {
  handleButton1();
  handleButton2();

  if (!wifiManagerActive) {
    scanForFingerprint();
  }
  // Check for idle timeout to enter low power mode
  if (millis() - lastActivityTime > idleTimeout) {
    enterLowPowerMode();
  }

  static unsigned long lastMapSyncAttempt = 0;
  const unsigned long MAP_SYNC_INTERVAL_MS = 3600000; // حاول كل ساعة (3.6 مليون مللي ثانية)

  if (WiFi.status() == WL_CONNECTED && millis() - lastMapSyncAttempt >= MAP_SYNC_INTERVAL_MS) {
      fetchAndStoreIdMapFromServer();
      lastMapSyncAttempt = millis();
    }

  // Handle periodic tasks
  syncOfflineLogs(); // Attempt to sync offline logs periodically

  delay(50); // Small delay to prevent high CPU usage
}

/**************************************************************************************************
 * CORE SYSTEM & UI FUNCTIONS
 **************************************************************************************************/

/**
 * @brief Displays a message on the LCD.
 * @param line1 The text for the first line.
 * @param line2 The text for the second line (optional).
 * @param delayMs The duration to display the message (optional).
 */
void displayMessage(String line1, String line2, int delayMs) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
  if (delayMs > 0) {
    delay(delayMs);
  }
  lastActivityTime = (millis());
}

/**
 * @brief Enters a low power sleep mode.
 */
void enterLowPowerMode() {
  displayMessage("Entering Sleep", "Press BTN 1&2...");
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_34, 0); // Wake up on BTN1 press
  esp_sleep_enable_ext1_wakeup((1ULL << BUTTON_PIN2), ESP_EXT1_WAKEUP_ALL_LOW); // Wake up on BTN2 press
  esp_deep_sleep_start();
}

/**************************************************************************************************
 * BUTTON HANDLING FUNCTIONS
 **************************************************************************************************/

/**
 * @brief Handles the logic for Button 1 (add/options).
 */
void handleButton1() {
  if (digitalRead(BUTTON_PIN1) == LOW) {
    if (button1PressTime == 0) {
      button1PressTime = millis();
    }
  } else {
    if (button1PressTime != 0) {
      unsigned long pressDuration = millis() - button1PressTime;
      if (pressDuration > longPressDuration) { // Long press
        if (currentMenuState == MenuState::MAIN_MENU) {
          showOptionsMenu();
        } else { // In options menu
          attemptToClearAllData();
        }
      } else { // Short press
        if (currentMenuState == MenuState::MAIN_MENU) {
          enrollNewFingerprint();
        }
      }
      button1PressTime = 0;
      lastActivityTime = millis();
      displayMessage("btn1:add finger", "btn2:manage wifi ");
    }
  }
}

/**
 * @brief Handles the logic for Button 2 (WiFiManager).
 */
void handleButton2() {
  if (digitalRead(BUTTON_PIN2) == LOW) {
    if (button2PressTime == 0) {
      button2PressTime = millis();
    }
    if (millis() - button2PressTime > longPressDuration && !wifiManagerActive) {
      startWifiManagerPortal();
    }
  } else {
    if (button2PressTime != 0) {
      button2PressTime = 0;
      lastActivityTime = millis();
    }
  }
}

/**************************************************************************************************
 * WIFI & SERVER FUNCTIONS
 **************************************************************************************************/

/**
 * @brief Sets up the WiFi connection.
 */
void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.setConnectTimeout(20);
  bool res = wm.autoConnect("FingerprintSetupAP");

  if (!res) {
    Serial.println("Failed to connect");
    displayMessage("WiFi Failed", "Check APP",200);
    
  } else {
    displayMessage("WiFi connected...yeey :)","",200);
    Serial.println("WiFi connected...yeey :)");
    syncRtcWithNtp();
    
  }
}

/**
 * @brief Starts the WiFiManager portal for on-demand configuration.
 */
void startWifiManagerPortal() {
  wifiManagerActive = true;
  displayMessage("WiFi Setup Mode", "Connect to APP");
  WiFiManager wm;
  wm.resetSettings();
  if (!wm.startConfigPortal("FingerprintSetupAP")) {
    Serial.println("failed to connect and hit timeout");
    delay(3000);
    ESP.restart();
    delay(5000);
  }
  Serial.println("connected...yeey :)");
  displayMessage("WiFi Connected", "Restarting...");
  delay(2000);
  ESP.restart();
}

/**
 * @brief Logs attendance data to the server.
 * @param id The user's fingerprint ID.
 * @param timestamp The time of attendance.
 * @return True if successful, false otherwise.
 */
bool logAttendanceToServer(uint16_t id, time_t timestamp) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  HTTPClient http;
  String serverPath = String(SERVER_HOST) + "/api/SensorData";
  http.begin(serverPath);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<200> doc;
  doc["fingerprintId"] = id;
  doc["timestamp"] = timestamp;

  String requestBody;
  serializeJson(doc, requestBody);

  int httpResponseCode = http.POST(requestBody);
  bool success = false;

  if (httpResponseCode > 0) {
    String payload = http.getString();
    Serial.println(httpResponseCode);
    Serial.println(payload);
    if (httpResponseCode == HTTP_CODE_OK) {
      success = true;
    }
  } else {
    Serial.print("Error on sending POST: ");
    Serial.println(httpResponseCode);
  }

  http.end();
  return success;
}


// --- WIFI & SERVER FUNCTIONS (تكملة للقسم الموجود) ---

// ... (باقي وظائف الواي فاي والخادم) ...

/**
 * @brief يرسل طلبًا إلى السيرفر لمسح جميع بيانات البصمات.
 * @return True إذا نجح الطلب، False بخلاف ذلك.
 */
bool clearAllFingerprintsFromServer() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected. Cannot clear data from server.");
    return false; // لا يمكن المسح من السيرفر بدون اتصال واي فاي
  }

  HTTPClient http;
  String serverPath = String(SERVER_HOST) + "/api/SensorData/clear"; // مسار الـ API لمسح البيانات
  http.begin(serverPath);

  // السيرفر غالبًا بيتوقع طلب DELETE لهذه العملية، تأكد من نوع الطلب الصحيح في الـ backend.
  // لو الـ Backend بتاعك بيستخدم POST لعملية المسح الكلي، غيرها لـ http.POST("").
  int httpResponseCode = http.POST(""); // إرسال طلب DELETE

  bool success = false;
  if (httpResponseCode > 0) {
    String payload = http.getString();
    Serial.print("Server Clear Data Response Code: ");
    Serial.println(httpResponseCode);
    Serial.print("Payload: ");
    Serial.println(payload);

    // عادةً ما يكون كود 200 OK أو 204 No Content هو مؤشر على النجاح في عمليات DELETE
    if (httpResponseCode == HTTP_CODE_OK || httpResponseCode == HTTP_CODE_NO_CONTENT) {
      success = true;
      Serial.println("Server data cleared successfully.");
    } else {
      Serial.println("Server reported an error during clear operation.");
    }
  } else {
    Serial.print("Error sending DELETE request to server: ");
    Serial.println(httpResponseCode);
  }

  http.end();
  return success;
}
/**************************************************************************************************
 * FINGERPRINT SENSOR FUNCTIONS
 **************************************************************************************************/

/**
 * @brief Scans for a fingerprint and logs attendance.
 */

void scanForFingerprint() {
    uint8_t p = finger.getImage();
    if (p != FINGERPRINT_OK) return; // ارجع لو مفيش إصبع أو مشكلة في الصورة

    p = finger.image2Tz();
    if (p != FINGERPRINT_OK) return; // ارجع لو فيه خطأ في تحويل الصورة

    p = finger.fingerSearch(); // ابحث عن البصمة في قاعدة بيانات الحساس
    if (p == FINGERPRINT_OK) {
        uint16_t foundSensorID = finger.fingerID;   // الـ ID اللي لقاه الحساس
        uint16_t confidence = finger.confidence;    // مدى الثقة في التطابق

        Serial.printf("Fingerprint found on sensor. Sensor ID: %d, Confidence: %d\n", foundSensorID, confidence);

        uint16_t primaryUserID = 0; // ده اللي هيشيل الـ ID الرئيسي

        if (WiFi.status() == WL_CONNECTED) {
            // لو فيه إنترنت، حاول تجيب الـ Primary User ID من السيرفر
            primaryUserID = getPrimaryUserIDFromServer(foundSensorID);
            // لو السيرفر رجع ID (مش 0)، ممكن نحدّث بيه الخريطة المحلية فورًا
            if (primaryUserID != 0) {
                 addIdMapping(foundSensorID, primaryUserID); // عشان لو لسه متضافش أو اتغير
                 saveIdMapToSD(); // وحفظ الخريطة بعد التحديث
            }
        } else {
            // لو مفيش إنترنت، استخدم الخريطة المحلية
            primaryUserID = getPrimaryIdFromLocalMap(foundSensorID);
            if (primaryUserID != 0) {
                Serial.println("Offline mode: Primary User ID found in local map.");
            } else {
                Serial.println("Offline mode: Primary User ID NOT found in local map for this Sensor ID.");
            }
        }

        if (primaryUserID != 0) {
            // *** هنا الجزء المطلوب: عرض "Accept" و الـ Primary User ID ***
            displayMessage("Accept!", "ID: " + String(primaryUserID), 2000);
            Serial.printf("Recognized Primary User ID: %d (via Sensor ID: %d).\n", primaryUserID, foundSensorID);
            DateTime now = rtc.now(); // جلب الوقت من الـ RTC
            // سجل الحضور دايماً، سواء أوفلاين أو أونلاين
            if (WiFi.status() == WL_CONNECTED) {
                logAttendanceToServer(primaryUserID, now.unixtime()); // لو النت موجود، ابعت للسيرفر
            } else {
                logAttendanceOffline(primaryUserID, now.unixtime()); // لو النت مقطوع، سجل أوفلاين
            }
        } else {
            // لو ملقاش الـ ID لا في السيرفر ولا في الخريطة المحلية (يعني بصمة غير مسجلة أو مجهولة)
            displayMessage("Unknown Finger", "Please Register", 1500);
            Serial.printf("Found Sensor ID %d (Confidence: %d) but no primary user found.\n", foundSensorID, confidence);
        }
    } else if (p == FINGERPRINT_NOFINGER) {
        // لا تفعل شيئًا، عادي لو مفيش إصبع.
    } else if (p == FINGERPRINT_NOTFOUND) {
        displayMessage("Finger not found", "", 1000); // رسالة عامة لو البصمة مش متسجلة
        Serial.println("Fingerprint not found in sensor database.");
    } else {
        displayMessage("Search Error", "Code: " + String(p), 1000);
        Serial.printf("Fingerprint search failed with error code: 0x%X\n", p);
    }
    lastActivityTime = millis(); // تحديث آخر وقت نشاط
    delay(1000); // تأخير بسيط
    displayMessage("btn1:add finger", "btn2:manage wifi "); // رسالة العودة للقائمة الرئيسية
}

/**
 * @brief Enrolls a new fingerprint.
 */
void enrollNewFingerprint() {
    displayMessage("Searching for ID", "Please wait...");

    // 1. الحصول على الـ ID الأساسي المتاح من السيرفر
    // ده الـ ID اللي هيرتبط بالمستخدم في قاعدة بيانات السيرفر
    uint16_t primaryUserId = getNextAvailableIDFromServer();

    if (primaryUserId == 128) { // Assuming 128 is an invalid or max ID
        displayMessage("No available IDs", "Storage full!");
        Serial.println("Error: No available IDs for enrollment on server.");
        return;
    }

    // هنا هنخزن كل الـ Sensor IDs اللي تم تسجيلها بنجاح على الحساس
    // وهنربطها بالـ Primary User ID اللي جبناه من السيرفر
    displayMessage("New User ID:", String(primaryUserId), 2000); // عرض الـ ID الأساسي للمستخدم
    delay(2000);
    uint16_t enrolledSensorIds[MAX_FINGERS_PER_USER];
    int currentEnrolledCount = 0;

    Serial.printf("Starting multi-finger enrollment for Primary User ID: %d\n", primaryUserId);

    for (int i = 0; i < MAX_FINGERS_PER_USER; i++) {
        // حساب الـ ID الجديد اللي هيتخزن على حساس البصمة
        // هنستخدم IDs متتالية بداية من الـ Primary User ID
        // (مثلاً لو Primary User ID هو 5، هنستخدم 5، 6، 7)
        uint16_t currentSensorId = primaryUserId + i;

        // التأكد إن الـ ID ده مش هيتجاوز سعة الحساس (127)
        // أو لو الجهاز بتاعك بيتعامل مع الـ IDs اللي جاية من السيرفر بشكل صارم
        // تأكد أن السيرفر مش هيرجع ID أساسي لو مفيش IDs كافية ومتاحة بعده
        if (currentSensorId >= finger.capacity) { // finger.capacity هي أقصى عدد IDs يدعمه الحساس
            Serial.println("Warning: Reached maximum sensor capacity during multi-enrollment, cannot store more fingerprints for this user.");
            break; // الخروج من الحلقة لو تجاوزنا السعة القصوى
        }

        displayMessage("Place finger", "ID #" + String(currentSensorId) + " (Pos " + String(i + 1) + "/" + String(MAX_FINGERS_PER_USER) + ")");
        Serial.printf("Enrolling Sensor ID %d, Position %d/%d\n", currentSensorId, i + 1, MAX_FINGERS_PER_USER);

        // التقاط الصورة الأولى
        displayMessage("Place finger", "(1/2) Pos " + String(i + 1));
        uint8_t p = getFingerprintImage(1); // ده بيحط الصورة في CharBuffer1
        if (p != FINGERPRINT_OK) {
            displayMessage("Error", "Try again (Img1)", 1500);
            Serial.printf("Failed to get image 1 for Sensor ID %d, code: 0x%X\n", currentSensorId, p);
            i--; // ارجع خطوة للخلف عشان يعيد نفس المحاولة لـ currentSensorId
            continue;
        }

        // التقاط الصورة الثانية (لإنشاء القالب)
        displayMessage("Place same", "(2/2) Pos " + String(i + 1));
        p = getFingerprintImage(2); // ده بيحط الصورة في CharBuffer2
        if (p != FINGERPRINT_OK) {
            displayMessage("Error", "Try again (Img2)", 1500);
            Serial.printf("Failed to get image 2 for Sensor ID %d, code: 0x%X\n", currentSensorId, p);
            i--; // ارجع خطوة للخلف عشان يعيد نفس المحاولة لـ currentSensorId
            continue;
        }

        // إنشاء القالب وربطه بالـ ID على الحساس
        p = finger.createModel(); // دمج الصورتين 1 و 2 لعمل قالب نهائي
        if (p == FINGERPRINT_OK) {
            p = finger.storeModel(currentSensorId); // تخزين القالب الجديد في الـ ID الحالي على الحساس
        }

        if (p == FINGERPRINT_OK) {
            displayMessage("Stored ID #" + String(currentSensorId), "", 1000);
            Serial.printf("Fingerprint stored successfully for Sensor ID %d.\n", currentSensorId);
            enrolledSensorIds[currentEnrolledCount++] = currentSensorId; // سجل الـ ID اللي تم تسجيله بنجاح
            addIdMapping(currentSensorId, primaryUserId);
            saveIdMapToSD();
            delay(1000); // إعطاء وقت للمستخدم لتغيير وضع الإصبع للـ "وجه" التالي
        } else {
            displayMessage("Store Error", "Code: " + String(p), 1500);
            Serial.printf("Failed to store model for Sensor ID %d, Code: 0x%X\n", currentSensorId, p);
            // لو فشل تخزين قالب، ممكن تسأل المستخدم يحاول تاني لنفس الـ ID
            i--; // ارجع خطوة للخلف عشان يعيد نفس المحاولة لـ currentSensorId
            continue;
        }
    }

    // 2. إبلاغ السيرفر بالـ IDs اللي تم استخدامها لهذا المستخدم
    if (currentEnrolledCount > 0) {
        displayMessage("Enrollment Done", "Syncing with server...", 1500);
        // استدعي الدالة الجديدة لإبلاغ السيرفر
        sendEnrollmentCompleteToServer(primaryUserId, enrolledSensorIds, currentEnrolledCount);
        displayMessage("Enrollment Done", "User " + String(primaryUserId) + " ready!", 2000);
    } else {
        displayMessage("Enrollment Failed", "No IDs stored!", 2000);
        Serial.println("Enrollment process completed, but no fingerprints were successfully stored.");
    }
}

/**
 * @brief Captures a fingerprint image.
 * @param step The enrollment step (1 or 2).
 * @return The status code from the sensor.
 */
int getFingerprintImage(int step) {
  uint8_t p = finger.getImage();
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
  }
  delay(100);
  p = finger.image2Tz(step);
  if (p == FINGERPRINT_OK) {
    displayMessage("Image taken");
  } else {
    displayMessage("Error", "Image error");
    return p;
  }
  return p;
}

/**
 * @brief Creates and stores a fingerprint model.
 * @param id The ID to store the model under.
 * @return The status code from the sensor.
 */
int createAndStoreModel(uint16_t id) {
  uint8_t p = finger.createModel();
  if (p == FINGERPRINT_OK) {
    displayMessage("Prints matched!","",200);
  } else {
    displayMessage("Mismatch", "Try again", 1000);
    return p;
  }

  p = finger.storeModel(id);
  if (p == FINGERPRINT_OK) {
    displayMessage("Stored!", "ID: " + String(id));
    DateTime now = rtc.now();
    logAttendanceToServer(id, now.unixtime()); // Log enrollment as first attendance
  } else {
    displayMessage("Store Error", "Code: " + String(p), 1000);
  }
  return p;
}

/**************************************************************************************************
 * RTC, SD CARD, AND LOGGING FUNCTIONS
 **************************************************************************************************/

/**
 * @brief Initializes the SD card.
 */
void setupSDCard() {
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("Card Mount Failed");
    displayMessage("SD Card Error", "Check card" , 1000);
    return;
  }
  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("No SD card attached");
    return;
  }
  Serial.println("SD Card initialized.");
}

/**
 * @brief Initializes the RTC module.
 */
void setupRTC() {
  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC");
    displayMessage("RTC Error", "Check module" , 1000);
    while (1);
  }
  if (rtc.lostPower()) {
    Serial.println("RTC lost power, let's set the time!");
    syncRtcWithNtp();
  }
}

/**
 * @brief Synchronizes the RTC time with an NTP server.
 */
void syncRtcWithNtp() {
  if (WiFi.status() == WL_CONNECTED) {
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      rtc.adjust(DateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec));
      Serial.println("RTC time synced with NTP.");
    }
  }
}

/**
 * @brief Logs attendance data to the SD card when offline.
 * @param id The user's fingerprint ID.
 * @param timestamp The time of attendance.
 */
void logAttendanceOffline(uint16_t id, time_t timestamp) {
  File file = SD.open("/attendance_log.txt", FILE_APPEND);
  if (!file) {
    Serial.println("Failed to open log file for writing");
    displayMessage("SD Write Error" ,"",1000);
    return;
  }
  if (file.println(String(id) + "," + String(timestamp))) {
    Serial.println("Offline log saved.");
    displayMessage("Saved to SD", "ID: " + String(id) ,500);
  } else {
    Serial.println("Write to log file failed");
  }
  file.close();
}

/**
 * @brief Synchronizes offline logs with the server.
 */
void syncOfflineLogs() {
  if (WiFi.status() != WL_CONNECTED) return;

  File file = SD.open("/attendance_log.txt");
  if (!file) return;

  File tempFile = SD.open("/temp_log.txt", FILE_WRITE);
  if (!tempFile) {
    file.close();
    return;
  }

  while (file.available()) {
    String line = file.readStringUntil('\n');
    int commaIndex = line.indexOf(',');
    if (commaIndex > 0) {
      uint16_t id = line.substring(0, commaIndex).toInt();
      time_t timestamp = line.substring(commaIndex + 1).toInt();
      if (!logAttendanceToServer(id, timestamp)) {
        tempFile.println(line);
      }
    }
  }

  file.close();
  tempFile.close();

  SD.remove("/attendance_log.txt");
  SD.rename("/temp_log.txt", "/attendance_log.txt");
}

/**************************************************************************************************
 * MENU ACTION FUNCTIONS
 **************************************************************************************************/

/**
 * @brief Shows the options menu.
 */
void showOptionsMenu() {
  currentMenuState = MenuState::OPTIONS_MENU;
  displayMessage("Hold BTN1: DEL", "Hold BTN2: Exit");
  unsigned long menuStartTime = millis();
  while (millis() - menuStartTime < 10000) { // 10s timeout
    handleButton1();
    if (digitalRead(BUTTON_PIN2) == LOW) { // Exit on BTN2 press
      break;
    }
    delay(50);
  }
  currentMenuState = MenuState::MAIN_MENU;
  displayMessage("btn1:add finger", "btn2:manage wifi ");
}

/**
 * @brief Attempts to clear all saved fingerprint models.
 */
void attemptToClearAllData() {
  displayMessage("Confirm Clear?", "Hold BTN1 again");
  unsigned long confirmStartTime = millis();
  bool confirmed = false;
  while (millis() - confirmStartTime < 5000) { // المهلة لتأكيد المسح (5 ثواني)
    if (digitalRead(BUTTON_PIN1) == LOW) { // لو الزر 1 مضغوط تاني
      confirmed = true;
      break; // أكد المسح واخرج من حلقة الانتظار
    }
    delay(50); // تأخير بسيط
  }

  if (confirmed) { // لو المستخدم أكد عملية المسح
    displayMessage("Deleting...", "Please wait"); // رسالة للمستخدم
    bool sensorClearSuccess = false;
    bool serverClearSuccess = false;

    // أولًا: مسح البيانات من حساس البصمة
    if (finger.emptyDatabase() == FINGERPRINT_OK) {
      sensorClearSuccess = true;
      Serial.println("Fingerprint sensor database cleared.");
    } else {
      Serial.println("Failed to clear fingerprint sensor database.");
    }

    // ثانيًا: مسح البيانات من السيرفر (فقط لو كان فيه اتصال)
    serverClearSuccess = clearAllFingerprintsFromServer();

    // عرض رسالة بناءً على نجاح العمليتين
    if (sensorClearSuccess && serverClearSuccess) {
      displayMessage("All models are", "Deleted from ALL!", 2000); // نجاح كامل
    } else if (sensorClearSuccess && !serverClearSuccess) {
      displayMessage("Sensor Cleared", "Server Sync Failed", 2000); // نجاح محلي، فشل السيرفر
      Serial.println("Warning: Sensor data cleared, but server sync failed.");
    } else if (!sensorClearSuccess) { // لو فشل المسح من الحساس نفسه
      displayMessage("Delete Failed", "Sensor Error!", 2000);
      Serial.println("Error: Failed to clear sensor database.");
    }
    // ملاحظة: لو serverClearSuccess كانت true و sensorClearSuccess كانت false،
    // هيدخل في حالة "Delete Failed, Sensor Error!" وده منطقي لأن الأهم هو مسح الحساس.

  } else { // لو المستخدم لم يؤكد المسح
    displayMessage("Delete Canceled", "", 2000);
    Serial.println("Clear all data operation canceled.");
  }
}

uint16_t findNextAvailableID() {
  Serial.println("Scanning fingerprint sensor storage...");
  for (uint16_t id = 0; id < 128; id++) {
    if (finger.loadModel(id) != FINGERPRINT_OK) { // لو الـ ID ده مش موجود في المستشعر
      Serial.print("Next available ID from sensor is: ");
      Serial.println(id);
      return id; // أول ID غير مستخدم نلاقيه، نرجعه.
    }
  }
  Serial.println("All IDs on sensor are full.");
  return 128; // fallback لو كل الـ IDs مليانة (للتنبيه بأن لا يوجد ID متاح)
}

// دالة الحصول على الـ ID التالي المتاح من السيرفر
uint16_t getNextAvailableIDFromServer() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, cannot check server IDs. Using local ID search.");
    return findNextAvailableID();
  }

  HTTPClient http;
  String serverPath = String(SERVER_HOST) + "/api/SensorData/last-id"; // مسار الـ API لمسح البيانات
  http.begin(serverPath);
  int httpCode = http.GET();

  if (httpCode != 200) {
    Serial.printf("Failed to fetch server data for ID check. HTTP code: %d\n", httpCode);
    http.end();
    return findNextAvailableID();
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(4096); // تأكد من أن الحجم كافٍ لجميع الـ IDs التي ستأتي من السيرفر
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.println("JSON parsing failed for server IDs. Using local ID search.");
    return findNextAvailableID();
  }

  bool usedIDs[128] = { false };
  for (JsonObject item : doc.as<JsonArray>()) {
    int id = item["id"]; // كل ما نحتاجه هو الـ ID فقط
    if (id >= 0 && id < 128) {
      usedIDs[id] = true;
    }
  }

  for (int i = 0; i < 128; i++) {
    if (!usedIDs[i]) {
      Serial.printf("First available ID from server: %d\n", i);
      return i;
    }
  }

  Serial.println("All IDs are used on server.");
  return 128;
}

// --- WIFI & SERVER FUNCTIONS (أضفها في هذا القسم) ---

// ... (باقي وظائف الواي فاي والخادم) ...

/**
 * @brief يرسل بيانات التسجيل النهائية للسيرفر، متضمنة الـ IDs المتعددة للبصمات.
 * @param primaryId الـ ID الأساسي للمستخدم (من السيرفر).
 * @param fingerIds مصفوفة الـ Sensor IDs التي تم تسجيلها لهذا المستخدم.
 * @param count عدد الـ Sensor IDs في المصفوفة.
 */
void sendEnrollmentCompleteToServer(uint16_t primaryId, uint16_t* fingerIds, int count) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected, cannot send enrollment data to server.");
        // هنا ممكن تضيف لوج محلي على SD Card لو فشل الاتصال
        return;
    }

    HTTPClient http;
    // URL للـ API اللي هيستقبل بيانات التسجيل
    // مثال: https://192.168.1.12:7069/api/enrollment/complete
    String url = String(SERVER_HOST) + "/api/SensorData";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    DynamicJsonDocument doc(512); // حجم JSON كافي

    doc["primaryUserId"] = primaryId;
    JsonArray idsArray = doc.createNestedArray("enrolledSensorIds"); // اسم المصفوفة في الـ JSON
    for (int i = 0; i < count; i++) {
        idsArray.add(fingerIds[i]);
    }
    doc["timestamp"] = rtc.now().unixtime(); // إضافة الوقت لعملية التسجيل

    String requestBody;
    serializeJson(doc, requestBody); // تحويل الـ JSON إلى نص

    Serial.print("Sending enrollment data to server: ");
    Serial.println(requestBody);

    int httpResponseCode = http.POST(requestBody); // إرسال طلب POST

    if (httpResponseCode == HTTP_CODE_OK || httpResponseCode == HTTP_CODE_CREATED) {
        Serial.println("Enrollment data sent to server successfully.");
    } else {
        Serial.printf("Failed to send enrollment data to server. HTTP code: %d\n", httpResponseCode);
        Serial.print("Server response: ");
        Serial.println(http.getString()); // اطبع الرد من السيرفر لو فيه خطأ
    }
    http.end();
}

// --- WIFI & SERVER FUNCTIONS (أضفها في هذا القسم) ---

// ... (باقي وظائف الواي فاي والخادم) ...

/**
 * @brief يرسل Sensor ID إلى السيرفر للحصول على Primary User ID المرتبط به.
 * @param fingerId الـ ID الذي تم العثور عليه على حساس البصمة.
 * @return Primary User ID المقابل، أو 0 إذا لم يتم العثور عليه أو حدث خطأ.
 */
uint16_t getPrimaryUserIDFromServer(uint16_t fingerId) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected. Cannot get primary user from server.");
        return 0; // ارجع 0 كإشارة للخطأ
    }

    HTTPClient http;
    // URL للـ API اللي هيستقبل الـ Sensor ID ويرجع الـ Primary User ID
    // مثال: https://192.168.1.12:7069/api/user/getPrimaryId?sensorId=X
    String url = String(SERVER_HOST) + "/api/SensorData?sensorId=" + String(fingerId);
    http.begin(url);
    http.addHeader("Content-Type", "application/json"); // بعض الـ APIs قد تتوقع هذا حتى لـ GET

    Serial.print("Requesting Primary User ID for Sensor ID ");
    Serial.print(fingerId);
    Serial.print(" from: ");
    Serial.println(url);

    int httpCode = http.GET(); // إرسال طلب GET

    uint16_t primaryUserID = 0; // القيمة الافتراضية لو حصل خطأ أو مفيش تطابق

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        Serial.print("Server Primary User ID Response: ");
        Serial.println(payload);

        DynamicJsonDocument doc(256); // حجم JSON كافي
        DeserializationError err = deserializeJson(doc, payload); // تحليل الـ JSON

        if (!err) {
            // تأكد إن الـ JSON فيه الحقل المتوقع "primaryUserId"
            if (doc.containsKey("primaryUserId")) {
                primaryUserID = doc["primaryUserId"].as<uint16_t>();
            } else {
                Serial.println("Server response missing 'primaryUserId' field.");
            }
        } else {
            Serial.print("JSON parsing failed: ");
            Serial.println(err.c_str());
        }
    } else {
        Serial.printf("Failed to get primary user ID from server. HTTP code: %d\n", httpCode);
        Serial.print("Server response: ");
        Serial.println(http.getString()); // اطبع الرد من السيرفر لو فيه
    }
    http.end();
    return primaryUserID;
}

void addIdMapping(uint16_t sId, uint16_t pId) {
    // نبحث لو الـ Sensor ID ده موجود قبل كده عشان نحدثه
    for (int i = 0; i < currentMapSize; i++) {
        if (idMap[i].sensorId == sId) {
            idMap[i].primaryUserId = pId; // تحديث الـ Primary ID
            Serial.printf("Updated mapping: SensorID %d -> PrimaryID %d\n", sId, pId);
            return;
        }
    }
    // لو مش موجود، نضيفه كعنصر جديد
    if (currentMapSize < 128) {
        idMap[currentMapSize].sensorId = sId;
        idMap[currentMapSize].primaryUserId = pId;
        currentMapSize++;
        Serial.printf("Added new mapping: SensorID %d -> PrimaryID %d. Total: %d\n", sId, pId, currentMapSize);
    } else {
        Serial.println("Warning: ID map is full, cannot add more mappings.");
    }
}

// دالة للبحث عن الـ Primary User ID باستخدام الـ Sensor ID في الخريطة المحلية
uint16_t getPrimaryIdFromLocalMap(uint16_t sensorId) {
    for (int i = 0; i < currentMapSize; i++) {
        if (idMap[i].sensorId == sensorId) {
            return idMap[i].primaryUserId;
        }
    }
    return 0; // لو ملقاش تطابق
}

/**
 * @brief تقوم بتحميل خريطة الـ IDs من ملف على كارت الـ SD عند بدء التشغيل.
 */
void loadIdMapFromSD() {
    const char* filename = "/id_map.json"; // اسم الملف اللي هنخزن فيه الخريطة
    File mapFile = SD.open(filename, FILE_READ); // فتح الملف للقراءة

    if (!mapFile) {
        Serial.println("No ID map file found on SD card or error opening it.");
        return;
    }

    Serial.println("Loading ID map from SD card...");
    // حجم الـ JSON ده لازم يكون كافي عشان يستوعب كل الـ mappings
    // 4096 بايت كافيين لحوالي 50-60 mapping
    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, mapFile); // قراءة وتحليل الـ JSON من الملف
    mapFile.close(); // قفل الملف بعد القراءة

    if (!err) {
        // لو مفيش أخطاء في قراءة الـ JSON، نبدأ نملى الـ idMap في الذاكرة
        currentMapSize = 0; // نصفر الحجم عشان نملى من جديد
        JsonArray mapsArray = doc.as<JsonArray>(); // نحول الـ JSON لـ JsonArray
        for (JsonObject map : mapsArray) { // نلف على كل عنصر (mapping) في الـ array
            uint16_t sId = map["sensorId"].as<uint16_t>(); // نستخرج الـ sensorId
            uint16_t pId = map["primaryUserId"].as<uint16_t>(); // نستخرج الـ primaryUserId
            addIdMapping(sId, pId); // نضيفه للخريطة في الذاكرة
        }
        Serial.printf("ID map loaded from SD. Total mappings: %d\n", currentMapSize);
    } else {
        Serial.print("JSON parsing failed for ID map from SD: ");
        Serial.println(err.c_str()); // لو فيه خطأ في تحليل الـ JSON
    }
}

/**
 * @brief تقوم بحفظ الخريطة الحالية للـ IDs الموجودة في الذاكرة إلى ملف على كارت الـ SD.
 */
void saveIdMapToSD() {
    const char* filename = "/id_map.json";
    File mapFile = SD.open(filename, FILE_WRITE); // فتح الملف للكتابة (FILE_WRITE هيمسح أي محتوى قديم)

    if (!mapFile) {
        Serial.println("Error: Could not open ID map file for writing on SD card.");
        return;
    }

    DynamicJsonDocument doc(4096);
    JsonArray mapsArray = doc.to<JsonArray>(); // إنشاء JsonArray من الـ doc

    // نلف على الخريطة في الذاكرة ونضيفها للـ JsonArray
    for (int i = 0; i < currentMapSize; i++) {
        JsonObject map = mapsArray.createNestedObject();
        map["sensorId"] = idMap[i].sensorId;
        map["primaryUserId"] = idMap[i].primaryUserId;
    }

    serializeJson(doc, mapFile); // حفظ الـ JSON كاملاً في الملف
    mapFile.close();
    Serial.println("ID map saved to SD card.");
}
/**
 * @brief تقوم بجلب خريطة IDs المستخدمين من السيرفر وتخزينها محليًا وعلى SD Card.
 */
void fetchAndStoreIdMapFromServer() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected. Cannot fetch ID map from server.");
        return;
    }

    HTTPClient http;
    // URL للـ API اللي هيرجعلك كل الـ mappings
    String url = String(SERVER_HOST) + "/api/SensorData";
    http.begin(url);

    Serial.println("Fetching ID map from server...");
    int httpCode = http.GET(); // إرسال طلب GET

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        Serial.println("Server ID Map Response:");
        Serial.println(payload);

        DynamicJsonDocument doc(4096); // حجم كافي للـ JSON اللي جاي من السيرفر
        DeserializationError err = deserializeJson(doc, payload);

        if (!err) {
            currentMapSize = 0; // نصفر الخريطة القديمة قبل ما نملى الجديدة
            JsonArray mapsArray = doc.as<JsonArray>();
            for (JsonObject map : mapsArray) {
                uint16_t sId = map["sensorId"].as<uint16_t>();
                uint16_t pId = map["primaryUserId"].as<uint16_t>();
                addIdMapping(sId, pId); // نضيف كل ربط للخريطة في الذاكرة
            }
            Serial.printf("ID map fetched. Total mappings: %d\n", currentMapSize);

            saveIdMapToSD(); // نحفظ الخريطة الجديدة على الـ SD Card
        } else {
            Serial.print("JSON parsing failed for ID map: ");
            Serial.println(err.c_str());
        }
    } else {
        Serial.printf("Failed to fetch ID map from server. HTTP code: %d\n", httpCode);
    }
    http.end();
}
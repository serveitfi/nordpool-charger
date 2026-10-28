/*
 * ESP32 Nord Pool Electricity Price - Dry Contact Controller
 * 
 * Features:
 * - WiFi captive portal for easy setup (shows visible SSIDs)
 * - Fetches day-ahead electricity prices from Nord Pool public API
 * - Web UI with price graph (15-minute intervals for Finland, hourly for others)
 * - Configurable price thresholds for dry contact relay control
 * - Cheapest hours mode: charge only during the N cheapest hours within a time window
 * - Roof price: never charge above it (only manual override can)
 * - Transistor-driven dry contact (normally closed when ESP32 loses power)
 * - mDNS for easy access (http://charger.local)
 * - Multiple area support (FI, SE1-4, DK1-2, NO1-5, EE, LV, LT, etc.)
 * - Finnish 25.5% VAT automatically applied (configurable per area)
 * 
 * Hardware:
 * - ESP32 / ESP32-S3 dev board
 * - NPN transistor (e.g., 2N2222) driving a relay
 *   - GPIO 14 (RELAY_PIN) -> 1kΩ resistor -> transistor base
 *   - Relay coil between VCC and transistor collector
 *   - Transistor emitter to GND
 *   - Flyback diode across relay coil
 *   - Relay wired as NORMALLY CLOSED so power loss = contact closed = charging ON
 * 
 * Logic:
 * - Transistor OFF (GPIO LOW) = Relay not energized = NC contact CLOSED = Charging ON
 * - Transistor ON  (GPIO HIGH) = Relay energized    = NC contact OPEN   = Charging OFF
 * - If ESP32 loses power, transistor is OFF, relay de-energizes, NC closes = Charging ON
 * 
 * Price conversion:
 * - Nord Pool API returns EUR/MWh (without VAT)
 * - Displayed as snt/kWh (or local currency equivalent)
 * - Formula: snt/kWh = EUR/MWh / 10 * (1 + VAT%)
 * - Finland: 25.5% VAT → snt/kWh = EUR/MWh / 10 * 1.255
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Update.h>
#include <time.h>

// ======================== CONFIGURATION ========================

#define FW_VERSION       "0.96"
#define FW_BUILD_DATE    __DATE__ " " __TIME__

#define RELAY_PIN        14       // GPIO for transistor base
#define LED_PIN          2        // Built-in LED
#define DNS_PORT         53
#define WEB_PORT         80
#define NTP_SERVER       "pool.ntp.org"
#define TIMEZONE         "EET-2EEST,M3.5.0/3,M10.5.0/4"  // Finland timezone
#define MDNS_NAME        "charger"
#define AP_SSID          "NordPool-Charger"
#define AP_PASSWORD      ""       // Open network for setup
#define PRICE_FETCH_INTERVAL  900000   // 15 minutes in ms
#define WIFI_CONNECT_TIMEOUT  15000    // 15 seconds

// ======================== VAT RATES BY AREA ========================
// These are the standard electricity VAT rates per country
// Finland: 25.5% (since 1.9.2024), Sweden: 25%, Norway: 25%, 
// Denmark: 25%, Estonia: 22%, Latvia: 21%, Lithuania: 21%

float getVATRate(const String &area) {
  if (area == "FI") return 25.5;
  if (area.startsWith("SE")) return 25.0;
  if (area.startsWith("NO")) return 25.0;
  if (area.startsWith("DK")) return 25.0;
  if (area == "EE") return 22.0;
  if (area == "LV") return 21.0;
  if (area == "LT") return 21.0;
  if (area == "AT") return 20.0;
  if (area == "BE") return 21.0;
  if (area == "DE-LU") return 19.0;
  if (area == "FR") return 20.0;
  if (area == "NL") return 21.0;
  if (area == "PL") return 23.0;
  return 0.0; // Default: no VAT
}

// Get default POSIX timezone string for a price area
const char* getDefaultTimezone(const String &area) {
  if (area == "FI") return "EET-2EEST,M3.5.0/3,M10.5.0/4";
  if (area == "EE") return "EET-2EEST,M3.5.0/3,M10.5.0/4";
  if (area == "LV") return "EET-2EEST,M3.5.0/3,M10.5.0/4";
  if (area == "LT") return "EET-2EEST,M3.5.0/3,M10.5.0/4";
  if (area.startsWith("SE")) return "CET-1CEST,M3.5.0,M10.5.0/3";
  if (area.startsWith("NO")) return "CET-1CEST,M3.5.0,M10.5.0/3";
  if (area.startsWith("DK")) return "CET-1CEST,M3.5.0,M10.5.0/3";
  if (area == "DE-LU") return "CET-1CEST,M3.5.0,M10.5.0/3";
  if (area == "AT") return "CET-1CEST,M3.5.0,M10.5.0/3";
  if (area == "BE") return "CET-1CEST,M3.5.0,M10.5.0/3";
  if (area == "FR") return "CET-1CEST,M3.5.0,M10.5.0/3";
  if (area == "NL") return "CET-1CEST,M3.5.0,M10.5.0/3";
  if (area == "PL") return "CET-1CEST,M3.5.0,M10.5.0/3";
  return "CET-1CEST,M3.5.0,M10.5.0/3";  // Default: CET
}

// ======================== GLOBALS ========================

WebServer server(WEB_PORT);
DNSServer dnsServer;
Preferences prefs;

// WiFi credentials
String wifiSSID = "";
String wifiPassword = "";
String mdnsName = "charger";  // User-configurable mDNS name
String timezone = TIMEZONE;  // User-configurable timezone (POSIX format)

// Price configuration
String priceArea = "FI";
String priceCurrency = "EUR";
float priceStop = -1;     // -1 means not set (in snt/kWh incl. VAT + tariff)
float priceRoof = -1;     // Never charge above this price (beats every rule except manual override), -1 = off
float tariff = 0;        // Electricity company margin/transfer in c/kWh (added to spot price)
// Manual override stored as start + duration so elapsed time (millis() - start)
// stays correct across the ~49-day millis() rollover
unsigned long overrideStart = 0;     // millis() when manual override started
unsigned long overrideDuration = 0;  // Override length in ms, 0 = no override
volatile bool fetchInProgress = false;  // True during price fetch to avoid slow page renders
int forceChargeStart = -1;  // Hour (0-23) when forced charging starts, -1 = disabled
int forceChargeEnd = -1;    // Hour (0-23) when forced charging ends, -1 = disabled
bool vatEnabled = true;   // Include VAT in displayed prices
float vatRate = 25.5;     // Current VAT rate %

// Cheapest hours mode
float cheapHours = 0;     // Hours of charging to pick within the window, 0 = disabled
int cheapStart = 22;      // Window start hour (0-23)
int cheapEnd = 7;         // Window end hour / deadline (0-23), start == end means 24h window

// Price data storage (max 192 entries: 96 today + 96 tomorrow for 15-min intervals)
struct PriceEntry {
  char timeStr[20];   // "HH:MM" display string
  float priceRaw;     // EUR/MWh from API (without VAT)
  long startEpoch;    // Unix timestamp for start of period
  bool isTomorrow;    // true if this is a tomorrow price
};

PriceEntry prices[192];
int priceCount = 0;
int todayCount = 0;       // Number of today's entries (first todayCount entries)
bool tomorrowLoaded = false;  // Whether tomorrow's prices have been fetched
float minPriceDisplay = 9999;
float maxPriceDisplay = -9999;
float currentPriceDisplay = 0;
bool pricesLoaded = false;
unsigned long lastPriceFetch = 0;
bool relayOpen = false;  // true = transistor ON = relay energized = contact OPEN = charging OFF
bool firstFetchDone = false;  // Ensures immediate fetch on boot
unsigned long priceDataVersion = 0;  // Bumped after every fetch, tells the cheap plan to recompute

// Cheapest hours plan for the current/upcoming window
// Stored as slot start epochs (not indices) so it survives the midnight price reset
long cheapSlots[96];          // Start epochs of selected slots (24h of 15-min slots max)
int cheapSlotCount = 0;
time_t cheapWindowStart = 0;
time_t cheapWindowEnd = 0;
bool cheapPlanComplete = false;  // true when every slot in the window had a known price
bool cheapPlanDirty = true;      // Settings changed -> recompute
unsigned long cheapPlanVersion = 0;  // priceDataVersion the plan was computed from
float cheapPlanAvg = 0;          // Average display price of selected slots
float cheapPlanHours = 0;        // Hours actually planned (less than requested if roof/window too tight)

// State machine
enum DeviceState {
  STATE_AP_MODE,
  STATE_CONNECTING,
  STATE_RUNNING
};
DeviceState deviceState = STATE_AP_MODE;

// ======================== HELPER: Convert raw price to display price ========================

// Convert EUR/MWh (raw from API) to snt/kWh (or local cents) with optional VAT
float rawToDisplay(float eurPerMwh) {
  float centsPerKwh = eurPerMwh / 10.0;  // EUR/MWh -> c/kWh
  if (vatEnabled) {
    centsPerKwh *= (1.0 + vatRate / 100.0);
  }
  centsPerKwh += tariff;  // Add electricity company margin/transfer
  return centsPerKwh;
}

// ======================== FORWARD DECLARATIONS ========================

void setupAPMode();
void setupStationMode();
void fetchPrices();
void fetchPricesForDate(WiFiClientSecure &client, const char* dateStr, bool filterTomorrow);
void parsePricesFromBuffer(const char* jsonBuf, size_t jsonLen, bool filterTomorrow);
void updateRelay();
void updateCheapPlan();
bool inCheapSlotNow();
long getSlotSeconds();
int findCurrentSlot();
void applyChargeArgs();
void handleRoot();
void handleSave();
void handleSettings();
void handleSaveQuick();
void handleOverride();
void handleSaveSettings();
void handleApi();
void handleNotFound();
void handleUpdateResult();
void handleUpdateUpload();
String getWiFiScanHTML();
void sendMainPageChunked();
void sendSettingsPageChunked();
void sendApiJsonChunked();

// ======================== SETUP ========================

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.printf("\n\n=== Nord Pool Charger Controller v%s ===\n", FW_VERSION);
  Serial.printf("Build: %s\n", FW_BUILD_DATE);

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  
  // Start with transistor OFF = relay de-energized = NC closed = charging ON
  digitalWrite(RELAY_PIN, LOW);
  relayOpen = false;
  
  // Load saved settings
  prefs.begin("charger", false);
  wifiSSID = prefs.getString("ssid", "");
  wifiPassword = prefs.getString("password", "");
  mdnsName = prefs.getString("mdnsName", "charger");
  timezone = prefs.getString("timezone", TIMEZONE);
  priceArea = prefs.getString("area", "FI");
  priceCurrency = prefs.getString("currency", "EUR");
  priceStop = prefs.getFloat("priceStop", -1);
  priceRoof = prefs.getFloat("priceRoof", -1);
  tariff = prefs.getFloat("tariff", 0);
  forceChargeStart = prefs.getInt("fcStart", -1);
  forceChargeEnd = prefs.getInt("fcEnd", -1);
  vatEnabled = prefs.getBool("vatEnabled", true);
  cheapHours = prefs.getFloat("chHours", 0);
  cheapStart = prefs.getInt("chStart", 22);
  cheapEnd = prefs.getInt("chEnd", 7);

  // Set VAT rate based on area
  vatRate = getVATRate(priceArea);
  
  Serial.printf("Loaded config: SSID='%s', Area='%s', VAT=%.1f%%, Stop=%.2f, ForceCharge=%d:00-%d:00\n",
                wifiSSID.c_str(), priceArea.c_str(), vatRate, priceStop, forceChargeStart, forceChargeEnd);

  if (wifiSSID.length() > 0) {
    deviceState = STATE_CONNECTING;
    setupStationMode();
  } else {
    deviceState = STATE_AP_MODE;
    setupAPMode();
  }
}

// ======================== MAIN LOOP ========================

void loop() {
  if (deviceState == STATE_AP_MODE) {
    dnsServer.processNextRequest();
    server.handleClient();
    // Blink LED fast in AP mode
    digitalWrite(LED_PIN, (millis() / 250) % 2);
  }
  else if (deviceState == STATE_CONNECTING) {
    // Already handled in setup
  }
  else if (deviceState == STATE_RUNNING) {
    server.handleClient();
    
    // Reset tomorrow data at midnight (when today becomes yesterday)
    // Must happen BEFORE fetch so new fetch uses correct date
    static int lastDay = -1;
    struct tm nowTm;
    if (getLocalTime(&nowTm)) {
      if (lastDay >= 0 && nowTm.tm_mday != lastDay) {
        Serial.println("New day detected, resetting prices");
        pricesLoaded = false;
        tomorrowLoaded = false;
        firstFetchDone = false;
        lastPriceFetch = 0;
      }
      lastDay = nowTm.tm_mday;
    }
    
    // Auto-reconnect WiFi if disconnected
    // After 2 minutes of failed reconnects, fall back to AP mode for reconfiguration
    static unsigned long lastWifiCheck = 0;
    static unsigned long wifiLostSince = 0;
    if (millis() - lastWifiCheck > 30000) {
      lastWifiCheck = millis();
      if (WiFi.status() != WL_CONNECTED) {
        if (wifiLostSince == 0) wifiLostSince = millis();
        
        unsigned long disconnectedFor = millis() - wifiLostSince;
        Serial.printf("WiFi disconnected for %lus, reconnecting...\n", disconnectedFor / 1000);
        
        if (disconnectedFor > 120000) {  // 2 minutes
          Serial.println("WiFi reconnect failed for 2 min, starting AP mode for reconfiguration");
          WiFi.disconnect();
          server.close();  // Stop web server before reconfiguring routes
          deviceState = STATE_AP_MODE;
          setupAPMode();
          wifiLostSince = 0;
          return;  // Exit running state processing
        }
        
        WiFi.disconnect();
        WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
      } else {
        wifiLostSince = 0;  // Reset counter when connected
      }
    }
    
    // Fetch prices: immediately on boot, then periodically
    unsigned long fetchInterval = PRICE_FETCH_INTERVAL;
    if (!pricesLoaded) {
      fetchInterval = 60000;  // 60s retry if no prices at all
    } else {
      struct tm ti;
      if (getLocalTime(&ti) && ti.tm_hour >= 14 && ti.tm_hour < 16 && !tomorrowLoaded) {
        fetchInterval = 300000;  // 5 min between 14-16 to catch tomorrow's release
      }
    }
    
    if (!firstFetchDone || (millis() - lastPriceFetch > fetchInterval)) {
      firstFetchDone = true;
      fetchPrices();
      lastPriceFetch = millis();
    }
    
    // Update relay state every second
    static unsigned long lastRelayUpdate = 0;
    if (millis() - lastRelayUpdate > 1000) {
      updateRelay();
      lastRelayUpdate = millis();
    }
    
    // Log heap status every 5 minutes for monitoring
    static unsigned long lastHeapLog = 0;
    if (millis() - lastHeapLog > 300000) {
      Serial.printf("Heap: %u free, %u min ever, %u largest block\n", 
                    ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
      lastHeapLog = millis();
    }
    
    // Solid LED when running
    digitalWrite(LED_PIN, HIGH);
  }
}

// ======================== WIFI SETUP ========================

void setupAPMode() {
  Serial.println("Starting AP mode...");
  WiFi.mode(WIFI_AP_STA);  // AP + STA so we can scan
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  
  Serial.printf("AP started: %s\n", AP_SSID);
  Serial.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());
  
  // DNS server to redirect all requests to captive portal
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  
  // Web server routes for AP mode
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/generate_204", handleRoot);        // Android captive portal
  server.on("/fwlink", handleRoot);              // Microsoft captive portal
  server.on("/hotspot-detect.html", handleRoot); // Apple captive portal
  server.onNotFound(handleRoot);                 // Catch all -> captive portal
  server.begin();
  
  Serial.println("Captive portal started");
}

void setupStationMode() {
  Serial.printf("Connecting to '%s'...\n", wifiSSID.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < WIFI_CONNECT_TIMEOUT) {
    delay(500);
    Serial.print(".");
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());
    
    // Setup NTP
    configTzTime(timezone.c_str(), NTP_SERVER);
    Serial.println("NTP configured");
    
    // Wait for proper time sync (year must be 2025+)
    struct tm timeinfo;
    int retries = 0;
    bool timeOk = false;
    while (retries < 20) {
      if (getLocalTime(&timeinfo) && timeinfo.tm_year >= (2025 - 1900)) {
        timeOk = true;
        Serial.printf("\nNTP synced: %04d-%02d-%02d %02d:%02d:%02d\n",
                      timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                      timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        break;
      }
      delay(500);
      retries++;
      Serial.print(".");
    }
    if (!timeOk) {
      Serial.println("\nWARNING: NTP sync failed, prices may be incorrect until synced");
    }
    
    // Setup mDNS
    if (MDNS.begin(mdnsName.c_str())) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("mDNS started: http://%s.local\n", mdnsName.c_str());
    }
    
    // Web server routes for running mode
    server.on("/", handleMainPage);
    server.on("/settings", handleSettings);
    server.on("/savequick", HTTP_POST, handleSaveQuick);
    server.on("/override", HTTP_POST, handleOverride);
    server.on("/savesettings", HTTP_POST, handleSaveSettings);
    server.on("/api/prices", handleApi);
    server.on("/reset", HTTP_POST, handleReset);
    server.on("/reboot", HTTP_POST, handleReboot);
    server.on("/rebooting", handleRebooting);
    server.on("/update", HTTP_POST, handleUpdateResult, handleUpdateUpload);
    server.onNotFound(handleNotFound);
    server.begin();
    
    deviceState = STATE_RUNNING;
    Serial.println("Running mode started");
  } else {
    Serial.println("\nFailed to connect! Starting AP mode...");
    WiFi.disconnect();
    deviceState = STATE_AP_MODE;
    setupAPMode();
  }
}

// ======================== PRICE FETCHING ========================

void fetchPrices() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, skipping price fetch");
    return;
  }
  
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo) || timeinfo.tm_year < (2025 - 1900)) {
    Serial.println("Time not synced yet, skipping price fetch");
    return;
  }
  
  Serial.printf("=== Fetching prices (heap: %u, min: %u) ===\n", 
                ESP.getFreeHeap(), ESP.getMinFreeHeap());
  
  fetchInProgress = true;
  
  // Create one shared TLS client for all API calls (saves ~40KB per extra handshake)
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(30);
  
  // Store prices in a temporary section starting at index 0
  // We'll keep old data until we confirm new data is valid
  int savedPriceCount = priceCount;
  int savedTodayCount = todayCount;
  float savedMin = minPriceDisplay;
  float savedMax = maxPriceDisplay;
  bool savedTomorrowLoaded = tomorrowLoaded;
  
  // Reset for new fetch
  priceCount = 0;
  todayCount = 0;
  tomorrowLoaded = false;
  minPriceDisplay = 9999;
  maxPriceDisplay = -9999;
  
  // Fetch yesterday (to catch early morning local hours due to UTC offset)
  time_t now;
  time(&now);
  time_t yesterday = now - 86400;
  struct tm yesterdayTm;
  localtime_r(&yesterday, &yesterdayTm);
  
  char dateStrYesterday[12];
  strftime(dateStrYesterday, sizeof(dateStrYesterday), "%Y-%m-%d", &yesterdayTm);
  char dateStrToday[12];
  strftime(dateStrToday, sizeof(dateStrToday), "%Y-%m-%d", &timeinfo);
  
  // Fetch yesterday + today, filtering to today's local date
  fetchPricesForDate(client, dateStrYesterday, false);
  fetchPricesForDate(client, dateStrToday, false);
  todayCount = priceCount;  // Mark where today's entries end
  
  // After 14:00 local time, try fetching tomorrow's prices
  // Nord Pool publishes next-day prices between 14:00-14:45 CET (~15:00-15:45 Finnish)
  // We start checking from 14:00 local to be safe
  if (timeinfo.tm_hour >= 14) {
    time_t tomorrow = now + 86400;
    struct tm tomorrowTm;
    localtime_r(&tomorrow, &tomorrowTm);
    char dateStrTomorrow[12];
    strftime(dateStrTomorrow, sizeof(dateStrTomorrow), "%Y-%m-%d", &tomorrowTm);
    
    // Also need today's API date to catch tomorrow's late-night hours
    int beforeCount = priceCount;
    fetchPricesForDate(client, dateStrToday, true);     // today's API date, tomorrow filter
    fetchPricesForDate(client, dateStrTomorrow, true);  // tomorrow's API date, tomorrow filter
    
    if (priceCount > beforeCount) {
      tomorrowLoaded = true;
      Serial.printf("Tomorrow's prices loaded: %d entries\n", priceCount - beforeCount);
    } else {
      Serial.println("Tomorrow's prices not yet available");
    }
  }
  
  if (priceCount >= 10) {
    // Accept data if we got a reasonable number of entries
    pricesLoaded = true;
    Serial.printf("Total %d entries (%d today + %d tomorrow). Range: %.2f - %.2f snt/kWh\n",
                  priceCount, todayCount, priceCount - todayCount, 
                  minPriceDisplay, maxPriceDisplay);
    updateCurrentPrice();
  } else if (priceCount > 0 && priceCount < 10) {
    // Got some data but suspiciously few — might be partial parse
    Serial.printf("WARNING: Only %d entries (expected 24-96), restoring old data\n", priceCount);
    if (savedPriceCount >= 10) {
      priceCount = savedPriceCount;
      todayCount = savedTodayCount;
      tomorrowLoaded = savedTomorrowLoaded;
      minPriceDisplay = 9999;
      maxPriceDisplay = -9999;
      for (int i = 0; i < todayCount; i++) {
        float dp = rawToDisplay(prices[i].priceRaw);
        if (dp < minPriceDisplay) minPriceDisplay = dp;
        if (dp > maxPriceDisplay) maxPriceDisplay = dp;
      }
      updateCurrentPrice();
      Serial.println("Restored previous price data");
    } else {
      // Even old data was bad, accept what we have
      pricesLoaded = true;
      updateCurrentPrice();
    }
  } else {
    Serial.printf("WARNING: No valid price entries! WiFi: %s, Heap: %u\n",
                  WiFi.status() == WL_CONNECTED ? "OK" : "DISCONNECTED",
                  ESP.getFreeHeap());
    // Restore previous data so we don't show empty chart
    if (savedPriceCount > 0) {
      priceCount = savedPriceCount;
      todayCount = savedTodayCount;
      tomorrowLoaded = savedTomorrowLoaded;
      // Recalculate min/max from actual today's data
      minPriceDisplay = 9999;
      maxPriceDisplay = -9999;
      for (int i = 0; i < todayCount; i++) {
        float dp = rawToDisplay(prices[i].priceRaw);
        if (dp < minPriceDisplay) minPriceDisplay = dp;
        if (dp > maxPriceDisplay) maxPriceDisplay = dp;
      }
      updateCurrentPrice();
      Serial.println("Restored previous price data");
    }
    // pricesLoaded stays as-is, will retry in 60s
  }
  priceDataVersion++;
  fetchInProgress = false;
}

void fetchPricesForDate(WiFiClientSecure &client, const char* dateStr, bool filterTomorrow) {
  char url[200];
  snprintf(url, sizeof(url), 
           "https://dataportal-api.nordpoolgroup.com/api/DayAheadPrices"
           "?date=%s&market=DayAhead&deliveryArea=%s&currency=%s",
           dateStr, priceArea.c_str(), priceCurrency.c_str());
  
  Serial.printf("Fetching %s %s (heap: %u)\n", dateStr, filterTomorrow ? "[tomorrow]" : "[today]", ESP.getFreeHeap());
  
  HTTPClient http;
  http.useHTTP10(true);
  http.begin(client, url);
  http.addHeader("Accept", "application/json");
  http.setTimeout(30000);
  
  int httpCode = http.GET();
  int contentLen = http.getSize();
  Serial.printf("HTTP %s response: %d, size: %d (heap: %u)\n", dateStr, httpCode, contentLen, ESP.getFreeHeap());
  
  if (httpCode == 200) {
    // Read full response body - handles both chunked and known-length responses
    // getString() properly decodes chunked transfer encoding
    String payload = http.getString();
    Serial.printf("  Received %d bytes (heap: %u)\n", payload.length(), ESP.getFreeHeap());
    
    if (payload.length() > 100) {
      parsePricesFromBuffer(payload.c_str(), payload.length(), filterTomorrow);
    } else if (payload.length() > 0) {
      Serial.printf("  Response too short: %s\n", payload.c_str());
    } else {
      Serial.println("  Empty response body");
    }
  } else if (httpCode < 0) {
    Serial.printf("HTTP error for %s: %s\n", dateStr, http.errorToString(httpCode).c_str());
  } else if (httpCode != 204) {
    Serial.printf("HTTP %d for %s\n", httpCode, dateStr);
  }
  
  http.end();
}

void parsePricesFromBuffer(const char* jsonBuf, size_t jsonLen, bool filterTomorrow) {
  // Use a filter to only parse the fields we need
  // This dramatically reduces memory usage for the large Nord Pool response
  JsonDocument filter;
  filter["multiAreaEntries"][0]["deliveryStart"] = true;
  filter["multiAreaEntries"][0]["entryPerArea"][priceArea] = true;
  
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, jsonBuf, jsonLen, DeserializationOption::Filter(filter));
  
  if (error) {
    Serial.printf("JSON parse error: %s (heap: %u)\n", error.c_str(), ESP.getFreeHeap());
    return;
  }
  
  JsonArray entries = doc["multiAreaEntries"].as<JsonArray>();
  if (entries.isNull()) {
    Serial.println("No multiAreaEntries found in response");
    JsonObject root = doc.as<JsonObject>();
    for (JsonPair kv : root) {
      Serial.printf("  root key: %s\n", kv.key().c_str());
    }
    return;
  }
  
  // Get today's and tomorrow's local date for filtering
  time_t nowEpoch;
  time(&nowEpoch);
  struct tm todayLocal;
  localtime_r(&nowEpoch, &todayLocal);
  
  time_t tomorrowEpoch = nowEpoch + 86400;
  struct tm tomorrowLocal;
  localtime_r(&tomorrowEpoch, &tomorrowLocal);
  
  // Target date for filtering
  int targetDay = filterTomorrow ? tomorrowLocal.tm_mday : todayLocal.tm_mday;
  int targetMon = filterTomorrow ? tomorrowLocal.tm_mon : todayLocal.tm_mon;
  int targetYear = filterTomorrow ? tomorrowLocal.tm_year : todayLocal.tm_year;
  
  int addedCount = 0;
  
  for (JsonObject entry : entries) {
    if (priceCount >= 192) break;
    
    const char* deliveryStart = entry["deliveryStart"];
    if (!deliveryStart) continue;
    
    JsonObject perArea = entry["entryPerArea"];
    
    // Get price for our area (EUR/MWh)
    float rawPrice = -9999;
    if (perArea[priceArea].is<float>()) {
      rawPrice = perArea[priceArea].as<float>();
    } else if (perArea[priceArea].is<int>()) {
      rawPrice = (float)perArea[priceArea].as<int>();
    }
    
    if (rawPrice < -500) continue; // Skip invalid entries
    
    // Parse UTC time from ISO string
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (sscanf(deliveryStart, "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) < 5) {
      continue;
    }
    
    // Convert UTC time to epoch manually (no setenv/tzset needed)
    // This is equivalent to timegm() which isn't available on all platforms
    struct tm utcTm = {0};
    utcTm.tm_year = year - 1900;
    utcTm.tm_mon = month - 1;
    utcTm.tm_mday = day;
    utcTm.tm_hour = hour;
    utcTm.tm_min = minute;
    utcTm.tm_sec = second;
    utcTm.tm_isdst = 0;
    
    // Manual UTC epoch: count days from 1970-01-01
    // Using a simplified algorithm that works for 2000-2099
    int y = year;
    int m = month;
    if (m <= 2) { y--; m += 12; }
    int daysSinceEpoch = 365 * y + y/4 - y/100 + y/400 + (153*(m-3)+2)/5 + day - 719469;
    time_t utcEpoch = (time_t)daysSinceEpoch * 86400 + hour * 3600 + minute * 60 + second;
    
    // Convert UTC epoch to local time (uses system TZ, no setenv needed)
    struct tm localTm;
    localtime_r(&utcEpoch, &localTm);
    
    // Filter: only keep entries matching target date (today or tomorrow)
    if (localTm.tm_mday != targetDay || localTm.tm_mon != targetMon || localTm.tm_year != targetYear) {
      continue;
    }
    
    // Deduplicate: check if this time+day slot already exists
    char candidateTime[20];
    snprintf(candidateTime, 20, "%02d:%02d", localTm.tm_hour, localTm.tm_min);
    bool duplicate = false;
    for (int d = 0; d < priceCount; d++) {
      if (strcmp(prices[d].timeStr, candidateTime) == 0 && prices[d].isTomorrow == filterTomorrow) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;
    
    // Store entry
    strncpy(prices[priceCount].timeStr, candidateTime, 19);
    prices[priceCount].startEpoch = utcEpoch;
    prices[priceCount].priceRaw = rawPrice;
    prices[priceCount].isTomorrow = filterTomorrow;
    
    // Track display min/max (today only)
    if (!filterTomorrow) {
      float displayPrice = rawToDisplay(rawPrice);
      if (displayPrice < minPriceDisplay) minPriceDisplay = displayPrice;
      if (displayPrice > maxPriceDisplay) maxPriceDisplay = displayPrice;
    }
    
    priceCount++;
    addedCount++;
    
    // Debug: log first entry conversion
    if (addedCount == 1) {
      Serial.printf("  First entry: UTC %04d-%02d-%02dT%02d:%02d -> Local %02d:%02d (day %d)\n",
                    year, month, day, hour, minute, localTm.tm_hour, localTm.tm_min, localTm.tm_mday);
    }
  }
  
  Serial.printf("  Added %d entries (total: %d, heap: %u)\n", addedCount, priceCount, ESP.getFreeHeap());
}

// Index of the price slot containing the current time, -1 if not in data.
// Matches on epoch, not "HH:MM", so today's 23:45 isn't confused with tomorrow's.
int findCurrentSlot() {
  time_t now;
  time(&now);
  long slotSecs = getSlotSeconds();
  for (int i = 0; i < priceCount; i++) {
    if (now >= prices[i].startEpoch && now < prices[i].startEpoch + slotSecs) return i;
  }
  return -1;
}

void updateCurrentPrice() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  int i = findCurrentSlot();
  if (i >= 0) {
    currentPriceDisplay = rawToDisplay(prices[i].priceRaw);
    return;
  }

  // If not found, use first entry
  if (priceCount > 0) {
    currentPriceDisplay = rawToDisplay(prices[0].priceRaw);
  }
}

// ======================== RELAY CONTROL ========================

// Remaining manual override time in ms, 0 = no override or expired
unsigned long overrideRemainingMs() {
  if (overrideDuration == 0) return 0;
  unsigned long elapsed = millis() - overrideStart;  // Rollover-safe unsigned subtraction
  return elapsed < overrideDuration ? overrideDuration - elapsed : 0;
}

void updateRelay() {
  // Manual override has highest priority
  if (overrideDuration > 0) {
    if (overrideRemainingMs() > 0) {
      digitalWrite(RELAY_PIN, LOW);
      relayOpen = false;
      return;
    } else {
      // Override expired
      overrideDuration = 0;
      Serial.println("Manual override expired");
    }
  }
  
  if (!pricesLoaded) {
    // No prices = keep charging ON (transistor OFF)
    digitalWrite(RELAY_PIN, LOW);
    relayOpen = false;
    return;
  }
  
  updateCurrentPrice();
  updateCheapPlan();  // Keep plan fresh for the chart even during the forced window

  // Check if we're in the forced charge time window
  bool inForceWindow = false;
  if (forceChargeStart >= 0 && forceChargeEnd >= 0) {
    struct tm ti;
    if (getLocalTime(&ti)) {
      int h = ti.tm_hour;
      if (forceChargeStart <= forceChargeEnd) {
        // Same-day window, e.g. 08:00-16:00
        inForceWindow = (h >= forceChargeStart && h < forceChargeEnd);
      } else {
        // Overnight window, e.g. 23:00-07:00
        inForceWindow = (h >= forceChargeStart || h < forceChargeEnd);
      }
    }
  }
  
  bool shouldCharge = true;

  if (inForceWindow) {
    // Force charging ON regardless of price
    shouldCharge = true;
  } else if (cheapHours > 0) {
    // Cheapest hours mode: charge in selected slots, or whenever price is below max price
    shouldCharge = inCheapSlotNow() || (priceStop >= 0 && currentPriceDisplay <= priceStop);
  } else if (priceStop >= 0) {
    // Normal price-based logic
    shouldCharge = (currentPriceDisplay <= priceStop);
  }
  // If neither force window active nor threshold set: always charge

  // Roof price beats all automatic rules (manual override returned earlier)
  if (priceRoof >= 0 && currentPriceDisplay > priceRoof) {
    shouldCharge = false;
  }

  if (shouldCharge) {
    digitalWrite(RELAY_PIN, LOW);
    relayOpen = false;
  } else {
    digitalWrite(RELAY_PIN, HIGH);
    relayOpen = true;
  }
}

// ======================== CHEAPEST HOURS PLAN ========================

// Slot length in seconds (900 for 15-min prices, 3600 for hourly)
long getSlotSeconds() {
  long best = 3600;
  for (int i = 1; i < priceCount; i++) {
    long d = prices[i].startEpoch - prices[i - 1].startEpoch;
    if (d > 0 && d < best) best = d;
  }
  return best;
}

// Window that ends at the next occurrence of cheapEnd:00 local time
void getCheapWindow(time_t now, time_t &wStart, time_t &wEnd) {
  struct tm t;
  localtime_r(&now, &t);
  t.tm_hour = cheapEnd;
  t.tm_min = 0;
  t.tm_sec = 0;
  t.tm_isdst = -1;
  wEnd = mktime(&t);
  if (wEnd <= now) {
    localtime_r(&now, &t);
    t.tm_mday += 1;
    t.tm_hour = cheapEnd;
    t.tm_min = 0;
    t.tm_sec = 0;
    t.tm_isdst = -1;
    wEnd = mktime(&t);
  }

  int windowHours = (cheapEnd - cheapStart + 24) % 24;
  if (windowHours == 0) windowHours = 24;
  struct tm s;
  localtime_r(&wEnd, &s);
  s.tm_hour -= windowHours;  // mktime normalizes negative hours to previous day
  s.tm_isdst = -1;
  wStart = mktime(&s);
}

// Pick the cheapest slots in the window. Recomputes when the window moves, settings
// change, or new prices arrive while the window was still missing prices.
// Slots that have already ended are skipped, so a reboot mid-window plans the
// full hours from now on (errs towards charging more, not less).
void updateCheapPlan() {
  if (cheapHours <= 0) {
    cheapSlotCount = 0;
    return;
  }
  if (!pricesLoaded || priceCount == 0) return;

  time_t now;
  time(&now);
  time_t wStart, wEnd;
  getCheapWindow(now, wStart, wEnd);

  if (!cheapPlanDirty && wEnd == cheapWindowEnd &&
      (cheapPlanComplete || cheapPlanVersion == priceDataVersion)) {
    return;
  }

  long slotSecs = getSlotSeconds();
  long needSecs = (long)(cheapHours * 3600.0f + 0.5f);

  long coveredSecs = 0;
  for (int i = 0; i < priceCount; i++) {
    if (prices[i].startEpoch >= wStart && prices[i].startEpoch < wEnd) coveredSecs += slotSecs;
  }

  bool used[192] = {false};
  long selectedSecs = 0;
  float sum = 0;
  cheapSlotCount = 0;
  while (selectedSecs < needSecs && cheapSlotCount < 96) {
    int best = -1;
    for (int i = 0; i < priceCount; i++) {
      long s = prices[i].startEpoch;
      if (used[i] || s < wStart || s >= wEnd || s + slotSecs <= now) continue;
      if (priceRoof >= 0 && rawToDisplay(prices[i].priceRaw) > priceRoof) continue;
      if (best < 0 || prices[i].priceRaw < prices[best].priceRaw) best = i;
    }
    if (best < 0) break;  // Fewer known slots (under roof) than requested hours
    used[best] = true;
    cheapSlots[cheapSlotCount++] = prices[best].startEpoch;
    selectedSecs += slotSecs;
    sum += rawToDisplay(prices[best].priceRaw);
  }

  cheapPlanAvg = cheapSlotCount > 0 ? sum / cheapSlotCount : 0;
  cheapPlanHours = selectedSecs / 3600.0f;
  cheapPlanComplete = coveredSecs >= (long)(wEnd - wStart);
  cheapWindowStart = wStart;
  cheapWindowEnd = wEnd;
  cheapPlanVersion = priceDataVersion;
  cheapPlanDirty = false;

  Serial.printf("Cheap plan: %d slots (%.2fh of %.2fh), avg %.2f, window %s\n",
                cheapSlotCount, selectedSecs / 3600.0f, cheapHours, cheapPlanAvg,
                cheapPlanComplete ? "complete" : "missing prices");
}

bool isCheapSlot(long startEpoch) {
  for (int i = 0; i < cheapSlotCount; i++) {
    if (cheapSlots[i] == startEpoch) return true;
  }
  return false;
}

// True if the current slot is selected. Unknown current price = true
// (fail-safe: charge, same as when no prices are loaded)
bool inCheapSlotNow() {
  int i = findCurrentSlot();
  return i < 0 || isCheapSlot(prices[i].startEpoch);
}

// Parse roof price + cheapest hours fields (shared by dashboard quick form and settings page)
void applyChargeArgs() {
  String roofVal = server.arg("price_roof");
  String hoursVal = server.arg("cheap_hours");
  String startVal = server.arg("cheap_start");
  String endVal = server.arg("cheap_end");

  priceRoof = roofVal.length() > 0 ? roofVal.toFloat() : -1;
  prefs.putFloat("priceRoof", priceRoof);

  float h = hoursVal.length() > 0 ? hoursVal.toFloat() : 0;
  if (h < 0) h = 0;
  if (h > 24) h = 24;
  cheapHours = h;
  prefs.putFloat("chHours", cheapHours);

  if (startVal.length() > 0) {
    cheapStart = constrain(startVal.toInt(), 0, 23);
    prefs.putInt("chStart", cheapStart);
  }
  if (endVal.length() > 0) {
    cheapEnd = constrain(endVal.toInt(), 0, 23);
    prefs.putInt("chEnd", cheapEnd);
  }

  cheapPlanDirty = true;
  updateCheapPlan();  // So the redirected dashboard already shows the new plan
}

// ======================== WEB HANDLERS - AP MODE ========================

void handleRoot() {
  server.send(200, "text/html", getWiFiScanHTML());
}

void handleSave() {
  String ssid = server.arg("ssid");
  String customSSID = server.arg("custom_ssid");
  String password = server.arg("password");
  String area = server.arg("area");
  
  // Use custom SSID if provided (hidden network)
  if (customSSID.length() > 0) {
    ssid = customSSID;
  }
  
  if (ssid.length() == 0) {
    server.send(400, "text/html", "<h2>Please provide an SSID</h2><a href='/'>Back</a>");
    return;
  }
  
  // Save to preferences
  prefs.putString("ssid", ssid);
  prefs.putString("password", password);
  if (area.length() > 0) {
    prefs.putString("area", area);
  }
  
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:20px;";
  html += "background:#1a1a2e;color:#e0e0e0;text-align:center;}";
  html += "h2{color:#00d4aa;}</style></head><body>";
  html += "<h2>&#10004; Settings Saved!</h2>";
  html += "<p>Connecting to: <strong>" + ssid + "</strong></p>";
  html += "<p>Area: <strong>" + (area.length() > 0 ? area : priceArea) + "</strong></p>";
  html += "<p>Device will reboot in 3 seconds...</p>";
  html += "<p>After reboot, connect to your WiFi and visit:<br>";
  html += "<strong>http://" + mdnsName + ".local</strong></p>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
  delay(3000);
  ESP.restart();
}

// ======================== WEB HANDLERS - RUNNING MODE ========================

void handleMainPage() {
  if (fetchInProgress) {
    // Don't try to render full page during fetch — TLS uses most of the heap
    server.send(200, "text/html", 
      "<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<meta http-equiv='refresh' content='3'>"
      "<style>body{font-family:sans-serif;background:#0f0f23;color:#ccc;"
      "display:flex;justify-content:center;align-items:center;height:100vh}"
      "</style></head><body><div style='text-align:center'>"
      "<h2 style='color:#00d4aa'>Fetching prices...</h2>"
      "<p>Page will refresh automatically.</p></div></body></html>");
    return;
  }
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  sendMainPageChunked();
}

void handleSettings() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  sendSettingsPageChunked();
}

void handleSaveQuick() {
  // Quick save from dashboard: max price + force charge window
  String stopVal = server.arg("price_stop");
  String fcStartVal = server.arg("fc_start");
  String fcEndVal = server.arg("fc_end");
  
  if (stopVal.length() > 0) {
    priceStop = stopVal.toFloat();
    prefs.putFloat("priceStop", priceStop);
  } else {
    priceStop = -1;
    prefs.putFloat("priceStop", -1);
  }
  
  if (fcStartVal.length() > 0 && fcEndVal.length() > 0) {
    forceChargeStart = fcStartVal.toInt();
    forceChargeEnd = fcEndVal.toInt();
    prefs.putInt("fcStart", forceChargeStart);
    prefs.putInt("fcEnd", forceChargeEnd);
  } else {
    forceChargeStart = -1;
    forceChargeEnd = -1;
    prefs.putInt("fcStart", -1);
    prefs.putInt("fcEnd", -1);
  }

  applyChargeArgs();

  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "Redirecting...");
}

void handleOverride() {
  String durationVal = server.arg("duration");
  
  if (durationVal == "0" || durationVal == "cancel") {
    overrideDuration = 0;
    Serial.println("Manual override cancelled");
  } else if (durationVal.length() > 0) {
    int minutes = durationVal.toInt();
    if (minutes > 0 && minutes <= 1440) {  // Max 24 hours
      overrideStart = millis();
      overrideDuration = (unsigned long)minutes * 60000UL;
      Serial.printf("Manual override: charging ON for %d minutes\n", minutes);
    }
  }
  
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "Redirecting...");
}

void handleSaveSettings() {
  String stopVal = server.arg("price_stop");
  String fcStartVal = server.arg("fc_start");
  String fcEndVal = server.arg("fc_end");
  String area = server.arg("area");
  String currency = server.arg("currency");
  String vatEn = server.arg("vat_enabled");
  String newSsid = server.arg("wifi_ssid");
  String newPass = server.arg("wifi_pass");
  String newMdns = server.arg("mdns_name");
  String newTz = server.arg("timezone");
  
  if (stopVal.length() > 0) {
    priceStop = stopVal.toFloat();
    prefs.putFloat("priceStop", priceStop);
  } else {
    priceStop = -1;
    prefs.putFloat("priceStop", -1);
  }
  
  if (fcStartVal.length() > 0 && fcEndVal.length() > 0) {
    forceChargeStart = fcStartVal.toInt();
    forceChargeEnd = fcEndVal.toInt();
    prefs.putInt("fcStart", forceChargeStart);
    prefs.putInt("fcEnd", forceChargeEnd);
  } else {
    forceChargeStart = -1;
    forceChargeEnd = -1;
    prefs.putInt("fcStart", -1);
    prefs.putInt("fcEnd", -1);
  }
  
  bool areaChanged = false;
  bool tzChanged = false;
  if (area.length() > 0 && area != priceArea) {
    priceArea = area;
    prefs.putString("area", area);
    vatRate = getVATRate(priceArea);
    areaChanged = true;
    // Auto-set timezone to match the new area (unless user set a custom one)
    if (newTz.length() == 0 || newTz == timezone) {
      timezone = getDefaultTimezone(priceArea);
      prefs.putString("timezone", timezone);
      tzChanged = true;
    }
  }
  
  // Manual timezone override
  if (newTz.length() > 0 && newTz != timezone) {
    timezone = newTz;
    prefs.putString("timezone", timezone);
    tzChanged = true;
  }
  
  if (tzChanged) {
    configTzTime(timezone.c_str(), NTP_SERVER);
    Serial.printf("Timezone changed to: %s\n", timezone.c_str());
  }
  
  if (currency.length() > 0) {
    priceCurrency = currency;
    prefs.putString("currency", currency);
  }
  
  vatEnabled = (vatEn == "on" || vatEn == "1");
  prefs.putBool("vatEnabled", vatEnabled);
  
  String tariffVal = server.arg("tariff");
  if (tariffVal.length() > 0) {
    tariff = tariffVal.toFloat();
  } else {
    tariff = 0;
  }
  prefs.putFloat("tariff", tariff);

  applyChargeArgs();

  // mDNS name change
  bool needReboot = false;
  if (newMdns.length() > 0 && newMdns != mdnsName) {
    mdnsName = newMdns;
    prefs.putString("mdnsName", mdnsName);
    needReboot = true;
  }
  
  // WiFi change
  if (newSsid.length() > 0 && newSsid != wifiSSID) {
    wifiSSID = newSsid;
    wifiPassword = newPass;
    prefs.putString("ssid", wifiSSID);
    prefs.putString("password", wifiPassword);
    needReboot = true;
  } else if (newPass.length() > 0 && newPass != wifiPassword) {
    wifiPassword = newPass;
    prefs.putString("password", wifiPassword);
    needReboot = true;
  }
  
  // Force re-fetch prices if area changed
  if (areaChanged) {
    pricesLoaded = false;
    tomorrowLoaded = false;
    lastPriceFetch = 0;
    firstFetchDone = false;
  } else if (pricesLoaded) {
    // Recalculate min/max/current when VAT or tariff changed (today only)
    minPriceDisplay = 9999;
    maxPriceDisplay = -9999;
    for (int i = 0; i < todayCount; i++) {
      float dp = rawToDisplay(prices[i].priceRaw);
      if (dp < minPriceDisplay) minPriceDisplay = dp;
      if (dp > maxPriceDisplay) maxPriceDisplay = dp;
    }
    updateCurrentPrice();
  }
  
  if (needReboot) {
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
    html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<style>body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:20px;";
    html += "background:#1a1a2e;color:#e0e0e0;text-align:center;}";
    html += "h2{color:#00d4aa;}</style></head><body>";
    html += "<h2>Settings saved!</h2>";
    html += "<p>Rebooting to apply WiFi/mDNS changes...</p>";
    html += "<p>Visit: <strong>http://" + mdnsName + ".local</strong></p>";
    html += "</body></html>";
    server.send(200, "text/html", html);
    delay(3000);
    ESP.restart();
  }
  
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "Redirecting...");
}

void handleApi() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  sendApiJsonChunked();
}

void handleReset() {
  prefs.clear();
  server.send(200, "text/html", "<h2>Settings cleared. Rebooting...</h2>");
  delay(2000);
  ESP.restart();
}

void handleReboot() {
  // POST handler — redirect to GET page so refresh won't re-trigger
  server.sendHeader("Location", "/rebooting");
  server.send(303, "text/plain", "Redirecting...");
}

void handleRebooting() {
  // GET page — shows message, schedules reboot, auto-redirects to home
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='8;url=/'>";
  html += "<style>body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:20px;";
  html += "background:#1a1a2e;color:#e0e0e0;text-align:center;}";
  html += "h2{color:#00d4aa;}</style></head><body>";
  html += "<h2>Rebooting...</h2>";
  html += "<p>Device will restart in a few seconds.</p>";
  html += "<p>Redirecting to <strong>http://" + mdnsName + ".local</strong></p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
  delay(1000);
  ESP.restart();
}

void handleNotFound() {
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "Redirecting...");
}

// ======================== OTA FIRMWARE UPDATE ========================

void handleUpdateResult() {
  // Called after upload completes
  bool success = !Update.hasError();
  
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  if (success) html += "<meta http-equiv='refresh' content='10;url=/'>";
  html += "<style>body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:20px;";
  html += "background:#1a1a2e;color:#e0e0e0;text-align:center;}";
  html += "h2{color:" + String(success ? "#00d4aa" : "#ff4444") + ";}</style></head><body>";
  
  if (success) {
    html += "<h2>&#10004; Update Successful!</h2>";
    html += "<p>Rebooting with new firmware...</p>";
    html += "<p>Redirecting in 10 seconds.</p>";
  } else {
    html += "<h2>&#10008; Update Failed</h2>";
    html += "<p>Error: " + String(Update.errorString()) + "</p>";
    html += "<p><a href='/settings' style='color:#00d4aa'>Back to Settings</a></p>";
  }
  
  html += "</body></html>";
  server.send(200, "text/html", html);
  
  if (success) {
    delay(1000);
    ESP.restart();
  }
}

void handleUpdateUpload() {
  HTTPUpload& upload = server.upload();
  
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("OTA Update: %s (%u bytes free)\n", upload.filename.c_str(), ESP.getFreeHeap());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Serial.printf("Update.begin failed: %s\n", Update.errorString());
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Serial.printf("Update.write failed: %s\n", Update.errorString());
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("OTA Update success: %u bytes\n", upload.totalSize);
    } else {
      Serial.printf("Update.end failed: %s\n", Update.errorString());
    }
  }
}

// ======================== HTML GENERATORS ========================

String getWiFiScanHTML() {
  // Scan for networks
  int n = WiFi.scanNetworks();
  
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>NordPool Charger - WiFi Setup</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body { 
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
      background: #0f0f23; color: #ccc; padding: 20px;
      min-height: 100vh;
    }
    .container { max-width: 420px; margin: 0 auto; }
    h1 { color: #00d4aa; font-size: 1.4em; text-align: center; margin-bottom: 5px; }
    .subtitle { color: #888; text-align: center; font-size: 0.9em; margin-bottom: 25px; }
    .card {
      background: #1a1a2e; border-radius: 12px; padding: 20px;
      margin-bottom: 15px; border: 1px solid #2a2a4a;
    }
    .card h3 { color: #00d4aa; margin-bottom: 12px; font-size: 1em; }
    .network-list { list-style: none; }
    .network-item {
      padding: 12px; margin: 4px 0; border-radius: 8px;
      background: #16162b; cursor: pointer; display: flex;
      justify-content: space-between; align-items: center;
      transition: background 0.2s;
    }
    .network-item:hover { background: #1e1e3a; }
    .network-item.selected { background: #0a3d2e; border: 1px solid #00d4aa; }
    .network-name { font-weight: 500; color: #e0e0e0; }
    .network-signal { color: #888; font-size: 0.85em; }
    label { display: block; color: #aaa; font-size: 0.85em; margin-bottom: 4px; margin-top: 12px; }
    input, select {
      width: 100%; padding: 10px 12px; border-radius: 8px;
      border: 1px solid #2a2a4a; background: #16162b; color: #e0e0e0;
      font-size: 1em; outline: none;
    }
    input:focus, select:focus { border-color: #00d4aa; }
    .btn {
      display: block; width: 100%; padding: 14px; margin-top: 20px;
      border: none; border-radius: 8px; font-size: 1.05em;
      font-weight: 600; cursor: pointer; transition: all 0.2s;
    }
    .btn-primary { background: #00d4aa; color: #0f0f23; }
    .btn-primary:hover { background: #00f0c0; }
    .hidden { display: none; }
    .divider { border-top: 1px solid #2a2a4a; margin: 15px 0; }
    .hint { color: #666; font-size: 0.8em; margin-top: 4px; }
  </style>
</head>
<body>
  <div class="container">
    <h1>&#9889; NordPool Charger</h1>
    <p class="subtitle">WiFi Setup</p>
    
    <form action="/save" method="POST">
      <div class="card">
        <h3>&#128246; Available Networks</h3>
        <ul class="network-list" id="networks">
)rawliteral";

  for (int i = 0; i < n; i++) {
    int rssi = WiFi.RSSI(i);
    String signalStr = rssi > -50 ? "&#9679;&#9679;&#9679;" : 
                       rssi > -70 ? "&#9679;&#9679;&#9675;" : "&#9679;&#9675;&#9675;";
    String lock = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) ? "&#128274;" : "&#128275;";
    
    html += "<li class='network-item' onclick=\"selectNetwork('" + WiFi.SSID(i) + "',this)\">";
    html += "<span class='network-name'>" + lock + " " + WiFi.SSID(i) + "</span>";
    html += "<span class='network-signal'>" + signalStr + " " + String(rssi) + "dBm</span>";
    html += "</li>";
  }
  
  if (n == 0) {
    html += "<li class='network-item'><span class='network-name'>No networks found</span></li>";
  }

  html += R"rawliteral(
        </ul>
        <input type="hidden" name="ssid" id="ssid_field" value="">
        
        <div class="divider"></div>
        
        <label>Or enter hidden network SSID:</label>
        <input type="text" name="custom_ssid" placeholder="Hidden network name">
        
        <label>WiFi Password:</label>
        <input type="password" name="password" id="password" placeholder="Network password">
      </div>
      
      <div class="card">
        <h3>&#127760; Price Area</h3>
        <select name="area">
          <option value="FI" selected>&#127467;&#127470; Finland (FI) - ALV 25,5%</option>
          <option value="SE1">&#127480;&#127466; Sweden SE1 (Lule&aring;) - Moms 25%</option>
          <option value="SE2">&#127480;&#127466; Sweden SE2 (Sundsvall) - Moms 25%</option>
          <option value="SE3">&#127480;&#127466; Sweden SE3 (Stockholm) - Moms 25%</option>
          <option value="SE4">&#127480;&#127466; Sweden SE4 (Malm&ouml;) - Moms 25%</option>
          <option value="DK1">&#127465;&#127472; Denmark DK1 (West) - Moms 25%</option>
          <option value="DK2">&#127465;&#127472; Denmark DK2 (East) - Moms 25%</option>
          <option value="NO1">&#127475;&#127476; Norway NO1 (Oslo) - MVA 25%</option>
          <option value="NO2">&#127475;&#127476; Norway NO2 (Kristiansand) - MVA 25%</option>
          <option value="NO3">&#127475;&#127476; Norway NO3 (Trondheim) - MVA 25%</option>
          <option value="NO4">&#127475;&#127476; Norway NO4 (Troms&oslash;) - MVA 25%</option>
          <option value="NO5">&#127475;&#127476; Norway NO5 (Bergen) - MVA 25%</option>
          <option value="EE">&#127466;&#127466; Estonia (EE) - KM 22%</option>
          <option value="LV">&#127473;&#127483; Latvia (LV) - PVN 21%</option>
          <option value="LT">&#127473;&#127481; Lithuania (LT) - PVM 21%</option>
          <option value="AT">&#127462;&#127481; Austria (AT) - MwSt 20%</option>
          <option value="BE">&#127463;&#127466; Belgium (BE) - BTW 21%</option>
          <option value="DE-LU">&#127465;&#127466; Germany (DE-LU) - MwSt 19%</option>
          <option value="FR">&#127467;&#127479; France (FR) - TVA 20%</option>
          <option value="NL">&#127475;&#127473; Netherlands (NL) - BTW 21%</option>
          <option value="PL">&#127477;&#127473; Poland (PL) - VAT 23%</option>
        </select>
      </div>
      
      <button type="submit" class="btn btn-primary">Save &amp; Connect</button>
    </form>
  </div>
  
  <script>
    function selectNetwork(ssid, el) {
      document.getElementById('ssid_field').value = ssid;
      document.querySelectorAll('.network-item').forEach(e => e.classList.remove('selected'));
      el.classList.add('selected');
      document.getElementById('password').focus();
    }
  </script>
</body>
</html>
)rawliteral";

  WiFi.scanDelete();
  return html;
}


// ======================== CHUNKED SEND HELPERS ========================
// These avoid String allocation entirely - critical for long-running stability

// Send a small formatted string using stack buffer
void sf(const char* fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  server.sendContent(buf);
}

// Get unit label as const char*
const char* getUnitLabel() {
  if (priceCurrency == "SEK" || priceCurrency == "NOK" || priceCurrency == "DKK") return "c/kWh";
  return "c/kWh";
}

// ======================== MAIN PAGE (CHUNKED) ========================

void sendMainPageChunked() {
  // Snapshot all volatile globals to avoid inconsistency during chunked send
  // (WiFi operations during sendContent may trigger yield() -> re-entry issues)
  int snapPriceCount = priceCount;
  int snapTodayCount = todayCount;
  float snapCurrentPrice = currentPriceDisplay;
  float snapMinPrice = minPriceDisplay;
  float snapMaxPrice = maxPriceDisplay;
  float snapPriceStop = priceStop;
  float snapPriceRoof = priceRoof;
  float snapTariff = tariff;
  int snapFcStart = forceChargeStart;
  int snapFcEnd = forceChargeEnd;
  bool snapPricesLoaded = pricesLoaded;
  bool snapRelayOpen = relayOpen;
  bool snapTomorrowLoaded = tomorrowLoaded;
  bool snapVatEnabled = vatEnabled;
  float snapVatRate = vatRate;
  float snapCheapHours = cheapHours;
  int snapCheapStart = cheapStart;
  int snapCheapEnd = cheapEnd;
  int snapCheapCount = cheapSlotCount;
  bool snapCheapComplete = cheapPlanComplete;
  float snapCheapAvg = cheapPlanAvg;
  float snapCheapPlanned = cheapPlanHours;
  unsigned long snapOverrideMs = overrideRemainingMs();
  int overrideRemainMin = 0;
  if (snapOverrideMs > 0) {
    overrideRemainMin = (int)(snapOverrideMs / 60000UL) + 1;
  }
  
  struct tm timeinfo;
  getLocalTime(&timeinfo);
  
  char timeStr[20];
  strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
  char dateStr[20];
  strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", &timeinfo);
  
  bool overRoofNow = snapPriceRoof >= 0 && snapCurrentPrice > snapPriceRoof;
  const char* chargingStatus = !snapPricesLoaded ? "LOADING PRICES..." :
                               (overrideRemainMin > 0 ? "OVERRIDE ON" :
                               (snapRelayOpen ? (overRoofNow ? "OFF &ndash; ABOVE ROOF" : "CHARGING OFF") :
                                "CHARGING ON"));
  const char* statusColor = !snapPricesLoaded ? "#ffa500" : 
                             (overrideRemainMin > 0 ? "#4488ff" :
                             (snapRelayOpen ? "#ff4444" : "#00d4aa"));
  const char* unitLabel = getUnitLabel();

  // --- PART 1: HTML head + CSS (static, stored in flash) ---
  server.sendContent_P(PSTR(
    "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>NordPool Charger</title>"
    "<meta http-equiv=\"refresh\" content=\"60\">"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
    "background:#0f0f23;color:#ccc;padding:15px;min-height:100vh}"
    ".container{max-width:600px;margin:0 auto}"
    ".header{text-align:center;margin-bottom:20px}"
    ".header h1{color:#00d4aa;font-size:1.3em}"
    ".header .time{color:#888;font-size:0.85em}"
    ".status-card{background:#1a1a2e;border-radius:12px;padding:20px;"
    "margin-bottom:15px;border:1px solid #2a2a4a;text-align:center}"
    ".status-big{font-size:2em;font-weight:700;margin:5px 0}"
    ".status-label{color:#888;font-size:0.85em}"
    ".price-now{display:flex;justify-content:space-around;margin:15px 0}"
    ".price-box{text-align:center;flex:1}"
    ".price-box .value{font-size:1.4em;font-weight:600}"
    ".price-box .label{color:#888;font-size:0.75em}"
    ".vat-note{color:#666;font-size:0.75em;margin-top:8px}"
    ".chart-card{background:#1a1a2e;border-radius:12px;padding:15px;"
    "margin-bottom:15px;border:1px solid #2a2a4a;position:relative}"
    ".chart-title{color:#00d4aa;font-size:0.9em;margin-bottom:10px}"
    "#chartContainer{position:relative}"
    "canvas{width:100%!important;height:280px!important}"
    "#tooltip{display:none;position:absolute;background:#2a2a4a;color:#e0e0e0;"
    "padding:8px 12px;border-radius:8px;font-size:0.85em;pointer-events:none;"
    "z-index:10;border:1px solid #00d4aa;white-space:nowrap;"
    "box-shadow:0 4px 12px rgba(0,0,0,0.4)}"
    "#tooltip .tip-time{color:#00d4aa;font-weight:600}"
    "#tooltip .tip-price{font-size:1.1em;font-weight:700;margin:2px 0}"
    "#tooltip .tip-status{font-size:0.8em}"
    ".ctrl-card{background:#1a1a2e;border-radius:12px;padding:15px;"
    "margin-bottom:15px;border:1px solid #2a2a4a}"
    ".ctrl-row{display:flex;gap:12px}"
    ".ctrl-item{flex:1}"
    ".ctrl-item label{display:block;color:#888;font-size:0.8em;margin-bottom:4px}"
    ".ctrl-item input,.ctrl-item select{width:100%;padding:8px;border-radius:6px;"
    "border:1px solid #2a2a4a;background:#16162b;color:#e0e0e0;font-size:0.95em;outline:none}"
    ".ctrl-item input:focus,.ctrl-item select:focus{border-color:#00d4aa}"
    ".fc-row{display:flex;align-items:center;gap:4px}"
    ".fc-row select{flex:1;padding:8px 4px;border-radius:6px;"
    "border:1px solid #2a2a4a;background:#16162b;color:#e0e0e0;font-size:0.95em}"
    ".fc-row span{color:#888}"
    ".btn-save{display:block;width:100%;padding:10px;margin-top:10px;"
    "border:none;border-radius:8px;font-size:0.95em;font-weight:600;"
    "cursor:pointer;background:#00d4aa;color:#0f0f23}"
    ".btn-save:hover{background:#00f0c0}"
    ".ovr-card{background:#1a1a2e;border-radius:12px;padding:12px 15px;"
    "margin-bottom:15px;border:1px solid #2a2a4a}"
    ".ovr-row{display:flex;align-items:center;gap:8px}"
    ".ovr-row select{padding:8px;border-radius:6px;border:1px solid #2a2a4a;"
    "background:#16162b;color:#e0e0e0;font-size:0.95em}"
    ".ovr-row .btn-ovr{padding:8px 16px;border:none;border-radius:6px;"
    "font-size:0.9em;font-weight:600;cursor:pointer;white-space:nowrap}"
    ".btn-go{background:#4488ff;color:#fff}.btn-go:hover{background:#5599ff}"
    ".btn-cancel{background:#ff4444;color:#fff}.btn-cancel:hover{background:#ff6666}"
    ".ovr-label{color:#888;font-size:0.8em;margin-bottom:4px}"
    ".ovr-active{color:#4488ff;font-size:0.85em;margin-top:4px}"
    ".links{text-align:center;margin-top:15px}"
    ".links a{color:#00d4aa;text-decoration:none;margin:0 10px;font-size:0.9em}"
    ".links a:hover{text-decoration:underline}"
    "</style></head><body><div class=\"container\">"
    "<div class=\"header\"><h1>&#9889; NordPool Charger</h1><div class=\"time\">"
  ));

  // --- Dynamic: date/time/area ---
  sf("%s %s | %s", dateStr, timeStr, priceArea.c_str());
  
  server.sendContent_P(PSTR(
    "</div></div>"
    "<div class=\"status-card\"><div class=\"status-label\">RELAY STATUS</div>"
    "<div class=\"status-big\" style=\"color:"
  ));
  
  sf("%s\">%s", statusColor, chargingStatus);
  
  server.sendContent_P(PSTR(
    "</div><div class=\"price-now\">"
    "<div class=\"price-box\"><div class=\"value\" style=\"color:#00d4aa\">"
  ));
  
  if (snapPricesLoaded) sf("%.2f", snapCurrentPrice); else server.sendContent("--");
  
  sf("</div><div class=\"label\">Current (%s)</div></div>"
     "<div class=\"price-box\"><div class=\"value\" style=\"color:#4488ff\">", unitLabel);
  
  if (snapPricesLoaded) sf("%.2f", snapMinPrice); else server.sendContent("--");
  
  server.sendContent_P(PSTR(
    "</div><div class=\"label\">Today Min</div></div>"
    "<div class=\"price-box\"><div class=\"value\" style=\"color:#ff6644\">"
  ));
  
  if (snapPricesLoaded) sf("%.2f", snapMaxPrice); else server.sendContent("--");
  
  server.sendContent_P(PSTR(
    "</div><div class=\"label\">Today Max</div></div></div>"
    "<div class=\"vat-note\">Prices "
  ));
  
  if (snapVatEnabled) {
    sf("incl. VAT %.1f%%", snapVatRate);
  } else {
    server.sendContent("excl. VAT");
  }
  if (snapTariff > 0) sf(" + %.2f snapTariff", snapTariff);
  sf(" | %s", unitLabel);
  server.sendContent("</div>");

  if (snapCheapHours > 0) {
    sf("<div class=\"vat-note\" style=\"color:#00d4aa\">&#128176; Cheapest %gh in %02d:00&ndash;%02d:00",
       snapCheapHours, snapCheapStart, snapCheapEnd);
    if (snapPricesLoaded && snapCheapCount > 0) sf(" &middot; avg %.2f %s", snapCheapAvg, unitLabel);
    if (snapPricesLoaded && !snapCheapComplete) {
      server.sendContent(" &middot; <span style=\"color:#ffa500\">waiting for next-day prices</span>");
    } else if (snapPricesLoaded && snapCheapPlanned + 0.01f < snapCheapHours) {
      sf(" &middot; <span style=\"color:#ffa500\">only %gh %s</span>", snapCheapPlanned,
         snapPriceRoof >= 0 ? "below roof" : "left in window");
    }
    server.sendContent("</div>");
  }

  server.sendContent_P(PSTR(
    "</div>"
    "<div class=\"chart-card\"><div class=\"chart-title\">&#128200; Electricity Prices ("
  ));
  
  sf("%s, ", unitLabel);
  if (snapVatEnabled) sf("incl. VAT %.1f%%", snapVatRate); else server.sendContent("excl. VAT");
  if (snapTomorrowLoaded) server.sendContent(" &mdash; Today + Tomorrow");
  
  server.sendContent_P(PSTR(
    ")</div><div id=\"chartContainer\">"
    "<canvas id=\"priceChart\"></canvas>"
    "<div id=\"tooltip\">"
    "<div class=\"tip-time\" id=\"tipTime\"></div>"
    "<div class=\"tip-price\" id=\"tipPrice\"></div>"
    "<div class=\"tip-status\" id=\"tipStatus\"></div>"
    "</div></div></div>"
  ));
  
  // --- Override card ---
  server.sendContent_P(PSTR(
    "<div class=\"ovr-card\"><div class=\"ovr-label\">Manual override — force charging ON</div>"
    "<div class=\"ovr-row\">"
    "<form action=\"/override\" method=\"POST\" style=\"display:flex;gap:8px;align-items:center;flex:1\">"
    "<select name=\"duration\">"
    "<option value=\"30\">0.5h</option>"
    "<option value=\"60\">1h</option>"
    "<option value=\"90\">1.5h</option>"
    "<option value=\"120\" selected>2h</option>"
    "<option value=\"180\">3h</option>"
    "<option value=\"240\">4h</option>"
    "<option value=\"360\">6h</option>"
    "<option value=\"480\">8h</option>"
    "<option value=\"720\">12h</option>"
    "</select>"
    "<button type=\"submit\" class=\"btn-ovr btn-go\">&#9889; Charge</button>"
    "</form>"
  ));
  
  if (overrideRemainMin > 0) {
    server.sendContent_P(PSTR(
      "<form action=\"/override\" method=\"POST\">"
      "<input type=\"hidden\" name=\"duration\" value=\"cancel\">"
      "<button type=\"submit\" class=\"btn-ovr btn-cancel\">&#10005; Cancel</button>"
      "</form>"
    ));
  }
  
  server.sendContent_P(PSTR("</div>"));
  
  if (overrideRemainMin > 0) {
    int hrs = overrideRemainMin / 60;
    int mins = overrideRemainMin % 60;
    if (hrs > 0) {
      sf("<div class=\"ovr-active\">&#9889; Override active — %dh %dmin remaining</div>", hrs, mins);
    } else {
      sf("<div class=\"ovr-active\">&#9889; Override active — %d min remaining</div>", mins);
    }
  }
  
  server.sendContent_P(PSTR(
    "</div>"
    "<form action=\"/savequick\" method=\"POST\"><div class=\"ctrl-card\"><div class=\"ctrl-row\">"
    "<div class=\"ctrl-item\"><label>Max price ("
  ));
  
  sf("%s)</label><input type=\"number\" name=\"price_stop\" step=\"0.1\" min=\"-10\" max=\"200\" placeholder=\"No limit\"", unitLabel);
  if (snapPriceStop >= 0) sf(" value=\"%.1f\"", snapPriceStop);
  
  server.sendContent_P(PSTR(
    "></div><div class=\"ctrl-item\"><label>Always allowed</label>"
    "<div class=\"fc-row\"><select name=\"fc_start\"><option value=\"\">Off</option>"
  ));
  
  for (int h = 0; h < 24; h++) {
    sf("<option value=\"%d\"%s>%02d</option>", h, (snapFcStart==h)?" selected":"", h);
  }
  
  server.sendContent_P(PSTR(
    "</select><span>-</span><select name=\"fc_end\"><option value=\"\">Off</option>"
  ));
  
  for (int h = 0; h < 24; h++) {
    sf("<option value=\"%d\"%s>%02d</option>", h, (snapFcEnd==h)?" selected":"", h);
  }
  
  server.sendContent_P(PSTR(
    "</select></div></div></div>"
    "<div class=\"ctrl-row\" style=\"margin-top:10px\">"
    "<div class=\"ctrl-item\"><label>Cheapest hours</label>"
    "<input type=\"number\" name=\"cheap_hours\" step=\"0.25\" min=\"0\" max=\"24\" placeholder=\"Off\""
  ));

  if (snapCheapHours > 0) sf(" value=\"%g\"", snapCheapHours);

  server.sendContent_P(PSTR(
    "></div><div class=\"ctrl-item\"><label>Within window</label>"
    "<div class=\"fc-row\"><select name=\"cheap_start\">"
  ));

  for (int h = 0; h < 24; h++) {
    sf("<option value=\"%d\"%s>%02d</option>", h, (snapCheapStart==h)?" selected":"", h);
  }

  server.sendContent_P(PSTR("</select><span>-</span><select name=\"cheap_end\">"));

  for (int h = 0; h < 24; h++) {
    sf("<option value=\"%d\"%s>%02d</option>", h, (snapCheapEnd==h)?" selected":"", h);
  }

  server.sendContent_P(PSTR(
    "</select></div></div></div>"
    "<div class=\"ctrl-row\" style=\"margin-top:10px\">"
    "<div class=\"ctrl-item\"><label>Never charge above ("
  ));

  sf("%s)</label><input type=\"number\" name=\"price_roof\" step=\"0.1\" min=\"-10\" max=\"200\" placeholder=\"Off\"", unitLabel);
  if (snapPriceRoof >= 0) sf(" value=\"%.1f\"", snapPriceRoof);

  server.sendContent_P(PSTR(
    "></div><div class=\"ctrl-item\"></div></div>"
    "<button type=\"submit\" class=\"btn-save\">Save</button>"
    "</div></form>"
    "<div class=\"links\"><a href=\"/settings\">&#9881; Settings</a>"
    "<a href=\"/api/prices\">&#128203; API</a></div></div>"
    "<script>const prices=["
  ));
  
  // --- Price data: sent entry by entry, zero allocation ---
  for (int i = 0; i < snapPriceCount; i++) {
    float displayP = rawToDisplay(prices[i].priceRaw);
    char entry[96];
    snprintf(entry, sizeof(entry), "%s{\"t\":\"%s\",\"p\":%.2f,\"tm\":%d,\"c\":%d}",
             i > 0 ? "," : "", prices[i].timeStr, displayP, prices[i].isTomorrow ? 1 : 0,
             isCheapSlot(prices[i].startEpoch) ? 1 : 0);
    server.sendContent(entry);
  }
  
  // --- JS config variables ---
  sf("];const todayCount=%d;const thresholdStop=%.2f;const fcStart=%d;const fcEnd=%d;const unit='%s';"
     "const cheapMode=%d;const roofPrice=%.2f;",
     snapTodayCount, snapPriceStop, snapFcStart, snapFcEnd, unitLabel, snapCheapHours > 0 ? 1 : 0,
     snapPriceRoof);
  
  // --- PART 3: Chart JS (static, stored in flash) ---
  server.sendContent_P(PSTR(
    "let barRects=[];"
    "function drawChart(){"
    "const canvas=document.getElementById('priceChart');"
    "const ctx=canvas.getContext('2d');"
    "const dpr=window.devicePixelRatio||1;"
    "const rect=canvas.getBoundingClientRect();"
    "canvas.width=rect.width*dpr;canvas.height=rect.height*dpr;"
    "ctx.scale(dpr,dpr);"
    "const W=rect.width,H=rect.height;"
    "const pad={top:20,right:15,bottom:35,left:50};"
    "const chartW=W-pad.left-pad.right,chartH=H-pad.top-pad.bottom;"
    "barRects=[];"
    "if(prices.length===0){"
    "ctx.fillStyle='#888';ctx.font='14px sans-serif';ctx.textAlign='center';"
    "ctx.fillText('No price data available',W/2,H/2);return;}"
    "let pMin=Infinity,pMax=-Infinity;"
    "prices.forEach(p=>{if(p.p<pMin)pMin=p.p;if(p.p>pMax)pMax=p.p;});"
    "const range=pMax-pMin,yPad=Math.max(range*0.15,0.5);"
    "const yMin=Math.min(pMin-yPad,0),yMax=pMax+yPad,yRange=yMax-yMin||1;"
    "ctx.fillStyle='#16162b';ctx.fillRect(pad.left,pad.top,chartW,chartH);"
    "ctx.strokeStyle='#2a2a4a';ctx.lineWidth=0.5;"
    "for(let i=0;i<=5;i++){"
    "const y=pad.top+(chartH/5)*i;"
    "ctx.beginPath();ctx.moveTo(pad.left,y);ctx.lineTo(pad.left+chartW,y);ctx.stroke();"
    "const val=yMax-(yRange/5)*i;"
    "ctx.fillStyle='#888';ctx.font='10px sans-serif';ctx.textAlign='right';"
    "ctx.fillText(val.toFixed(1),pad.left-5,y+3);}"
    "if(yMin<0&&yMax>0){"
    "const zeroY=pad.top+chartH-((0-yMin)/yRange)*chartH;"
    "ctx.strokeStyle='#555';ctx.lineWidth=1;"
    "ctx.beginPath();ctx.moveTo(pad.left,zeroY);ctx.lineTo(pad.left+chartW,zeroY);ctx.stroke();}"
    "if(thresholdStop>=0){"
    "const y=pad.top+chartH-((thresholdStop-yMin)/yRange)*chartH;"
    "if(y>=pad.top&&y<=pad.top+chartH){"
    "ctx.strokeStyle='#ff6644';ctx.lineWidth=1.5;ctx.setLineDash([5,3]);"
    "ctx.beginPath();ctx.moveTo(pad.left,y);ctx.lineTo(pad.left+chartW,y);ctx.stroke();"
    "ctx.setLineDash([]);ctx.fillStyle='#ff6644';ctx.font='bold 10px sans-serif';"
    "ctx.textAlign='left';ctx.fillText('Stop '+thresholdStop.toFixed(1),pad.left+4,y-4);}}"
    "if(roofPrice>=0){"
    "const y=pad.top+chartH-((roofPrice-yMin)/yRange)*chartH;"
    "if(y>=pad.top&&y<=pad.top+chartH){"
    "ctx.strokeStyle='#ffa500';ctx.lineWidth=1.5;ctx.setLineDash([2,2]);"
    "ctx.beginPath();ctx.moveTo(pad.left,y);ctx.lineTo(pad.left+chartW,y);ctx.stroke();"
    "ctx.setLineDash([]);ctx.fillStyle='#ffa500';ctx.font='bold 10px sans-serif';"
    "ctx.textAlign='right';ctx.fillText('Roof '+roofPrice.toFixed(1),pad.left+chartW-4,y-4);}}"
    "if(todayCount>0&&todayCount<prices.length){"
    "const sepX=pad.left+todayCount*(chartW/prices.length);"
    "ctx.strokeStyle='#00d4aa';ctx.lineWidth=1.5;ctx.setLineDash([3,3]);"
    "ctx.beginPath();ctx.moveTo(sepX,pad.top);ctx.lineTo(sepX,pad.top+chartH);ctx.stroke();"
    "ctx.setLineDash([]);ctx.fillStyle='#00d4aa';ctx.font='bold 10px sans-serif';"
    "ctx.textAlign='left';ctx.fillText('Tomorrow',sepX+4,pad.top+12);}"
  ));
  
  server.sendContent_P(PSTR(
    "const gap=1,totalBarW=chartW/prices.length,barW=Math.max(1,totalBarW-gap);"
    "const now=new Date(),nowMins=now.getHours()*60+now.getMinutes();"
    "prices.forEach((p,i)=>{"
    "const x=pad.left+i*totalBarW+gap/2;"
    "const barH=((p.p-yMin)/yRange)*chartH,y=pad.top+chartH-barH;"
    "const pH=parseInt(p.t.split(':')[0]);"
    "let inFC=false;"
    "if(fcStart>=0&&fcEnd>=0){"
    "if(fcStart<=fcEnd){inFC=(pH>=fcStart&&pH<fcEnd);}"
    "else{inFC=(pH>=fcStart||pH<fcEnd);}}"
    "let wouldCharge=true;"
    "if(inFC){wouldCharge=true;}"
    "else if(cheapMode){wouldCharge=!!p.c||(thresholdStop>=0&&p.p<=thresholdStop);}"
    "else if(thresholdStop>=0){wouldCharge=p.p<=thresholdStop;}"
    "const overRoof=roofPrice>=0&&p.p>roofPrice;"
    "if(overRoof){wouldCharge=false;inFC=false;}"
    "let color=wouldCharge?'#00d4aa':'#ff4444';"
    "if(inFC&&!wouldCharge)color='#4488ff';"
    "if(inFC&&wouldCharge&&thresholdStop>=0)color='#4488ff';"
    "if(p.tm){if(inFC)color='#3366cc';else color=wouldCharge?'#00a088':'#cc3333';}"
    "if(overRoof)color=p.tm?'#772233':'#992233';"
    "const parts=p.t.split(':'),pMins=parseInt(parts[0])*60+parseInt(parts[1]);"
    "let nextMins=1440;"
    "if(i+1<prices.length){"
    "const np=prices[i+1],nm=parseInt(np.t.split(':')[0])*60+parseInt(np.t.split(':')[1]);"
    "if(np.tm&&!p.tm)nextMins=1440;else nextMins=nm;}"
    "const isCurrent=!p.tm&&nowMins>=pMins&&nowMins<nextMins;"
    "if(isCurrent){ctx.fillStyle='rgba(255,255,255,0.08)';"
    "ctx.fillRect(x-1,pad.top,totalBarW+2,chartH);color='#ffffff';}"
    "if(p.p>=0){"
    "const bfz=((p.p-Math.max(yMin,0))/yRange)*chartH;"
    "const zY=pad.top+chartH-((0-yMin)/yRange)*chartH;"
    "ctx.fillStyle=color;ctx.fillRect(x,zY-bfz,barW,bfz);}"
    "else{const zY=pad.top+chartH-((0-yMin)/yRange)*chartH;"
    "const nH=((0-p.p)/yRange)*chartH;"
    "ctx.fillStyle=color;ctx.fillRect(x,zY,barW,nH);}"
    "barRects.push({x:x,y:Math.min(y,pad.top+chartH),w:barW,h:Math.abs(barH),"
    "time:p.t,price:p.p,tomorrow:p.tm,charge:wouldCharge,current:isCurrent,forced:inFC,cheap:!!p.c,roof:overRoof,"
    "hitX:x,hitY:pad.top,hitW:totalBarW,hitH:chartH});});"
  ));
  
  server.sendContent_P(PSTR(
    "ctx.fillStyle='#888';ctx.font='10px sans-serif';ctx.textAlign='center';"
    "const li=prices.length>96?8:(prices.length>24?4:2);"
    "prices.forEach((p,i)=>{"
    "const h=parseInt(p.t.split(':')[0]),m=parseInt(p.t.split(':')[1]);"
    "if(m===0&&h%li===0){"
    "const x=pad.left+i*totalBarW+totalBarW/2;"
    "ctx.fillText(p.t,x,pad.top+chartH+15);}});"
    "ctx.save();ctx.translate(12,pad.top+chartH/2);ctx.rotate(-Math.PI/2);"
    "ctx.fillStyle='#888';ctx.font='10px sans-serif';ctx.textAlign='center';"
    "ctx.fillText(unit,0,0);ctx.restore();}"
  ));
  
  // Tooltip JS
  server.sendContent_P(PSTR(
    "function setupTooltip(){"
    "const canvas=document.getElementById('priceChart');"
    "const tooltip=document.getElementById('tooltip');"
    "const container=document.getElementById('chartContainer');"
    "function showTooltip(e){"
    "const rect=canvas.getBoundingClientRect();"
    "const mx=e.clientX-rect.left,my=e.clientY-rect.top;"
    "let found=null;"
    "for(const bar of barRects){"
    "if(mx>=bar.hitX&&mx<=bar.hitX+bar.hitW&&my>=bar.hitY&&my<=bar.hitY+bar.hitH)"
    "{found=bar;break;}}"
    "if(found){"
    "const idx=barRects.indexOf(found);"
    "const nextTime=(idx+1<barRects.length)?barRects[idx+1].time:'00:00';"
    "const dayLabel=found.tomorrow?' (Tomorrow)':'';"
    "document.getElementById('tipTime').textContent=found.time+' - '+nextTime+dayLabel;"
    "document.getElementById('tipPrice').textContent=found.price.toFixed(2)+' '+unit;"
    "let st;"
    "if(found.roof){st=found.current?'\\u25ba Current (above roof)':'\\u26d4 Above roof price';}"
    "else if(found.current){st=found.forced?'\\u25ba Current (forced ON)':'\\u25ba Current period';}"
    "else if(found.forced){st='\\u26a1 Always charge window';}"
    "else{st=found.charge?(found.cheap?'\\u25cf Charging (cheapest)':'\\u25cf Charging ON'):'\\u25cb Charging OFF';}"
    "const se=document.getElementById('tipStatus');"
    "se.textContent=found.tomorrow?(found.roof?'\\u26d4 Above roof price':found.forced?'\\u26a1 Always charge window':"
    "(found.charge?(found.cheap?'\\u25cf Would charge (cheapest)':'\\u25cf Would charge'):'\\u25cb Would stop')):st;"
    "se.style.color=found.forced?'#4488ff':(found.current?'#ffffff':"
    "(found.charge?'#00d4aa':'#ff4444'));"
    "let tx=found.hitX+found.hitW/2,ty=found.hitY-5;"
    "tooltip.style.display='block';"
    "const tw=tooltip.offsetWidth;"
    "if(tx+tw/2>rect.width)tx=rect.width-tw/2-5;"
    "if(tx-tw/2<0)tx=tw/2+5;"
    "tooltip.style.left=(tx-tw/2)+'px';"
    "tooltip.style.bottom=(rect.height-ty+5)+'px';"
    "tooltip.style.top='auto';"
    "}else{tooltip.style.display='none';}}"
    "canvas.addEventListener('mousemove',showTooltip);"
    "canvas.addEventListener('touchstart',function(e){"
    "e.preventDefault();const t=e.touches[0];"
    "showTooltip({clientX:t.clientX,clientY:t.clientY});},{passive:false});"
    "canvas.addEventListener('touchmove',function(e){"
    "e.preventDefault();const t=e.touches[0];"
    "showTooltip({clientX:t.clientX,clientY:t.clientY});},{passive:false});"
    "canvas.addEventListener('mouseleave',function(){tooltip.style.display='none';});"
    "canvas.addEventListener('touchend',function(){"
    "setTimeout(()=>{tooltip.style.display='none';},2000);});}"
    "drawChart();setupTooltip();"
    "window.addEventListener('resize',function(){drawChart();setupTooltip();});"
    "</script></body></html>"
  ));
  
  server.sendContent("");  // End chunked transfer
}


// ======================== SETTINGS PAGE (CHUNKED) ========================

void sendSettingsPageChunked() {
  const char* unitLabel = getUnitLabel();
  
  server.sendContent_P(PSTR(
    "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>NordPool Charger - Settings</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
    "background:#0f0f23;color:#ccc;padding:20px;min-height:100vh}"
    ".container{max-width:420px;margin:0 auto}"
    "h1{color:#00d4aa;font-size:1.3em;text-align:center;margin-bottom:20px}"
    ".card{background:#1a1a2e;border-radius:12px;padding:20px;"
    "margin-bottom:15px;border:1px solid #2a2a4a}"
    ".card h3{color:#00d4aa;margin-bottom:12px;font-size:1em}"
    "label{display:block;color:#aaa;font-size:0.85em;margin-bottom:4px;margin-top:12px}"
    "input,select{width:100%;padding:10px 12px;border-radius:8px;"
    "border:1px solid #2a2a4a;background:#16162b;color:#e0e0e0;font-size:1em;outline:none}"
    "input:focus,select:focus{border-color:#00d4aa}"
    ".hint{color:#666;font-size:0.8em;margin-top:4px}"
    ".checkbox-row{display:flex;align-items:center;margin-top:12px}"
    ".checkbox-row input[type=\"checkbox\"]{width:auto;margin-right:8px;accent-color:#00d4aa}"
    ".checkbox-row label{margin:0}"
    ".btn{display:block;width:100%;padding:14px;margin-top:20px;"
    "border:none;border-radius:8px;font-size:1.05em;font-weight:600;cursor:pointer}"
    ".btn-primary{background:#00d4aa;color:#0f0f23}"
    ".btn-primary:hover{background:#00f0c0}"
    ".btn-reboot{background:#2a2a4a;color:#ccc;margin-top:10px;border:1px solid #3a3a5a}"
    ".btn-reboot:hover{background:#3a3a5a}"
    ".btn-danger{background:#ff4444;color:#fff;margin-top:10px}"
    ".btn-danger:hover{background:#ff6666}"
    ".back{display:block;text-align:center;margin-top:15px;color:#00d4aa;text-decoration:none}"
    ".info-box{background:#16162b;border-radius:8px;padding:12px;"
    "margin-top:12px;font-size:0.85em;line-height:1.6}"
    ".info-box strong{color:#00d4aa}"
    ".fc-row{display:flex;gap:8px;align-items:center}"
    ".fc-row select{flex:1}.fc-row span{color:#888}"
    ".warn{color:#ffa500;font-size:0.8em;margin-top:6px}"
    ".current-val{color:#00d4aa;font-size:0.8em}"
    "</style></head><body><div class=\"container\">"
    "<h1>&#9881; Settings</h1>"
  ));
  
  sf("<div style=\"text-align:center;color:#666;font-size:0.75em;margin-bottom:15px\">"
     "Firmware v%s | Built %s</div>", FW_VERSION, FW_BUILD_DATE);
  
  server.sendContent_P(PSTR(
    "<form action=\"/savesettings\" method=\"POST\">"
    "<div class=\"card\"><h3>&#128246; WiFi Connection</h3>"
    "<div class=\"current-val\">Connected to: "
  ));
  
  sf("%s (%s)", wifiSSID.c_str(), WiFi.localIP().toString().c_str());
  
  server.sendContent_P(PSTR(
    "</div><label>WiFi SSID</label>"
    "<input type=\"text\" name=\"wifi_ssid\" placeholder=\"Leave empty to keep current\">"
    "<label>WiFi Password</label>"
    "<input type=\"password\" name=\"wifi_pass\" placeholder=\"Leave empty to keep current\">"
    "<div class=\"warn\">&#9888; Changing WiFi will reboot the device</div></div>"
    "<div class=\"card\"><h3>&#127760; Device Name (mDNS)</h3>"
    "<div class=\"current-val\">Current: http://"
  ));
  
  sf("%s.local", mdnsName.c_str());
  
  server.sendContent_P(PSTR("</div><label>Hostname</label><input type=\"text\" name=\"mdns_name\" value=\""));
  server.sendContent(mdnsName);
  server.sendContent_P(PSTR(
    "\"><div class=\"hint\">Access your charger at http://[name].local</div>"
    "<div class=\"warn\">&#9888; Changing name will reboot the device</div></div>"
    "<div class=\"card\"><h3>&#127760; Price Area &amp; Currency</h3>"
    "<label>Delivery area</label><select name=\"area\" id=\"area\">"
    "<option value=\"FI\">Finland (FI) - ALV 25,5%</option>"
    "<option value=\"SE1\">Sweden SE1 - Moms 25%</option>"
    "<option value=\"SE2\">Sweden SE2 - Moms 25%</option>"
    "<option value=\"SE3\">Sweden SE3 - Moms 25%</option>"
    "<option value=\"SE4\">Sweden SE4 - Moms 25%</option>"
    "<option value=\"DK1\">Denmark DK1 - Moms 25%</option>"
    "<option value=\"DK2\">Denmark DK2 - Moms 25%</option>"
    "<option value=\"NO1\">Norway NO1 - MVA 25%</option>"
    "<option value=\"NO2\">Norway NO2 - MVA 25%</option>"
    "<option value=\"NO3\">Norway NO3 - MVA 25%</option>"
    "<option value=\"NO4\">Norway NO4 - MVA 25%</option>"
    "<option value=\"NO5\">Norway NO5 - MVA 25%</option>"
    "<option value=\"EE\">Estonia - KM 22%</option>"
    "<option value=\"LV\">Latvia - PVN 21%</option>"
    "<option value=\"LT\">Lithuania - PVM 21%</option>"
    "<option value=\"AT\">Austria - MwSt 20%</option>"
    "<option value=\"BE\">Belgium - BTW 21%</option>"
    "<option value=\"DE-LU\">Germany - MwSt 19%</option>"
    "<option value=\"FR\">France - TVA 20%</option>"
    "<option value=\"NL\">Netherlands - BTW 21%</option>"
    "<option value=\"PL\">Poland - VAT 23%</option>"
    "</select><label>Currency</label><select name=\"currency\" id=\"currency\">"
    "<option value=\"EUR\">EUR (c/kWh)</option>"
    "<option value=\"SEK\">SEK (c/kWh)</option>"
    "<option value=\"NOK\">NOK (c/kWh)</option>"
    "<option value=\"DKK\">DKK (c/kWh)</option>"
    "</select><div class=\"checkbox-row\">"
    "<input type=\"checkbox\" name=\"vat_enabled\" id=\"vat_enabled\" value=\"on\""
  ));
  
  if (vatEnabled) server.sendContent(" checked");
  
  server.sendContent_P(PSTR(
    "><label for=\"vat_enabled\">Include VAT in prices</label></div>"
    "<div class=\"info-box\"><strong>VAT rates:</strong> Finland 25,5% (ALV) | "
    "Sweden/Norway/Denmark 25% | Estonia 22% | Latvia/Lithuania 21% | "
    "Germany 19% | Austria/France 20%<br>"
    "Nord Pool API returns prices without VAT. When enabled, VAT is added automatically."
    "</div></div>"
    "<div class=\"card\"><h3>&#128336; Timezone</h3>"
    "<div class=\"current-val\">Current: "
  ));
  
  server.sendContent(timezone);
  
  server.sendContent_P(PSTR(
    "</div><label>Select timezone</label>"
    "<select name=\"timezone\" id=\"tz\">"
    "<option value=\"EET-2EEST,M3.5.0/3,M10.5.0/4\">Finland / Estonia / Latvia / Lithuania (EET, UTC+2)</option>"
    "<option value=\"CET-1CEST,M3.5.0,M10.5.0/3\">Sweden / Norway / Denmark / Germany / France / etc (CET, UTC+1)</option>"
    "<option value=\"GMT0BST,M3.5.0/1,M10.5.0\">UK / Ireland / Portugal (GMT, UTC+0)</option>"
    "<option value=\"WET0WEST,M3.5.0/1,M10.5.0\">Western European (WET, UTC+0)</option>"
    "</select>"
    "<div class=\"hint\">Auto-set when you change the price area. Override manually if needed.</div>"
    "</div>"
  ));
  
  // Set the timezone dropdown to current value via JS
  // (done at the end of the page with other JS selects)
  
  server.sendContent_P(PSTR(
    "<div class=\"card\"><h3>&#128178; Electricity Tariff / Margin</h3><label>Company margin + transfer fee ("
  ));
  
  sf("%s)</label><input type=\"number\" name=\"tariff\" step=\"0.01\" min=\"0\" max=\"50\" placeholder=\"0\"", unitLabel);
  if (tariff > 0) sf(" value=\"%.2f\"", tariff);
  
  server.sendContent_P(PSTR(
    "><div class=\"hint\">Added on top of spot price + VAT. Set to 0 or leave empty for pure spot price.</div></div>"
    "<div class=\"card\"><h3>&#128176; Price Threshold ("
  ));
  
  sf("%s", unitLabel);
  if (vatEnabled) sf(", incl. VAT %.1f%%", vatRate); else server.sendContent(", excl. VAT");
  if (tariff > 0) server.sendContent(" + tariff");
  
  server.sendContent_P(PSTR(
    ")</h3><label>Stop charging when price exceeds</label>"
    "<input type=\"number\" name=\"price_stop\" step=\"0.1\" min=\"-10\" max=\"200\" "
    "placeholder=\"Leave empty = always charge\""
  ));
  
  if (priceStop >= 0) sf(" value=\"%.1f\"", priceStop);

  server.sendContent_P(PSTR(
    "><div class=\"hint\">Relay opens (charging stops) when current price is above this value</div>"
    "<label>Never charge above (roof price)</label>"
    "<input type=\"number\" name=\"price_roof\" step=\"0.1\" min=\"-10\" max=\"200\" "
    "placeholder=\"Leave empty = no roof\""
  ));

  if (priceRoof >= 0) sf(" value=\"%.1f\"", priceRoof);

  server.sendContent_P(PSTR(
    "><div class=\"hint\">Hard limit: blocks charging above this price, also during always allowed hours "
    "and cheapest hours. Only the manual override can charge above it.</div></div>"
    "<div class=\"card\"><h3>&#128337; Always Allowed Charging Hours</h3>"
    "<p style=\"color:#aaa;font-size:0.85em;margin-bottom:12px\">"
    "During this window charging is always ON, even if price exceeds the threshold.</p>"
    "<div class=\"fc-row\"><select name=\"fc_start\"><option value=\"\">Off</option>"
  ));
  
  for (int h = 0; h < 24; h++) {
    sf("<option value=\"%d\"%s>%02d:00</option>", h, (forceChargeStart==h)?" selected":"", h);
  }
  
  server.sendContent_P(PSTR(
    "</select><span>&#8594;</span><select name=\"fc_end\"><option value=\"\">Off</option>"
  ));
  
  for (int h = 0; h < 24; h++) {
    sf("<option value=\"%d\"%s>%02d:00</option>", h, (forceChargeEnd==h)?" selected":"", h);
  }
  
  server.sendContent_P(PSTR(
    "</select></div>"
    "<div class=\"hint\">Example: 23:00 &#8594; 07:00 = always charge overnight. Both Off = disabled.</div></div>"
    "<div class=\"card\"><h3>&#128176; Cheapest Hours</h3>"
    "<p style=\"color:#aaa;font-size:0.85em;margin-bottom:12px\">"
    "Charge only during the cheapest hours within a time window, e.g. 4 cheapest hours between 22:00 and 07:00.</p>"
    "<label>Hours to charge</label>"
    "<input type=\"number\" name=\"cheap_hours\" step=\"0.25\" min=\"0\" max=\"24\" placeholder=\"Leave empty = off\""
  ));

  if (cheapHours > 0) sf(" value=\"%g\"", cheapHours);

  server.sendContent_P(PSTR(
    "><label>Window</label><div class=\"fc-row\"><select name=\"cheap_start\">"
  ));

  for (int h = 0; h < 24; h++) {
    sf("<option value=\"%d\"%s>%02d:00</option>", h, (cheapStart==h)?" selected":"", h);
  }

  server.sendContent_P(PSTR("</select><span>&#8594;</span><select name=\"cheap_end\">"));

  for (int h = 0; h < 24; h++) {
    sf("<option value=\"%d\"%s>%02d:00</option>", h, (cheapEnd==h)?" selected":"", h);
  }

  server.sendContent_P(PSTR(
    "</select></div>"
    "<div class=\"hint\">Same start and end = 24h window. Start the window after next-day prices are "
    "published (~14:00 CET) so the whole window is known when planning.</div></div>"
    "<div class=\"card\"><div class=\"info-box\">"
    "<strong>How charging logic works:</strong><br><br>"
    "&#8226; <strong>Max price set:</strong> Charges when price &#8804; max, stops when above<br>"
    "&#8226; <strong>Cheapest hours set:</strong> Charges only in the cheapest slots of the window. "
    "If max price is also set, charges additionally whenever price &#8804; max<br>"
    "&#8226; <strong>Always allowed hours:</strong> Overrides max price &#8212; charges regardless during this window<br>"
    "&#8226; <strong>Roof price set:</strong> Never charges above it, whatever the other rules say. "
    "Cheapest hours skips slots above the roof, so you may get fewer hours than requested<br>"
    "&#8226; <strong>Manual override:</strong> Always charges, even above the roof<br>"
    "&#8226; <strong>Nothing set:</strong> Always charges<br><br>"
    "<strong>Relay wiring:</strong> Use COM + NC (Normally Closed) terminals. "
    "If ESP32 loses power, relay de-energizes and NC closes = charging stays ON.<br><br>"
    "<strong>Next-day prices:</strong> Published around 14:00-15:00 CET. "
    "The device checks every 5 minutes after 14:00 local time until tomorrow's prices appear."
    "</div></div>"
    "<button type=\"submit\" class=\"btn btn-primary\">Save Settings</button></form>"
    "<div class=\"card\"><h3>&#128190; Firmware Update</h3>"
    "<p style=\"color:#aaa;font-size:0.85em;margin-bottom:12px\">"
    "Upload a new .bin firmware file compiled from Arduino IDE "
    "(Sketch &#8594; Export Compiled Binary).</p>"
    "<form action=\"/update\" method=\"POST\" enctype=\"multipart/form-data\">"
    "<input type=\"file\" name=\"firmware\" accept=\".bin\" "
    "style=\"width:100%;padding:8px;margin-bottom:10px;border-radius:6px;"
    "border:1px solid #2a2a4a;background:#16162b;color:#e0e0e0\">"
    "<button type=\"submit\" class=\"btn btn-primary\" "
    "onclick=\"return confirm('Upload new firmware? Device will reboot after update.')\">"
    "&#128190; Upload &amp; Update</button></form></div>"
    "<form action=\"/reboot\" method=\"POST\" onsubmit=\"return confirm('Reboot the device?')\">"
    "<button type=\"submit\" class=\"btn btn-reboot\">&#128260; Reboot Device</button></form>"
    "<form action=\"/reset\" method=\"POST\" onsubmit=\"return confirm("
    "'Reset ALL settings and reboot? WiFi, area, thresholds - everything will be cleared.')\">"
    "<button type=\"submit\" class=\"btn btn-danger\">&#128465; Factory Reset</button></form>"
    "<a href=\"/\" class=\"back\">&#8592; Back to Dashboard</a></div>"
    "<script>document.getElementById('area').value='"
  ));
  
  server.sendContent(priceArea);
  server.sendContent_P(PSTR("';document.getElementById('currency').value='"));
  server.sendContent(priceCurrency);
  server.sendContent_P(PSTR("';document.getElementById('tz').value='"));
  server.sendContent(timezone);
  server.sendContent_P(PSTR("';</script></body></html>"));
  
  server.sendContent("");  // End chunked transfer
}


// ======================== API JSON (CHUNKED) ========================

void sendApiJsonChunked() {
  int ovRemain = 0;
  unsigned long ovMs = overrideRemainingMs();
  if (ovMs > 0) {
    ovRemain = (int)(ovMs / 60000UL) + 1;
  }
  sf("{\"version\":\"%s\",\"build\":\"%s\",\"area\":\"%s\",\"currency\":\"%s\",\"timezone\":\"%s\",\"vatEnabled\":%s,\"vatRate\":%.1f,\"tariff\":%.2f,",
     FW_VERSION, FW_BUILD_DATE, priceArea.c_str(), priceCurrency.c_str(), timezone.c_str(),
     vatEnabled ? "true" : "false", vatRate, tariff);
  sf("\"currentPrice\":%.2f,\"unit\":\"c/kWh\",\"relayOpen\":%s,\"charging\":%s,",
     currentPriceDisplay, relayOpen ? "true" : "false", !relayOpen ? "true" : "false");
  sf("\"priceStop\":%.1f,\"forceChargeStart\":%d,\"forceChargeEnd\":%d,"
     "\"overrideMinRemaining\":%d,",
     priceStop, forceChargeStart, forceChargeEnd, ovRemain);
  sf("\"priceRoof\":%.1f,\"cheapHours\":%.2f,\"cheapStart\":%d,\"cheapEnd\":%d,\"cheapSlots\":%d,"
     "\"cheapPlannedHours\":%.2f,\"cheapPlanComplete\":%s,\"cheapAvg\":%.2f,",
     priceRoof, cheapHours, cheapStart, cheapEnd, cheapSlotCount, cheapPlanHours,
     cheapPlanComplete ? "true" : "false", cheapPlanAvg);
  sf("\"priceCount\":%d,\"todayCount\":%d,\"tomorrowLoaded\":%s,"
     "\"minPrice\":%.2f,\"maxPrice\":%.2f,\"freeHeap\":%u,\"prices\":[",
     priceCount, todayCount, tomorrowLoaded ? "true" : "false",
     minPriceDisplay, maxPriceDisplay, ESP.getFreeHeap());
  
  for (int i = 0; i < priceCount; i++) {
    float dp = rawToDisplay(prices[i].priceRaw);
    char entry[96];
    snprintf(entry, sizeof(entry), "%s{\"time\":\"%s\",\"price\":%.2f,\"tomorrow\":%s,\"cheap\":%s}",
             i > 0 ? "," : "", prices[i].timeStr, dp,
             prices[i].isTomorrow ? "true" : "false",
             isCheapSlot(prices[i].startEpoch) ? "true" : "false");
    server.sendContent(entry);
  }
  
  server.sendContent("]}");
  server.sendContent("");  // End chunked transfer
}


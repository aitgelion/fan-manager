#include <Arduino.h>

// WIFI
#include <WiFi.h>
#include <AsyncTCP.h>
#include <WiFiManager.h>
  // Update firmware
#include <ElegantOTA.h>

// Persistence
#include <Preferences.h>
// Temperature
#include <OneWire.h>
#include <DallasTemperature.h>
//REST API
#include <ESPAsyncWebServer.h>
#include "AsyncJson.h"
#include "ArduinoJson.h"

// Constants:
  // Milliseconds
#define LOOP_PERIOD_MS 1000
#define WIFI_ON_TIMEOUT 10*60
  // Hertzs, ex: 40000
#define PWM_FREQ 25000
  // Seconds
#define WIFI_SETUP_TIMEOUT 120 
  // Seconds before fan is turned off
#define FAN_MIN_SECONDS_ON 10
  // Max supported simultaneous fans
#define FAN_NUMBER_MAX 8

// PINs USED
#define PIN_LED 8
#define PIN_ONE_WIRE_BUS 10

// FIRST pin to use with fans
#define PIN_FAN_0 0

// Status bits
#define STATUS_STOPPING 1<<0
#define STATUS_REFRESH  1<<1
#define STATUS_ABSOLUTE 1<<2

// Variables:
unsigned long loop_count = 0, ms = 0;
int wifi_count_on = WIFI_ON_TIMEOUT;
  // Library objects
Preferences preferences;
AsyncWebServer server(80);
  // Sensors
OneWire oneWire(PIN_ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
  // Reference temperature sensor:
DeviceAddress tmp_ref_addr;

struct fan_status {
  uint8_t pin_fan;

  // The sensor associated to this fan
  DeviceAddress tmp_addr;

  // Current values:
  uint8_t pwm;
  float tmp;
  
  // Ranges:
  uint8_t pwm_min;
  uint8_t pwm_max;
  float tmp_diff_start; // Temperature difference to start
  float tmp_diff_end; // temperature difference to be on pwm_max
  
  // Precalculated slope:
  float slope;

  int16_t timmer; // If set manually, seconds to be on this speed
  uint8_t status; // refresh, stopping, absolute mode
} fans[FAN_NUMBER_MAX];
uint8_t fan_number = 0;

float tmp_ref = 0;
bool led = false;
int temp_precission = 12;
bool relative_temp = false;

// put function declarations here:
void setup_pins();
void load_configuration();
void manage_fans();
void setup_server();
void manage_wifi();
// OTA Functions
void onOTAStart();

void setup() {
  Serial.begin(115200);
  Serial.println("\n Starting");

  setup_pins();
  load_configuration();

  // Configure WIFI:
  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.setConfigPortalTimeout(WIFI_SETUP_TIMEOUT);
  wm.setCaptivePortalEnable(true);

  if (!wm.autoConnect("FanMan")) {
    Serial.println("Failed to connect");
    wifi_count_on = 0;
  } else {
    // Configure Server
    setup_server();
    // Start services
    ElegantOTA.begin(&server);
    ElegantOTA.onStart(onOTAStart);
    server.begin();
  }
}

void loop() {
  ms = millis();
  if ((unsigned long)(ms - loop_count) > LOOP_PERIOD_MS)
  {
    loop_count = ms;
    
    led = false;
    manage_fans();
    // Led changes to UP if PWM changed in any fan
    digitalWrite(PIN_LED, led ? LOW : HIGH);

    manage_wifi();
  } else {
    // Avoid spin at full speed
    delay(20);
  }

  if (wifi_count_on != 0) {
    ElegantOTA.loop();
  }
}

// Functions
void setup_pins()
{
  // Onboard led:
  pinMode(PIN_LED, OUTPUT);

  // Configure PWM pins
  for (uint8_t i = 0; i < FAN_NUMBER_MAX; i++) {
    ledcSetup(i, PWM_FREQ, 8);
    ledcAttachPin(PIN_FAN_0 + i, i);
    ledcWrite(i, 0);
  }

  Serial.println("Init sensors");
  sensors.begin();
  uint8_t count = sensors.getDeviceCount();
  
  sensors.requestTemperatures();
  for (uint8_t i = 0; i < count; i++) {
    DeviceAddress addr;
    sensors.getAddress(addr, i);
    sensors.setResolution(addr, temp_precission);
  }
}

void load_configuration()
{
  preferences.begin("c", false);

  char name[] = "??"; // {index}{property_name} lowercase for min, uppercase for max
  
  // Reference fan address:
  name[0] = 'r';
  name[1] = 'a';
  preferences.getBytes(name, tmp_ref_addr, sizeof(tmp_ref_addr));
  Serial.print("Ref addr: ");
  for (int j = 0; j < 8; j++) {
    Serial.print("0x");
    Serial.print(tmp_ref_addr[j], HEX);
    Serial.print(",");
  }
  Serial.println();
  
  // Get if wifi must be shut down or not
  name[0] = 'g';
  name[1] = 'w';
  bool wifi_auto_off = preferences.getBool(name, true);
  if (!wifi_auto_off) wifi_count_on = -1;
  
  // "gn" : number of enabled fans
  name[0] = 'g';
  name[1] = 'n';
  fan_number = preferences.getUChar(name, 0);
  Serial.print("Enabled fans: ");
  Serial.println(fan_number);

  for (uint8_t i = 0; i < fan_number; i++) {
    name[0] = ((uint8_t)'0') + i;

    // Default values:
    fans[i].pin_fan = PIN_FAN_0 + i;
    fans[i].timmer = 0;
    fans[i].pwm = 0;
    fans[i].slope = -1;

    // Mode: relative (default), absolute
    name[1] = 'm';
    fans[i].status = preferences.getUChar(name, 0);
    relative_temp = !(fans[i].status & STATUS_ABSOLUTE);

    // Sensor
    name[1] = 'a';
    preferences.getBytes(name, fans[i].tmp_addr, sizeof(fans[i].tmp_addr));
    
    // PWM:
    name[1] = 'p';
    fans[i].pwm_min = preferences.getUChar(name, 0);
    name[1] = 'P';
    fans[i].pwm_max = preferences.getUChar(name, 255);

    // Temperature range
    name[1] = 't';
    fans[i].tmp_diff_start = preferences.getFloat(name, 0);
    name[1] = 'T';
    fans[i].tmp_diff_end = preferences.getFloat(name, 5);
  }

  preferences.end();
}

void manage_fans() {
  if (fan_number == 0) 
  {
    Serial.println("No fans enabled");
    return;
  }

  sensors.requestTemperatures();

  // If at least one fan is managed relatively, get the reference sensor temp:
  if (relative_temp) {
    tmp_ref = sensors.getTempC(tmp_ref_addr);
  }

  float tmp_diff = 0;
  uint8_t pwm_new = 0;

  for (uint8_t i = 0; i < fan_number; i++) {
    if (fans[i].timmer < 0) {
      // Infinite time with set PWM
      ledcWrite(i, fans[i].pwm);
      continue;
    }

    pwm_new = 0;
    // Get temperature from sensor
    fans[i].tmp = sensors.getTempC(fans[i].tmp_addr);
    
    tmp_diff = fans[i].tmp;
    if (!(fans[i].status & STATUS_ABSOLUTE)) {
      tmp_diff -= tmp_ref;
      if (tmp_diff < 0) tmp_diff = 0;
    }
    
    // If fan has to be turned off, ensure a minimum time to be off
    if (tmp_diff <= fans[i].tmp_diff_start) {
      if (!(fans[i].status & STATUS_STOPPING) && fans[i].pwm > 0 && fans[i].timmer == 0) {
        // If fan need to be turned off, set min value some time
        fans[i].status |= STATUS_STOPPING;
        fans[i].timmer = FAN_MIN_SECONDS_ON;
        pwm_new = max(fans[i].pwm, fans[i].pwm_min);
      }
    } else if (tmp_diff >= fans[i].tmp_diff_end) {
      fans[i].status &= ~STATUS_STOPPING; // Clear stopping bit
      pwm_new = fans[i].pwm_max;
    } else {
      fans[i].status &= ~STATUS_STOPPING;

      // Calculate slope if needed
      if (fans[i].slope < 0) {
        fans[i].slope = ((float)(fans[i].pwm_max - fans[i].pwm_min)) / (fans[i].tmp_diff_end - fans[i].tmp_diff_start);
      }

      // Calculate the PWM mapping ranges with diffs & PWM values
      pwm_new = fans[i].pwm_min + roundf(fans[i].slope * (tmp_diff - fans[i].tmp_diff_start));
    }
    
    // If fan has a timmer, do not change until timmer has ended or new PWM is HIGHER
    if (fans[i].timmer > 0) {
      if (pwm_new <= fans[i].pwm) {
        // Maintain higher PWM until timmer ends
        fans[i].timmer -= 1;
        // Crear stopping status if reached 0
        if(fans[i].timmer > 0) {
          pwm_new = fans[i].pwm;
        } else {
          fans[i].status &= ~STATUS_STOPPING;
          pwm_new = 0;
        }
      } else {
        fans[i].timmer = 0;
      }
    }

    if (fans[i].pwm != pwm_new || fans[i].status & STATUS_REFRESH) {
      fans[i].status &= ~STATUS_REFRESH;
      fans[i].pwm = pwm_new;
      ledcWrite(i, fans[i].pwm);

      Serial.printf("PWM[%d] = %d \n", i, fans[i].pwm);
      // Visually indicate PWM has changed
      led = true;
    }
  }
}

void setup_server() {

  // CORS & fallback
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  server.onNotFound([](AsyncWebServerRequest *request) {
    if (request->method() == HTTP_OPTIONS) {
      request->send(200);
    } else {
      request->send(404);
    }
  });
  
  // Routes
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (wifi_count_on > 0) wifi_count_on = WIFI_ON_TIMEOUT;

    // Json data
    JsonDocument doc;
    
    if (request->hasParam("fan")) {
      // Request status & config for one
      uint8_t i = request->getParam("fan")->value().toInt();
      
      // Status
      doc["tmp"] = fans[i].tmp;
      doc["pwm"] = fans[i].pwm;
      doc["timmer"] = fans[i].timmer;
      doc["stopping"] = (fans[i].status & STATUS_STOPPING) != 0;
      
      // Config
      JsonArray addrArr = doc["addr"].to<JsonArray>();
      for (uint8_t j=0; j < sizeof(tmp_ref_addr); j++) {
        addrArr.add(fans[i].tmp_addr[j]);
      }
      
      doc["absolute"] = (fans[i].status & STATUS_ABSOLUTE) != 0;
      doc["pwm-min"] = fans[i].pwm_min;
      doc["pwm-max"] = fans[i].pwm_max;
      doc["diff-min"] = fans[i].tmp_diff_start;
      doc["diff-max"] = fans[i].tmp_diff_end;

    } else {
      // Status for all of them
      if (relative_temp) {
        doc["tmp-ref"] = tmp_ref;
      }
      for (uint8_t i = 0; i < fan_number; i++) {
        doc["fans"][i]["tmp"] = fans[i].tmp;
        doc["fans"][i]["pwm"] = fans[i].pwm;
        doc["fans"][i]["timmer"] = fans[i].timmer;
        doc["fans"][i]["stopping"] = (fans[i].status & STATUS_STOPPING) != 0;
      }
    }

    char response[256];
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  server.addHandler(new AsyncCallbackJsonWebHandler("/set", [](AsyncWebServerRequest *request, JsonVariant &docJson) {
    if (wifi_count_on > 0) wifi_count_on = WIFI_ON_TIMEOUT;

    auto&& obj = docJson.as<JsonObject>();

    bool set = false;
    char name[] = "??";

    if (!request->hasParam("fan")) {
      // Process general settings
      if (!obj["addr"].isNull()) {
        if (!set) {set = true; preferences.begin("c", false);}

        JsonArray array = obj["addr"].as<JsonArray>();
        uint8_t j = 0;
        for (JsonVariant v : array) {
          if (j < sizeof(tmp_ref_addr)) {
            tmp_ref_addr[j] = v.as<uint8_t>();
            j++;
          }
        }
        // Save:
        name[0] = 'r';
        name[1] = 'a';
        preferences.putBytes(name, tmp_ref_addr, sizeof(tmp_ref_addr));
        preferences.end();
      }

      if (!obj["wifi-auto-off"].isNull()) {
        if (!set) {set = true; preferences.begin("c", false);}

        bool wifi_auto_off = obj["wifi-auto-off"].as<bool>();
        
        if (wifi_auto_off) {
          // Turn off in 5 seconds
          if (wifi_count_on > 5) wifi_auto_off = 5;
        } else {
          // 
          if (wifi_count_on > 1) wifi_auto_off = -1;
        }
        // Save:
        name[0] = 'g';
        name[1] = 'w';
        preferences.putBool(name, wifi_auto_off);
      }

      if (!obj["fans"].isNull()) {
        if (!set) {set = true; preferences.begin("c", false);}

        fan_number = obj["fans"].as<uint8_t>();
        // Save:
        name[0] = 'g';
        name[1] = 'n';
        preferences.putUChar(name, fan_number);
      }
      
      if (set) {
        preferences.end();
        request->send(200, "application/json");
        return;
      }

      request->send(400, "application/json", "Missing 'fan=x' query param");
      return;
    }

    uint8_t i = request->getParam("fan")->value().toInt();
    if (i > FAN_NUMBER_MAX) {
      request->send(400, "application/json", "Fan index out of limits");
      return;
    }

    // Set run speed:
    if (!obj["pwm"].isNull() && !obj["timmer"].isNull()){
      set = true;
      fans[i].timmer = obj["timmer"].as<int16_t>();
      fans[i].pwm = obj["pwm"].as<uint8_t>();
      fans[i].status &= ~STATUS_STOPPING;
      fans[i].status |= STATUS_REFRESH;
    }


    // Setup specified fan:
    name[0] = ((uint8_t)'0') + i;

    // Set & save config
    if (!obj["absolute"].isNull()) {
      if (!set) {set = true; preferences.begin("c", false);}

      if(obj["absolute"].as<bool>()){
        // Set bit
        fans[i].status |= STATUS_ABSOLUTE;
      } else {
        // Clear bit
        fans[i].status  &= ~STATUS_ABSOLUTE;
      }
      fans[i].slope = -1;
      // Save:
      name[1] = 'm';
      preferences.putUChar(name,  fans[i].status  & STATUS_ABSOLUTE);
    }

    if (!obj["pwm-min"].isNull()) {
      if (!set) {set = true; preferences.begin("c", false);}

      fans[i].pwm_min = obj["pwm-min"].as<uint8_t>();
      fans[i].slope = -1;
      // Save:
      name[1] = 'p';
      preferences.putUChar(name, fans[i].pwm_min);
    }
    if (!obj["pwm-max"].isNull()) {
      if (!set) {set = true; preferences.begin("c", false);}

      fans[i].pwm_max = obj["pwm-max"].as<uint8_t>();
      fans[i].slope = -1;
      // Save:
      name[1] = 'P';
      preferences.putUChar(name, fans[i].pwm_max);
    }

    if (!obj["diff-min"].isNull()) {
      if (!set) {set = true; preferences.begin("c", false);}

      fans[i].tmp_diff_start = obj["diff-min"].as<float>();
      fans[i].slope = -1;
      // Save:
      name[1] = 't';
      preferences.putFloat(name, fans[i].tmp_diff_start);
    }
    if (!obj["diff-max"].isNull()) {
      if (!set) {set = true; preferences.begin("c", false);}

      fans[i].tmp_diff_end = obj["diff-max"].as<float>();
      fans[i].slope = -1;
      // Save:
      name[1] = 'T';
      preferences.putFloat(name, fans[i].tmp_diff_end);
    }

    if (!obj["addr"].isNull()) {
      if (!set) {set = true; preferences.begin("c", false);}

      JsonArray array = obj["addr"].as<JsonArray>();
      uint8_t j = 0;
      for (JsonVariant v : array) {
        if (j < sizeof(tmp_ref_addr)) {
          fans[i].tmp_addr[j] = v.as<uint8_t>();
          j++;
        }
      }
      // Save:
      name[1] = 'a';
      preferences.putBytes(name, fans[i].tmp_addr, sizeof(tmp_ref_addr));
    }

    if (set) {preferences.end();}

    request->send(set ? 200 : 400, "application/json");
  }));

  server.on("/scan", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (wifi_count_on > 0) wifi_count_on = WIFI_ON_TIMEOUT;

    JsonDocument doc;
    
    Serial.println("Probing...");
    uint8_t count = sensors.getDeviceCount();
    Serial.println(count);

    sensors.requestTemperatures();
    for (uint8_t i = 0; i < count; i++) {
      DeviceAddress addr;
      sensors.getAddress(addr, i);
      float temp = sensors.getTempC(addr);
      
      // Write sensor address
      Serial.print(i);
      Serial.print("- ADDR =");
      for (int j = 0; j < 8; j++) {
        Serial.print("0x");
        Serial.print(addr[j], HEX);
        Serial.print(",");
      }
      
      // Write temp
      Serial.print("->");
      Serial.println(temp);

      // Add to response:
      JsonArray addrArr = doc[i]["addr"].to<JsonArray>();
      for (uint8_t j=0; j < sizeof(tmp_ref_addr); j++) {
        addrArr.add(addr[j]);
      }
      doc[i]["tmp"] = temp;
    }
    Serial.println("---");

    char response[256];
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  server.on("/disconnect", HTTP_GET, [](AsyncWebServerRequest *request) {
    wifi_count_on = wifi_count_on > 0 ? 5 : 0;
    Serial.println("Turn off wifi");
    request->send(200, "application/json");
  });
}

void manage_wifi() {
  if (wifi_count_on > 0) {
    wifi_count_on--; 
    if (wifi_count_on == 1) {
      // Stop API
      server.end();
      // Turn off wifi
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
    }
  }
}

void onOTAStart() {
  if (wifi_count_on > 0) wifi_count_on = WIFI_ON_TIMEOUT;
  Serial.println("OTA update started!");
}
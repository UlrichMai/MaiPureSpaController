/*
 * MaiPureSpaController.ino
 * the pool controller sketch
 *
 *  Created on: 2020-05-16
 *      Author: Ulrich Mai
 *      
 *  Updated on: 2021-01-23
 *      Author: James Gibbard
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ESP8266WebServer.h>
#include <ArduinoOTA.h>
#include <ArduinoQueue.h>
#include <PubSubClient.h>

#define MQTT_CLIENT     "Hot_Tub_Controller"                 // mqtt client_id
#define MQTT_SERVER     "192.168.0.2"                        // mqtt server
#define MQTT_PORT       1883                                 // mqtt port
#define MQTT_TOPIC      "home/hot_tub_controller"            // mqtt topic
#define MQTT_USER       "ha-sonoff"                          // mqtt user

#define VERSION    "\n\n-------------- Intex Spa WiFi Controller v1.01pOTA  --------------"

boolean OTAupdate = false;                                   // (Do not Change)
boolean requestRestart = false;                              // (Do not Change)

int kUpdFreq = 1;                                            // Update frequency in Mintes to check for mqtt connection
int kRetries = 50;                                           // WiFi retry count. Increase if not connecting to router.

unsigned long TTasks;                                        // (Do not Change)

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient, MQTT_SERVER, MQTT_PORT);

char hostname[] = "HotTub_XXXXXX";

void generateHostname() {
  uint8_t mac[WL_MAC_ADDR_LENGTH];
  WiFi.macAddress(mac);
  snprintf(hostname, 11, "HotTub_%02X%02X%02X", mac[3], mac[4], mac[5]);
}

/******************************************************************************************/
// Software SPI Client catching Intex pool controller input

// GPIO Pins            
const int CLK     = D7; 
const int LAT     = D6; 
const int DAT_IN  = D5; 
const int DAT_OUT = D0; //emulate button press      
const int BUZ     = D2; //out=buzzer beep, low=shut off; in=buzzer works normal, 

#define GPIO_IN ((volatile uint32_t*) 0x60000318)     //GPIO Read Register
#define GPIO_OUT ((volatile uint32_t*) 0x60000300)    //GPIO Write Register

// 16-Bit Shift Register, 
volatile uint8_t clkCount = 0;
volatile uint16_t buf = 0;

bool flushSync = false;                               // True when display reset, used to sync buffer

char digit[5] = "????";
int  curTempTmp;                                      // current temperature candidate
bool curTempTmpValid = false;                         // current temperature candidate is valid / has not timed out
int  curTemp = 20;                                    // current temperature
int  setTemp = 38;                                    // target temperature
int  lstTemp;                                         // last valid display reading

bool BuzzerEnabled = false;                           // (Do not Change) Internal buzzer state
bool mqttLastBuzzerState = false;                     // (Do not Change) Internal buzzer state

bool powerState = false;                              // (Do not Change) Internal power state
bool pumpState = false;                               // (Do not Change) Internal pump state
bool bubbleState = false;                             // (Do not Change) Internal bubble state
bool heaterState = false;                             // (Do not Change) Internal heater state
bool heatingState = false;                            // (Do not Change) Internal heating state

int targetTemp = setTemp;                             // (Do not Change) Internal target temp state
int currentTemp = curTemp;                            // (Do not Change) Internal current temp state

enum ButtonT {
  BTN_POWER  = 0,
  BTN_UP     = 1,
  BTN_DOWN   = 2,
  BTN_FILTER = 3,
  BTN_HEATER = 4,
  BTN_BUBBLE = 5,
  BTN_FC     = 6
};

volatile bool    btnRequest[7] = {};
volatile uint8_t btnCount[7] = {};
const int btnCycles  = 10;
bool btnPulse = false;

// Prototypen
void writeButton(ButtonT button);
void handleButton(ButtonT button);
void handleWebButton(ButtonT button);

//Just clock the data bits into the buffer
ICACHE_RAM_ATTR void SPI_handleClock() {  
  clkCount++;
  buf = buf << 1;                                     // Shift buffer along
  if (bitRead(*GPIO_IN, DAT_IN) == 1) bitSet(buf, 0); // Flip data bit in buffer if needed.
}

ICACHE_RAM_ATTR void SPI_handleLatch() {
  if (!simulateButtonPress()) {
    if (flushSync) {
      if (clkCount == 16) {
        // Valid if 16 clock cycles detected since last latch
        if (false);
        else if (bitRead(buf, 6) == 0)  readSegment(0);
        else if (bitRead(buf, 5) == 0)  readSegment(1);
        else if (bitRead(buf, 11) == 0) readSegment(2);
        else if (bitRead(buf, 2) == 0)  readSegment(3);
        else if (bitRead(buf, 14) == 0) readLEDStates();
      }
      // reset buffer
      flushSync = false;
      buf = 0;
      clkCount = 0;
     } else {
      if ((buf | 0xF00) == 0xFFFF) { //If idle, we can use to mark a sync.
        flushSync = true;
        buf = 0;
        clkCount = 0;
      }
    }
  }
}

ICACHE_RAM_ATTR void readSegment(int seg) {
  uint16_t d = buf & 13976;      // mask and keep segment bits
  
  if      (d == 16)    digit[seg]='0';
  else if (d == 9368)  digit[seg]='1';
  else if (d == 520)   digit[seg]='2';
  else if (d == 136)   digit[seg]='3';
  else if (d == 9344)  digit[seg]='4';
  else if (d == 4224)  digit[seg]='5';
  else if (d == 4096)  digit[seg]='6';
  else if (d == 1176)  digit[seg]='7';
  else if (d == 0)     digit[seg]='8';
  else if (d == 128)   digit[seg]='9';
  else if (d == 1152)  digit[seg]='9';
 
  else if (d == 4624)  digit[seg]='C';
  else if (d == 5632)  digit[seg]='F';
  else if (d == 4608)  digit[seg]='E';
  else if (d == 13976) digit[seg]=' '; //blank

  // if this is the last digit, decide what temp value this is
  if (seg == 3) classifyTemperature();
}

const int reqCycles = 90;
int dispCycles = reqCycles; //non blank display cycles

ICACHE_RAM_ATTR void classifyTemperature() {
  if (digit[0] != ' ') { // non blank display
    // remember the last valid temp reading
    lstTemp = atoi(digit);
    
    if (--dispCycles < 0) {
      // temperature, that is not followed by an empty display for 90 (>82) cycles, is the current temperature
      if (validTempValue(curTempTmp) && (curTempTmpValid && curTemp != curTempTmp)) {
        curTemp = curTempTmp;
        Serial.println("Current Temp?: " + String(curTemp));
        current_temperature_changed_event(curTemp);      
      }
      dispCycles = reqCycles;
      curTempTmp = lstTemp; 
      curTempTmpValid = true;
    }
  } 
  else { // blank display during blinking
    // temperature before an blank display is the target temperature
    if (validTempValue(lstTemp) && (setTemp != lstTemp)) {
      setTemp = lstTemp;
      Serial.println("Target Temp?: " + String(setTemp));
      target_temperature_changed_event(setTemp);
    }
    dispCycles = reqCycles;
    curTempTmpValid = false;
  }
}

ICACHE_RAM_ATTR bool validTempValue(int value) {
  return (value >= 20 && value <= 40);
}

enum Led {
  LED_POWER        =0,
  LED_BUBBLE       =1,
  LED_HEATER_GREEN =2,
  LED_HEATER_RED   =3,
  LED_FILTER       =4  
};
uint8_t ledStates;

ICACHE_RAM_ATTR void readLEDStates() {
    uint8_t ls = ledStates;
    ledStates = 0;

    if (bitRead(buf, 0 ) == 0) bitSet(ledStates, LED_POWER);
    if (bitRead(buf, 12) == 0) bitSet(ledStates, LED_FILTER);
    if (bitRead(buf, 10) == 0) bitSet(ledStates, LED_BUBBLE);
    if (bitRead(buf, 9 ) == 0) bitSet(ledStates, LED_HEATER_GREEN);
    if (bitRead(buf, 7 ) == 0) bitSet(ledStates, LED_HEATER_RED);

    if (ls != ledStates) {      
      if (bitRead(ledStates, LED_POWER) != bitRead(ls, LED_POWER)) {
          Serial.println("LED_POWER_STATE_CHANGED");
          controller_power_state_changed_event();
      }
      if (bitRead(ledStates, LED_FILTER) != bitRead(ls, LED_FILTER)) {
        Serial.println("LED_FILTER_STATE_CHANGED");
        controller_pump_state_changed_event();
      }
      if (bitRead(ledStates, LED_BUBBLE) != bitRead(ls, LED_BUBBLE)) {
        Serial.println("LED_BUBBLE_STATE_CHANGED");
        controller_bubble_state_changed_event();
      }
      if ((bitRead(ledStates, LED_HEATER_GREEN) || bitRead(ledStates, LED_HEATER_RED)) != (bitRead(ls, LED_HEATER_GREEN) ||  bitRead(ls, LED_HEATER_RED))) {
        Serial.println("LED_HEATER_STATE_CHANGED");
        controller_heater_state_changed_event();
      }
      if (bitRead(ledStates, LED_HEATER_RED) != bitRead(ls, LED_HEATER_RED)) {
        Serial.println("LED_HEATING_STATE_CHANGED");
        controller_heating_state_changed_event();
      }
   }
}

ICACHE_RAM_ATTR bool simulateButtonPress() {
  // called at each latch
  if (btnPulse) {
    // reset pulse
    digitalWrite(DAT_OUT, 1);
    btnPulse = false;
  }
  uint16_t b = buf | 0x0100;                       // mask buzzer
  int button = -1;
  
  if      (b == 0xFFFF)   return false ; // no button
  else if (b == 0xFFFD)   button = BTN_FILTER; //65533
  else if (b == 0x7FFF)   button = BTN_HEATER; //32767
  else if (b == 0xEFFF)   button = BTN_UP;     //61439
  else if (b == 0xFF7F)   button = BTN_DOWN;   //65407
  else if (b == 0xFBFF)   button = BTN_POWER;  //64511
  else if (b == 0xFFF7)   button = BTN_BUBBLE; //65527
  else if (b == 0xDFFF)   button = BTN_FC; 
  else return false;

  if (btnRequest[button]) {
    if (--btnCount[button] <= 0) {
      btnRequest[button] = false;
    }
    // start button pulse (until next latch)
    digitalWrite(DAT_OUT, 0);
    btnPulse = true;
  }
  return true;
}

ICACHE_RAM_ATTR void writeButton(ButtonT button) {
    btnRequest[button] = true;
    btnCount[button] = btnCycles;
};

void switchBuzz(bool bOn) {
  BuzzerEnabled = bOn;
  if (bOn) {
    pinMode(BUZ, INPUT);
  } else {
    pinMode(BUZ, OUTPUT);
    digitalWrite(BUZ, 0);
  };
};

void buzzSignal(int nBeep) {
  pinMode(BUZ, OUTPUT);
  digitalWrite(BUZ, 0);
  for (;nBeep>0;nBeep--) {
    digitalWrite(BUZ, 1);
    delay(200);
    digitalWrite(BUZ, 0);
    delay(200);     
  }
  switchBuzz(BuzzerEnabled);
};

void initial_publish() {
  mqttLog("Publishing initial values . . .");
  mqttPublish("power_state", (powerState ? "on":"off"));
  mqttPublish("buzzer_state", (BuzzerEnabled ? "on":"off"));
  mqttPublish("pump_state", (powerState ? "on":"off"));
  mqttPublish("bubble_state", (bubbleState ? "on":"off"));
  mqttPublish("heater_state", (heaterState ? "on":"off"));
  mqttPublish("heating_state", (heatingState ? "on":"off"));
}

void SPI_begin() {
  mqttLog("SPI: Configuring shift register . . .");
  pinMode(CLK, INPUT);
  pinMode(LAT, INPUT);
  pinMode(DAT_IN, INPUT);
  pinMode(DAT_OUT, OUTPUT);
  digitalWrite(DAT_OUT, 1); //default high
  switchBuzz(BuzzerEnabled);

  mqttLog("SPI: Attaching handlers to interrupts . . .");
  attachInterrupt(digitalPinToInterrupt(CLK), SPI_handleClock, RISING);
  attachInterrupt(digitalPinToInterrupt(LAT), SPI_handleLatch, RISING);
}

void SPI_end() {
  mqttLog("SPI: Detaching handlers from interrupts . . .");
  detachInterrupt(digitalPinToInterrupt(CLK));
  detachInterrupt(digitalPinToInterrupt(LAT));
}

//---------------------------------------------------------------
// highlevel pool controller methods
//---------------------------------------------------------------
typedef enum {
    EVT_POWER_STATE_CHANGED,
    EVT_PUMP_STATE_CHANGED,
    EVT_BUBBLE_STATE_CHANGED,
    EVT_HEATER_STATE_CHANGED,
    EVT_HEATING_STATE_CHANGED,
    EVT_CURRENT_TEMPERATURE_CHANGED,
    EVT_TARGET_TEMPERATURE_CHANGED
} EventType;
  
typedef struct {
  EventType evt;
  bool state; 
  int temp;
} EventT;

ArduinoQueue<EventT> EventQueue(200);

//***********************************
// Temperature Events 
//***********************************

// CURRENT TEMP value changed event
ICACHE_RAM_ATTR void current_temperature_changed_event(int temp) {
  EventT e;
  e.evt = EVT_CURRENT_TEMPERATURE_CHANGED;
  e.state = false;
  e.temp = temp;
  EventQueue.enqueue(e);
}

// TARGET TEMP value changed event
ICACHE_RAM_ATTR void target_temperature_changed_event(int temp) {
  // called inside ISR, be fast!
  EventT e;
  e.evt = EVT_TARGET_TEMPERATURE_CHANGED;
  e.state = false;
  e.temp = temp;
  EventQueue.enqueue(e);
}

//***********************************
// Controller Events
//***********************************

// POWER state changed event
ICACHE_RAM_ATTR void controller_power_state_changed_event() {
  EventT e;
  e.evt = EVT_POWER_STATE_CHANGED;
  e.state = controller_power_state_get();
  e.temp = 0.0;
  EventQueue.enqueue(e);
}

// PUMP state changed event
ICACHE_RAM_ATTR void controller_pump_state_changed_event() {
  EventT e;
  e.evt = EVT_PUMP_STATE_CHANGED;
  e.state = controller_pump_state_get();
  e.temp = 0.0;
  EventQueue.enqueue(e);
}

// BUBBLE state changed event
ICACHE_RAM_ATTR void controller_bubble_state_changed_event() {
  EventT e;
  e.evt = EVT_BUBBLE_STATE_CHANGED;
  e.state = controller_bubble_state_get();
  e.temp = 0.0;
  EventQueue.enqueue(e);
}

// HEATER state changed event
ICACHE_RAM_ATTR void controller_heater_state_changed_event() {
  EventT e;
  e.evt = EVT_HEATER_STATE_CHANGED;
  e.state = controller_heater_state_get();
  e.temp = 0.0;
  EventQueue.enqueue(e);
}

// HEATING state changed event
ICACHE_RAM_ATTR void controller_heating_state_changed_event() {
  EventT e;
  e.evt = EVT_HEATING_STATE_CHANGED;
  e.state = controller_heating_state_get();
  e.temp = 0.0;
  EventQueue.enqueue(e);
}

//***********************************
// Controller Getters
//***********************************

// POWER get state
ICACHE_RAM_ATTR bool controller_power_state_get() {
  return (bitRead(ledStates, LED_POWER) == 1);
}

// PUMP get state
ICACHE_RAM_ATTR bool controller_pump_state_get() {
  return (bitRead(ledStates, LED_FILTER) == 1);
}

// BUBBLE get state
ICACHE_RAM_ATTR bool controller_bubble_state_get() {
  return (bitRead(ledStates, LED_BUBBLE) == 1);
}

// HEATER get state
ICACHE_RAM_ATTR bool controller_heater_state_get() {
  return (bitRead(ledStates, LED_HEATER_GREEN) == 1 || bitRead(ledStates, LED_HEATER_RED) == 1);
}

// HEATING get state
ICACHE_RAM_ATTR bool controller_heating_state_get() {
  return (bitRead(ledStates, LED_HEATER_RED) == 1);
}

// CURRENT TEMP get value
ICACHE_RAM_ATTR int controller_current_temperature_get() {
  return curTemp;
}

// TARGET TEMP get value
ICACHE_RAM_ATTR int controller_target_temperature_get() {
  return setTemp;
}

//***********************************
// Controller Setters
//***********************************

// POWER set state
ICACHE_RAM_ATTR void controller_power_state_set(bool newValue) {
  int retry = 1;
  while (controller_power_state_get() != newValue) {
    writeButton(BTN_POWER);
    delay(500);
    if (--retry < 0) return;
  }
}

// PUMP set state
ICACHE_RAM_ATTR void controller_pump_state_set(bool newValue) {
  int retry = 1;
  while (controller_pump_state_get() != newValue) {
    writeButton(BTN_FILTER);
    delay(500);
    if (--retry < 0) return;
  }
}

// BUBBLE set state
ICACHE_RAM_ATTR void controller_bubble_state_set(bool newValue) {
  int retry = 1;
  while (controller_bubble_state_get() != newValue) {
    writeButton(BTN_BUBBLE);
    delay(500);
    if (--retry < 0) return;
  }
}

// HEATER set state
ICACHE_RAM_ATTR void controller_heater_state_set(bool newValue) {
  int retry = 1;
  while (controller_heater_state_get() != newValue) {
    writeButton(BTN_HEATER);
    delay(500);
    if (--retry < 0) return;
  }
}

// TARGET TEMP set value
ICACHE_RAM_ATTR void controller_target_temperature_set(int newValue) {
  int retry = 20;
  // only works if power is on
  if (controller_power_state_get()) {
    while (controller_target_temperature_get() != newValue) {
      if (controller_target_temperature_get() > newValue) {
        writeButton(BTN_DOWN);
      } else {
        writeButton(BTN_UP);
      }
      delay(500);
      if (--retry < 0) return;
    }
  }
}

// TARGET TEMP increase value
ICACHE_RAM_ATTR void controller_target_temperature_increase() {
  int retry = 1;
  int tempNow = controller_target_temperature_get();
  // only works if power is on
  if (controller_power_state_get()) {
    while (controller_target_temperature_get() != (tempNow + 1)) {
      writeButton(BTN_UP);
      delay(600);
      if (--retry < 0) return;
    }
  }
}

// TARGET TEMP decrease value
ICACHE_RAM_ATTR void controller_target_temperature_decrease() {
  int retry = 1;
  int tempNow = controller_target_temperature_get();
  // only works if power is on
  if (controller_power_state_get()) {
    while (controller_target_temperature_get() != (tempNow - 1)) {
      writeButton(BTN_DOWN);
      delay(600);
      if (--retry < 0) return;
    }
  }
}

//***********************************
// Controller Button Handler
//***********************************

void handleBuzz(bool bOn) {
  switchBuzz( bOn );
  buzzSignal( (bOn ? 2 : 1) );
}

void handleButton(ButtonT button) {
   switch (button) {
      case BTN_POWER:
        controller_power_state_set(!controller_power_state_get());
        break;
      case BTN_FILTER:
        controller_pump_state_set(!controller_pump_state_get());
        break;
      case BTN_HEATER:
        controller_heater_state_set(!controller_heater_state_get());
        break;
      case BTN_BUBBLE:
        controller_bubble_state_set(!controller_bubble_state_get());
        break;
      case BTN_UP:
        controller_target_temperature_increase();
        break;
      case BTN_DOWN:
        controller_target_temperature_decrease();
        break;
   }
}

//***********************************
// Controller Event Loop - process
//***********************************

void controller_loop() {
  //handle events from controller like user interaction, current temp changes, heater changes

   if (mqttLastBuzzerState != BuzzerEnabled) {
     mqttLog("EVT_BUZZER_STATE_CHANGED: " + String(mqttLastBuzzerState ? "on":"off") + " -> " + String(BuzzerEnabled ? "on":"off"));
     mqttPublish("buzzer_state", String(BuzzerEnabled ? "on":"off"));
     mqttLastBuzzerState = BuzzerEnabled;
   }

  if (!EventQueue.isEmpty()) {
    EventT e = EventQueue.dequeue();
    switch (e.evt) {
      case EVT_POWER_STATE_CHANGED:
        if (powerState != e.state) {
          mqttLog("EVT_POWER_STATE_CHANGED: " + String(powerState ? "on":"off") + " -> " + String(e.state ? "on":"off"));
          powerState = e.state;
          mqttPublish("power_state", String(powerState ? "on":"off"));
        }
        break;
       case EVT_PUMP_STATE_CHANGED:
         if (pumpState != e.state) {
          mqttLog("EVT_PUMP_STATE_CHANGED: " + String(pumpState ? "on":"off") + " -> " + String(e.state ? "on":"off"));
          pumpState = e.state;
          mqttPublish("pump_state", String(pumpState ? "on":"off"));
         }
         break;
       case EVT_BUBBLE_STATE_CHANGED:
         if (bubbleState != e.state) {
          mqttLog("EVT_BUBBLE_STATE_CHANGED: " + String(bubbleState ? "on":"off") + " -> " + String(e.state ? "on":"off"));
          bubbleState = e.state;
          mqttPublish("bubble_state", String(bubbleState ? "on":"off"));
         }
         break;
       case EVT_HEATER_STATE_CHANGED:
         if (heaterState != e.state) {
           mqttLog("EVT_HEATER_STATE_CHANGED: " + String(heaterState ? "on":"off") + " -> " + String(e.state ? "on":"off"));
           heaterState = e.state;
           mqttPublish("heater_state", String(heaterState ? "on":"off"));
         }
         break;
       case EVT_HEATING_STATE_CHANGED:
         if (heatingState != e.state) {
            mqttLog("EVT_HEATING_STATE_CHANGED: " + String(heatingState ? "on":"off") + " -> " + String(e.state ? "on":"off"));
             heatingState = e.state;
             mqttPublish("heating_state", String(heatingState ? "on":"off"));
         }
         break;
       case EVT_CURRENT_TEMPERATURE_CHANGED: 
         if (currentTemp != e.temp && validTempValue(e.temp)) {
           mqttLog("EVT_CURRENT_TEMPERATURE_CHANGED: " + String(currentTemp) + " -> " +  String(e.temp));
           currentTemp = int(e.temp);
           mqttPublish("current_temperature", String(currentTemp));
         }
         break;
       case EVT_TARGET_TEMPERATURE_CHANGED:
         if (targetTemp != e.temp && validTempValue(e.temp)) {
           mqttLog("EVT_TARGET_TEMPERATURE_CHANGED: " + String(targetTemp) + " -> " +  String(e.temp));
           targetTemp = int(e.temp);
           mqttPublish("target_temperature", String(targetTemp));
         }
         break;
      }
   }
}

// Private.h contains the ssid and password as a temporary measure until a config page is added
#include "Private.h"

//---------------------------------------------------------------
// Webserver

ESP8266WebServer server(80);

void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(200, "text/plain", message);
}

void returnToStatus()
{
    delay(500);
    server.sendHeader("Location", "/",true); 
    server.send(302, "text/plain","");  
    server.client().stop();
}

const char htmlTemplate[] = 
"<!DOCTYPE html>"
"<html lang='en'>"
"<head>"
" <title>%s</title>"
" <metax http-equiv='refresh' content='5' />"
" <meta charset='utf-8' />"
" <meta name='viewport' content='width=device-width,initial-scale=1'/>"
" <link href=\"https://cdn.jsdelivr.net/npm/bootstrap@5.0.0-beta1/dist/css/bootstrap.min.css\" rel=\"stylesheet\" integrity=\"sha384-giJF6kkoqNQ00vy+HMDP7azOuL0xtbfIcaT9wjKHr8RbDVddVHyTfAAsrekwKmP1\" crossorigin=\"anonymous\" />"
"</head>"
"<body>"
" <h1>Intex Hot Tub Controller</h1>"
"<ul class=\"list-group\">"
"  <li class=\"list-group-item\">Water temperature:  <span class=\"badge bg-secondary\">%d</span></li>"
"  <li class=\"list-group-item\">Target temperature: <span class=\"badge bg-secondary\">%d</span></li>"
"  <li class=\"list-group-item\">"
"     <a class=\"btn btn-primary\" role=\"button\" href='/up'>INCREASE</a>"
"     <a class=\"btn btn-primary\" role=\"button\" href='/down'>DECREASE</a>"
"  </li>"
"  <li class=\"list-group-item\">Power <a class=\"btn btn-primary\" role=\"button\" href='/power'> %s </a></li>"
"  <li class=\"list-group-item\">Filter <a class=\"btn btn-primary\" role=\"button\" href='/filter'> %s </a></li>"
"  <li class=\"list-group-item\">Heater <a class=\"btn btn-primary\" role=\"button\" href='/heater'> %s </a></li>"
"  <li class=\"list-group-item\">Bubble <a class=\"btn btn-primary\" role=\"button\" href='/bubble'> %s </a></li>"
"  <li class=\"list-group-item\">Buzzer <a class=\"btn btn-primary\" role=\"button\" href='/buzz%s'> %s </a></li>"
"  <li class=\"list-group-item\"><a class=\"btn btn-primary\" role=\"button\" href='/'>Refresh</a></li>"
"</ul>"

"</body>"
"</html>";

char message[3000];
void handleStatus() {
  
  snprintf(message, 3000, htmlTemplate, 
    hostname,
    curTemp, 
    setTemp, 
    controller_power_state_get() ? "ON" : "OFF",
    controller_pump_state_get() ? "ON" : "OFF",
    controller_heater_state_get() ? "ON" : "OFF",
    controller_bubble_state_get() ? "ON" : "OFF",
    (BuzzerEnabled?"0":"1"), (BuzzerEnabled?"ON":"OFF")
  );
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  server.send(200, "text/html", message);
  Serial.printf("strlen(message): %d\n",strlen(message));
};

void handleWebButton(ButtonT button) {
  handleButton(button);
  returnToStatus();
};

void handleWebBuzz(bool bOn) {
  handleBuzz(bOn);
  returnToStatus();
};

void server_setup() {
  server.on("/", handleStatus);

  server.on("/reboot", []() {
    server.send(200, "text/plain", "Rebooting...");
    delay(1);
    ESP.reset();
  });

  server.on("/power",  []() {handleWebButton(BTN_POWER);});
  server.on("/up",     []() {handleWebButton(BTN_UP);});
  server.on("/down",   []() {handleWebButton(BTN_DOWN);});
  server.on("/filter", []() {handleWebButton(BTN_FILTER);});
  server.on("/heater", []() {handleWebButton(BTN_HEATER);});
  server.on("/bubble", []() {handleWebButton(BTN_BUBBLE);});
  server.on("/buzz0",  []() {handleWebBuzz(0);});
  server.on("/buzz1",  []() {handleWebBuzz(1);});
  server.onNotFound(handleNotFound);

  server.begin();
}
//******************************************************************************************

void ota_setup() {
  
  ArduinoOTA.setHostname(hostname);
  ArduinoOTA.setPassword(passwordOTA);

  ArduinoOTA.onStart([]() {
    OTAupdate = true;
    SPI_end(); // disable interrupts, otherwise OTA is failing
    mqttLog("OTA Update Initiated . . .");
  });
  
  ArduinoOTA.onEnd([]() {
    mqttLog("\nOTA Update Ended . . .");
    ESP.restart();
  });
  
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  
  ArduinoOTA.onError([](ota_error_t error) {
    OTAupdate = false;
    Serial.printf("OTA Error [%u] ", error);
    String otaErr = "";
    if (error == OTA_AUTH_ERROR) otaErr = ". . . . . . . . . . . . . . . Auth Failed";
    else if (error == OTA_BEGIN_ERROR) otaErr = ". . . . . . . . . . . . . . . Begin Failed";
    else if (error == OTA_CONNECT_ERROR) otaErr = ". . . . . . . . . . . . . . . Connect Failed";
    else if (error == OTA_RECEIVE_ERROR) otaErr = ". . . . . . . . . . . . . . . Receive Failed";
    else if (error == OTA_END_ERROR) otaErr = ". . . . . . . . . . . . . . . End Failed";

    mqttLog(otaErr);
  });
  
  ArduinoOTA.begin();
}

//******************************************************************************************

void callback(const MQTT::Publish& pub) {
  if (pub.topic() == MQTT_TOPIC"/power") {
    if (pub.payload_string() == "on") controller_power_state_set(true);
    else if (pub.payload_string() == "off") controller_power_state_set(false);
  }
  else if (pub.topic() == MQTT_TOPIC"/pump") {
    if (pub.payload_string() == "on") controller_pump_state_set(true);
    else if (pub.payload_string() == "off") controller_pump_state_set(false);
  }
  else if (pub.topic() == MQTT_TOPIC"/heater") {
    if (pub.payload_string() == "on") controller_heater_state_set(true);
    else if (pub.payload_string() == "off") controller_heater_state_set(false);
  }
  else if (pub.topic() == MQTT_TOPIC"/bubble") {
    if (pub.payload_string() == "on") controller_bubble_state_set(true);
    else if (pub.payload_string() == "off") controller_bubble_state_set(false);
  } 
  else if (pub.topic() == MQTT_TOPIC"/target_temp") {
    int new_temp = atoi((char *)pub.payload());
    if ((validTempValue(new_temp)) && (new_temp != targetTemp)) {
      controller_target_temperature_set(new_temp);
    }  
  }
  else {
    if (pub.payload_string() == "reset") {
      requestRestart = true;
    }
  }
}

//******************************************************************************************

void setup() {
  buzzSignal(1); //start setup
  Serial.begin(115200);
  Serial.setRxBufferSize(32);
  Serial.setDebugOutput(false);

  Serial.println('start setup');
  generateHostname();

  // Initalise the WiFi Connection
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  WiFi.hostname(hostname);
 
  Serial.println(VERSION);
  Serial.print("\nUnit ID: ");
  Serial.print("esp8266-");
  Serial.print(ESP.getChipId(), HEX);
  Serial.print("\nConnecting to "); Serial.print(ssid); Serial.print(" Wifi"); 
  
  while ((WiFi.status() != WL_CONNECTED) && kRetries --) {
    delay(500);
    Serial.print(" .");
  }
  
  if (WiFi.status() == WL_CONNECTED) {  
    Serial.println(" DONE");
    Serial.print("IP Address is: "); Serial.println(WiFi.localIP());
    Serial.print("Hostname is: "); Serial.println(hostname);
    Serial.print("Connecting to "); Serial.print(MQTT_SERVER); Serial.print(" Broker . .");
    delay(500);
    
    while (!mqttClient.connect(MQTT::Connect(MQTT_CLIENT).set_keepalive(90).set_auth(MQTT_USER, MQTT_PASS)) && kRetries --) {
      Serial.print(" .");
      delay(1000);
    }
    
    if(mqttClient.connected()) {
      Serial.println(" DONE");
      Serial.println("\n----------------------------  Logs  ----------------------------");
      Serial.println();
      
      mqttClient.subscribe(MQTT_TOPIC"/#");
      mqttClient.set_callback(callback);

      buzzSignal(1); //connected
      ota_setup();
      server_setup();
      initial_publish();
      SPI_begin();
   }
   else {
      Serial.println(" FAILED!");
      Serial.println("\n----------------------------------------------------------------");
      Serial.println();
    }
  }
  else {
    Serial.println(" WiFi FAILED!");
    Serial.println("\n----------------------------------------------------------------");
    Serial.println();
  }
}

void loop() {
  ArduinoOTA.handle();
  if (OTAupdate == false) { 
    mqttClient.loop();
    yield();
    timedTasks();
    yield();
    server.handleClient();
    yield();
    controller_loop();
    yield();
    if (requestRestart) {
      ESP.restart();
    }
  }
}

void checkConnection() {
  if (WiFi.status() == WL_CONNECTED)  {
    if (mqttClient.connected()) {
      mqttLog("mqtt broker connection . . . . . . . . . . OK");
    } 
    else {
      Serial.println("mqtt broker connection . . . . . . . . . . LOST");
      requestRestart = true;
    }
  }
  else { 
    Serial.println("WiFi connection . . . . . . . . . . LOST");
    requestRestart = true;
  }
}

void timedTasks() {
  if ((millis() > TTasks + (kUpdFreq*60000)) || (millis() < TTasks)) { 
    TTasks = millis();
    checkConnection();
  }
}

void mqttLog(String text) {
  Serial.println(text);
  if (mqttClient.connected()) {
    mqttClient.publish(MQTT::Publish(MQTT_TOPIC"/log", text).set_retain().set_qos(1));
  }
}

void mqttPublish(String topic, String text) {
  if (mqttClient.connected()) {
    mqttClient.publish(MQTT::Publish(MQTT_TOPIC"/" + topic, text).set_retain().set_qos(1));
  }
}

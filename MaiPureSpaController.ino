/*
 * MaiPureSpaController.ino
 * the pool controller sketch
 *
 *  Created on: 2020-05-16
 *      Author: Ulrich Mai
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <arduino_homekit_server.h>
#include <ESP8266WebServer.h>
#include <ArduinoOTA.h>
#include <ArduinoQueue.h>

//******************************************************************************************
//Homekit

#define SIMPLE_INFO(fmt, ...)   printf_P(PSTR(fmt "\n") , ##__VA_ARGS__);

extern "C" homekit_server_config_t config;
extern "C" homekit_characteristic_t accessory_name;
extern "C" void accessory_init();

extern "C" void homekit_current_temperature_set(float newValue);
extern "C" void homekit_target_temperature_set(float newValue);
extern "C" void homekit_current_heating_cooling_state_set(bool newValue);
extern "C" void homekit_target_heating_cooling_state_set(bool newValue);
extern "C" void homekit_power_on_set(bool newValue);
extern "C" void homekit_pump_on_set(bool newValue);

extern "C" void controller_power_state_set(bool newValue);
extern "C" bool controller_power_state_get();
extern "C" void controller_power_state_changed_event();
extern "C" void controller_pump_state_set(bool newValue);
extern "C" bool controller_pump_state_get();
extern "C" void controller_pump_state_changed_event();
extern "C" bool controller_current_heating_state_get();
extern "C" void controller_current_heating_state_changed_event();
extern "C" bool controller_target_heating_state_get();
extern "C" void controller_target_heating_state_set(bool newValue);
extern "C" void controller_target_heating_state_changed_event();
extern "C" int controller_current_temperature_get();
extern "C" void controller_current_temperature_changed_event(int temp);
extern "C" void controller_target_temperature_set(int newValue);
extern "C" int controller_target_temperature_get();
extern "C" void controller_target_temperature_changed_event(int temp);

char hostname[] = "Pool_XXXXXX";

void generateHostname() {
  uint8_t mac[WL_MAC_ADDR_LENGTH];
  WiFi.macAddress(mac);
  snprintf(hostname, 11, "Pool_%02X%02X%02X", mac[3], mac[4], mac[5]);
}

void homekit_setup() {
  SIMPLE_INFO("");
  SIMPLE_INFO("SketchSize: %d", ESP.getSketchSize());
  SIMPLE_INFO("FreeSketchSpace: %d", ESP.getFreeSketchSpace());
  SIMPLE_INFO("FlashChipSize: %d", ESP.getFlashChipSize());
  SIMPLE_INFO("FlashChipRealSize: %d", ESP.getFlashChipRealSize());
  SIMPLE_INFO("FlashChipSpeed: %d", ESP.getFlashChipSpeed());
  SIMPLE_INFO("SdkVersion: %s", ESP.getSdkVersion());
  SIMPLE_INFO("FullVersion: %s", ESP.getFullVersion().c_str());
  SIMPLE_INFO("CpuFreq: %dMHz", ESP.getCpuFreqMHz());
  SIMPLE_INFO("FreeHeap: %d", ESP.getFreeHeap());
  SIMPLE_INFO("ResetInfo: %s", ESP.getResetInfo().c_str());
  SIMPLE_INFO("ResetReason: %s", ESP.getResetReason().c_str());
  INFO_HEAP();

  accessory_init();
  accessory_name.value = HOMEKIT_STRING_CPP(hostname);

  arduino_homekit_setup(&config);

  INFO_HEAP();
}

void homekit_loop() {

  arduino_homekit_loop();
  static uint32_t next_heap_millis = 0;
  uint32_t time = millis();
  if (time > next_heap_millis) {
    SIMPLE_INFO("heap: %d, sockets: %d",
        ESP.getFreeHeap(), arduino_homekit_connected_clients_count());
    next_heap_millis = time + 5000;
  }
}

/******************************************************************************************/
// Software SPI Client catching Intex pool controller input

// GPIO Pin
                        
const int CLK     = D7; 
const int LAT     = D6; 
const int DAT_IN  = D5; 
const int DAT_OUT = D0; //emulate button press      
const int BUZ     = D2; //out=buzzer beep, low=shut off; in=buzzer works normal, 

#define GPIO_IN ((volatile uint32_t*) 0x60000318)     //GPIO Read Register
#define GPIO_OUT ((volatile uint32_t*) 0x60000300)    //GPIO Write Register

//16-Bit Shift Register, 
volatile uint8_t clkCount = 0;
volatile uint16_t buf = 0;   

char digit[5] = "????";
int  lstTemp;                 //last valid display reading
int  curTempTmp;              //current temperature candidate
bool curTempTmpValid = false; //current temperature candidate is valid / has not timed out
int  curTemp = 15;            //current temperature
int  setTemp = 25;            //target temperature
bool BuzzerEnabled = false;


enum ButtonT {
  BTN_POWER  = 0,
  BTN_UP     = 1,
  BTN_DOWN   = 2,
  BTN_FILTER = 3,
  BTN_HEATER = 4,
  BTN_BUBBLE = 5,
  BTN_FC     = 6
};

volatile bool    btnRequest[6] = {};
volatile uint8_t btnCount[6] = {};
const int btnCycles  = 10;
bool btnPulse = false;

// Prototypen
void writeButton(ButtonT button);
void handleButton(ButtonT button);

//Just clock the data bits into the buffer
void ICACHE_RAM_ATTR SPI_handleClock() {  
  clkCount++;
  buf = buf << 1;                                 //Shift buffer along
  if (bitRead(*GPIO_IN, DAT_IN) == 1) bitSet(buf, 0); //Flip data bit in buffer if needed.
}
/*
void ICACHE_RAM_ATTR SPI_handleClock() {  
  if (digitalRead(DAT_IN)==HIGH) {
    buf = buf << 1;  
    buf++;
  } else {
    buf = buf << 1;  
  }
  clkCount++;
}
*/

//
void ICACHE_RAM_ATTR SPI_handleLatch() {

      if (clkCount == 16) {
        //Valid if 16 clock cycles detected since last latch
        if (false);
        else if (simulateButtonPress());
        else if (bitRead(buf, 6) == 0)  readSegment(0);
        else if (bitRead(buf, 5) == 0)  readSegment(1);
        else if (bitRead(buf, 11)== 0)  readSegment(2);
        else if (bitRead(buf, 2) == 0)  readSegment(3);
        else if (bitRead(buf, 14)== 0)  readLEDStates();
      }
      // reset buffer
      buf = 0;
      clkCount = 0;
}

void readSegment(int seg) {
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
  if (seg == 3) classifyTemperatur();
}

const int reqCycles = 90;
int dispCycles = reqCycles; //non blank display cycles

void classifyTemperatur() {
  if (digit[0] != ' ') { // non blank display
    // remember the last valid temp reading
    lstTemp = atoi(digit);
    if (--dispCycles < 0) {
      // temperature, that is not followed by an empty display for 90 (>82) cycles, is the current temperature
      if (curTempTmpValid && curTemp != curTempTmp) {
        curTemp = curTempTmp;
        controller_current_temperature_changed_event(curTemp);      
      }
      dispCycles = reqCycles;
      curTempTmp = lstTemp; 
      curTempTmpValid = true;
    }
  } else { // blank display during blinking
    // temperature before an blank display is the target temperature
    if (setTemp != lstTemp) {
      setTemp = lstTemp;
      controller_target_temperature_changed_event(setTemp);
    }
    dispCycles = reqCycles;
    curTempTmpValid = false;
  }
}

enum Led {
  LED_POWER        =0,
  LED_BUBBLE       =1,
  LED_HEATER_GREEN =2,
  LED_HEATER_RED   =3,
  LED_FILTER       =4  
};
uint8_t ledStates;

void readLEDStates() {
    uint8_t ls = ledStates;
    ledStates = 0;

    if (bitRead(buf, 0 ) == 0) bitSet(ledStates, LED_POWER);
    if (bitRead(buf, 10) == 0) bitSet(ledStates, LED_BUBBLE);
    if (bitRead(buf, 9 ) == 0) bitSet(ledStates, LED_HEATER_GREEN);
    if (bitRead(buf, 7 ) == 0) bitSet(ledStates, LED_HEATER_RED);
    if (bitRead(buf, 12) == 0) bitSet(ledStates, LED_FILTER); 

    if (ls != ledStates) {
      // changed, use or to what
      ls = ls ^ ledStates;
      if (bitRead(ls, LED_POWER) != 0) controller_power_state_changed_event();
      if (bitRead(ls, LED_FILTER) != 0) controller_pump_state_changed_event();
      if ((bitRead(ls, LED_HEATER_GREEN) ^ bitRead(ls, LED_HEATER_RED)) != 0) controller_target_heating_state_changed_event();
      if (bitRead(ls, LED_HEATER_RED) != 0) controller_current_heating_state_changed_event();
    }
}

bool simulateButtonPress() {
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

void writeButton(ButtonT button) {
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

void SPI_begin() {
  //Configure shift register
  pinMode(CLK, INPUT);
  pinMode(LAT, INPUT);
  pinMode(DAT_IN, INPUT);
  pinMode(DAT_OUT, OUTPUT);
  digitalWrite(DAT_OUT, 1); //default high
  switchBuzz(BuzzerEnabled);
  
  attachInterrupt(digitalPinToInterrupt(CLK), SPI_handleClock, RISING);
  attachInterrupt(digitalPinToInterrupt(LAT), SPI_handleLatch, RISING);
}

void SPI_end() {
  detachInterrupt(digitalPinToInterrupt(CLK));
  detachInterrupt(digitalPinToInterrupt(LAT));
}




//---------------------------------------------------------------
// highlevel pool controller methods
//---------------------------------------------------------------
typedef enum {
    EVT_POWER_STATE_CHANGED,
    EVT_PUMP_STATE_CHANGED,
    EVT_CURRENT_TEMPERATURE_CHANGED,
    EVT_TARGET_TEMPERATURE_CHANGED,
    EVT_TARGET_HEATING_STATE_CHANGED,
    EVT_CURRENT_HEATING_STATE_CHANGED
} EventType;
  
typedef struct {
  EventType evt ;
  bool state; 
  int temp;
} EventT;

ArduinoQueue<EventT> EventQueue(100);
void PrintEvent( EventT e );

void PrintEvent( EventT e ) {
    switch (e.evt) {
      case EVT_POWER_STATE_CHANGED:
        Serial.printf("EVT_POWER_STATE_CHANGED %d\n", e.state);
        break;
      case EVT_PUMP_STATE_CHANGED: 
        Serial.printf("EVT_PUMP_STATE_CHANGED %d\n", e.state);
        break;
      case EVT_CURRENT_TEMPERATURE_CHANGED: 
        Serial.printf("EVT_CURRENT_TEMPERATURE_CHANGED %d\n", e.temp);
        break;
      case EVT_TARGET_TEMPERATURE_CHANGED:
        Serial.printf("EVT_TARGET_TEMPERATURE_CHANGED %d\n", e.temp);
        break;
      case EVT_TARGET_HEATING_STATE_CHANGED: 
        Serial.printf("EVT_TARGET_HEATING_STATE_CHANGED %d\n", e.state);
        break;
      case EVT_CURRENT_HEATING_STATE_CHANGED:
        Serial.printf("EVT_CURRENT_HEATING_STATE_CHANGED %d\n", e.state);
        break;
    }  
}

// controller: power_state_set, power_state_get, power_state_changed_event
void controller_power_state_set(bool newValue) {
  int retry = 1;
  while (controller_power_state_get() != newValue) {
    writeButton(BTN_POWER);
    delay(500);
    if (--retry < 0) return;
  }
}
bool controller_power_state_get() {
  return (bitRead(ledStates, LED_POWER) == 1);
}
void controller_power_state_changed_event() {
  EventT e;
  e.evt = EVT_POWER_STATE_CHANGED;
  e.state = controller_power_state_get();
  e.temp = 0.0;
  EventQueue.enqueue(e);
}

// controller: pump_state_set, pump_state_get, pump_state_changed_event
void controller_pump_state_set(bool newValue) {
  int retry = 1;
  while (controller_pump_state_get() != newValue) {
    writeButton(BTN_FILTER);
    delay(500);
    if (--retry < 0) return;
  }
}
bool controller_pump_state_get() {
  return (bitRead(ledStates, LED_FILTER) == 1);
}
void controller_pump_state_changed_event() {
  EventT e;
  e.evt = EVT_PUMP_STATE_CHANGED;
  e.state = controller_pump_state_get();
  e.temp = 0.0;
  EventQueue.enqueue(e);
}

// controller: current_heating_state_get, current_heating_state_changed_event
bool controller_current_heating_state_get() {
  return (bitRead(ledStates, LED_HEATER_RED) == 1);
}
void controller_current_heating_state_changed_event() {
  EventT e;
  e.evt = EVT_CURRENT_HEATING_STATE_CHANGED;
  e.state = controller_current_heating_state_get();
  e.temp = 0.0;
  EventQueue.enqueue(e);
}

// controller: target_heating_state_set, target_heating_state_get, target_heating_state_changed_event
bool controller_target_heating_state_get() {
  return (bitRead(ledStates, LED_HEATER_GREEN) == 1 || bitRead(ledStates, LED_HEATER_RED) == 1);
}
void controller_target_heating_state_set(bool newValue) {
  int retry = 1;
  while (controller_target_heating_state_get() != newValue) {
    writeButton(BTN_HEATER);
    delay(500);
    if (--retry < 0) return;
  }
}
void controller_target_heating_state_changed_event() {
  EventT e;
  e.evt = EVT_TARGET_HEATING_STATE_CHANGED;
  e.state = controller_target_heating_state_get();
  e.temp = 0.0;
  EventQueue.enqueue(e);
}

// controller: current_temperature_get, current_temperature_changed_event
int controller_current_temperature_get() {
  return curTemp;
}
void controller_current_temperature_changed_event(int temp) {
  EventT e;
  e.evt = EVT_CURRENT_TEMPERATURE_CHANGED;
  e.state = false;
  e.temp = temp;
  EventQueue.enqueue(e);
}

// controller: target_temperature_get,set,changed_event
void controller_target_temperature_set(int newValue) {
  int retry = 20;
  // only works if power is on
  if (controller_power_state_get()) {
    while (controller_target_temperature_get() != newValue) {
      if (controller_target_temperature_get() > newValue) {
        writeButton(BTN_DOWN);
      } else {
        writeButton(BTN_UP);
      }
      delay(600);
      if (--retry < 0) return;
    }
  }
}
int controller_target_temperature_get() {
  return setTemp;
}
void controller_target_temperature_changed_event(int temp) {
  // called inside ISR, be fast!
  EventT e;
  e.evt = EVT_TARGET_TEMPERATURE_CHANGED;
  e.state = false;
  e.temp = temp;
  EventQueue.enqueue(e);
}

void controller_loop() {
  //handle events from controller like user interaction, current temp changes, heater changes

  if (!EventQueue.isEmpty()) {
    EventT e = EventQueue.dequeue();
    PrintEvent(e);
    switch (e.evt) {
      case EVT_POWER_STATE_CHANGED:
        homekit_power_on_set(e.state);
        break;
      case EVT_PUMP_STATE_CHANGED: 
        homekit_pump_on_set(e.state);
        break;
      case EVT_CURRENT_TEMPERATURE_CHANGED: 
        homekit_current_temperature_set(float(e.temp));
        break;
      case EVT_TARGET_TEMPERATURE_CHANGED:
        homekit_target_temperature_set(float(e.temp));
        break;
      case EVT_TARGET_HEATING_STATE_CHANGED: 
        homekit_target_heating_cooling_state_set(e.state);
        break;
      case EVT_CURRENT_HEATING_STATE_CHANGED:
        homekit_current_heating_cooling_state_set(e.state);
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
" <metax http-equiv='refresh' content='5' >"
" <meta charset='utf-8'>"
" <meta name='viewport' content='width=device-width,initial-scale=1'/>"
"</head>"
"<body>"
" <h1>Whirlpool Controller</h1>"
"Water temperature:  %d<br>"
"Target temperature: %d"
"&nbsp;&nbsp;"
"<a href='/up'>&nbsp;&uArr;&nbsp;</a>"
"&nbsp;"
"<a href='/down'>&nbsp;&dArr;&nbsp;</a>"
"<br>"
"<br>"
"<a href='/power'>Power</a>: %s <br> "
"<a href='/filter'>Filter</a>: %s <br> "
"<a href='/heater'>Heater</a>: %s <br> "
"<a href='/bubble'>Bubble</a>: %s <br> "
"<br>"
"<a href='/buzz%s'>Buzzer</a>: %s <br> "
"<a href='/homekit-reset'>HomeKit Reset</a><br> "
"<br>"
"<a href='/'>refresh</a><br> "
"</body>"
"</html>";

char message[1000];
void handleStatus() {
  
  snprintf(message, 1000, htmlTemplate, 
    hostname,
    curTemp, 
    setTemp, 
    (bitRead(ledStates, LED_POWER) == 1)?"ON" : "OFF",
    (bitRead(ledStates, LED_FILTER) == 1)?"ON" : "OFF",
    (bitRead(ledStates, LED_HEATER_GREEN) == 1)?"GREEN" : (bitRead(ledStates, LED_HEATER_RED) == 1)?"RED" : "OFF",
    (bitRead(ledStates, LED_BUBBLE) == 1)?"ON" : "OFF",
    (BuzzerEnabled?"0":"1"), (BuzzerEnabled?"ON":"OFF")
  );
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  server.send(200, "text/html", message);
  Serial.printf("strlen(message): %d\n",strlen(message));
};

void handleButton(ButtonT button) {
  writeButton(button);
  returnToStatus();
};

void handleBuzz(bool bOn) {
  switchBuzz( bOn );
  buzzSignal( (bOn ? 2 : 1) );
  returnToStatus();
};

void handleSet() {
  if (server.args()>=1) {
    if (server.argName(0).equals("power")) 
      controller_power_state_set(server.arg(0).equals("on"));
      
    else if (server.argName(0).equals("temp")) 
      controller_target_temperature_set(server.arg(0).toInt());

  }
  server.send(200, "text/plain", "ok");
  //returnToStatus();
};

void server_setup() {
  server.on("/", handleStatus);

  server.on("/reboot", []() {
    server.send(200, "text/plain", "Rebooting...");
    delay(1);
    ESP.reset();
  });

  server.on("/homekit-reset", []() {
    server.send(200, "text/plain", "HomeKit reset paring...");
    SPI_end(); // disable interrupts
    homekit_storage_reset();
    delay(1);
    ESP.restart();
  });

  server.on("/power",  []() {handleButton(BTN_POWER);});
  server.on("/up",     []() {handleButton(BTN_UP);});
  server.on("/down",   []() {handleButton(BTN_DOWN);});
  server.on("/filter", []() {handleButton(BTN_FILTER);});
  server.on("/heater", []() {handleButton(BTN_HEATER);});
  server.on("/bubble",[]() {handleButton(BTN_BUBBLE);});
  server.on("/fc",     []() {handleButton(BTN_FC);});
  server.on("/buzz0",     []() {handleBuzz(0);});
  server.on("/buzz1",     []() {handleBuzz(1);});
  server.onNotFound(handleNotFound);

  server.on("/power1",  []() {controller_power_state_set(true);returnToStatus();});
  server.on("/power0",  []() {controller_power_state_set(false);returnToStatus();});
  server.on("/set",  []() {handleSet();});

  server.begin();

}
//******************************************************************************************

void ota_setup() {
  
  ArduinoOTA.setHostname(hostname);
  ArduinoOTA.setPassword(passwordOTA);

  ArduinoOTA.onStart([]() {
    SPI_end(); // disable interrupts, otherwise OTA is failing
    Serial.println("Start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();

}

//******************************************************************************************

void setup() {
  buzzSignal(1); //start setup
  Serial.begin(115200);
  Serial.setRxBufferSize(32);
  Serial.setDebugOutput(false);

  Serial.println("start setup");
  generateHostname();
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.disconnect(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);

  int dotCount=0;
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(250);
    dotCount++;
    if (dotCount >4*60) {
        Serial.println("!");
        ESP.reset();
    }
  }
  
  Serial.println("");
  Serial.println("Intex Spa WiFi Controller");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  Serial.print("Hostname: ");
  Serial.println(hostname);

  buzzSignal(1); //connected

  WiFi.hostname(hostname);
  
  ota_setup();
  
  server_setup();
  
  homekit_setup();

  buzzSignal(1); // homekit setup

  SPI_begin();
}

void loop() {
  ArduinoOTA.handle();
  yield();
  arduino_homekit_loop();
  yield();
  server.handleClient();
  yield();
  controller_loop();
  yield();
}

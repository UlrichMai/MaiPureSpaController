/*
 * accessory.c
 * Define the homekit accessories for the pool controller
 *
 *  Created on: 2020-05-16
 *      Author: Ulrich Mai
 */

#include <Arduino.h>
#include <homekit/types.h>
#include <homekit/homekit.h>
#include <homekit/characteristics.h>
#include <stdio.h>
#include <port.h>

extern void controller_power_state_set(bool newValue);
extern bool controller_power_state_get();
extern void controller_power_state_changed_event();
extern void controller_pump_state_set(bool newValue);
extern bool controller_pump_state_get();
extern void controller_pump_state_changed_event();
extern bool controller_current_heating_state_get();
extern void controller_current_heating_state_changed_event();
extern bool controller_target_heating_state_get();
extern void controller_target_heating_state_set(bool newValue);
extern void controller_target_heating_state_changed_event();
extern int controller_current_temperature_get();
extern void controller_current_temperature_changed_event(int temp);
extern void controller_target_temperature_set(int newValue);
extern int controller_target_temperature_get();
extern void controller_target_temperature_changed_event(int temp);

#define ACCESSORY_NAME  ("Pool")
#define ACCESSORY_SN  ("SN_0123456")  //SERIAL_NUMBER
#define ACCESSORY_MANUFACTURER ("UM")
#define ACCESSORY_MODEL  ("SpaController")

homekit_value_t current_temp_get() {
  float t = (float)controller_current_temperature_get();
  printf("current_temp_get(%d)\n",round(t));
  return HOMEKIT_FLOAT(t); 
}
homekit_value_t target_temp_get() {
  float t = (float)controller_target_temperature_get();
  printf("target_temp_get(%d)\n",round(t));
  return HOMEKIT_FLOAT(t);
}
void target_temp_set(homekit_value_t value) {
  if (value.format != homekit_format_float) { 
    printf("Invalid target_temp value format: %d\n", value.format);
    return;
  }
  int t = round(value.float_value);
  controller_target_temperature_set(t);
  printf("target_temp_set(%d)\n",t);
}

homekit_value_t current_heating_state_get() {
  bool s = controller_current_heating_state_get();
  printf("current_heating_state_get(%d)\n",s);
  return HOMEKIT_UINT8((s?1:0)); 
}
homekit_value_t target_heating_state_get() {
  bool s = controller_target_heating_state_get();
  printf("target_heating_state_get(%d)\n",s);
  return HOMEKIT_UINT8((s?1:0)); 
}
void target_heating_state_set(homekit_value_t value) {
  if (value.format != homekit_format_uint8) { 
    printf("Invalid target_heating_state value format: %d\n", value.format);
    return;
  }
  bool s = (value.uint8_value != 0);
  controller_target_heating_state_set(s);
  printf("target_heating_state_set(%d)\n",s);
}

homekit_value_t power_state_get() {
  bool s = controller_power_state_get();
  printf("power_state_get(%d)\n",s);
  return HOMEKIT_BOOL(s);
}
void power_state_set(homekit_value_t value) {
  if (value.format != homekit_format_bool) {
    printf("Invalid power_state value format: %d\n", value.format);
    return;
  }
  bool s = value.bool_value;
  // send power_state to controller
  controller_power_state_set(s);
  printf("power_state_set(%d)\n",s);
}

homekit_value_t pump_state_get() {
  bool s = controller_pump_state_get();
  printf("pump_state_get(%d)\n",s);
  return HOMEKIT_BOOL(s);
}
void pump_state_set(homekit_value_t value) {
  if (value.format != homekit_format_bool) {
    printf("Invalid pump_state value format: %d\n", value.format);
    return;
  }
  bool s = value.bool_value;
  // send pump_state to controller
  controller_pump_state_set(s);
  printf("pump_state_set(%d)\n",s);
}

//---------------------------------------------------------------

homekit_characteristic_t accessory_name = HOMEKIT_CHARACTERISTIC_(NAME, ACCESSORY_NAME);
homekit_characteristic_t serial_number = HOMEKIT_CHARACTERISTIC_(SERIAL_NUMBER, ACCESSORY_SN);

homekit_characteristic_t current_temperature           = HOMEKIT_CHARACTERISTIC_(CURRENT_TEMPERATURE, 19.0, .min_step  = (float[]) {1.0},  .getter=current_temp_get );
homekit_characteristic_t target_temperature            = HOMEKIT_CHARACTERISTIC_(TARGET_TEMPERATURE,  31.0, .min_value = (float[]) {10.0}, .max_value = (float[]) {40.0}, .min_step = (float[]) {1.0}, .getter=target_temp_get, .setter=target_temp_set );
homekit_characteristic_t current_heating_cooling_state = HOMEKIT_CHARACTERISTIC_(CURRENT_HEATING_COOLING_STATE, 0, .valid_values = {.count = 2, .values = (uint8_t[]) {0,1} }, .getter=current_heating_state_get );
homekit_characteristic_t target_heating_cooling_state  = HOMEKIT_CHARACTERISTIC_(TARGET_HEATING_COOLING_STATE,  0, .valid_values = {.count = 2, .values = (uint8_t[]) {0,1} }, .getter=target_heating_state_get, .setter=target_heating_state_set  );
//homekit_characteristic_t target_heating_cooling_state  = HOMEKIT_CHARACTERISTIC_(TARGET_HEATING_COOLING_STATE,  0, .valid_values = {.count = 4, .values = (uint8_t[]) {0,1,2,3} }, .getter=target_heating_state_get, .setter=target_heating_state_set  );
homekit_characteristic_t power_on                      = HOMEKIT_CHARACTERISTIC_(ON, false, .getter=power_state_get, .setter=power_state_set );
homekit_characteristic_t pump_on                       = HOMEKIT_CHARACTERISTIC_(ON, false, .getter=pump_state_get, .setter=pump_state_set );

void homekit_target_temperature_set(float newValue) {
  target_temperature.value = HOMEKIT_FLOAT(newValue);
  homekit_characteristic_notify(&target_temperature, target_temperature.value);
}

void homekit_current_temperature_set(float newValue) {
  current_temperature.value = HOMEKIT_FLOAT(newValue);
  homekit_characteristic_notify(&current_temperature, current_temperature.value);
}

void homekit_current_heating_cooling_state_set(bool newValue) {
  current_heating_cooling_state.value = HOMEKIT_UINT8((newValue?1:0));
  homekit_characteristic_notify(&current_heating_cooling_state, current_heating_cooling_state.value);
}

void homekit_target_heating_cooling_state_set(bool newValue) {
  target_heating_cooling_state.value = HOMEKIT_UINT8((newValue?1:0));
  homekit_characteristic_notify(&target_heating_cooling_state, target_heating_cooling_state.value);
}

void homekit_power_on_set(bool newValue) {
  power_on.value = HOMEKIT_BOOL(newValue);
  homekit_characteristic_notify(&power_on, power_on.value);
}

void homekit_pump_on_set(bool newValue) {
  pump_on.value = HOMEKIT_BOOL(newValue);
  homekit_characteristic_notify(&pump_on, pump_on.value);
}

void accessory_identify(homekit_value_t _value) {
	printf("accessory identify\n");
}

homekit_accessory_t *accessories[] =
		{
				HOMEKIT_ACCESSORY(
						.id = 1,
						.category = homekit_accessory_category_thermostat,
						.services=(homekit_service_t*[]){
							HOMEKIT_SERVICE(ACCESSORY_INFORMATION,
								.characteristics=(homekit_characteristic_t*[]){
									&accessory_name,
									HOMEKIT_CHARACTERISTIC(MANUFACTURER, ACCESSORY_MANUFACTURER),
									&serial_number,
									HOMEKIT_CHARACTERISTIC(MODEL, ACCESSORY_MODEL),
									HOMEKIT_CHARACTERISTIC(FIRMWARE_REVISION, "0.0.9" ),
									HOMEKIT_CHARACTERISTIC(IDENTIFY, accessory_identify),
									NULL
								}),
             
              HOMEKIT_SERVICE(THERMOSTAT, .primary=true,
                .characteristics=(homekit_characteristic_t*[]) {
                  HOMEKIT_CHARACTERISTIC(NAME, "Pool Heater"),
                  //HOMEKIT_CHARACTERISTIC(ACTIVE, false),
                  &current_temperature,
                  &target_temperature,
                  &current_heating_cooling_state,
                  &target_heating_cooling_state,
                  HOMEKIT_CHARACTERISTIC(TEMPERATURE_DISPLAY_UNITS, 0), //Celsius

                  //HOMEKIT_CHARACTERISTIC(CURRENT_RELATIVE_HUMIDITY, 0),
                  //HOMEKIT_CHARACTERISTIC(TARGET_RELATIVE_HUMIDITY, 0),
                  //HOMEKIT_CHARACTERISTIC(COOLING_THRESHOLD_TEMPERATURE, 0),
                  //HOMEKIT_CHARACTERISTIC(HEATING_THRESHOLD_TEMPERATURE, 0),
                  NULL
                }),

              HOMEKIT_SERVICE(SWITCH, .primary=false,
                .characteristics=(homekit_characteristic_t*[]){
                  HOMEKIT_CHARACTERISTIC(NAME, "Pool Power"),
                  &power_on,
                  NULL
                }),

              HOMEKIT_SERVICE(SWITCH, .primary=false,
                .characteristics=(homekit_characteristic_t*[]){
                  HOMEKIT_CHARACTERISTIC(NAME, "Pool Pump"),
                  &pump_on,
                  NULL
                }),
							NULL
						}),
				NULL
		};

homekit_server_config_t config = {
		.accessories = accessories,
		.password = "111-11-111",
		//.on_event = on_homekit_event,
		.setupId = "ABCD"
};

void accessory_init() {

}

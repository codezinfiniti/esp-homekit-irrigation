/*
 * Example of using esp-homekit library to control
 * Irrigation, by providing abilit to swith on and off a series of relays
 *
 */

#define DEVICE_MANUFACTURER "maccoylton"
#define DEVICE_NAME "Irrigation"
#define DEVICE_MODEL "2"
#define DEVICE_SERIAL "12345678"
#define FW_VERSION "1.0"

#include <stdio.h>
#include <espressif/esp_wifi.h>
#include <espressif/esp_sta.h>
#include <espressif/esp_common.h>
#include <esp/uart.h>
#include <esp8266.h>
#include <FreeRTOS.h>
#include <task.h>

#include <homekit/homekit.h>
#include <homekit/characteristics.h>
#include <wifi_config.h>


#include <adv_button.h>
#include <led_codes.h>
#include <udplogger.h>
#include <custom_characteristics.h>
#include <shared_functions.h>

// add this section to make your device OTA capable
// create the extra characteristic &ota_trigger, at the end of the primary service (before the NULL)
// it can be used in Eve, which will show it, where Home does not
// and apply the four other parameters in the accessories_information section

#include <ota-api.h>

#define SAVE_DELAY 2000
#define SAFETY_DELAY 1800000

ETSTimer safety_timer;


void valve_1_active_callback(homekit_characteristic_t *_ch, homekit_value_t on, void *context);


homekit_characteristic_t wifi_check_interval   = HOMEKIT_CHARACTERISTIC_(CUSTOM_WIFI_CHECK_INTERVAL, 10, .setter=wifi_check_interval_set);
/* checks the wifi is connected and flashes status led to indicated connected */
homekit_characteristic_t task_stats   = HOMEKIT_CHARACTERISTIC_(CUSTOM_TASK_STATS, false , .setter=task_stats_set);
homekit_characteristic_t wifi_reset   = HOMEKIT_CHARACTERISTIC_(CUSTOM_WIFI_RESET, false, .setter=wifi_reset_set);
homekit_characteristic_t ota_beta     = HOMEKIT_CHARACTERISTIC_(CUSTOM_OTA_BETA, false, .setter=ota_beta_set);
homekit_characteristic_t lcm_beta    = HOMEKIT_CHARACTERISTIC_(CUSTOM_LCM_BETA, false, .setter=lcm_beta_set);

homekit_characteristic_t ota_trigger  = API_OTA_TRIGGER;
homekit_characteristic_t name         = HOMEKIT_CHARACTERISTIC_(NAME, DEVICE_NAME);
homekit_characteristic_t manufacturer = HOMEKIT_CHARACTERISTIC_(MANUFACTURER,  DEVICE_MANUFACTURER);
homekit_characteristic_t serial       = HOMEKIT_CHARACTERISTIC_(SERIAL_NUMBER, DEVICE_SERIAL);
homekit_characteristic_t model        = HOMEKIT_CHARACTERISTIC_(MODEL,         DEVICE_MODEL);
homekit_characteristic_t revision     = HOMEKIT_CHARACTERISTIC_(FIRMWARE_REVISION,  FW_VERSION);


homekit_characteristic_t valve_1_active = HOMEKIT_CHARACTERISTIC_(ACTIVE, false, .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(valve_1_active_callback) );
homekit_characteristic_t valve_1_in_use = HOMEKIT_CHARACTERISTIC_(IN_USE, 0);
homekit_characteristic_t valve_1_valve_type = HOMEKIT_CHARACTERISTIC_(VALVE_TYPE, 1);


// The GPIO pin that is connected to the first relay on the irrigation controller.
const int VALVE_1_GPIO = 4;
// The GPIO pin that is connected to the second relay on the irrigation controller.
const int VALVE_2_GPIO = 5;
// The GPIO pin that is connected to the LED on the irrigation controller.
const int LED_GPIO = 2;
// The GPIO pin that is oconnected to the button on the irrigation controller.
const int BUTTON_GPIO = 0;

int led_off_value=1; /* global varibale to support LEDs set to 0 where the LED is connected to GND, 1 where +3.3v */

const int status_led_gpio = 2; /*set the gloabl variable for the led to be used for showing status */

void safety_timer_function(){

    printf ("%s:  Switching off water\n", __func__);
    valve_1_in_use.value.bool_value = 0;
    valve_1_active.value.bool_value = false;
    relay_write(false, VALVE_1_GPIO);
    /* relay requies low for on */
    led_write(false, LED_GPIO);
    homekit_characteristic_notify(&valve_1_in_use, valve_1_in_use.value);
    homekit_characteristic_notify(&valve_1_active, valve_1_active.value);
    sdk_os_timer_arm (&save_timer, SAVE_DELAY, 0 );
}


void button_single_press_callback(uint8_t gpio, void* args, uint8_t param) {
    
    printf("Button event single press on GPIO : %d\n", gpio);
    printf("Toggling switch one\n");
    valve_1_active.value.bool_value = !valve_1_active.value.bool_value;
    if (valve_1_active.value.bool_value == true){
        valve_1_in_use.value.bool_value = 1;
        sdk_os_timer_arm (&save_timer, SAVE_DELAY, 0 );
    } else {
        valve_1_in_use.value.bool_value = 0;
        sdk_os_timer_disarm (&save_timer);
    }
    relay_write(valve_1_active.value.bool_value, VALVE_1_GPIO);
    homekit_characteristic_notify(&valve_1_active, valve_1_active.value);
    sdk_os_timer_arm (&save_timer, SAVE_DELAY, 0 );

}


void button_double_press_callback(uint8_t gpio, void* args, uint8_t param) {
    
    printf("Button event long press on GPIO : %d\n", gpio);
}

void button_long_press_callback(uint8_t gpio, void* args, uint8_t param) {
    
    printf("Button event long press on GPIO : %d\n", gpio);
}

void button_very_long_press_callback(uint8_t gpio, void* args, uint8_t param) {
    
    printf("Button event very long press on GPIO : %d\n", gpio);
    reset_configuration();
    
}

void gpio_init() {

    gpio_enable(LED_GPIO, GPIO_OUTPUT);
    led_write(false, LED_GPIO);

    gpio_enable(VALVE_1_GPIO, GPIO_OUTPUT);
    relay_write(!valve_1_active.value.bool_value, VALVE_1_GPIO);
    /* relay requires 1 for off and 0 for on */
    
    adv_button_set_evaluate_delay(10);
    
    /* GPIO for button, pull-up resistor, inverted */
    printf("Initialising buttons\n");
    adv_button_create(BUTTON_GPIO, true, false);
    adv_button_register_callback_fn(BUTTON_GPIO, button_single_press_callback, SINGLEPRESS_TYPE, NULL, 0);
    adv_button_register_callback_fn(BUTTON_GPIO, button_double_press_callback, DOUBLEPRESS_TYPE, NULL, 0);
    adv_button_register_callback_fn(BUTTON_GPIO, button_long_press_callback, LONGPRESS_TYPE, NULL, 0);
    adv_button_register_callback_fn(BUTTON_GPIO, button_very_long_press_callback, VERYLONGPRESS_TYPE, NULL, 0);

}


void valve_1_active_callback(homekit_characteristic_t *_ch, homekit_value_t on, void *context) {
    
    printf ("%s: Value: %d\n", __func__,valve_1_active.value.bool_value);
    if (valve_1_active.value.bool_value == true){
        valve_1_in_use.value.bool_value = 1;
        sdk_os_timer_arm (&save_timer, SAVE_DELAY, 0 );
    } else {
        valve_1_in_use.value.bool_value = 0;
        sdk_os_timer_disarm (&save_timer);
    }
    relay_write(!valve_1_active.value.bool_value, VALVE_1_GPIO);
    /* relay requies low for on */
    led_write(valve_1_active.value.bool_value, LED_GPIO);
    homekit_characteristic_notify(&valve_1_in_use, valve_1_in_use.value);
    sdk_os_timer_arm (&safety_timer, SAVE_DELAY, 0 );
}

homekit_accessory_t *accessories[] = {
    HOMEKIT_ACCESSORY(.id=1, .category=homekit_accessory_category_sprinkler, .services=(homekit_service_t*[]){
        HOMEKIT_SERVICE(ACCESSORY_INFORMATION, .characteristics=(homekit_characteristic_t*[]){
            &name,
            &manufacturer,
            &serial,
            &model,
            &revision,
            HOMEKIT_CHARACTERISTIC(IDENTIFY, identify),
            NULL
        }),
        

        HOMEKIT_SERVICE(VALVE, .primary=true, .characteristics=(homekit_characteristic_t*[]){
            HOMEKIT_CHARACTERISTIC(NAME, "Valve 1"),
            &valve_1_active,
            &valve_1_in_use,
            &valve_1_valve_type,
            &ota_trigger,
            &wifi_reset,
            &ota_beta,
            &lcm_beta,
            &task_stats,
            &wifi_check_interval,
            NULL
        }),
        NULL
    }),
    NULL
};

homekit_server_config_t config = {
    .accessories = accessories,
    .password = "111-11-111",
    .setupId = "1234",
    .on_event = on_homekit_event
};


void recover_from_reset (int reason){
    /* called if we restarted abnormally */
    printf ("%s: reason %d\n", __func__, reason);
}

void save_characteristics ( ){
    
    printf ("%s:\n", __func__);
    save_characteristic_to_flash(&wifi_check_interval, wifi_check_interval.value);
    save_characteristic_to_flash(&valve_1_active, valve_1_active.value);
    save_characteristic_to_flash(&valve_1_in_use, valve_1_in_use.value);
}


void accessory_init_not_paired (void) {
    /* initalise anything you don't want started until wifi and homekit imitialisation is confirmed, but not paired */
    
}

void accessory_init (void ){
    /* initalise anything you don't want started until wifi and pairing is confirmed */
    load_characteristic_from_flash(&wifi_check_interval);
    save_characteristic_to_flash(&valve_1_active, valve_1_active.value);
    save_characteristic_to_flash(&valve_1_in_use, valve_1_in_use.value);
    homekit_characteristic_notify(&wifi_check_interval, wifi_check_interval.value);
    homekit_characteristic_notify(&valve_1_active, valve_1_active.value);
    homekit_characteristic_notify(&valve_1_in_use, valve_1_in_use.value);
    sdk_os_timer_setfn(&safety_timer, safety_timer_function, NULL);
}

void user_init(void) {
    
    standard_init (&name, &manufacturer, &model, &serial, &revision);

    gpio_init();

    wifi_config_init(DEVICE_NAME, NULL, on_wifi_ready);
    
}

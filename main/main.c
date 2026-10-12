#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "mqtt_client.h"

#include "secrets.h"

#define RED_LED_PIN 2
#define BUTTON_PIN 15
#define DEBOUNCE_TIMOUT 10000 //us
#define LOADING_ANIMATION_LATENCY 50000 //us

esp_mqtt_client_handle_t mqtt_client;
esp_timer_handle_t debounce_timer;
esp_timer_handle_t loading_timer;

char mqtt_tx_buffer;
bool mqtt_client_subscribed = false;
int led = 0;

static IRAM_ATTR void button_interrupt_handler(void *args) {
    ESP_ERROR_CHECK(gpio_intr_disable(BUTTON_PIN));
    ESP_ERROR_CHECK(esp_timer_start_once(debounce_timer, DEBOUNCE_TIMOUT));
}

void debounce_timer_callback(void *args) {
    ESP_LOGI("LOG", "Button pressed ");
    if (mqtt_client_subscribed) {
        ESP_LOGI("LOG", "Valid button press");
        mqtt_tx_buffer = 1 - gpio_get_level(BUTTON_PIN) + '0';
        esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC, &mqtt_tx_buffer, 1, 2, 1); //blocking publish
    }
    ESP_ERROR_CHECK(gpio_intr_enable(BUTTON_PIN));
}

void loading_timer_callback(void *args) {
    ESP_LOGI("LOG", "Loading timer triggered");
    led = (led==0) ? 1 : 0;        
    gpio_set_level(RED_LED_PIN, led);
}

void mqtt_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    ESP_LOGI("MQTT EVENT", "MQTT event received: base=%s, id=%" PRId32, event_base, event_id);
    
    esp_mqtt_event_handle_t event = event_data;
    
    char topic[event->topic_len + 1];
    memcpy(topic, event->topic, event->topic_len);
    topic[event->topic_len] = '\0'; // string termination

    char data[event->data_len + 1];
    memcpy(data, event->data, event->data_len);
    data[event->data_len] = '\0'; // string termination

    int msg_id;

    switch (event_id)
    {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI("MQTT", "Connected to MQTT Broker!");
            msg_id = esp_mqtt_client_subscribe_single(mqtt_client, MQTT_TOPIC, 2);
            if (msg_id != -1 && msg_id != -2) {
                esp_timer_stop(loading_timer);
                esp_timer_delete(loading_timer);
                
                led = 0;
                gpio_set_level(RED_LED_PIN, led);

                ESP_LOGI("MQTT", "sent subscribe successful, msg_id=%d", msg_id);
                mqtt_client_subscribed = true;
            } else {
                ESP_LOGE("MQTT", "SENT SUBSCRIBE UNSUCCESSFUL, REBOOTING, msg_id=%d", msg_id);
                esp_restart();
            }
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI("MQTT", "Subscribed to topic");
            break;

        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI("MQTT", "Unsubscribed from topic %s", topic);
            if (strcmp(topic, MQTT_TOPIC) == 0) {
                mqtt_client_subscribed = false;
            }
            break;

        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI("MQTT", "Published %s to topic %s",data, topic);
            break;
        
        case MQTT_EVENT_DATA:
            ESP_LOGI("MQTT", "Recieved %s from topic %s",data, topic);
            if (strcmp(topic, MQTT_TOPIC) == 0) {
                led = data[0] - '0';
                gpio_set_level(RED_LED_PIN, led);
            }
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGE("MQTT", "DISCONECTED FROM MQTT BROKER, REBOOTING");
            esp_restart();
            break;

        default:
            break;
    }
}

void mqtt_init() {
    esp_mqtt_client_config_t mqtt_client_config = {
        .broker.address.uri = MQTT_BROKER_URI
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_client_config);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, mqtt_client));
    ESP_LOGI("MQTT", "MQTT event handler registered");
    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt_client));
    ESP_LOGI("MQTT", "mqtt_init() finished");
}

void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    ESP_LOGI("WIFI EVENT", "WIFI event received: base=%s, id=%" PRId32, event_base, event_id);

    switch (event_id)
    {
        case WIFI_EVENT_STA_START:
            ESP_LOGI("WIFI", "Connecting to wifi...");
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI("WIFI", "Connected to wifi!");
            break;
        case WIFI_EVENT_STA_DISCONNECTED: 
            ESP_LOGE("WIFI", "LOST WIFI CONNECTION, REBOOTING");
            esp_restart();
            break;
        default:
            break;
    }

}

void ip_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    ESP_LOGI("IP EVENT", "IP event received: base=%s, id=%" PRId32, event_base, event_id);

    switch (event_id)
    {
        case IP_EVENT_STA_GOT_IP:
            ESP_LOGI("IP", "Got IP!");
            mqtt_init();
            break;
        case IP_EVENT_STA_LOST_IP:
            ESP_LOGE("IP", "LOST IP, REBOOTING");
            esp_restart();
            break;
        default:
            break;
    }

}

void wifi_init_sta() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    ESP_LOGI("WIFI", "Created STA interface");

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wifi_init_config);
    ESP_LOGI("WIFI", "WIFI initializated");

    esp_event_handler_instance_t instance_wifi_event_handler;
    esp_event_handler_instance_t instance_ip_event_handler;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_wifi_event_handler));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler, NULL, &instance_ip_event_handler));
    ESP_LOGI("WIFI", "WIFI and IP event handlers registered");


    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    
    ESP_LOGI("WIFI", "wifi_init_sta() finished");
}


void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_NONE);
    //esp_log_level_set("*", ESP_LOG_INFO);

    esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGW("BOOT", "Reset reason: %d", (int)reason);

    //GPIO CONFIGURATION
    gpio_config_t leds_gpio_config = {
        .pin_bit_mask = (1ULL<<RED_LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&leds_gpio_config);
    ESP_LOGI("LOG", "Led GPIO configured");

    gpio_config_t button_gpio_config = {
        .pin_bit_mask = (1ULL << BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLDOWN_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE
    };
    gpio_config(&button_gpio_config);
    ESP_LOGI("LOG", "Button GPIO configured");

    gpio_install_isr_service(ESP_INTR_FLAG_LOWMED);
    gpio_isr_handler_add(BUTTON_PIN, button_interrupt_handler, (void *)BUTTON_PIN);
    ESP_LOGI("LOG", "Button interrupt setted");

    //TIMER CONFIGURATION
    esp_timer_create_args_t loading_timer_args = {
        .callback = &loading_timer_callback,
        .name = "loading timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&loading_timer_args, &loading_timer));
    ESP_LOGI("LOG", "Loading timer created");
    ESP_ERROR_CHECK(esp_timer_start_periodic(loading_timer, LOADING_ANIMATION_LATENCY));
    
    esp_timer_create_args_t debounce_timer_args = {
        .callback = &debounce_timer_callback,
        .name = "Debounce timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&debounce_timer_args, &debounce_timer));
    ESP_LOGI("LOG", "Debounce timer created");


    nvs_flash_init();
    vTaskDelay(100 / portTICK_PERIOD_MS);
    //START WIFI CONNECTION
    wifi_init_sta();
}

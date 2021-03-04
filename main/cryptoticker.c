// ESP32-C3 BTC ticker by BugerDread

#include <stdio.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "protocol_examples_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include <max7219.h>

#define NO_DATA_TIMEOUT_SEC 25
#define SUBSCRIBE_TIMEOUT_SEC 5

#define HOST SPI2_HOST
#define PIN_NUM_MOSI 7
#define PIN_NUM_CLK  6
#define PIN_NUM_CS   10
#define DISP_DIGITS 8
#define CONFIG_WEBSOCKET_URI "wss://api.bitfinex.com/ws/2"

typedef enum {WS_DISCONNECTED, WS_CONNECTED, WS_WAIT_4_INFO, WS_GOT_INFO, WS_SENT_SUBSCRIBE, WS_SUBSCRIBED} cstate_t;

typedef struct {
    max7219_t * p_maxdevice;
    char * p_dbuffer;
    SemaphoreHandle_t * p_semaphore;
} dispparams_t;

typedef struct {
    char * p_dbuffer;
    SemaphoreHandle_t * p_subscribe_sema;
    SemaphoreHandle_t * p_display_sema;
} wsparams_t;

typedef struct {
    cstate_t * p_cstate;
    SemaphoreHandle_t * p_subssema;
    esp_websocket_client_handle_t * p_client;
} cparams_t;

static const char *subscribe_str = "{\"event\":\"subscribe\",\"channel\":\"ticker\",\"symbol\":\"tBTCUSD\"}";

static cstate_t conn_state = WS_DISCONNECTED;
//static TimerHandle_t shutdown_signal_timer;
static SemaphoreHandle_t subscribe_sema, display_sema;

/// this must be GLOBAL, if defined in main before task initialization and the main task is finished the ESP crashes
char *dispbuf;
static const spi_bus_config_t cfg = {
    .mosi_io_num = PIN_NUM_MOSI,
    .miso_io_num = -1,
    .sclk_io_num = PIN_NUM_CLK,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .max_transfer_sz = 0,
    .flags = 0};
max7219_t dev = {
    .cascade_size = 1,
    .digits = DISP_DIGITS,
    .mirrored = true};
esp_websocket_client_handle_t client;
cparams_t cparams;
dispparams_t disppar;
esp_websocket_client_config_t websocket_cfg;
wsparams_t wsparams;

static void jsonparser (esp_websocket_event_data_t *data, wsparams_t * dispparams)
{
    //json parser
    static const char *TAG = "JSON";
    cJSON *rootj = cJSON_ParseWithLength((char *)data->data_ptr, data->data_len);
    if (rootj == NULL) {
        ESP_LOGI(TAG, "Not a json");
    } else {
        //check if its an event info
        cJSON *jstemp = cJSON_GetObjectItem(rootj, "event");
        if (cJSON_IsString(jstemp)) {
            char *ev = jstemp->valuestring;
            ESP_LOGI(TAG, "Event: %s", ev);
            //check events
            if (strcmp(ev, "subscribed") == 0) {
                ESP_LOGI(TAG, "We are now subscribed");
                //max7219_draw_text_7seg(&dev, 0, "API  YES");
                conn_state = WS_SUBSCRIBED;
            } else if ((strcmp(ev, "info") == 0) && (conn_state == WS_DISCONNECTED)) {
                ESP_LOGI(TAG, "Got server info and need to subscribe");
                conn_state = WS_CONNECTED;
                //max7219_draw_text_7seg(&dev, 0, "API     ");
                xSemaphoreGive(*(dispparams->p_subscribe_sema));
            }
        }
        //check if it is an array
        if (cJSON_IsArray(rootj)) {
            //yes
            ESP_LOGI(TAG, "its array");
            cJSON *x = cJSON_GetArrayItem(rootj, 1);
            if (cJSON_IsArray(x)) {
            //the subarray exists
                x = cJSON_GetArrayItem(x, 6);
                if (cJSON_IsNumber(x)) { 
                    ESP_LOGI(TAG, "Bitcoin value: %d", x->valueint);
                    //show it on display
                    sprintf(dispparams->p_dbuffer, "%8d.", x->valueint);
                    //max7219_draw_text_7seg(&dev, 0, buf);
                    //btc_prize = x->valueint;
                    xSemaphoreGive(*(dispparams->p_display_sema));
                }
            }
        }                 
        cJSON_Delete(rootj);
    }
}

void websocket_event_handler(void * anonParams, esp_event_base_t base, long int event_id, void *event_data)
{
    wsparams_t * handler_args = (wsparams_t *) anonParams;
    static const char *TAG = "WEBSOCKET";
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED");
        //conn_state = WS_CONNECTED;
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
        conn_state = WS_DISCONNECTED;
        //max7219_draw_text_7seg(&dev, 0, "DISCNCTD");
        break;
    case WEBSOCKET_EVENT_DATA:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_DATA");
        ESP_LOGI(TAG, "Received opcode=%d", data->op_code);
        if (data->op_code == 0x08 && data->data_len == 2) {
            ESP_LOGW(TAG, "Received closed message with code=%d", 256*data->data_ptr[0] + data->data_ptr[1]);
        } else {
            ESP_LOGI(TAG, "Received=%.*s", data->data_len, (char *)data->data_ptr);
            jsonparser(data, handler_args); 
        }
        ESP_LOGI(TAG, "Total payload length=%d, data_len=%d, current payload offset=%d", data->payload_len, data->data_len, data->payload_offset);
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WEBSOCKET_EVENT_ERROR");
        break;
    }
}


void display_task ( void * anonParams  )

//(dispparams_t * pvParameters )
{
    dispparams_t * pvParameters = (dispparams_t *) anonParams;
    static const char *TAG = "DISPLAY";
    while (true) 
    {
        // Task code goes here - show prize
        //personPtr->age is equivalent to (*personPtr).age
        xSemaphoreTake(*(pvParameters->p_semaphore), portMAX_DELAY);   //wait until semaphored
        ESP_LOGI(TAG, "Got semaphore, price to display"); 
        max7219_draw_text_7seg(pvParameters->p_maxdevice, 0, pvParameters->p_dbuffer);
    }
}

void control_task ( void * anonParams  )
//(cparams_t * p_ctask)
{
    cparams_t * p_ctask = (cparams_t *) anonParams;
    static const char *TAG = "CONTROL";
    while (true) 
    {
        xSemaphoreTake(*(p_ctask->p_subssema), portMAX_DELAY);
        ESP_LOGI(TAG, "Got subscribe semaphore");
        while ((*(p_ctask->p_cstate)) == WS_CONNECTED)
        {   //if subscribe message got lost this will send it every 5s while WS_CONNECTED
            //we need to subscribe
            ESP_LOGI(TAG, "Sending subscribe request: %s", subscribe_str);
            esp_websocket_client_send_text(*(p_ctask->p_client), subscribe_str, strlen(subscribe_str), SUBSCRIBE_TIMEOUT_SEC / portTICK_RATE_MS);
            vTaskDelay(5000 / portTICK_RATE_MS);        //wait 5s
        }
    }
}

void app_main(void)
{
    static const char *TAG = "MAIN";
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("WEBSOCKET_CLIENT", ESP_LOG_DEBUG);
    esp_log_level_set("TRANSPORT_WS", ESP_LOG_DEBUG);
    esp_log_level_set("TRANS_TCP", ESP_LOG_DEBUG);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    //init display
    //configure SPI bus
    spi_bus_initialize(HOST, &cfg, 1);
    //configure MAX7219 device
    max7219_init_desc(&dev, HOST, PIN_NUM_CS);
    max7219_init(&dev);
    max7219_clear(&dev);
    max7219_set_brightness(&dev, CONFIG_DISP_BRIGHTNESS);
    //show some info
    max7219_draw_text_7seg(&dev, 0, "ESP32-C3");
    vTaskDelay(1000 / portTICK_RATE_MS);
    max7219_draw_text_7seg(&dev, 0, "WiFi    ");

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());
    
    max7219_draw_text_7seg(&dev, 0, "WiFi YES");
    
    //prepare and lauch display task
    dispbuf = (char *) malloc((DISP_DIGITS * 2) + 1); //because for example for 8digits there can be also 8 dots (8.8.8.8.8.8.8.8.) plus string temination character
    display_sema = xSemaphoreCreateBinary();    //create display semaphore used to show new BTC price
    disppar.p_maxdevice = &dev;
    disppar.p_dbuffer = dispbuf;
    disppar.p_semaphore = &display_sema;
    TaskHandle_t xDisplayHandle = NULL;
    xTaskCreate(display_task, "display_task", 2048, &disppar, tskIDLE_PRIORITY, &xDisplayHandle ); 
   
    subscribe_sema = xSemaphoreCreateBinary();
    websocket_cfg.uri = CONFIG_WEBSOCKET_URI;
    websocket_cfg.pingpong_timeout_sec = 60;
    wsparams.p_dbuffer = dispbuf;
    wsparams.p_subscribe_sema = &subscribe_sema;
    wsparams.p_display_sema = &display_sema;
    ESP_LOGI(TAG, "Connecting to %s...", websocket_cfg.uri);
    client = esp_websocket_client_init(&websocket_cfg);
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, &wsparams);
    
    //create control task before starting WS client
    TaskHandle_t xContolHandle = NULL;
    cparams.p_cstate = &conn_state;
    cparams.p_subssema = &subscribe_sema;
    cparams.p_client = &client;
    xTaskCreate(control_task, "control_task", 4096, &cparams, tskIDLE_PRIORITY, &xContolHandle);
    
    //start the WS client
    esp_websocket_client_start(client);
    
//    while (true) 
//    {
//        vTaskDelay(1000 / portTICK_RATE_MS);
//    }
}


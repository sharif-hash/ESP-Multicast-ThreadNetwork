#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_cli.h"
#include "esp_openthread_netif_glue.h"
#include "openthread/cli.h"
#include "openthread/instance.h"
#include "openthread/udp.h"
#include "openthread/logging.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h" 

// --- REQUIRED HEADERS ---
#include "esp_vfs_eventfd.h"
#include "esp_openthread_types.h"
#include "esp_ot_config.h"

#define TAG "ot_esp_cli"

// =============================================================
// CONFIGURATION
// =============================================================
#define MQTT_BROKER_URI "mqtt://mqtt.forthtrack.com" 
#define MQTT_USERNAME   "tracking"
#define MQTT_PASSWORD   "forth1234"

#define MQTT_TOPIC_TARGETS "ble/targets/set"
#define MQTT_TOPIC_REPORTS "ble/reports"
#define REPORT_INTERVAL_MS 60000 // Publish Array every 60 seconds
// =============================================================

// --- GLOBALS ---
static otUdpSocket sUdpSocket;
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_mqtt_started = false;
static SemaphoreHandle_t s_data_mutex = NULL; // Thread safety for Cache

// --- STORAGE STRUCTURES ---
#define MAX_TARGETS 20

typedef struct {
    char mac_str[13]; // Format: "7CD9F4..."
    int type;
} ble_target_t;

typedef struct {
    int8_t rssi;
    char data_hex[130]; // Hex string of data
    bool has_data;      // Flag to check if we have received data
} ble_cache_t;

// Shared Data
static ble_target_t s_ble_targets[MAX_TARGETS];
static ble_cache_t s_ble_cache[MAX_TARGETS];
static int s_num_targets = 0;

// --- HELPER: Hex Digit to Char ---
char hexDigit(uint8_t nibble) {
    return (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
}

// --- MQTT EVENT HANDLER ---
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT Connected. Subscribing to %s...", MQTT_TOPIC_TARGETS);
            esp_mqtt_client_subscribe(s_mqtt_client, MQTT_TOPIC_TARGETS, 1);
            break;
        case MQTT_EVENT_DATA:
            if (strncmp(event->topic, MQTT_TOPIC_TARGETS, event->topic_len) == 0) {
                // Parse Targets
                cJSON *root = cJSON_ParseWithLength(event->data, event->data_len);
                if (root && cJSON_IsArray(root)) {
                    if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                        s_num_targets = 0;
                        int count = cJSON_GetArraySize(root);
                        for (int i = 0; i < count && i < MAX_TARGETS; i++) {
                            cJSON *item = cJSON_GetArrayItem(root, i);
                            cJSON *mac = cJSON_GetObjectItem(item, "MAC");
                            cJSON *type = cJSON_GetObjectItem(item, "Type");
                            if (cJSON_IsString(mac) && cJSON_IsNumber(type)) {
                                // Strip colons if present
                                const char *src = mac->valuestring;
                                char *dst = s_ble_targets[i].mac_str;
                                int k = 0;
                                for (int j = 0; src[j] != '\0' && k < 12; j++) {
                                    if (src[j] != ':') dst[k++] = src[j];
                                }
                                dst[k] = '\0';
                                s_ble_targets[i].type = type->valueint;
                                
                                // Reset cache for this slot
                                s_ble_cache[i].has_data = false;
                                s_num_targets++;
                            }
                        }
                        xSemaphoreGive(s_data_mutex);
                        ESP_LOGI(TAG, "Updated Target List: %d devices", s_num_targets);
                    }
                    cJSON_Delete(root);
                }
            }
            break;
        default: break;
    }
}

// --- PERIODIC PUBLISH TASK (FIX FOR STUCK CODE & ARRAY FORMAT) ---
static void mqtt_periodic_worker_task(void *pvParameters) {
    ESP_LOGI(TAG, "Periodic MQTT Task Started. Publishing every %d ms.", REPORT_INTERVAL_MS);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(REPORT_INTERVAL_MS));

        if (!s_mqtt_started || !s_mqtt_client) continue;

        // 1. Lock Data
        if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            
            // 2. Build JSON Array: [ {Dev1}, {Dev2}, ... ]
            cJSON *root_array = cJSON_CreateArray();

            for (int i = 0; i < s_num_targets; i++) {
                cJSON *item = cJSON_CreateObject();
                
                // --- MAC (Formatted with Colons) ---
                char fmt_mac[18];
                const char *r = s_ble_targets[i].mac_str;
                snprintf(fmt_mac, sizeof(fmt_mac), "%c%c:%c%c:%c%c:%c%c:%c%c:%c%c",
                         r[0], r[1], r[2], r[3], r[4], r[5],
                         r[6], r[7], r[8], r[9], r[10], r[11]);
                cJSON_AddStringToObject(item, "MAC", fmt_mac);

                // --- RSSI & Data ---
                if (s_ble_cache[i].has_data) {
                    // RSSI as String (Requested format)
                    char rssi_str[8];
                    snprintf(rssi_str, sizeof(rssi_str), "%d", s_ble_cache[i].rssi);
                    cJSON_AddStringToObject(item, "RSSI", rssi_str);
                    
                    // Data
                    cJSON_AddStringToObject(item, "Data", s_ble_cache[i].data_hex);
                    
                    // Optional: Reset 'has_data' if you only want fresh updates
                    // s_ble_cache[i].has_data = false; 
                } else {
                    cJSON_AddStringToObject(item, "RSSI", "-100"); // Default low RSSI
                    cJSON_AddStringToObject(item, "Data", "NotFound");
                }
                
                cJSON_AddItemToArray(root_array, item);
            }

            // 3. Publish
            if (s_num_targets > 0) {
                char *json_str = cJSON_PrintUnformatted(root_array);
                if (json_str) {
                    ESP_LOGI(TAG, "Sending Report Array (%d devices)...", s_num_targets);
                    esp_mqtt_client_publish(s_mqtt_client, MQTT_TOPIC_REPORTS, json_str, 0, 0, 0);
                    free(json_str);
                }
            }

            cJSON_Delete(root_array);
            xSemaphoreGive(s_data_mutex);
        }
    }
}

// --- UDP HANDLER (Fast & Non-Blocking) ---
void handleUdpReceive(void *aContext, otMessage *aMessage, const otMessageInfo *aMessageInfo)
{
    uint8_t buf[128];
    uint16_t length = otMessageRead(aMessage, otMessageGetOffset(aMessage), buf, sizeof(buf) - 1);
    buf[length] = '\0';

    // 1. Handle REQ_MAC (Respond to MTD)
    if (length >= 7 && strncmp((char *)buf, "REQ_MAC", 7) == 0) {
        otInstance *instance = esp_openthread_get_instance();
        // Respond logic (simplified for brevity, relies on existing targets)
        for (int i = 0; i < s_num_targets; i++) {
            char responsePayload[32];
            snprintf(responsePayload, sizeof(responsePayload), "SET_MAC:%d:%s", i, s_ble_targets[i].mac_str);
            otMessage *replyMsg = otUdpNewMessage(instance, NULL);
            if (replyMsg) {
                (void)otMessageAppend(replyMsg, responsePayload, strlen(responsePayload));
                otMessageInfo replyInfo;
                memset(&replyInfo, 0, sizeof(replyInfo));
                replyInfo.mPeerAddr = aMessageInfo->mPeerAddr;
                replyInfo.mPeerPort = 234;
                (void)otUdpSend(instance, &sUdpSocket, replyMsg, &replyInfo);
                usleep(5000); 
            }
        }
        return;
    }

    // 2. Handle Binary Data -> UPDATE CACHE ONLY
    if (length >= 7) {
        uint8_t *mac = buf;           // Bytes 0-5
        int8_t rssi = (int8_t)buf[6]; // Byte 6
        uint8_t *data = &buf[7];      // Byte 7+
        uint16_t dataLen = length - 7;
        
        // Convert received MAC to String "7CD9..."
        char recv_mac_str[13];
        snprintf(recv_mac_str, sizeof(recv_mac_str), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        // Lock Mutex (Don't wait long, drop packet if busy)
        if (xSemaphoreTake(s_data_mutex, 0) == pdTRUE) { 
            
            // Find Target Index
            for (int i = 0; i < s_num_targets; i++) {
                if (strncmp(s_ble_targets[i].mac_str, recv_mac_str, 12) == 0) {
                    
                    // Update Cache
                    s_ble_cache[i].rssi = rssi;
                    s_ble_cache[i].has_data = true;
                    
                    // Hex Convert
                    for (int k = 0; k < dataLen && k < 64; k++) {
                        s_ble_cache[i].data_hex[k*2]     = hexDigit(data[k] >> 4);
                        s_ble_cache[i].data_hex[k*2 + 1] = hexDigit(data[k] & 0x0F);
                    }
                    s_ble_cache[i].data_hex[dataLen * 2] = '\0';
                    
                    break;
                }
            }
            xSemaphoreGive(s_data_mutex);
        }
    }
}

// --- INIT HELPERS ---
static void mqtt_starter_task(void *pvParameters) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.username = MQTT_USERNAME,
        .credentials.authentication.password = MQTT_PASSWORD,
    };
    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    vTaskDelay(pdMS_TO_TICKS(5000)); // Wait for Thread
    esp_mqtt_client_start(s_mqtt_client);
    s_mqtt_started = true;
    vTaskDelete(NULL);
}

static esp_netif_t *init_openthread_netif(const esp_openthread_platform_config_t *config) {
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_OPENTHREAD();
    esp_netif_t *netif = esp_netif_new(&cfg);
    assert(netif != NULL);
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_openthread_netif_glue_init(config)));
    return netif;
}

static void ot_task_worker(void *aContext) {
    esp_openthread_platform_config_t config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };
    ESP_ERROR_CHECK(esp_openthread_init(&config));
    (void)otLoggingSetLevel(OT_LOG_LEVEL_NOTE); // Silence Logs
    
    esp_netif_t *openthread_netif = init_openthread_netif(&config);
    esp_netif_set_default_netif(openthread_netif);

    otSockAddr bindAddr;
    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.mPort = 123;
    (void)otUdpOpen(esp_openthread_get_instance(), &sUdpSocket, handleUdpReceive, NULL);
    (void)otUdpBind(esp_openthread_get_instance(), &sUdpSocket, &bindAddr, OT_NETIF_THREAD);

    otOperationalDatasetTlvs dataset;
    otError error = otDatasetGetActiveTlvs(esp_openthread_get_instance(), &dataset);
    ESP_ERROR_CHECK(esp_openthread_auto_start((error == OT_ERROR_NONE) ? &dataset : NULL));

    esp_openthread_launch_mainloop();
    vTaskDelete(NULL);
}

void app_main(void) {
    esp_vfs_eventfd_config_t eventfd_config = { .max_fds = 3 };
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));
    
    // 1. Create Mutex
    s_data_mutex = xSemaphoreCreateMutex();

    // 2. Start Tasks
    xTaskCreate(ot_task_worker, "ot_cli_main", 10240, xTaskGetCurrentTaskHandle(), 5, NULL);
    xTaskCreate(mqtt_starter_task, "mqtt_starter", 4096, NULL, 6, NULL);
    xTaskCreate(mqtt_periodic_worker_task, "mqtt_periodic", 4096, NULL, 6, NULL);
}
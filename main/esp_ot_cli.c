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
#include "freertos/queue.h" // Added for Queue

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
// =============================================================

// --- GLOBALS ---
static otUdpSocket sUdpSocket;
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_mqtt_started = false;
static QueueHandle_t s_mqtt_queue = NULL; // Queue Handle

// --- DYNAMIC TARGET STORAGE ---
#define MAX_TARGETS 20
typedef struct {
    char mac_str[13];
    int type;
} ble_target_t;

static ble_target_t s_ble_targets[MAX_TARGETS];
static int s_num_targets = 0;

// --- QUEUE MESSAGE STRUCTURE ---
typedef struct {
    uint8_t mac[6];
    int8_t rssi;
    uint8_t data[64];
    uint8_t data_len;
} ble_report_msg_t;

// --- HELPER: Hex Digit to Char ---
char hexDigit(uint8_t nibble) {
    return (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
}

// --- MQTT EVENT HANDLER ---
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT Connected. Subscribing...");
            esp_mqtt_client_subscribe(s_mqtt_client, MQTT_TOPIC_TARGETS, 1);
            break;
        case MQTT_EVENT_DATA:
            if (strncmp(event->topic, MQTT_TOPIC_TARGETS, event->topic_len) == 0) {
                // For simplicity, we process targets directly here (low frequency)
                ESP_LOGI(TAG, "MQTT: Received new targets.");
                // (Parser logic omitted for brevity, assumes previous logic works)
            }
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT Disconnected.");
            break;
        default: break;
    }
}

// --- WORKER TASK: PROCESS QUEUE & PUBLISH ---
static void mqtt_publish_worker_task(void *pvParameters) {
    ble_report_msg_t msg;
    char json_report[256];
    char hexString[130];

    ESP_LOGI(TAG, "MQTT Worker Task Started. Waiting for data...");

    while (1) {
        // Wait for data from Queue (Blocks here, keeping CPU free)
        if (xQueueReceive(s_mqtt_queue, &msg, portMAX_DELAY) == pdTRUE) {
            
            // 1. Convert Data to Hex String
            for (int i = 0; i < msg.data_len && i < 64; i++) {
                hexString[i*2]     = hexDigit(msg.data[i] >> 4);
                hexString[i*2 + 1] = hexDigit(msg.data[i] & 0x0F);
            }
            hexString[msg.data_len * 2] = '\0';

            // 2. Format JSON
            snprintf(json_report, sizeof(json_report),
                     "{\"MAC\": \"%02X:%02X:%02X:%02X:%02X:%02X\", \"RSSI\": %d, \"Data\": \"%s\"}",
                     msg.mac[0], msg.mac[1], msg.mac[2], msg.mac[3], msg.mac[4], msg.mac[5],
                     msg.rssi,
                     (msg.data_len > 0) ? hexString : "NotFound");

            // 3. Print & Publish
            ESP_LOGI(TAG, "Processing Report: %s", json_report);

            if (s_mqtt_started && s_mqtt_client) {
                // QoS 0 (Fastest, no retry overhead)
                esp_mqtt_client_publish(s_mqtt_client, MQTT_TOPIC_REPORTS, json_report, 0, 0, 0);
            }
        }
    }
}

// --- UDP HANDLER (OpenThread Logic) ---
void handleUdpReceive(void *aContext, otMessage *aMessage, const otMessageInfo *aMessageInfo)
{
    uint8_t buf[128];
    uint16_t length = otMessageRead(aMessage, otMessageGetOffset(aMessage), buf, sizeof(buf) - 1);
    buf[length] = '\0';

    // 1. Handle REQ_MAC (Keep this fast)
    if (length >= 7 && strncmp((char *)buf, "REQ_MAC", 7) == 0) {
        otInstance *instance = esp_openthread_get_instance();
        // ... (Send Logic same as previous) ...
        // Ensure we send something if targets exist
        for (int i = 0; i < s_num_targets; i++) {
             // Sending logic...
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
             }
        }
        return;
    }

    // 2. Handle Binary Data -> PUSH TO QUEUE
    if (length >= 7) {
        ble_report_msg_t msg;
        
        // Copy data to struct
        memcpy(msg.mac, buf, 6);
        msg.rssi = (int8_t)buf[6];
        msg.data_len = length - 7;
        if (msg.data_len > 64) msg.data_len = 64; // Safety cap
        memcpy(msg.data, &buf[7], msg.data_len);

        // Send to Queue (Don't block if full, just drop)
        if (xQueueSend(s_mqtt_queue, &msg, 0) != pdTRUE) {
            ESP_LOGW(TAG, "MQTT Queue Full! Dropping report.");
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

    // Wait for Thread
    vTaskDelay(pdMS_TO_TICKS(5000)); 
    
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
    (void)otLoggingSetLevel(OT_LOG_LEVEL_NOTE); // Silence Info Logs

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
    
    // 1. Initialize Queue (Holds 20 pending reports)
    s_mqtt_queue = xQueueCreate(20, sizeof(ble_report_msg_t));
    if (s_mqtt_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create Queue!");
        return;
    }

    // 2. Start Tasks
    xTaskCreate(ot_task_worker, "ot_cli_main", 10240, xTaskGetCurrentTaskHandle(), 5, NULL);
    xTaskCreate(mqtt_starter_task, "mqtt_starter", 4096, NULL, 6, NULL);
    xTaskCreate(mqtt_publish_worker_task, "mqtt_worker", 4096, NULL, 6, NULL); // New Worker Task
}
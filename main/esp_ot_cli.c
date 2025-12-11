#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"       

/* NimBLE Headers */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#define TAG "WIFI_BLE_GW"

// =============================================================
// CONFIGURATION
// =============================================================
#define WIFI_SSID       "MyNet"      // <--- SET THIS
#define WIFI_PASS       "Phayatha1234"  // <--- SET THIS

#define MQTT_BROKER_URI "mqtt://mqtt.forthtrack.com" 
#define MQTT_USERNAME   "tracking"
#define MQTT_PASSWORD   "forth1234"

#define MQTT_TOPIC_TARGETS "ble/targets/set"
#define MQTT_TOPIC_REPORTS "ble/reports"
#define REPORT_INTERVAL_MS 60000   // 1 Minute
#define BLE_DATA_TIMEOUT_MS 120000 // 2 Minutes
// =============================================================

// --- GLOBALS ---
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_mqtt_connected = false;
static SemaphoreHandle_t s_data_mutex = NULL;

// --- STORAGE ---
#define MAX_TARGETS 20

typedef struct {
    char mac_str[13]; // Format: "AABBCCDDEEFF"
    uint8_t mac_addr[6]; // Binary format {0xAA, 0xBB...}
    int type;
} ble_target_t;

typedef struct {
    int8_t rssi;
    char data_hex[130]; 
    bool has_data;
    int64_t last_update_ms;
} ble_cache_t;

static ble_target_t s_ble_targets[MAX_TARGETS];
static ble_cache_t s_ble_cache[MAX_TARGETS];
static int s_num_targets = 0;

// --- PROTOTYPES ---
void ble_scan_start(void);

// --- HELPERS ---
char hexDigit(uint8_t nibble) {
    return (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
}

// Convert String MAC "AABB..." to Binary {0xAA, 0xBB...}
void parse_mac_string(const char *str, uint8_t *out_addr) {
    for (int i = 0; i < 6; i++) {
        char buf[3] = {str[i*2], str[i*2+1], 0};
        out_addr[i] = (uint8_t)strtol(buf, NULL, 16);
    }
}

// --- NIMBLE SCAN EVENT HANDLER (CORRECTED) ---
static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            if (xSemaphoreTake(s_data_mutex, 0) == pdTRUE) {
                for (int i = 0; i < s_num_targets; i++) {
                    bool match = true;
                    // Check MAC
                    for(int k=0; k<6; k++) {
                        if (s_ble_targets[i].mac_addr[k] != event->disc.addr.val[5-k]) {
                            match = false;
                            break;
                        }
                    }

                    if (match) {
                        // 1. ALWAYS Update RSSI & Timestamp (Presence is valid even without data)
                        s_ble_cache[i].rssi = event->disc.rssi;
                        s_ble_cache[i].has_data = true;
                        s_ble_cache[i].last_update_ms = esp_timer_get_time() / 1000;

                        // 2. ONLY Update Data if Length > 0 (Prevent overwriting with empty strings)
                        if (event->disc.length_data > 0) {
                            int len = event->disc.length_data;
                            if (len > 64) len = 64; 

                            for (int k = 0; k < len; k++) {
                                s_ble_cache[i].data_hex[k*2]     = hexDigit(event->disc.data[k] >> 4);
                                s_ble_cache[i].data_hex[k*2 + 1] = hexDigit(event->disc.data[k] & 0x0F);
                            }
                            s_ble_cache[i].data_hex[len * 2] = '\0';
                            
                            // Log only when we get new data
                            ESP_LOGI(TAG, "MATCH! MAC: %s | RSSI: %d | Data Len: %d", 
                                     s_ble_targets[i].mac_str, event->disc.rssi, len);
                        } else {
                             ESP_LOGD(TAG, "MATCH! MAC: %s (Empty Packet - Kept Old Data)", s_ble_targets[i].mac_str);
                        }
                        break; 
                    }
                }
                xSemaphoreGive(s_data_mutex);
            }
            break;
        }

        case BLE_GAP_EVENT_DISC_COMPLETE:
            ble_scan_start(); 
            break;
            
        default:
            break;
    }
    return 0;
}

void ble_scan_start(void) {
    uint8_t own_addr_type;
    struct ble_gap_disc_params disc_params;
    int rc;

    ble_hs_id_infer_auto(0, &own_addr_type);

    memset(&disc_params, 0, sizeof(disc_params));
    disc_params.filter_duplicates = 0; // Don't filter, we handle caching manually
    disc_params.passive = 0;           // Active scan to get scan response data
    disc_params.itvl = 0;              // Default interval
    disc_params.window = 0;            // Default window

    // Scan for 10 seconds (10000ms), then restart in callback
    rc = ble_gap_disc(own_addr_type, 10000, &disc_params, ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error initiating GAP discovery; rc=%d", rc);
    }
}

// --- MQTT EVENT HANDLER ---
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT Connected");
            s_mqtt_connected = true;
            esp_mqtt_client_subscribe(s_mqtt_client, MQTT_TOPIC_TARGETS, 1);
            break;
        case MQTT_EVENT_DISCONNECTED:
             ESP_LOGI(TAG, "MQTT Disconnected");
             s_mqtt_connected = false;
             break;
        case MQTT_EVENT_DATA:
            if (strncmp(event->topic, MQTT_TOPIC_TARGETS, event->topic_len) == 0) {
                ESP_LOGI(TAG, "Received New Targets List");
                cJSON *root = cJSON_ParseWithLength(event->data, event->data_len);
                if (root && cJSON_IsArray(root)) {
                    if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                        s_num_targets = 0;
                        int count = cJSON_GetArraySize(root);
                        for (int i = 0; i < count && i < MAX_TARGETS; i++) {
                            cJSON *item = cJSON_GetArrayItem(root, i);
                            cJSON *mac = cJSON_GetObjectItem(item, "MAC");
                            cJSON *type = cJSON_GetObjectItem(item, "Type");
                            if (cJSON_IsString(mac)) {
                                // 1. Store String (Stripping Colons)
                                const char *src = mac->valuestring;
                                char *dst = s_ble_targets[i].mac_str;
                                int k = 0;
                                for (int j = 0; src[j] != '\0' && k < 12; j++) {
                                    if (src[j] != ':') dst[k++] = src[j];
                                }
                                dst[k] = '\0';
                                
                                // 2. Store Binary (For Scan Matching)
                                parse_mac_string(s_ble_targets[i].mac_str, s_ble_targets[i].mac_addr);

                                s_ble_targets[i].type = cJSON_IsNumber(type) ? type->valueint : 0;
                                s_ble_cache[i].has_data = false; 
                                s_num_targets++;
                            }
                        }
                        xSemaphoreGive(s_data_mutex);
                        ESP_LOGI(TAG, "Updated %d targets.", s_num_targets);
                    }
                    cJSON_Delete(root);
                }
            }
            break;
        default: break;
    }
}

// --- WIFI HANDLER ---
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi Disconnected. Retrying...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

// --- WORKER TASK: PERIODIC REPORT ---
static void periodic_worker_task(void *pvParameters) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000)); // Check every second

        static int report_timer = 0;
        report_timer += 1000;
        
        // Report every 60 seconds (REPORT_INTERVAL_MS)
        if (report_timer >= REPORT_INTERVAL_MS) {
            report_timer = 0;

            if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                int64_t now = esp_timer_get_time() / 1000;

                // 1. CLEANUP OLD BLE DATA (Timeout > 2 mins)
                for (int i = 0; i < s_num_targets; i++) {
                    if (s_ble_cache[i].has_data) {
                        if ((now - s_ble_cache[i].last_update_ms) > BLE_DATA_TIMEOUT_MS) {
                            s_ble_cache[i].has_data = false; 
                            ESP_LOGW(TAG, "Target %s timed out (no data for 2 mins)", s_ble_targets[i].mac_str);
                        }
                    }
                }

                // 2. BUILD JSON
                cJSON *root = cJSON_CreateObject();
                cJSON *report_array = cJSON_CreateArray();
                
                for (int i = 0; i < s_num_targets; i++) {
                    cJSON *item = cJSON_CreateObject();
                    cJSON_AddStringToObject(item, "MAC", s_ble_targets[i].mac_str);

                    if (s_ble_cache[i].has_data) {
                        // Add RSSI as Number
                        cJSON_AddNumberToObject(item, "RSSI", s_ble_cache[i].rssi);
                        cJSON_AddStringToObject(item, "Data", s_ble_cache[i].data_hex);
                        // Add timestamp of last seen (optional, useful for debugging)
                        cJSON_AddNumberToObject(item, "ts", (double)s_ble_cache[i].last_update_ms);
                    } else {
                        cJSON_AddNumberToObject(item, "RSSI", -100);
                        cJSON_AddStringToObject(item, "Data", "NotFound");
                    }
                    cJSON_AddItemToArray(report_array, item);
                }
                cJSON_AddItemToObject(root, "reports", report_array);

                // 3. PUBLISH
                if (s_mqtt_connected && s_num_targets > 0) {
                    char *json_str = cJSON_PrintUnformatted(root);
                    if (json_str) {
                        // --- DEBUG: Print JSON to Console ---
                        printf(">> REPORT JSON: %s\n", json_str); 
                        // ------------------------------------
                        
                        esp_mqtt_client_publish(s_mqtt_client, MQTT_TOPIC_REPORTS, json_str, 0, 0, 0);
                        free(json_str);
                    }
                }
                cJSON_Delete(root);
                xSemaphoreGive(s_data_mutex);
            }
        }
    }
}

// --- INIT FUNCTIONS ---
void wifi_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void ble_app_on_sync(void) {
    int rc;
    rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);
    ESP_LOGI(TAG, "[BLE] Stack Synced. Starting Scan...");
    ble_scan_start();
}

void nimble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_init(void) {
    // NimBLE Initialization
    nimble_port_init();
    ble_hs_cfg.sync_cb = ble_app_on_sync;
    nimble_port_freertos_init(nimble_host_task);
}

static void mqtt_start(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.username = MQTT_USERNAME,
        .credentials.authentication.password = MQTT_PASSWORD,
    };
    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt_client);
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    s_data_mutex = xSemaphoreCreateMutex();

    ESP_LOGI(TAG, "System Init...");
    wifi_init();
    ble_init();
    mqtt_start();

    xTaskCreate(periodic_worker_task, "worker_task", 4096, NULL, 5, NULL);
}
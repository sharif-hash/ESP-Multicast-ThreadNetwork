
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_openthread.h"
#include "esp_openthread_cli.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_ot_config.h"
#include "esp_vfs_eventfd.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "hal/uart_types.h"
#include "nvs_flash.h"
#include "openthread/cli.h"
#include "openthread/instance.h"
#include "openthread/logging.h"
#include "openthread/tasklet.h"
#include "openthread/udp.h"
#include "openthread/ip6.h"

#if CONFIG_OPENTHREAD_STATE_INDICATOR_ENABLE
#include "ot_led_strip.h"
#endif

#if CONFIG_OPENTHREAD_CLI_ESP_EXTENSION
#include "esp_ot_cli_extension.h"
#endif

#define TAG "ot_esp_cli"

#define MAX_MTD_COUNT 10
#define MY_TARGET_BLE_MAC "AABBCCDDEEFF" // Your BLE Target

static otUdpSocket sUdpSocket;
static TimerHandle_t s_periodic_timer;

// --- GLOBAL VARIABLES FOR MULTI-UNICAST ---
static otIp6Address sMtdList[MAX_MTD_COUNT]; // List of addresses
static int sMtdCount = 0;                    // How many we found so far
// ------------------------------------------

// --- 1. UDP Receive Handler (Consolidated) ---
// Helper to convert byte to hex digit
char hexDigit(uint8_t nibble) {
    return (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
}

// Define the 9 Target MACs for the "Real Event"
const char *TARGET_MACS[] = {
    "7CD9F41B47EB", // [0] Original
    "7CD9F412CD63", // [1]
    "7CD9F410E29F", // [2]
    "7CD9F4117ADA", // [3]
    "7CD9F41126CB", // [4]
    "7CD9F410DECF", // [5]
    "E466E53BB5AD", // [6]
    "E4B323B4738E", // [7]
    "84C2E4DCDE5B"  // [8]
};
#define NUM_TARGETS 9

void handleUdpReceive(void *aContext, otMessage *aMessage, const otMessageInfo *aMessageInfo)
{
    uint8_t buf[64];
    uint16_t length = otMessageRead(aMessage, otMessageGetOffset(aMessage), buf, sizeof(buf) - 1);
    buf[length] = '\0'; // Safety null-termination

    // ==========================================================
    // LOGIC 1: Handle Request from MTD ("REQ_MAC")
    // ==========================================================
    if (length >= 7 && strncmp((char *)buf, "REQ_MAC", 7) == 0)
    {
        ESP_LOGI("APP", "Received Request: REQ_MAC. Sending %d targets...", NUM_TARGETS);

        otInstance *instance = esp_openthread_get_instance();

        for (int i = 0; i < NUM_TARGETS; i++)
        {
            char responsePayload[32];
            // Format: SET_MAC:Index:MAC (e.g., "SET_MAC:0:7CD9...")
            snprintf(responsePayload, sizeof(responsePayload), "SET_MAC:%d:%s", i, TARGET_MACS[i]);

            otMessage *replyMsg = otUdpNewMessage(instance, NULL);
            if (replyMsg)
            {
                (void)otMessageAppend(replyMsg, responsePayload, strlen(responsePayload));
                
                otMessageInfo replyInfo;
                memset(&replyInfo, 0, sizeof(replyInfo));
                replyInfo.mPeerAddr = aMessageInfo->mPeerAddr; // Reply to sender
                replyInfo.mPeerPort = 234; // MTD Port

                (void)otUdpSend(instance, &sUdpSocket, replyMsg, &replyInfo);
                
                // Small delay to prevent queue flooding
                usleep(20000); 
            }
        }
        ESP_LOGI("APP", "Sent All Targets.");
        return; // Stop here (Don't try to parse REQ as Data)
    }

    // ==========================================================
    // LOGIC 2: Handle Binary Data Report (PRINT LOG DATA)
    // ==========================================================
    if (length >= 7)
    {
        uint8_t *mac = buf;           // Bytes 0-5
        int8_t rssi = (int8_t)buf[6]; // Byte 6
        uint8_t *data = &buf[7];      // Byte 7+
        uint16_t dataLen = length - 7;

        // Generate Hex String for Data
        char hexString[65];
        for (int i = 0; i < dataLen && i < 31; i++) {
            hexString[i*2]     = hexDigit(data[i] >> 4);
            hexString[i*2 + 1] = hexDigit(data[i] & 0x0F);
        }
        hexString[dataLen * 2] = '\0';

        // PRINT THE JSON LOG
        ESP_LOGI("APP", "{\"MAC\": \"%02X:%02X:%02X:%02X:%02X:%02X\", \"RSSI\": %d, \"Data\": \"%s\"}",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 rssi,
                 (dataLen > 0) ? hexString : "NotFound");
    }
}


// --- 2. Helper Function to Send (Modified for Unicast) ---
// Change function signature to accept the payload string
void send_command_helper(const char *command_string)
{
    otInstance *instance = esp_openthread_get_instance();
    otMessage *message = NULL;
    otMessageInfo messageInfo;
    otError error;

    // 1. Send to Sleepy FTD (Multicast still works fine for Router/FTD)
    // ... (Keep your existing FTD multicast code here if you want) ...

    // 2. Send UNICAST to ALL Sleepy MTDs
    if (sMtdCount > 0)
    {
        ESP_LOGI("APP", "Sending to %d MTDs...", sMtdCount);

        // Loop through every known MTD
        for (int i = 0; i < sMtdCount; i++)
        {
            message = otUdpNewMessage(instance, NULL);
            if (message) {
                (void)otMessageAppend(message, command_string, strlen(command_string));
                
                memset(&messageInfo, 0, sizeof(messageInfo));
                
                // Set Destination to the saved address from the list
                messageInfo.mPeerAddr = sMtdList[i]; 
                messageInfo.mPeerPort = 234; 

                error = otUdpSend(instance, &sUdpSocket, message, &messageInfo);
                
                if (error != OT_ERROR_NONE) {
                    otMessageFree(message);
                    ESP_LOGE("APP", "Failed to send to MTD #%d", i);
                }
            }
        }
    }
    else
    {
        ESP_LOGW("APP", "No MTDs found yet. Press button on MTDs to register.");
    }
}

// --- 3. FreeRTOS Timer Callback ---
void vPeriodicTimerCallback(TimerHandle_t xTimer)
{
/*    if (esp_openthread_lock_acquire(portMAX_DELAY))
    {
        send_command_helper("LED:1");
        esp_openthread_lock_release();
    }*/
}

// --- 4. Initialization Function ---
void init_custom_multicast(otInstance *instance)
{
    otError error;
    otSockAddr bindAddr;
    otIp6Address multicastAddr;

    // Listen on ff03::1 so we can hear the MTD's broadcast
    (void)otIp6AddressFromString("ff03::1", &multicastAddr);
    (void)otIp6SubscribeMulticastAddress(instance, &multicastAddr);

    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.mPort = 123;

    (void)otUdpOpen(instance, &sUdpSocket, handleUdpReceive, NULL);
    (void)otUdpBind(instance, &sUdpSocket, &bindAddr, OT_NETIF_THREAD);
    
    ESP_LOGI("APP", "Listening on ff03::1 port 123. Waiting for MTD...");

    // Start Timer
    s_periodic_timer = xTimerCreate("PeriodicTx", pdMS_TO_TICKS(5000), pdTRUE, (void *)0, vPeriodicTimerCallback);
    if (s_periodic_timer != NULL) {
        xTimerStart(s_periodic_timer, 0);
    }
}

static esp_netif_t *init_openthread_netif(const esp_openthread_platform_config_t *config)
{
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_OPENTHREAD();
    esp_netif_t *netif = esp_netif_new(&cfg);
    assert(netif != NULL);
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_openthread_netif_glue_init(config)));

    return netif;
}

static void ot_task_worker(void *aContext)
{
    esp_openthread_platform_config_t config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };

    // Initialize the OpenThread stack
    ESP_ERROR_CHECK(esp_openthread_init(&config));

#if CONFIG_OPENTHREAD_STATE_INDICATOR_ENABLE
    ESP_ERROR_CHECK(esp_openthread_state_indicator_init(esp_openthread_get_instance()));
#endif

#if CONFIG_OPENTHREAD_LOG_LEVEL_DYNAMIC
    // The OpenThread log level directly matches ESP log level
    (void)otLoggingSetLevel(CONFIG_LOG_DEFAULT_LEVEL);
#endif
    // Initialize the OpenThread cli
#if CONFIG_OPENTHREAD_CLI
    esp_openthread_cli_init();
#endif

    esp_netif_t *openthread_netif;
    // Initialize the esp_netif bindings
    openthread_netif = init_openthread_netif(&config);
    esp_netif_set_default_netif(openthread_netif);
    
    // --- Initialize Custom Multicast & Timer ---
    init_custom_multicast(esp_openthread_get_instance());

#if CONFIG_OPENTHREAD_CLI_ESP_EXTENSION
    esp_cli_custom_command_init();
#endif // CONFIG_OPENTHREAD_CLI_ESP_EXTENSION

    // Run the main loop
#if CONFIG_OPENTHREAD_CLI
    esp_openthread_cli_create_task();
#endif
#if CONFIG_OPENTHREAD_AUTO_START
    otOperationalDatasetTlvs dataset;
    otError error = otDatasetGetActiveTlvs(esp_openthread_get_instance(), &dataset);
    ESP_ERROR_CHECK(esp_openthread_auto_start((error == OT_ERROR_NONE) ? &dataset : NULL));
#endif
    esp_openthread_launch_mainloop();

    // Clean up
    esp_openthread_netif_glue_deinit();
    esp_netif_destroy(openthread_netif);

    esp_vfs_eventfd_unregister();
    vTaskDelete(NULL);
}

void app_main(void)
{
    // Used eventfds:
    // * netif
    // * ot task queue
    // * radio driver
    esp_vfs_eventfd_config_t eventfd_config = {
        .max_fds = 3,
    };

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));
    xTaskCreate(ot_task_worker, "ot_cli_main", 10240, xTaskGetCurrentTaskHandle(), 5, NULL);
}
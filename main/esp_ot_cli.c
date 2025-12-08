
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
void handleUdpReceive(void *aContext, otMessage *aMessage, const otMessageInfo *aMessageInfo)
{
    uint8_t buf[64];
    uint16_t length = otMessageRead(aMessage, otMessageGetOffset(aMessage), buf, sizeof(buf) - 1);
    buf[length] = '\0';

    ESP_LOGI("UDP", "Received: %s", buf);

    // --- LOGIC: If MTD sends "REQ_MAC", we reply with "SET_MAC..." ---
    if (strncmp((char *)buf, "REQ_MAC", 7) == 0)
    {
        ESP_LOGI("APP", "MTD requested MAC. Sending Reply...");

        // 1. Prepare the response (SET_MAC : Index : MAC)
        // Change "AABBCCDDEEFF" to your actual BLE MAC address
        char responsePayload[32];
        snprintf(responsePayload, sizeof(responsePayload), "SET_MAC:0:%s", "7CD9F41B47EB");

        // 2. Send Unicast Reply
        otInstance *instance = esp_openthread_get_instance();
        otMessage *replyMsg = otUdpNewMessage(instance, NULL);
        if (replyMsg)
        {
            (void)otMessageAppend(replyMsg, responsePayload, strlen(responsePayload));
            
            otMessageInfo replyInfo;
            memset(&replyInfo, 0, sizeof(replyInfo));
            // Reply specifically to the device that asked
            replyInfo.mPeerAddr = aMessageInfo->mPeerAddr; 
            replyInfo.mPeerPort = 234; // MTD Port

            (void)otUdpSend(instance, &sUdpSocket, replyMsg, &replyInfo);
            ESP_LOGI("APP", "Sent Reply: %s", responsePayload);
        }
    }
    // (Optional) Keep the old registration logic if you still want it
    else if (strncmp((char *)buf, "mtd button", 10) == 0)
    {
        ESP_LOGI("APP", "MTD Registered (Button Press)");
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
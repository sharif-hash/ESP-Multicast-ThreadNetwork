
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

static otUdpSocket sUdpSocket;
static TimerHandle_t s_periodic_timer;

// --- GLOBAL VARIABLES FOR UNICAST ---
static otIp6Address sMtdAddress;   // Store the MTD's address here
static bool sHasMtdAddress = false; // Flag to check if we found the MTD
// ------------------------------------

// --- 1. UDP Receive Handler ---
// We listen for "mtd button". If we hear it, we save the sender's address.
void handleUdpReceive(void *aContext, otMessage *aMessage, const otMessageInfo *aMessageInfo)
{
    uint8_t buf[64];
    uint16_t length = otMessageRead(aMessage, otMessageGetOffset(aMessage), buf, sizeof(buf) - 1);
    buf[length] = '\0';

    // FIX: Access mFields.m16[7] directly (Index 7 is the last 16-bit chunk of the address)
    ESP_LOGI("UDP", "Received message: %s from IP ending in :%04x", buf, 
             aMessageInfo->mPeerAddr.mFields.m16[7]);

    // Check if this is the MTD (matches logic in sleepy-ftd.c)
    if (strncmp((char *)buf, "mtd button", 10) == 0)
    {
        // SAVE THE ADDRESS
        sMtdAddress = aMessageInfo->mPeerAddr;
        sHasMtdAddress = true;
        ESP_LOGI("APP", "Captured MTD Address! Will reply via Unicast.");
    }
}

// --- 2. Helper Function to Send (Modified for Unicast) ---
/*void send_command_helper(void)
{
    otInstance *instance = esp_openthread_get_instance();
    otMessage *message = NULL;
    otMessageInfo messageInfo;
    otError error;
    
    // A. Send Multicast to FTD (Port 123) - This usually works for routers
    const char *payload_ftd = "FTD message";
    message = otUdpNewMessage(instance, NULL);
    if (message) {
        (void)otMessageAppend(message, payload_ftd, strlen(payload_ftd));
        memset(&messageInfo, 0, sizeof(messageInfo));
        (void)otIp6AddressFromString("ff03::1", &messageInfo.mPeerAddr);
        messageInfo.mPeerPort = 123; 
        (void)otUdpSend(instance, &sUdpSocket, message, &messageInfo);
    }

    // B. Send UNICAST to Sleepy MTD (Port 234)
    // We only send if we have heard from the MTD at least once.
    if (sHasMtdAddress)
    {
        const char *payload_mtd = "MTD message";
        message = otUdpNewMessage(instance, NULL);
        if (message) {
            (void)otMessageAppend(message, payload_mtd, strlen(payload_mtd));
            
            memset(&messageInfo, 0, sizeof(messageInfo));
            // USE THE SAVED UNICAST ADDRESS
            messageInfo.mPeerAddr = sMtdAddress; 
            messageInfo.mPeerPort = 234; 

            error = otUdpSend(instance, &sUdpSocket, message, &messageInfo);
            if (error == OT_ERROR_NONE) {
                ESP_LOGI("APP", "Sent UNICAST to MTD");
            } else {
                otMessageFree(message);
                ESP_LOGE("APP", "Failed to send unicast: %d", error);
            }
        }
    }
    else
    {
        ESP_LOGW("APP", "Cannot send to MTD yet - Waiting for 'mtd button' message...");
    }
}*/

// Change function signature to accept the payload string
void send_command_helper(const char *command_string)
{
    otInstance *instance = esp_openthread_get_instance();
    otMessage *message = NULL;
    otMessageInfo messageInfo;
    otError error;

    // Use the dynamic string passed to the function
    const char *payload_mtd = command_string;

    // We only send if we have heard from the MTD at least once.
    if (sHasMtdAddress)
    {
        message = otUdpNewMessage(instance, NULL);
        if (message) {
            (void)otMessageAppend(message, payload_mtd, strlen(payload_mtd));
            
            memset(&messageInfo, 0, sizeof(messageInfo));
            // USE THE SAVED UNICAST ADDRESS
            messageInfo.mPeerAddr = sMtdAddress; 
            messageInfo.mPeerPort = 234; 

            error = otUdpSend(instance, &sUdpSocket, message, &messageInfo);
            if (error == OT_ERROR_NONE) {
                ESP_LOGI("APP", "Sent Command: %s", payload_mtd);
            } else {
                otMessageFree(message);
                ESP_LOGE("APP", "Failed to send: %d", error);
            }
        }
    }
}

// --- 3. FreeRTOS Timer Callback ---
void vPeriodicTimerCallback(TimerHandle_t xTimer)
{
    if (esp_openthread_lock_acquire(portMAX_DELAY))
    {
        send_command_helper("LED:1");
        esp_openthread_lock_release();
    }
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
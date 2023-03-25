#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <nvs_flash.h>
#include <stdio.h>
#include <string.h>
#include <webp/demux.h>
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_ble.h>

static uint8_t spriteBuffer[50000];

static const char *PROV_TAG = "[smx/provision]";
static const char *MATRIX_TAG = "[smx/display]";
static const char *WIFI_TAG = "[smx/wifi]";

TaskHandle_t matrixTask;
TaskHandle_t workerTask;
QueueHandle_t workerQueue;

struct workerQueueItem {
    uint8_t type;
    int numericParameter;
    char charParameter[300];
};

static void startProvisioning() {
    wifi_prov_security_t security = WIFI_PROV_SECURITY_0;

    /* Configuration for the provisioning manager */
    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM};
    /* Initialize provisioning manager with the
     * configuration parameters set above */
    ESP_ERROR_CHECK(wifi_prov_mgr_init(config));

    char service_name[18];
    uint8_t eth_mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
    snprintf(service_name, 18, "%s%02X%02X%02X%02X%02X%02X", "PROV_",
             eth_mac[0], eth_mac[1], eth_mac[2], eth_mac[3], eth_mac[4],
             eth_mac[5]);

    /* Start provisioning service */
    ESP_ERROR_CHECK(
        wifi_prov_mgr_start_provisioning(security, NULL, service_name, NULL));
}

/* Event handler for catching system events */
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data) {
    static uint8_t wifiConnectionAttempts;
    static bool provisioned;
    static bool provisioning;

    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
            case WIFI_PROV_CRED_FAIL:
                ESP_LOGE(PROV_TAG, "provisioning error");
                wifi_prov_mgr_reset_sm_state_on_failure();
                break;
            case WIFI_PROV_CRED_SUCCESS:
                ESP_LOGI(PROV_TAG, "provisioning successful");
                provisioned = true;
                break;
            case WIFI_PROV_END:
                provisioning = false;
                ESP_LOGI(PROV_TAG, "provisioning end");
                wifi_prov_mgr_deinit();
                break;
            case WIFI_PROV_START:
                provisioning = true;
                ESP_LOGI(PROV_TAG, "provisioning started");
                break;
            default:
                break;
        }
    } else if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(WIFI_TAG, "STA started");
                if (!provisioning) {
                    provisioning = true;

                    /* check if device has been provisioned */
                    wifi_config_t wifi_cfg;
                    esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);
                    if (strlen((const char *)wifi_cfg.sta.ssid)) {
                        provisioned = true;
                    }

                    if (provisioned) {
                        ESP_LOGI(WIFI_TAG, "already provisioned, connecting..");
                        provisioning = false;
                        esp_wifi_connect();
                    } else {
                        ESP_LOGI(WIFI_TAG,
                                 "not provisioned, starting provisioner..");
                        startProvisioning();
                    }
                }
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                wifiConnectionAttempts++;
                ESP_LOGI(WIFI_TAG, "STA disconnected");
                if (wifiConnectionAttempts > 5 && !provisioning) {
                    ESP_LOGI(WIFI_TAG,
                             "failure count reached, restarting provisioner..");
                    provisioning = true;
                    startProvisioning();
                }
                ESP_LOGI(WIFI_TAG, "STA reconnecting..");
                esp_wifi_connect();
                break;
            default:
                break;
        }
    } else if(event_base == IP_EVENT) {
        if(event_id == IP_EVENT_STA_GOT_IP) {
            ESP_LOGI(WIFI_TAG, "STA connected!");
        }
    }
}

// MARK: Worker Task
void Worker_Task(void *arg) {
    workerQueue = xQueueCreate(5, sizeof(workerQueueItem));
    if (workerQueue == 0) {
        printf("Failed to create queue= %p\n", workerQueue);
    }

    workerQueueItem workItem;
    while (1) {
        if (xQueueReceive(workerQueue, &(workItem), (TickType_t)5)) {
            printf("Received data from queue == %d/n", workItem.type);
        }
        vTaskDelay(200 / portTICK_RATE_MS);
    }
}

// MARK: Matrix Task
void Matrix_Task(void *arg) {
    HUB75_I2S_CFG::i2s_pins _pins = {25, 26, 27, 14, 12, 13, 23,
                                     19, 5,  17, -1, 4,  15, 16};
    HUB75_I2S_CFG mxconfig(64, 32, 1, _pins);
    MatrixPanel_I2S_DMA matrix = MatrixPanel_I2S_DMA(mxconfig);
    matrix.begin();

    while (1) {
        vTaskDelay(5000 / portTICK_RATE_MS);
        ESP_LOGI(MATRIX_TAG, "Free heap: %d, largest free block: %d",
                 (int)esp_get_free_internal_heap_size(),
                 (int)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    }
}

extern "C" void app_main(void) {
    /* Initialize NVS partition */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* Initialize TCP/IP */
    ESP_ERROR_CHECK(esp_netif_init());

    /* Initialize the event loop */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &event_handler, NULL));

    xTaskCreate(Worker_Task, "WorkerTask", 5000, NULL, 0, &workerTask);
    xTaskCreate(Matrix_Task, "MatrixTask", 5000, NULL, 0, &matrixTask);

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(wifi_mode_t::WIFI_MODE_STA));

    ESP_ERROR_CHECK(esp_wifi_start());
}
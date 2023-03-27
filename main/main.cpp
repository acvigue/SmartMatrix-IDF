#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <cJSON.h>
#include <esp_event.h>
#include <esp_littlefs.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <mqtt_client.h>
#include <nvs_flash.h>
#include <stdio.h>
#include <string.h>
#include <webp/demux.h>
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_ble.h>

#include "constants.h"

uint8_t spriteBuffer[50000];
uint8_t decodedBuffer[4 * MATRIX_WIDTH * MATRIX_HEIGHT];
WebPData webpData;
WebPDemuxer *webpDemux;
WebPIterator webpIterator;

HUB75_I2S_CFG::i2s_pins _pins = {25, 26, 27, 14, 12, 13, 23,
                                 19, 5,  17, -1, 4,  15, 16};
HUB75_I2S_CFG mxconfig(64, 32, 1, _pins);
MatrixPanel_I2S_DMA matrix = MatrixPanel_I2S_DMA(mxconfig);

char serverCert[1850];
char clientCert[1850];
char clientKey[1850];

TaskHandle_t matrixTask;
TaskHandle_t workerTask;
QueueHandle_t workerQueue;
esp_mqtt_client_handle_t client;

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

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
    ESP_LOGD(MQTT_TAG,
             "Event dispatched from event loop base=%s, event_id=%" PRIi32,
             base, event_id);
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    esp_mqtt_client_handle_t client = event->client;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(MQTT_TAG, "connected to cloud");

            char device_id[7];
            char tmpTopic[40];
            uint8_t eth_mac[6];
            esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
            snprintf(device_id, 7, "%02X%02X%02X", eth_mac[3], eth_mac[4],
                     eth_mac[5]);
            sprintf(tmpTopic, "smartmatrix/%s/command", device_id);
            esp_mqtt_client_subscribe(client, tmpTopic, 1);
            sprintf(tmpTopic, "smartmatrix/%s/applet", device_id);
            esp_mqtt_client_subscribe(client, tmpTopic, 1);
            sprintf(tmpTopic, "smartmatrix/%s/schedule", device_id);
            esp_mqtt_client_subscribe(client, tmpTopic, 1);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(MQTT_TAG, "mqtt disconnected");
            break;
        case MQTT_EVENT_DATA:
            if (strstr(event->topic, "command")) {
                cJSON *root = cJSON_Parse(event->data);
                char *type = cJSON_GetObjectItem(root, "type")->valuestring;
                if (strcmp(type, "download_sprite") == 0) {
                    cJSON *params = cJSON_GetObjectItem(root, "params");
                    char *spriteURL =
                        cJSON_GetObjectItem(params, "url")->valuestring;
                    int spriteID =
                        cJSON_GetObjectItem(params, "spriteID")->valueint;

                    /* tell worker to download the sprite */
                    workerQueueItem workItem = {
                        .type = WORKITEM_TYPE_DOWNLOAD_SPRITE,
                        .numericParameter = spriteID,
                    };
                    strcpy(workItem.charParameter, spriteURL);
                    xQueueSend(workerQueue, &workItem, 100);
                } else {
                    ESP_LOGE(MQTT_TAG, "Received unknown command: %s", type);
                }
                cJSON_Delete(root);
            } else if(strstr(event->topic, "applet")) {
                ESP_LOGI(MQTT_TAG, "applet: %d: %s", event->data_len, event->data);
            }
            break;
        default:
            break;
    }
}

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

                /* show setup sprite */
                workerQueueItem workItem = {
                    .type = WORKITEM_TYPE_SHOW_SPRITE,
                };
                strcpy(workItem.charParameter, "setup");
                xQueueSend(workerQueue, &workItem, 100);
                break;
        }
    } else if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START: {
                ESP_LOGI(WIFI_TAG, "STA started");
                /* show connect wifi sprite */
                workerQueueItem workItem = {
                    .type = WORKITEM_TYPE_SHOW_SPRITE,
                };
                strcpy(workItem.charParameter, "connect_wifi");
                xQueueSend(workerQueue, &workItem, 100);

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
            }
            case WIFI_EVENT_STA_DISCONNECTED: {
                wifiConnectionAttempts++;
                ESP_LOGI(WIFI_TAG, "STA disconnected");
                esp_mqtt_client_stop(client);
                if (wifiConnectionAttempts > 5 && !provisioning) {
                    ESP_LOGI(WIFI_TAG,
                             "failure count reached, restarting provisioner..");
                    provisioning = true;
                    startProvisioning();
                }
                ESP_LOGI(WIFI_TAG, "STA reconnecting..");
                esp_wifi_connect();
                break;
            }
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            wifiConnectionAttempts = 0;
            ESP_LOGI(WIFI_TAG, "STA connected!");

            /* show connect_cloud sprite */
            workerQueueItem workItem = {
                .type = WORKITEM_TYPE_SHOW_SPRITE,
            };
            strcpy(workItem.charParameter, "connect_cloud");
            xQueueSend(workerQueue, &workItem, 100);

            esp_mqtt_client_start(client);
        }
    }
}

// MARK: Worker Task
void Worker_Task(void *arg) {
    workerQueueItem workItem;
    while (1) {
        if (xQueueReceive(workerQueue, &(workItem), (TickType_t)9999)) {
            if (workItem.type == WORKITEM_TYPE_SHOW_SPRITE) {
                char fileName[40];
                snprintf(fileName, 40, "/littlefs/%s.webp",
                         workItem.charParameter);
                FILE *f = fopen(fileName, "r");
                if (f == NULL) {
                    ESP_LOGW(WORKER_TAG, "couldn't find sprite %s", fileName);
                    continue;
                }

                fseek(f, 0, SEEK_END);
                size_t fileSize = ftell(f);
                fseek(f, 0, SEEK_SET);

                if (fileSize > sizeof(spriteBuffer)) {
                    ESP_LOGE(WORKER_TAG, "sprite %s overflows buffer: %d",
                             workItem.charParameter, fileSize);
                    continue;
                }

                /* Tell the matrix task to pause while we're moving stuff around
                 */
                xTaskNotify(matrixTask, MATRIX_TASK_NOTIF_NOT_READY,
                            eSetValueWithOverwrite);

                fread(spriteBuffer, fileSize, fileSize, f);

                webpData.bytes = spriteBuffer;
                webpData.size = fileSize;

                xTaskNotify(matrixTask, MATRIX_TASK_NOTIF_READY,
                            eSetValueWithOverwrite);
            }
        }
        ESP_LOGI(WORKER_TAG, "watermark: %d",
                 uxTaskGetStackHighWaterMark(NULL));
    }
}

void Matrix_Task(void *arg) {
    bool isReady = false;
    int frameCount = 0;
    int currentFrame = 0;
    uint32_t lastFrameDuration = 0;

    while (1) {
        uint32_t notifiedValue;
        uint32_t timeToWait = lastFrameDuration;
        if (timeToWait == 0) {
            timeToWait = 50;
        }

        if (xTaskNotifyWait(pdTRUE, pdTRUE, &notifiedValue,
                            pdMS_TO_TICKS(timeToWait))) {
            if (notifiedValue == MATRIX_TASK_NOTIF_NOT_READY) {
                WebPDemuxReleaseIterator(&webpIterator);
                WebPDemuxDelete(webpDemux);
                isReady = false;
            } else if (notifiedValue == MATRIX_TASK_NOTIF_READY) {
                webpDemux = WebPDemux(&webpData);
                frameCount = WebPDemuxGetI(webpDemux, WEBP_FF_FRAME_COUNT);
                currentFrame = 1;
                isReady = true;
            }
        }

        if (isReady) {
            if (WebPDemuxGetFrame(webpDemux, currentFrame, &webpIterator)) {
                if (WebPDecodeRGBAInto(
                        webpIterator.fragment.bytes, webpIterator.fragment.size,
                        decodedBuffer,
                        webpIterator.width * webpIterator.height * 4,
                        webpIterator.width * 4) != NULL) {
                    int px = 0;
                    for (int y = webpIterator.y_offset;
                         y < (webpIterator.y_offset + webpIterator.height);
                         y++) {
                        for (int x = webpIterator.x_offset;
                             x < (webpIterator.x_offset + webpIterator.width);
                             x++) {
                            // go pixel by pixel.

                            int pixelOffsetFT = px * 4;
                            int alphaValue = decodedBuffer[pixelOffsetFT + 3];

                            if (alphaValue == 255) {
                                matrix.drawPixel(
                                    x, y,
                                    matrix.color565(
                                        decodedBuffer[pixelOffsetFT],
                                        decodedBuffer[pixelOffsetFT + 1],
                                        decodedBuffer[pixelOffsetFT + 2]));
                            }

                            px++;
                        }
                    }

                    currentFrame++;
                    lastFrameDuration = webpIterator.duration;
                    if (currentFrame > frameCount) {
                        currentFrame = 1;
                    }
                }
            } else {
                ESP_LOGE(MATRIX_TAG, "couldn't get frame");
                isReady = false;
            }
        }
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

    /* Initialize LittleFS */
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/littlefs",
        .partition_label = "littlefs",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    if (esp_vfs_littlefs_register(&conf) != ESP_OK) {
        return;
    }

    /*Create worker queue & tasks */
    workerQueue = xQueueCreate(5, sizeof(workerQueueItem));
    if (workerQueue == 0) {
        printf("Failed to create queue= %p\n", workerQueue);
    }
    matrix.begin();
    xTaskCreate(Worker_Task, "WorkerTask", 2500, NULL, 0, &workerTask);
    xTaskCreatePinnedToCore(Matrix_Task, "MatrixTask", 3500, NULL, 30,
                            &matrixTask, 1);

    /* show boot sprite */
    workerQueueItem workItem = {
        .type = WORKITEM_TYPE_SHOW_SPRITE,
    };
    strcpy(workItem.charParameter, "boot");
    xQueueSend(workerQueue, &workItem, 100);

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

    // Force boot anim to finish.
    vTaskDelay(pdMS_TO_TICKS(5700));

    /* Initialize WiFi, this will start the provisioner/mqtt client as
     * necessary */
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(wifi_mode_t::WIFI_MODE_STA));

    ESP_ERROR_CHECK(esp_wifi_start());

    /* Read out certs from FS */
    FILE *f = fopen("/littlefs/certs/LetsEncryptR3.pem", "r");
    if (f == NULL) {
        ESP_LOGE(BOOT_TAG, "Failed to open file for reading");
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    fread(serverCert, size, size, f);
    fclose(f);

    f = fopen("/littlefs/certs/clientAuth.pem", "r");
    if (f == NULL) {
        ESP_LOGE(BOOT_TAG, "Failed to open file for reading");
        return;
    }

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    fread(clientCert, size, size, f);
    fclose(f);

    f = fopen("/littlefs/certs/clientAuth.key", "r");
    if (f == NULL) {
        ESP_LOGE(BOOT_TAG, "Failed to open file for reading");
        return;
    }

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    fread(clientKey, size, size, f);
    fclose(f);

    /* Setup MQTT */
    const esp_mqtt_client_config_t mqtt_cfg = {
        .uri = "mqtts://***REDACTED***:443",
        .cert_pem = serverCert,
        .client_cert_pem = clientCert,
        .client_key_pem = clientKey,
    };

    client = esp_mqtt_client_init(&mqtt_cfg);

    esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqtt_event_handler,
                                   NULL);

    while (1) {
        ESP_LOGI(BOOT_TAG, "free heap: %d, contigious: %d",
                 esp_get_free_heap_size(),
                 heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        vTaskDelay(2500 / portTICK_RATE_MS);
    }
}
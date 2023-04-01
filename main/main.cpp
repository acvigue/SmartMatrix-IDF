#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <cJSON.h>
#include <esp_event.h>
#include <esp_http_client.h>
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

static uint8_t spriteBuffer[50000];
static uint8_t decBuffer[4 * MATRIX_WIDTH * MATRIX_HEIGHT];
static WebPData iData;
WebPDemuxer *demux;
static WebPIterator iter;

HUB75_I2S_CFG::i2s_pins _pins = {25, 26, 27, 14, 12, 13, 23,
                                 19, 5,  17, -1, 4,  15, 16};
HUB75_I2S_CFG mxconfig(64, 32, 1, _pins);
MatrixPanel_I2S_DMA matrix = MatrixPanel_I2S_DMA(mxconfig);

char serverCert[1850];
char clientCert[1850];
char clientKey[1850];
char statusTopic[40];

TaskHandle_t matrixTask;
TaskHandle_t workerTask;
QueueHandle_t workerQueue;
esp_mqtt_client_handle_t mqttClient;

int currentlyShowingSprite = -1;
int currentSpriteDuration = 0;
unsigned long currentSpriteStartTime;
int scheduledSpriteCount = 0;
scheduledSprite scheduledSprites[100];

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

esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    static int current_data_offset;  // Stores number of bytes read
    switch (evt->event_id) {
        case HTTP_EVENT_ON_HEADER: {
            if (strcmp("ETag", evt->header_key) == 0) {
                strncpy(((httpDownloadItem *)evt->user_data)->receivedHash,
                        evt->header_value + 1, 32);
                char hashFileName[30];
                snprintf(hashFileName, 30, "/littlefs/sprites/%d.hash",
                         ((httpDownloadItem *)evt->user_data)->spriteID);
                FILE *existingHashFile = fopen(hashFileName, "r");
                if (existingHashFile != NULL) {
                    char existingHash[33];
                    fread(existingHash, 1, 33, existingHashFile);
                    if (strcmp(existingHash,
                               ((httpDownloadItem *)evt->user_data)
                                   ->receivedHash) == 0) {
                        ESP_LOGI(
                            HTTP_TAG,
                            "ETag hash matches stored sprite %d, skipping.",
                            ((httpDownloadItem *)evt->user_data)->spriteID);
                        ((httpDownloadItem *)evt->user_data)->shouldDownload =
                            false;
                        esp_http_client_close(evt->client);
                    }
                } else {
                    ESP_LOGW(HTTP_TAG, "couldn't open existing hash file: %s",
                             hashFileName);
                }
            }
            break;
        }
        case HTTP_EVENT_ON_DATA: {
            bool shouldDownload =
                ((httpDownloadItem *)evt->user_data)->shouldDownload;
            if (shouldDownload) {
                char tmpFileName[35];
                snprintf(tmpFileName, 35, "/littlefs/sprites/%d.webp.tmp",
                         ((httpDownloadItem *)evt->user_data)->spriteID);
                FILE *spriteFile = fopen(tmpFileName, "a");
                if (spriteFile != NULL) {
                    fwrite(evt->data, 1, evt->data_len, spriteFile);
                    fclose(spriteFile);

                    int total = esp_http_client_get_content_length(evt->client);
                    ESP_LOGD(HTTP_TAG, "HTTP_EVENT_ON_DATA, len=%d, offset=%d",
                             evt->data_len, current_data_offset);

                    if (current_data_offset + evt->data_len >= total) {
                        ESP_LOGD(
                            HTTP_TAG, "recv'd %d bytes of data for sprite %d",
                            total,
                            ((httpDownloadItem *)evt->user_data)->spriteID);
                        char newFileName[35];
                        snprintf(
                            newFileName, 35, "/littlefs/sprites/%d.webp",
                            ((httpDownloadItem *)evt->user_data)->spriteID);
                        if (rename(tmpFileName, newFileName) == 0) {
                            char hashFileName[30];
                            snprintf(
                                hashFileName, 30, "/littlefs/sprites/%d.hash",
                                ((httpDownloadItem *)evt->user_data)->spriteID);
                            FILE *hashFile = fopen(hashFileName, "w");
                            if (hashFile != NULL) {
                                fwrite(((httpDownloadItem *)evt->user_data)
                                           ->receivedHash,
                                       1, 33, hashFile);
                                fclose(hashFile);
                                ESP_LOGD(HTTP_TAG,
                                         "wrote new hash for sprite %d: %s",
                                         ((httpDownloadItem *)evt->user_data)
                                             ->spriteID,
                                         hashFileName);
                            } else {
                                ESP_LOGE(HTTP_TAG,
                                         "couldn't open new hash file for "
                                         "writing: %s",
                                         hashFileName);
                            }
                        } else {
                            ESP_LOGE(HTTP_TAG, "couldn't rename sprite file");
                        }
                    }
                    current_data_offset += evt->data_len;
                } else {
                    ESP_LOGE(HTTP_TAG,
                             "couldn't open sprite file for writing: %s",
                             tmpFileName);
                }
            }
            break;
        }
        case HTTP_EVENT_ON_FINISH: {
            ESP_LOGD(HTTP_TAG, "HTTP_EVENT_ON_FINISH");
            current_data_offset = 0;
            break;
        }
        default:
            break;
    }
    return ESP_OK;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    esp_mqtt_client_handle_t mqttClient = event->client;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(MQTT_TAG, "connected to cloud");

            char tmpTopic[40];
            uint8_t eth_mac[6];
            char device_id[7];
            esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
            snprintf(device_id, 7, "%02X%02X%02X", eth_mac[3], eth_mac[4],
                     eth_mac[5]);
            sprintf(tmpTopic, "smartmatrix/%s/command", device_id);
            esp_mqtt_client_subscribe(mqttClient, tmpTopic, 1);
            sprintf(tmpTopic, "smartmatrix/%s/schedule", device_id);
            esp_mqtt_client_subscribe(mqttClient, tmpTopic, 1);
            sprintf(statusTopic, "smartmatrix/%s/status", device_id);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(MQTT_TAG, "mqtt disconnected");
            esp_mqtt_client_reconnect(mqttClient);
            break;
        case MQTT_EVENT_DATA:
            if (strstr(event->topic, "command")) {
                cJSON *root = cJSON_Parse(event->data);
                if (!cJSON_HasObjectItem(root, "type")) {
                    ESP_LOGE(MQTT_TAG, "command event had no command!");
                    break;
                }
                char *type = cJSON_GetObjectItem(root, "type")->valuestring;
                if (strcmp(type, "new_sprite") == 0) {
                    cJSON *params = cJSON_GetObjectItem(root, "params");

                    workerQueueItem workItem = {
                        .type = WORKITEM_TYPE_DOWNLOAD_SPRITE,
                        .numericParameter =
                            cJSON_GetObjectItem(params, "spriteID")->valueint,
                    };

                    strcpy(workItem.charParameter,
                           cJSON_GetObjectItem(params, "url")->valuestring);

                    xQueueSend(workerQueue, &workItem, 100);
                } else if (strcmp(type, "new_schedule") == 0) {
                    cJSON *schedule = cJSON_GetObjectItem(root, "schedule");
                    const char *hash =
                        cJSON_GetObjectItem(root, "hash")->valuestring;
                    for (int i = 0; i < cJSON_GetArraySize(schedule); i++) {
                        cJSON *event = cJSON_GetArrayItem(schedule, i);
                        scheduledSprites[i].duration =
                            cJSON_GetObjectItem(event, "d")->valueint;
                        scheduledSprites[i].skipped =
                            cJSON_GetObjectItem(event, "s")->valueint;
                        scheduledSprites[i].pinned =
                            cJSON_GetObjectItem(event, "p")->valueint;
                    }
                    scheduledSpriteCount = cJSON_GetArraySize(schedule);
                    ESP_LOGI(MQTT_TAG, "received %d items for schedule",
                             scheduledSpriteCount);
                    char resp[100];
                    snprintf(resp, 100,
                             "{\"type\":\"schedule_loaded\",\"hash\":\"%s\"}",
                             hash);
                    esp_mqtt_client_publish(mqttClient, statusTopic, resp, 0, 1,
                                            0);
                } else {
                    ESP_LOGE(MQTT_TAG, "Received unknown command: %s", type);
                }
                cJSON_Delete(root);
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
                esp_mqtt_client_stop(mqttClient);
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

            esp_mqtt_client_start(mqttClient);
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
                    char resp[200];
                    snprintf(resp, 200,
                             "{\"type\":\"sprite_not_found\",\"params\":{"
                             "\"id\":%d}}",
                             workItem.numericParameter);
                    esp_mqtt_client_publish(mqttClient, statusTopic, resp, 0, 1,
                                            0);
                    continue;
                }

                fseek(f, 0, SEEK_END);
                size_t fileSize = ftell(f);
                fseek(f, 0, SEEK_SET);

                xTaskNotify(matrixTask, MATRIX_TASK_NOTIF_NOT_READY,
                            eSetValueWithOverwrite);

                fread(spriteBuffer, 1, fileSize, f);

                iData.bytes = spriteBuffer;
                iData.size = fileSize;

                xTaskNotify(matrixTask, MATRIX_TASK_NOTIF_READY,
                            eSetValueWithOverwrite);

                char resp[200];
                snprintf(resp, 200,
                         "{\"type\":\"sprite_shown\",\"params\":{\"id\":%d}}",
                         workItem.numericParameter);
                esp_mqtt_client_publish(mqttClient, statusTopic, resp, 0, 1, 0);
            } else if (workItem.type == WORKITEM_TYPE_DOWNLOAD_SPRITE) {
                ESP_LOGD(WORKER_TAG, "receiving sprite %d: %s",
                         workItem.numericParameter, workItem.charParameter);

                httpDownloadItem httpItem = {
                    .spriteID = workItem.numericParameter,
                    .shouldDownload = true,
                };

                esp_http_client_config_t config = {
                    .url = workItem.charParameter,
                    .event_handler = _http_event_handler,
                    .buffer_size = 2048,
                    .user_data = (void *)&httpItem,
                    .skip_cert_common_name_check = true,
                };

                esp_http_client_handle_t client = esp_http_client_init(&config);

                esp_http_client_perform(client);
                esp_http_client_cleanup(client);

                char resp[200];
                snprintf(resp, 200,
                         "{\"type\":\"sprite_loaded\",\"params\":{\"id\":%"
                         "d,\"hash\":\"%s\"}}",
                         workItem.numericParameter, httpItem.receivedHash);
                esp_mqtt_client_publish(mqttClient, statusTopic, resp, 0, 1, 0);
            }
        }
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
                WebPDemuxReleaseIterator(&iter);
                WebPDemuxDelete(demux);
                isReady = false;
            } else if (notifiedValue == MATRIX_TASK_NOTIF_READY) {
                demux = WebPDemux(&iData);
                if(demux == NULL) {
                    ESP_LOGE(MATRIX_TAG, "couldn't create demuxer!");
                    continue;
                }
                frameCount = WebPDemuxGetI(demux, WEBP_FF_FRAME_COUNT);
                currentFrame = 1;
                isReady = true;
            }
        }

        if (isReady) {
            if (WebPDemuxGetFrame(demux, currentFrame, &iter)) {
                if (WebPDecodeRGBAInto(
                        iter.fragment.bytes, iter.fragment.size,
                        decBuffer,
                        iter.width * iter.height * 4,
                        iter.width * 4) != NULL) {
                    int px = 0;
                    for (int y = iter.y_offset;
                         y < (iter.y_offset + iter.height);
                         y++) {
                        for (int x = iter.x_offset;
                             x < (iter.x_offset + iter.width);
                             x++) {

                            int pixelOffsetFT = px * 4;
                            int alphaValue = decBuffer[pixelOffsetFT + 3];

                            if (alphaValue == 255) {
                                matrix.drawPixel(
                                    x, y,
                                    matrix.color565(
                                        decBuffer[pixelOffsetFT],
                                        decBuffer[pixelOffsetFT + 1],
                                        decBuffer[pixelOffsetFT + 2]));
                            }

                            px++;
                        }
                    }

                    currentFrame++;
                    lastFrameDuration = iter.duration;
                    if (currentFrame > frameCount) {
                        currentFrame = 1;
                    }
                } else {
                    ESP_LOGE(MATRIX_TAG, "couldn't decode frame %d",
                             currentFrame);
                    WebPDemuxReleaseIterator(&iter);
                    WebPDemuxDelete(demux);
                    isReady = false;
                    xTaskNotify(matrixTask, MATRIX_TASK_NOTIF_READY,
                                eSetValueWithOverwrite);
                }
            } else {
                ESP_LOGE(MATRIX_TAG, "couldn't get frame %d", currentFrame + 1);
                WebPDemuxReleaseIterator(&iter);
                WebPDemuxDelete(demux);
                isReady = false;
                xTaskNotify(matrixTask, MATRIX_TASK_NOTIF_READY,
                            eSetValueWithOverwrite);
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
    } else {
        mkdir("/littlefs/sprites", 0775);
    }

    /*Create worker queue & tasks */
    workerQueue = xQueueCreate(5, sizeof(workerQueueItem));
    if (workerQueue == 0) {
        printf("Failed to create queue= %p\n", workerQueue);
    }
    matrix.begin();
    xTaskCreate(Worker_Task, "WorkerTask", 3500, NULL, 0, &workerTask);
    xTaskCreate(Matrix_Task, "MatrixTask", 3500, NULL, 10,
                            &matrixTask);

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

    fread(serverCert, 1, size, f);
    fclose(f);

    f = fopen("/littlefs/certs/clientAuth.pem", "r");
    if (f == NULL) {
        ESP_LOGE(BOOT_TAG, "Failed to open file for reading");
        return;
    }

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    fread(clientCert, 1, size, f);
    fclose(f);

    f = fopen("/littlefs/certs/clientAuth.key", "r");
    if (f == NULL) {
        ESP_LOGE(BOOT_TAG, "Failed to open file for reading");
        return;
    }

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    fread(clientKey, 1, size, f);
    fclose(f);

    /* Setup MQTT */
    const esp_mqtt_client_config_t mqtt_cfg = {
        .uri = "mqtt://***REDACTED***:1883",
        .username = "***REDACTED***",
        .password = "***REDACTED***"
    };

    mqttClient = esp_mqtt_client_init(&mqtt_cfg);

    esp_mqtt_client_register_event(mqttClient, MQTT_EVENT_ANY,
                                   mqtt_event_handler, NULL);

    int scheduleCheckID = -1;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(BOOT_TAG, "heap: %d", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        if (pdTICKS_TO_MS(xTaskGetTickCount()) - currentSpriteStartTime >
            currentSpriteDuration) {
            if (scheduledSpriteCount == 0) {
                continue;
            }

            int tcID = scheduleCheckID;
            scheduleCheckID++;

            if (scheduleCheckID >= scheduledSpriteCount) {
                scheduleCheckID = 0;
                tcID = scheduledSpriteCount - 1;
            }

            bool currentAppletPinned = scheduledSprites[tcID].pinned;
            bool skipApplet = scheduledSprites[scheduleCheckID].skipped;
            int duration = scheduledSprites[scheduleCheckID].duration;

            if (currentAppletPinned) {
                continue;
            }

            if (skipApplet) {
                continue;
            }

            ESP_LOGI(SCHEDULE_TAG, "showing %d", scheduleCheckID);

            char newAppletName[15];
            snprintf(newAppletName, 15, "sprites/%d", scheduleCheckID);

            /* show connect_cloud sprite */
            workerQueueItem workItem = {
                .type = WORKITEM_TYPE_SHOW_SPRITE,
            };
            strcpy(workItem.charParameter, newAppletName);
            xQueueSend(workerQueue, &workItem, 100);

            currentlyShowingSprite = scheduleCheckID;
            currentSpriteStartTime = pdTICKS_TO_MS(xTaskGetTickCount());
            currentSpriteDuration =
                scheduledSprites[currentlyShowingSprite].duration * 1000;
        }
    }
}
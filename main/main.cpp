#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_crt_bundle.h>
#include <esp_event.h>
#include <esp_littlefs.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <lwip/ip4_addr.h>
#include <mbedtls/base64.h>
#include <mqtt_client.h>
#include <nvs_flash.h>
#include <stdio.h>
#include <tsl2561.h>
#include <webp/demux.h>
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_ble.h>

#include "constants.h"
#include "secrets.h"

WebPData webPData;
esp_mqtt_client_handle_t mqttClient;

scheduledItem *scheduledItems = nullptr;

QueueHandle_t xWorkerQueue, xMqttMessageQueue;
TaskHandle_t workerTask, mqttMsgTask, matrixTask, scheduleTask;
TimerHandle_t tslTimer, brightnessTimer;

HUB75_I2S_CFG::i2s_pins _pins = {R1_PIN, G1_PIN, B1_PIN, R2_PIN, G2_PIN, B2_PIN, A_PIN, B_PIN, C_PIN, D_PIN, E_PIN, LAT_PIN, OE_PIN, CLK_PIN};
HUB75_I2S_CFG mxconfig(64, 32, 1, _pins);
MatrixPanel_I2S_DMA matrix = MatrixPanel_I2S_DMA(mxconfig);
tsl2561_t tslSensor;

char thing_name[18];
int currentlyDisplayingSprite, desiredBrightness, currentBrightness = 0;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    static char lastTopic[200];
    static int failureCount = 0;
    static char *dataBuf = nullptr;
    static bool hasConnected = false;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED: {
            ESP_LOGI(MQTT_TAG, "connected to broker..");
            failureCount = 0;

            // Device shadow topics.
            char tmpTopic[200];

            sprintf(tmpTopic, "smartmatrix/%s/schedule_delivery", thing_name);
            esp_mqtt_client_subscribe(event->client, tmpTopic, 1);

            sprintf(tmpTopic, "smartmatrix/%s/sprite_delivery", thing_name);
            esp_mqtt_client_subscribe(event->client, tmpTopic, 1);

            // Request to get the initial shadow state
            sprintf(tmpTopic, "smartmatrix/%s/status", thing_name);
            esp_mqtt_client_publish(event->client, tmpTopic, "{\"type\":\"get_schedule\"}", 0, 0, false);
            if (!hasConnected) {
                workItem newWorkItem;
                newWorkItem.workItemType = WorkItemType::SHOW_SPRITE;
                strcpy(newWorkItem.workItemString, "ready");
                xQueueSendToFront(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));
                hasConnected = true;
            }
            break;
        }
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(MQTT_TAG, "mqtt disconnected");
            failureCount++;
            if (failureCount > 5) {
                esp_restart();
            }
            break;
        case MQTT_EVENT_DATA:
            if (event->topic_len != 0) {
                memset(lastTopic, 0, 200);
                strncpy(lastTopic, event->topic, event->topic_len);
            }

            // fill buffer
            if (event->current_data_offset == 0) {
                ESP_LOGD(MQTT_TAG, "creating message buffer for topic %s (size %d)", lastTopic, event->total_data_len);
                dataBuf = (char *)heap_caps_malloc(event->total_data_len, MALLOC_CAP_SPIRAM);
            }

            memcpy((void *)(dataBuf + event->current_data_offset), event->data, event->data_len);
            if (event->data_len + event->current_data_offset >= event->total_data_len) {
                char *messagePtr = dataBuf;

                mqttMessage newMessageItem;
                newMessageItem.messageLen = event->total_data_len;
                newMessageItem.pMessage = messagePtr;
                strcpy(newMessageItem.topic, lastTopic);
                if (xQueueSend(xMqttMessageQueue, &(newMessageItem), pdMS_TO_TICKS(1000)) != pdTRUE) {
                    ESP_LOGE(MQTT_TAG, "couldn't post message for topic %s (len %d) to queue!", lastTopic, event->total_data_len);
                }
            }
            break;
        default:
            break;
    }
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    static uint8_t wifiConnectionAttempts;
    static bool provisioned;
    static bool provisioning;

    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
            case WIFI_PROV_CRED_FAIL: {
                ESP_LOGE(PROV_TAG, "provisioning error");

                workItem newWorkItem;
                newWorkItem.workItemType = WorkItemType::SHOW_SPRITE;
                strcpy(newWorkItem.workItemString, "cred_failed");
                xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));
                wifi_prov_mgr_reset_sm_state_on_failure();

                break;
            }
            case WIFI_PROV_CRED_SUCCESS: {
                ESP_LOGI(PROV_TAG, "provisioning successful");
                provisioned = true;

                break;
            }
            case WIFI_PROV_END: {
                provisioning = false;
                ESP_LOGI(PROV_TAG, "provisioning end");
                wifi_prov_mgr_deinit();
                break;
            }
            case WIFI_PROV_START: {
                provisioning = true;
                ESP_LOGI(PROV_TAG, "provisioning started");

                workItem newWorkItem;
                newWorkItem.workItemType = WorkItemType::SHOW_SPRITE;
                strcpy(newWorkItem.workItemString, "setup_wifi");
                xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));
                break;
            }
            case WIFI_PROV_CRED_RECV: {
                wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;

                workItem newWorkItem;
                newWorkItem.workItemType = WorkItemType::SHOW_SPRITE;
                strcpy(newWorkItem.workItemString, "check_cred");
                xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));
                ESP_LOGI(PROV_TAG,
                         "Received Wi-Fi credentials"
                         "\n\tSSID     : %s\n\tPassword : %s",
                         (const char *)wifi_sta_cfg->ssid, (const char *)wifi_sta_cfg->password);
                break;
            }
        }
    } else if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START: {
                ESP_LOGI(WIFI_TAG, "STA started");

                workItem newWorkItem;
                newWorkItem.workItemType = WorkItemType::SHOW_SPRITE;
                strcpy(newWorkItem.workItemString, "connect_wifi");
                xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));

                if (!provisioning) {
                    provisioning = true;

                    /* check if device has been provisioned */
                    wifi_config_t wifi_cfg;
                    esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);
                    if (strlen((const char *)wifi_cfg.sta.ssid)) {
                        provisioned = true;
                    }

                    if (provisioned) {
                        ESP_LOGI(PROV_TAG, "provisioned!");
                        provisioning = false;
                        esp_wifi_connect();
                    } else {
                        ESP_LOGI(PROV_TAG, "not provisioned!");

                        wifi_prov_mgr_config_t config = {.scheme = wifi_prov_scheme_ble,
                                                         .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM};
                        ESP_ERROR_CHECK(wifi_prov_mgr_init(config));
                        ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_0, NULL, thing_name, NULL));
                    }
                }
                break;
            }
            case WIFI_EVENT_STA_DISCONNECTED: {
                wifiConnectionAttempts++;
                ESP_LOGI(WIFI_TAG, "STA disconnected");
                esp_mqtt_client_stop(mqttClient);
                if (wifiConnectionAttempts > 5 && !provisioning) {
                    ESP_LOGI(WIFI_TAG, "failure count (5) reached.");
                    provisioning = true;

                    wifi_prov_mgr_config_t config = {.scheme = wifi_prov_scheme_ble,
                                                     .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM};
                    ESP_ERROR_CHECK(wifi_prov_mgr_init(config));
                    ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_0, NULL, thing_name, NULL));
                }
                ESP_LOGI(WIFI_TAG, "STA reconnecting..");
                esp_wifi_connect();
                break;
            }
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            wifiConnectionAttempts = 0;
            ESP_LOGI(WIFI_TAG, "STA connected, starting MQTT connection");

            workItem newWorkItem;
            newWorkItem.workItemType = WorkItemType::SHOW_SPRITE;
            strcpy(newWorkItem.workItemString, "connect_cloud");
            xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));

            /* Setup MQTT */
            const esp_mqtt_client_config_t mqtt_cfg = {
                .broker = {.address = MQTT_HOST, .verification = {.crt_bundle_attach = esp_crt_bundle_attach}},
                .credentials = {.username = MQTT_USERNAME, .client_id = thing_name, .authentication = {.password = MQTT_PASSWORD}},
                .network =
                    {
                        .reconnect_timeout_ms = 2500,
                        .timeout_ms = 10000,
                    },
                .buffer = {
                    .size = 4096,
                }};

            mqttClient = esp_mqtt_client_init(&mqtt_cfg);

            esp_mqtt_client_register_event(mqttClient, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
            esp_mqtt_client_start(mqttClient);
        }
    }
}

void MqttMsg_Task(void *arg) {
    mqttMessage currentMessage;

    // Create queue
    xMqttMessageQueue = xQueueCreate(5, sizeof(mqttMessage));
    if (xMqttMessageQueue == NULL) {
        ESP_LOGE(MQTT_TASK_TAG, "couldn't create xMqttMessageQueue!!");
        return;
    }

    while (1) {
        if (xMqttMessageQueue != NULL) {
            if (xQueueReceive(xMqttMessageQueue, &(currentMessage), pdMS_TO_TICKS(1000)) == pdPASS) {
                ESP_LOGD(MQTT_TASK_TAG, "got item from queue!");
                if (strstr(currentMessage.topic, "sprite_delivery") != NULL) {
                    cJSON *spriteDeliveryDoc = cJSON_ParseWithLength(currentMessage.pMessage, currentMessage.messageLen);

                    // jobDocument has (spriteID: int, spriteHash: string (33), spriteSize: int, data: string (spriteSize))
                    int spriteID = cJSON_GetObjectItem(spriteDeliveryDoc, "spriteID")->valueint;
                    size_t spriteSize = cJSON_GetObjectItem(spriteDeliveryDoc, "spriteSize")->valueint;
                    size_t encodedSpriteSize = cJSON_GetObjectItem(spriteDeliveryDoc, "encodedSpriteSize")->valueint;

                    uint8_t *tmpBuf = (uint8_t *)heap_caps_malloc(spriteSize, MALLOC_CAP_SPIRAM);
                    const char *encData = cJSON_GetObjectItem(spriteDeliveryDoc, "data")->valuestring;

                    size_t oLen;
                    int decoded = mbedtls_base64_decode(tmpBuf, spriteSize, &oLen, (uint8_t *)encData, encodedSpriteSize);
                    if (decoded == 0) {
                        ESP_LOGI(MQTT_TASK_TAG, "incoming delivery for sprite %d (len %d)", spriteID, spriteSize);

                        // notify the worker task to store the new sprite!
                        workItem newWorkItem;
                        sprintf(newWorkItem.workItemString, "%d", spriteID);
                        newWorkItem.workItemInteger = spriteSize;
                        newWorkItem.workItemType = WorkItemType::STORE_RECEIVED_SPRITE;
                        newWorkItem.pArg = (void *)tmpBuf;
                        xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));
                    } else {
                        heap_caps_free(tmpBuf);
                    }

                    cJSON_Delete(spriteDeliveryDoc);
                } else if (strstr(currentMessage.topic, "schedule_delivery") != NULL) {
                    cJSON *schedule = cJSON_ParseWithLength(currentMessage.pMessage, currentMessage.messageLen);

                    if (schedule != nullptr) {
                        int itemCount = cJSON_GetArraySize(schedule);
                        ESP_LOGI(MQTT_TASK_TAG, "received new schedule with %d items", itemCount);
                        for (int i = 0; i < itemCount; i++) {
                            cJSON *item = cJSON_GetArrayItem(schedule, i);
                            int itemDuration = cJSON_GetObjectItem(item, "duration")->valueint;
                            bool itemPinned = cJSON_GetObjectItem(item, "pinned")->valueint;
                            bool itemSkipped = cJSON_GetObjectItem(item, "skipped")->valueint;
                            scheduledItems[i].show_duration = itemDuration;
                            scheduledItems[i].is_pinned = itemPinned;
                            scheduledItems[i].is_skipped = itemSkipped;
                        }
                        scheduledItems[itemCount].show_duration = 0;
                    }

                    cJSON_Delete(schedule);
                }

                // when we're done w/ the message, free it
                heap_caps_free((void *)currentMessage.pMessage);
            }
        } else {
            break;
        }
    }
}

void Worker_Task(void *arg) {
    workItem currentWorkItem;

    xWorkerQueue = xQueueCreate(10, sizeof(workItem));

    if (xWorkerQueue == NULL) {
        return;
    }

    while (1) {
        if (xWorkerQueue != NULL) {
            if (xQueueReceive(xWorkerQueue, &(currentWorkItem), pdMS_TO_TICKS(1000)) == pdPASS) {
                if (currentWorkItem.workItemType == WorkItemType::SHOW_SPRITE) {
                    if (currentWorkItem.workItemInteger == 1) {
                        // RAM sprite
                        int spriteID = atoi(currentWorkItem.workItemString);
                        ESP_LOGI(WORKER_TAG, "showing RAM sprite at index %d", spriteID);
                        if (scheduledItems[spriteID].pData == nullptr) {
                            ESP_LOGE(WORKER_TAG, "cannot copy buf from nullptr for sprite %d", spriteID);

                            if (scheduledItems[atoi(currentWorkItem.workItemString)].reported_error == false) {
                                scheduledItems[atoi(currentWorkItem.workItemString)].reported_error = true;

                                ESP_LOGD(WORKER_TAG, "reporting error for %d", spriteID);
                                char resp[200];
                                char tmpTopic[200];
                                snprintf(tmpTopic, 200, "smartmatrix/%s/error", thing_name);
                                snprintf(resp, 200, "{\"spriteID\":%d, \"reason\":\"not_found\"}", atoi(currentWorkItem.workItemString));
                                esp_mqtt_client_publish(mqttClient, tmpTopic, resp, 0, 0, false);

                                xTaskNotify(scheduleTask, SCHEDULE_TASK_NOTIF_SKIP_TO_NEXT, eSetValueWithOverwrite);
                            }

                            continue;
                        }

                        ESP_LOGD(WORKER_TAG, "sprite %d found in memory: %d length", spriteID, scheduledItems[spriteID].dataLen);

                        xTaskNotify(matrixTask, MATRIX_TASK_NOTIF_NOT_READY, eSetValueWithOverwrite);
                        WebPDataClear(&webPData);

                        // setup webp buffer and populate from temporary buffer
                        webPData.size = scheduledItems[spriteID].dataLen;
                        webPData.bytes = (uint8_t *)heap_caps_malloc(webPData.size, MALLOC_CAP_SPIRAM);

                        // Fill buffer!
                        memcpy((void *)webPData.bytes, scheduledItems[spriteID].pData, webPData.size);
                    } else {
                        // FS sprite
                        ESP_LOGI(WORKER_TAG, "showing FS sprite: '%s'", currentWorkItem.workItemString);

                        char filePath[40];
                        snprintf(filePath, 40, "/fs/%s.webp", currentWorkItem.workItemString);
                        struct stat st;
                        if (stat(filePath, &st) != 0) {
                            ESP_LOGE(WORKER_TAG, "couldn't find file %s", filePath);
                            continue;
                        }

                        FILE *f = fopen(filePath, "r");

                        // Set buffers
                        xTaskNotify(matrixTask, MATRIX_TASK_NOTIF_NOT_READY, eSetValueWithOverwrite);

                        WebPDataClear(&webPData);

                        fseek(f, 0, SEEK_END);       // seek to end of file
                        size_t fileSize = ftell(f);  // get current file pointer
                        fseek(f, 0, SEEK_SET);       // seek back to beginning of file

                        // setup webp buffer and populate from temporary buffer
                        webPData.size = fileSize;
                        webPData.bytes = (uint8_t *)heap_caps_malloc(fileSize, MALLOC_CAP_SPIRAM);

                        // Fill buffer!
                        fread((void *)webPData.bytes, 1, fileSize, f);
                        fclose(f);
                    }

                    if (strncmp((const char *)webPData.bytes, "RIFF", 4) == 0) {
                        xTaskNotify(matrixTask, MATRIX_TASK_NOTIF_READY, eSetValueWithOverwrite);
                    } else {
                        ESP_LOGE(WORKER_TAG, "magic bytes corrupt, skipping");
                        xTaskNotify(scheduleTask, SCHEDULE_TASK_NOTIF_SKIP_TO_NEXT, eSetValueWithOverwrite);
                    }
                } else if (currentWorkItem.workItemType == WorkItemType::STORE_RECEIVED_SPRITE) {
                    int spriteID = atoi((const char *)currentWorkItem.workItemString);
                    size_t spriteSize = currentWorkItem.workItemInteger;
                    uint8_t *buf = (uint8_t *)currentWorkItem.pArg;

                    ESP_LOGD(WORKER_TAG, "putting sprite %d into RAM, %d bytes", spriteID, spriteSize);

                    // reset buffers if necessary
                    if (scheduledItems[spriteID].pData != nullptr) {
                        heap_caps_free(scheduledItems[spriteID].pData);
                        scheduledItems[spriteID].pData = nullptr;
                        scheduledItems[spriteID].dataLen = 0;
                    }

                    scheduledItems[spriteID].dataLen = spriteSize;
                    scheduledItems[spriteID].pData = (uint8_t *)heap_caps_malloc(spriteSize, MALLOC_CAP_SPIRAM);
                    memcpy((void *)scheduledItems[spriteID].pData, buf, spriteSize);
                    ESP_LOGI(WORKER_TAG, "put %d bytes", spriteSize);

                    // this job is complete, we can mark it
                    heap_caps_free(buf);

                    // show if we're currently showing it already
                    if (currentlyDisplayingSprite == spriteID) {
                        workItem newWorkItem;
                        newWorkItem.workItemType = WorkItemType::SHOW_SPRITE;
                        newWorkItem.workItemInteger = 1;
                        snprintf(newWorkItem.workItemString, 5, "%d", spriteID);
                        xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));
                    }
                }
            }
        }
    }
}

void Matrix_Task(void *arg) {
    bool isReady = false;
    unsigned long animStartTS = 0;
    int lastFrameTimestamp = 0;
    int currentFrame = 0;
    bool isSleeping = false;
    uint32_t notifiedValue;
    WebPAnimDecoder *dec = nullptr;

    while (1) {
        if (xTaskNotifyWait(pdTRUE, pdTRUE, &notifiedValue, pdMS_TO_TICKS(16))) {
            if (notifiedValue == MATRIX_TASK_NOTIF_NOT_READY) {
                isReady = false;
            } else if (notifiedValue == MATRIX_TASK_NOTIF_READY) {
                if (dec != nullptr) {
                    ESP_LOGD(MATRIX_TAG, "recreating decoder");
                    WebPAnimDecoderDelete(dec);
                    dec = nullptr;
                }
                dec = WebPAnimDecoderNew(&webPData, NULL);
                if (dec == NULL) {
                    ESP_LOGE(MATRIX_TAG, "we cannot decode with a null animdec!!!");
                    xTaskNotify(scheduleTask, SCHEDULE_TASK_NOTIF_SKIP_TO_NEXT, eSetValueWithOverwrite);
                    continue;
                }
                animStartTS = pdTICKS_TO_MS(xTaskGetTickCount());
                lastFrameTimestamp = 0;
                currentFrame = 0;
                isReady = true;
            } else if (notifiedValue == MATRIX_TASK_NOTIF_WAKE_UP) {
                isSleeping = false;
            } else if (notifiedValue == MATRIX_TASK_NOTIF_SLEEP) {
                isSleeping = true;
            }
        }

        if (isReady) {
            if (pdTICKS_TO_MS(xTaskGetTickCount()) - animStartTS > lastFrameTimestamp) {
                if (currentFrame == 0) {
                    animStartTS = pdTICKS_TO_MS(xTaskGetTickCount());
                    lastFrameTimestamp = 0;
                }

                bool hasMoreFrames = WebPAnimDecoderHasMoreFrames(dec);
                if (hasMoreFrames) {
                    uint8_t *buf;
                    if (WebPAnimDecoderGetNext(dec, &buf, &lastFrameTimestamp)) {
                        int px = 0;
                        if (!isSleeping) {
                            for (int y = 0; y < MATRIX_HEIGHT; y++) {
                                for (int x = 0; x < MATRIX_WIDTH; x++) {
                                    matrix.drawPixelRGB888(x, y, buf[px * 4], buf[px * 4 + 1], buf[px * 4 + 2]);
                                    px++;
                                }
                            }
                        } else {
                            matrix.fillScreenRGB888(0, 0, 0);
                        }
                        currentFrame++;
                        if (!WebPAnimDecoderHasMoreFrames(dec)) {
                            currentFrame = 0;
                            WebPAnimDecoderReset(dec);
                        }
                    } else {
                        ESP_LOGE(MATRIX_TAG, "we cannot get the next frame!");
                        xTaskNotify(scheduleTask, SCHEDULE_TASK_NOTIF_SKIP_TO_NEXT, eSetValueWithOverwrite);
                        isReady = false;
                    }
                }
            }
        }

        if (currentBrightness != desiredBrightness) {
            if (desiredBrightness < currentBrightness) {
                currentBrightness--;
            } else {
                currentBrightness++;
            }
            matrix.setBrightness((uint8_t)currentBrightness);
        }
    }
}

void Schedule_Task(void *arg) {
    /* Initialize the scheduler items */
    scheduledItems = new scheduledItem[100];
    unsigned long long currentSpriteStartTime = 0;
    unsigned long long lastScheduleRequestTime = 0;
    bool needToSkip = false;

    for (int i = 0; i < 100; i++) {
        scheduledItems[i].show_duration = 0;
        scheduledItems[i].is_pinned = false;
        scheduledItems[i].is_skipped = false;
        scheduledItems[i].reported_error = false;
    }

    uint32_t notifiedValue;

    while (1) {
        if (xTaskNotifyWait(pdTRUE, pdTRUE, &notifiedValue, pdMS_TO_TICKS(1000))) {
            if (notifiedValue == SCHEDULE_TASK_NOTIF_SKIP_TO_NEXT) {
                needToSkip = true;
            }
        }

        if (!mqttClient) {
            continue;
        }

        if (scheduledItems[0].show_duration == 0 || (pdTICKS_TO_MS(xTaskGetTickCount()) - lastScheduleRequestTime > (60 * 5 * 1000))) {
            // we don't have a schedule? or schedule is outdated
            const char *resp = "{\"type\": \"get_schedule\"}";
            char tmpTopic[200];
            snprintf(tmpTopic, 200, "smartmatrix/%s/status", thing_name);
            esp_mqtt_client_publish(mqttClient, tmpTopic, resp, 0, 0, false);
            ESP_LOGI(SCHEDULE_TAG, "requesting latest schedule..");
            lastScheduleRequestTime = pdTICKS_TO_MS(xTaskGetTickCount());

            // only skip loop if no schedule at all
            if (scheduledItems[0].show_duration == 0) {
                continue;
            }
        }

        // main schedule loop
        if ((pdTICKS_TO_MS(xTaskGetTickCount()) - currentSpriteStartTime > (scheduledItems[currentlyDisplayingSprite].show_duration * 1000)) ||
            needToSkip) {
            if (scheduledItems[currentlyDisplayingSprite].is_pinned && !needToSkip) {
                currentSpriteStartTime = pdTICKS_TO_MS(xTaskGetTickCount());
                continue;
            }

            currentlyDisplayingSprite++;

            if (scheduledItems[currentlyDisplayingSprite].show_duration == 0) {
                currentlyDisplayingSprite = 0;
            }

            if (scheduledItems[currentlyDisplayingSprite].is_skipped) {
                continue;
            }

            workItem newWorkItem;
            newWorkItem.workItemType = WorkItemType::SHOW_SPRITE;
            newWorkItem.workItemInteger = 1;
            snprintf(newWorkItem.workItemString, 4, "%d", currentlyDisplayingSprite);
            xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));
            currentSpriteStartTime = pdTICKS_TO_MS(xTaskGetTickCount());

            int nextSpriteID = currentlyDisplayingSprite + 1;
            while (true) {
                if (scheduledItems[nextSpriteID].show_duration == 0) {
                    nextSpriteID = 0;
                }
                if (scheduledItems[nextSpriteID].is_skipped == false) {
                    break;
                }
                ESP_LOGI(SCHEDULE_TAG, "skipping sprite #%d", nextSpriteID);
                nextSpriteID++;
            }

            char resp[200];
            char tmpTopic[200];
            snprintf(tmpTopic, 200, "smartmatrix/%s/status", thing_name);
            const esp_app_desc_t* app_desc = esp_app_get_description();
            snprintf(resp, 200, "{\"type\": \"report\", \"currentSpriteID\":%d, \"nextSpriteID\": %d, \"appVersion\": \"%s\"}",
                     currentlyDisplayingSprite, nextSpriteID, app_desc->version);
            esp_mqtt_client_publish(mqttClient, tmpTopic, resp, 0, 0, false);

            needToSkip = false;
        }
    }
}

extern "C" void app_main(void) {
    /* Start the matrix */
    matrix.begin();
    currentBrightness = 0;
    desiredBrightness = 255;
    matrix.setBrightness8(0);

    /* Initialize NVS partition */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/fs",
        .partition_label = "littlefs",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&conf));

    ESP_ERROR_CHECK(i2cdev_init());

    tsl2561_init_desc(&tslSensor, 0x39, (i2c_port_t)0, (gpio_num_t)7, (gpio_num_t)6);
    tsl2561_init(&tslSensor);

    xTaskCreatePinnedToCore(Worker_Task, "WorkerTask", 3500, NULL, 5, &workerTask, 1);
    xTaskCreatePinnedToCore(MqttMsg_Task, "MqttMsgTask", 4000, NULL, 5, &mqttMsgTask, 1);
    xTaskCreatePinnedToCore(Matrix_Task, "MatrixTask", 4000, NULL, 5, &matrixTask, 1);
    xTaskCreatePinnedToCore(Schedule_Task, "ScheduleTask", 3500, NULL, 5, &scheduleTask, 1);

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(wifi_mode_t::WIFI_MODE_STA));

    esp_netif_set_hostname(netif, thing_name);

    char device_id[7];
    uint8_t eth_mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
    snprintf(device_id, 7, "%02X%02X%02X", eth_mac[3], eth_mac[4], eth_mac[5]);
    snprintf(thing_name, 18, "SmartMatrix%s", device_id);

    workItem newWorkItem;
    newWorkItem.workItemType = WorkItemType::SHOW_SPRITE;
    strcpy(newWorkItem.workItemString, "connect_wifi");
    xQueueSend(xWorkerQueue, &newWorkItem, portMAX_DELAY);

    ESP_ERROR_CHECK(esp_wifi_start());

    while (1) {
        uint32_t lux;
        esp_err_t res;
        if ((res = tsl2561_read_lux(&tslSensor, &lux)) == ESP_OK) {
            if (lux > 5) {
                desiredBrightness = 100;
            } else if (lux > 0) {
                desiredBrightness = 25;
            } else {
                desiredBrightness = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

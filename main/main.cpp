#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <cJSON.h>
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
#include <string.h>
#include <webp/demux.h>
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_ble.h>

#include "constants.h"

WebPData webPData;
esp_mqtt_client_handle_t mqttClient;

scheduledItem *scheduledItems = nullptr;
uint8_t *currentStreamTemporaryBuffer = nullptr;

char thing_name[18];
char currentJobDocument[8192];
char tmpTopic[200];

spriteDeliveryItem currentSpriteDeliveryItem;
QueueHandle_t xWorkerQueue, xMqttMessageQueue, xSpriteDeliveryQueue;
TaskHandle_t workerTask, mqttMsgTask, matrixTask, scheduleTask, spriteDeliveryTask;
MatrixPanel_I2S_DMA *matrix;

bool isStreaming = false;
int currentlyDisplayingSprite = 0;

char *nvs_load_value_if_exist(nvs_handle handle, const char *key) {
    // Try to get the size of the item
    size_t value_size;
    if (nvs_get_str(handle, key, NULL, &value_size) != ESP_OK) {
        ESP_LOGE(BOOT_TAG, "Failed to get size of key: %s", key);
        return NULL;
    }

    char *value = (char *)malloc(value_size);
    if (nvs_get_str(handle, key, value, &value_size) != ESP_OK) {
        ESP_LOGE(BOOT_TAG, "Failed to load key: %s", key);
        return NULL;
    }

    return value;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    static char lastTopic[200];
    static int failureCount = 0;
    static char *dataBuf = nullptr;
    static bool hasConnected = false;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(MQTT_TAG, "connected to iot core..");
            failureCount = 0;

            // Device shadow topics.
            sprintf(tmpTopic, "$aws/things/%s/shadow/get/accepted", thing_name);
            esp_mqtt_client_subscribe(event->client, tmpTopic, 1);
            sprintf(tmpTopic, "$aws/things/%s/shadow/get/rejected", thing_name);
            esp_mqtt_client_subscribe(event->client, tmpTopic, 1);
            sprintf(tmpTopic, "$aws/things/%s/shadow/update/delta", thing_name);
            esp_mqtt_client_subscribe(event->client, tmpTopic, 1);

            // Job topics
            sprintf(tmpTopic, "$aws/things/%s/jobs/notify-next", thing_name);
            esp_mqtt_client_subscribe(event->client, tmpTopic, 1);
            sprintf(tmpTopic, "$aws/things/%s/jobs/get/rejected", thing_name);
            esp_mqtt_client_subscribe(event->client, tmpTopic, 1);
            sprintf(tmpTopic, "$aws/things/%s/jobs/get/accepted", thing_name);
            esp_mqtt_client_subscribe(event->client, tmpTopic, 1);

            // Stream topics
            sprintf(tmpTopic, "$aws/things/%s/streams/+/data/json", thing_name);
            esp_mqtt_client_subscribe(event->client, tmpTopic, 1);
            sprintf(tmpTopic, "$aws/things/%s/streams/+/description/json", thing_name);
            esp_mqtt_client_subscribe(event->client, tmpTopic, 1);

            // Smartmatrix specific topics
            sprintf(tmpTopic, "smartmatrix/%s/sprite_delivery", thing_name);
            esp_mqtt_client_subscribe(event->client, tmpTopic, 1);
            sprintf(tmpTopic, "$aws/things/%s/streams/+/description/json", thing_name);
            esp_mqtt_client_subscribe(event->client, tmpTopic, 1);

            // Request to get the initial shadow state
            sprintf(tmpTopic, "$aws/things/%s/shadow/get", thing_name);
            esp_mqtt_client_publish(event->client, tmpTopic, "{}", 0, 0, false);

            // Request to get the next pending job
            sprintf(tmpTopic, "$aws/things/%s/jobs/$next/get", thing_name);
            char resp[200];
            snprintf(resp, 200, "{\"jobId\":\"$next\",\"thingName\":\"%s\"}", thing_name);
            esp_mqtt_client_publish(event->client, tmpTopic, resp, 0, 0, false);

            if (!hasConnected) {
                workItem newWorkItem;
                newWorkItem.workItemType = WorkItemType::SHOW_SPRITE;
                strcpy(newWorkItem.workItemString, "ready");
                xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));
                hasConnected = true;
            }

            isStreaming = false;
            free(currentStreamTemporaryBuffer);
            break;
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
                dataBuf = (char *)malloc(event->total_data_len);
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
                workItem newWorkItem;
                newWorkItem.workItemType = WorkItemType::SHOW_SPRITE;
                strcpy(newWorkItem.workItemString, "setup");
                xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));
                break;
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
            ESP_LOGI(WIFI_TAG, "STA connected!");

            workItem newWorkItem;
            newWorkItem.workItemType = WorkItemType::SHOW_SPRITE;
            strcpy(newWorkItem.workItemString, "connect_cloud");
            xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));

            char *private_key = nullptr;
            char *certificate = nullptr;

            /* Get TLS client credentials from NVS */

            nvs_flash_init_partition(MFG_PARTITION_NAME);

            nvs_handle handle;
            nvs_open_from_partition(MFG_PARTITION_NAME, "certs", NVS_READONLY, &handle);

            private_key = nvs_load_value_if_exist(handle, "client_key");
            certificate = nvs_load_value_if_exist(handle, "client_cert");
            nvs_close(handle);

            // Check if both items have been correctly retrieved
            if (private_key == NULL || certificate == NULL) {
                ESP_LOGE(MQTT_TAG, "Private key and/or cert could not be loaded");
                esp_restart();
            }

            /* Setup MQTT */
            const esp_mqtt_client_config_t mqtt_cfg = {.broker = {.address = "mqtts://a2o3d87gplncoj-ats.iot.us-east-1.amazonaws.com:8883",
                                                                  .verification =
                                                                      {
                                                                          .crt_bundle_attach = esp_crt_bundle_attach,
                                                                      }},
                                                       .credentials = {.client_id = thing_name,
                                                                       .authentication =
                                                                           {
                                                                               .certificate = certificate,
                                                                               .key = private_key,
                                                                           }},
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

                if (strstr(currentMessage.topic, "shadow") != NULL) {
                    if (strstr(currentMessage.topic, "get/accepted") != NULL || strstr(currentMessage.topic, "update/delta") != NULL) {
                        ESP_LOGD(MQTT_TASK_TAG, "parsing desired shadow");
                        cJSON *shadowDoc = cJSON_ParseWithLength(currentMessage.pMessage, currentMessage.messageLen);
                        cJSON *state = cJSON_GetObjectItem(shadowDoc, "state");

                        cJSON *schedule = nullptr;
                        if (strstr(currentMessage.topic, "get") != NULL) {
                            cJSON *desired = cJSON_GetObjectItem(state, "desired");
                            schedule = cJSON_GetObjectItem(desired, "schedule");
                        } else if (strstr(currentMessage.topic, "delta") != NULL) {
                            schedule = cJSON_GetObjectItem(state, "schedule");
                        }

                        if (schedule != nullptr) {
                            int itemCount = cJSON_GetArraySize(schedule);
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

                            workItem newWorkItem;
                            newWorkItem.workItemType = WorkItemType::UPDATE_REPORTED_SHADOW;
                            xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));
                        }

                        cJSON_Delete(shadowDoc);
                    } else if (strstr(currentMessage.topic, "get/rejected") != NULL) {
                        // AWS rejected our get shadow request, generate and post the reported state.
                        ESP_LOGE(MQTT_TAG, "couldn't fetch shadow, creating one");

                        workItem newWorkItem;
                        newWorkItem.workItemType = WorkItemType::UPDATE_REPORTED_SHADOW;
                        xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));
                    }
                } else if (strstr(currentMessage.topic, "jobs") != NULL) {
                    if (strstr(currentMessage.topic, "notify-next") != NULL || strstr(currentMessage.topic, "get/accepted") != NULL) {
                        cJSON *jobDoc = cJSON_ParseWithLength(currentMessage.pMessage, currentMessage.messageLen);
                        if (cJSON_HasObjectItem(jobDoc, "execution") == false) {
                            continue;
                        }

                        cJSON *execution = cJSON_GetObjectItem(jobDoc, "execution");

                        const char *status = cJSON_GetObjectItem(execution, "status")->valuestring;
                        if (strcmp(status, "QUEUED") != 0) {
                            continue;
                        }

                        ESP_LOGI(MQTT_TASK_TAG, "we got a new job!");

                        cJSON *jobParameters = cJSON_GetObjectItem(execution, "jobDocument");
                        IoTJobOperation operation = (IoTJobOperation)cJSON_GetObjectItem(jobParameters, "operation")->valueint;

                        // Mark the job as in progress, then we can do something w/ the job type (int param)
                        const char *jobID = cJSON_GetObjectItem(execution, "jobId")->valuestring;
                        ESP_LOGD(MQTT_TASK_TAG, "with ID %s", jobID);

                        const char *resp = "{\"status\":\"IN_PROGRESS\", \"expectedVersion\": \"1\"}";
                        sprintf(tmpTopic, "$aws/things/%s/jobs/%s/update", thing_name, jobID);
                        esp_mqtt_client_publish(mqttClient, tmpTopic, resp, 0, 0, false);

                        strncpy(currentJobDocument, currentMessage.pMessage, currentMessage.messageLen);
                        /*
                        if (operation == IoTJobOperation::SPRITE_DELIVERY) {
                            ESP_LOGD(MQTT_TASK_TAG, "it's a sprite delivery");
                            // jobDocument has (spriteID: int, streamID: string (128))
                            const char *streamID = cJSON_GetObjectItem(jobParameters, "streamID")->valuestring;

                            // start the streaming process by requesting the stream.
                            snprintf(tmpTopic, 200, "$aws/things/%s/streams/%s/describe/json", thing_name, streamID);
                            ESP_LOGD(MQTT_TASK_TAG, "requesting the stream description");
                            char resp[200];
                            snprintf(resp, 200, "{\"c\":\"%s\"}", streamID);
                            esp_mqtt_client_publish(mqttClient, tmpTopic, resp, 0, 0, false);
                        }
                        */
                        cJSON_Delete(jobDoc);
                    }
                } else if (strstr(currentMessage.topic, "streams") != NULL) {
                    if (strstr(currentMessage.topic, "description") != NULL) {
                        ESP_LOGD(MQTT_TASK_TAG, "we got a stream description!");
                        cJSON *streamDescriptionDoc = cJSON_ParseWithLength(currentMessage.pMessage, currentMessage.messageLen);
                        cJSON *streamFiles = cJSON_GetObjectItem(streamDescriptionDoc, "r");
                        int streamFilesCount = cJSON_GetArraySize(streamFiles);
                        if (streamFilesCount != 1) {
                            ESP_LOGE(MQTT_TASK_TAG, "stream contained more than 1 file! skipping..");
                            continue;
                        }
                        ESP_LOGD(MQTT_TASK_TAG, "contains 1 file...");

                        cJSON *streamFile = cJSON_GetArrayItem(streamFiles, 0);
                        size_t fileSize = cJSON_GetObjectItem(streamFile, "z")->valueint;

                        ESP_LOGI(MQTT_TASK_TAG, "of size %d", fileSize);

                        currentStreamTemporaryBuffer = (uint8_t *)malloc(fileSize);

                        const char *streamID = cJSON_GetObjectItem(streamDescriptionDoc, "c")->valuestring;

                        workItem newWorkItem;
                        newWorkItem.workItemType = WorkItemType::REQUEST_STREAM_CHUNK;
                        newWorkItem.workItemInteger = 0;
                        strcpy(newWorkItem.workItemString, streamID);
                        xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));
                        cJSON_Delete(streamDescriptionDoc);
                    } else if (strstr(currentMessage.topic, "data") != NULL) {
                        ESP_LOGI(MQTT_TASK_TAG, "we got a stream block");
                        cJSON *streamDataDoc = cJSON_ParseWithLength(currentMessage.pMessage, currentMessage.messageLen);
                        size_t blockLenDecoded = cJSON_GetObjectItem(streamDataDoc, "l")->valueint;
                        int blockID = cJSON_GetObjectItem(streamDataDoc, "i")->valueint;
                        const uint8_t *blockData = (uint8_t *)cJSON_GetObjectItem(streamDataDoc, "p")->valuestring;
                        size_t blockLenEncoded = strlen((const char *)blockData);
                        ESP_LOGD(MQTT_TASK_TAG, "size (decoded): %d", blockLenDecoded);
                        ESP_LOGD(MQTT_TASK_TAG, "size raw: %d", blockLenEncoded);
                        ESP_LOGI(MQTT_TASK_TAG, "block no: %d", blockID);

                        // can we decode here?
                        size_t decodedChunkSize;
                        int decoded = mbedtls_base64_decode((currentStreamTemporaryBuffer + (blockID * STREAM_CHUNK_SIZE)), ULONG_MAX,
                                                            &decodedChunkSize, blockData, blockLenEncoded);
                        if (decoded == 0) {
                            ESP_LOGI(MQTT_TASK_TAG, "b64 decoded: %d bytes", decodedChunkSize);

                        } else {
                            ESP_LOGE(MQTT_TASK_TAG, "b64 decode err: %d", decoded);
                            esp_restart();
                        }

                        if (blockLenDecoded < STREAM_CHUNK_SIZE) {
                            // calc total size of bytes
                            int start = (blockID * STREAM_CHUNK_SIZE);
                            int fin = start + blockLenDecoded;
                            ESP_LOGD(MQTT_TASK_TAG, "we got %d bytes in total!", fin);

                            workItem newWorkItem;
                            newWorkItem.workItemType = WorkItemType::HANDLE_COMPLETE_STREAM_DATA;
                            xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));
                        } else {
                            // more blocks needed to recreate the file.. request another
                            const char *streamID = cJSON_GetObjectItem(streamDataDoc, "c")->valuestring;
                            ESP_LOGD(MQTT_TASK_TAG, "requesting the next block, %d for %s", blockID + 1, streamID);
                            workItem newWorkItem;
                            newWorkItem.workItemType = WorkItemType::REQUEST_STREAM_CHUNK;
                            newWorkItem.workItemInteger = blockID + 1;
                            strcpy(newWorkItem.workItemString, streamID);
                            xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));
                        }

                        cJSON_Delete(streamDataDoc);
                    }
                } else if (strstr(currentMessage.topic, "sprite_delivery") != NULL) {
                    ESP_LOGI(MQTT_TASK_TAG, "we got a new sprite delivery!");
                    cJSON *spriteDeliveryDoc = cJSON_ParseWithLength(currentMessage.pMessage, currentMessage.messageLen);

                    // jobDocument has (spriteID: int, streamID: string (128), size: int)
                    const char *streamID = cJSON_GetObjectItem(spriteDeliveryDoc, "streamID")->valuestring;
                    int spriteID = cJSON_GetObjectItem(spriteDeliveryDoc, "spriteID")->valueint;
                    size_t spriteSize = cJSON_GetObjectItem(spriteDeliveryDoc, "size")->valueint;

                    spriteDeliveryItem newSpriteDelivery = {
                        .spriteID = spriteID,
                        .spriteSize = spriteSize,
                    };
                    strcpy(newSpriteDelivery.streamID, streamID);
                    xQueueSend(xSpriteDeliveryQueue, &newSpriteDelivery, pdMS_TO_TICKS(1000));
                }

                // when we're done w/ the message, free it
                free((void *)currentMessage.pMessage);
            }
        } else {
            break;
        }
    }
}

void SpriteDelivery_Task(void *arg) {
    xSpriteDeliveryQueue = xQueueCreate(20, sizeof(spriteDeliveryItem));

    if (xSpriteDeliveryQueue == NULL) {
        return;
    }

    while (1) {
        if (xSpriteDeliveryQueue != NULL) {
            if (!isStreaming) {
                if (xQueueReceive(xSpriteDeliveryQueue, &currentSpriteDeliveryItem, pdMS_TO_TICKS(1000)) == pdPASS) {
                    // start the streaming process by requesting the stream.
                    isStreaming = true;
                    snprintf(tmpTopic, 200, "$aws/things/%s/streams/%s/describe/json", thing_name, currentSpriteDeliveryItem.streamID);
                    ESP_LOGD(MQTT_TASK_TAG, "requesting the stream description");
                    char resp[200];
                    snprintf(resp, 200, "{\"c\":\"%s\"}", currentSpriteDeliveryItem.streamID);
                    esp_mqtt_client_publish(mqttClient, tmpTopic, resp, 0, 0, false);
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
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
                if (currentWorkItem.workItemType == WorkItemType::UPDATE_REPORTED_SHADOW) {
                    ESP_LOGI(WORKER_TAG, "updating reported shadow");
                    cJSON *state = cJSON_CreateObject();
                    cJSON *schedule = cJSON_CreateArray();
                    cJSON_AddItemToObject(state, "schedule", schedule);

                    for (int i = 0; i < 100; i++) {
                        if (scheduledItems[i].show_duration == 0) {
                            break;
                        }
                        cJSON *scheduleItem = cJSON_CreateObject();
                        cJSON_AddItemToArray(schedule, scheduleItem);

                        cJSON_AddItemToObject(scheduleItem, "duration", cJSON_CreateNumber(scheduledItems[i].show_duration));
                        cJSON_AddItemToObject(scheduleItem, "skipped", cJSON_CreateBool(scheduledItems[i].is_skipped));
                        cJSON_AddItemToObject(scheduleItem, "pinned", cJSON_CreateBool(scheduledItems[i].is_pinned));
                    }

                    char *string = NULL;
                    string = cJSON_PrintUnformatted(state);
                    cJSON_Delete(state);

                    char *fullUpdateState = (char *)malloc(8192);
                    snprintf(fullUpdateState, 8192, "{\"state\":{\"reported\":%s}}", string);
                    sprintf(tmpTopic, "$aws/things/%s/shadow/update", thing_name);
                    esp_mqtt_client_publish(mqttClient, tmpTopic, fullUpdateState, 0, 0, false);
                    free(fullUpdateState);
                    free(string);
                } else if (currentWorkItem.workItemType == WorkItemType::SHOW_SPRITE) {
                    char filePath[40];
                    snprintf(filePath, 40, "/fs/%s.webp", currentWorkItem.workItemString);
                    struct stat st;
                    if (stat(filePath, &st) == 0) {
                        FILE *f = fopen(filePath, "r");

                        // Set buffers
                        xTaskNotify(matrixTask, MATRIX_TASK_NOTIF_NOT_READY, eSetValueWithOverwrite);

                        WebPDataClear(&webPData);

                        fseek(f, 0, SEEK_END);       // seek to end of file
                        size_t fileSize = ftell(f);  // get current file pointer
                        fseek(f, 0, SEEK_SET);       // seek back to beginning of file

                        // setup webp buffer and populate from temporary buffer
                        webPData.size = fileSize;
                        webPData.bytes = (uint8_t *)WebPMalloc(fileSize);

                        // Fill buffer!
                        fread((void *)webPData.bytes, 1, fileSize, f);
                        fclose(f);

                        // Show!
                        if (strncmp((const char *)webPData.bytes, "RIFF", 4) == 0) {
                            xTaskNotify(matrixTask, MATRIX_TASK_NOTIF_READY, eSetValueWithOverwrite);
                            ESP_LOGI(WORKER_TAG, "showing sprite %s", currentWorkItem.workItemString);

                            // this is a sprite
                            if (currentWorkItem.workItemInteger == 1) {
                                scheduledItems[atoi(currentWorkItem.workItemString)].reported_error = false;
                            }
                        } else {
                            ESP_LOGE(WORKER_TAG, "file %s has invalid header!", filePath);
                        }
                    } else {
                        ESP_LOGE(WORKER_TAG, "couldn't find file %s", filePath);

                        // this is a sprite
                        if (currentWorkItem.workItemInteger == 1) {
                            if (scheduledItems[atoi(currentWorkItem.workItemString)].reported_error == false) {
                                scheduledItems[atoi(currentWorkItem.workItemString)].reported_error = true;

                                char resp[200];
                                snprintf(resp, 200, "{\"spriteID\":%d, \"thingName\":\"%s\"}", atoi(currentWorkItem.workItemString), thing_name);
                                strcpy(tmpTopic, "$aws/rules/smartmatrix_sprite_error");
                                esp_mqtt_client_publish(mqttClient, tmpTopic, resp, 0, 0, false);

                                xTaskNotify(scheduleTask, SCHEDULE_TASK_NOTIF_SKIP_TO_NEXT, eSetValueWithOverwrite);
                            }
                        }
                    }
                } else if (currentWorkItem.workItemType == WorkItemType::REQUEST_STREAM_CHUNK) {
                    int blockNo = currentWorkItem.workItemInteger;
                    const char *streamID = currentWorkItem.workItemString;
                    ESP_LOGD(WORKER_TAG, "requesting stream chunk no %d for %s", blockNo, streamID);

                    char resp[200];
                    sprintf(resp, "{\"c\":\"%s\",\"f\":0,\"l\":%d,\"o\":%d,\"n\":1}", streamID, STREAM_CHUNK_SIZE, blockNo);
                    snprintf(tmpTopic, 200, "$aws/things/%s/streams/%s/get/json", thing_name, streamID);
                    esp_mqtt_client_publish(mqttClient, tmpTopic, resp, 0, 0, false);
                    ESP_LOGD(WORKER_TAG, "published %s to %s", resp, tmpTopic);
                } else if (currentWorkItem.workItemType == WorkItemType::HANDLE_COMPLETE_STREAM_DATA) {
                    ESP_LOGI(WORKER_TAG, "stream data is decoded, let's handle it");

                    int deliveringSpriteID = currentSpriteDeliveryItem.spriteID;
                    size_t deliveringSpriteSize = currentSpriteDeliveryItem.spriteSize;

                    ESP_LOGD(WORKER_TAG, ".. sprite delivery for %d!", deliveringSpriteID);

                    char filePath[40];
                    snprintf(filePath, 40, "/fs/%d.webp", deliveringSpriteID);
                    struct stat st;
                    if (stat(filePath, &st) == 0) {
                        unlink(filePath);
                    }

                    ESP_LOGD(WORKER_TAG, "opening %s for writing", filePath);
                    FILE *f = fopen(filePath, "w");
                    if (f != NULL) {
                        uint8_t wtf[2];
                        fwrite((currentStreamTemporaryBuffer) ?: wtf, 1, deliveringSpriteSize, f);
                        fclose(f);
                        ESP_LOGI(WORKER_TAG, "wrote %d bytes", deliveringSpriteSize);
                    } else {
                        ESP_LOGE(WORKER_TAG, "couldn't open %s for writing!", filePath);
                    }

                    // this job is complete, we can mark it
                    isStreaming = false;
                    free(currentStreamTemporaryBuffer);

                    //show if we're currently showing it already
                    if(currentlyDisplayingSprite == deliveringSpriteID) {
                        workItem newWorkItem;
                        newWorkItem.workItemType = WorkItemType::SHOW_SPRITE;
                        newWorkItem.workItemInteger = 1;
                        snprintf(newWorkItem.workItemString, 5, "%d", deliveringSpriteID);
                        xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));
                    }
                } else if (currentWorkItem.workItemType == WorkItemType::MARK_JOB_COMPLETE) {
                    // The current job document *should* still be in currentJobDocument
                    ESP_LOGI(WORKER_TAG, "marking job complete");
                    cJSON *jobDoc = cJSON_Parse(currentJobDocument);
                    cJSON *execution = cJSON_GetObjectItem(jobDoc, "execution");

                    const char *jobID = cJSON_GetObjectItem(execution, "jobId")->valuestring;
                    ESP_LOGD(WORKER_TAG, "... for job %s", jobID);
                    const char *resp = "{\"status\":\"SUCCEEDED\", \"expectedVersion\": \"2\"}";

                    sprintf(tmpTopic, "$aws/things/%s/jobs/%s/update", thing_name, jobID);
                    esp_mqtt_client_publish(mqttClient, tmpTopic, resp, 0, 0, false);

                    strcpy(currentJobDocument, "");
                    cJSON_Delete(jobDoc);
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
    uint32_t notifiedValue;
    WebPAnimDecoder *dec = nullptr;

    while (1) {
        if (xTaskNotifyWait(pdTRUE, pdTRUE, &notifiedValue, pdMS_TO_TICKS(16))) {
            if (notifiedValue == MATRIX_TASK_NOTIF_NOT_READY) {
                WebPAnimDecoderDelete(dec);
                isReady = false;
            } else if (notifiedValue == MATRIX_TASK_NOTIF_READY) {
                dec = WebPAnimDecoderNew(&webPData, NULL);
                if (dec == NULL) {
                    ESP_LOGE(MATRIX_TAG, "we cannot decode with a null animdec!!!");
                    continue;
                }
                animStartTS = pdTICKS_TO_MS(xTaskGetTickCount());
                lastFrameTimestamp = 0;
                currentFrame = 0;
                isReady = true;
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
                        for (int y = 0; y < MATRIX_HEIGHT; y++) {
                            for (int x = 0; x < MATRIX_WIDTH; x++) {
                                matrix->drawPixelRGB888(x, y, buf[px * 4], buf[px * 4 + 1], buf[px * 4 + 2]);
                                px++;
                            }
                        }
                        currentFrame++;
                        if (!WebPAnimDecoderHasMoreFrames(dec)) {
                            currentFrame = 0;
                            WebPAnimDecoderReset(dec);
                        }
                    } else {
                        ESP_LOGE(MATRIX_TAG, "we cannot get the next frame!");
                        isReady = false;
                    }
                }
            }
        }
    }
}

void Schedule_Task(void *arg) {
    /* Initialize the scheduler items */
    scheduledItems = new scheduledItem[100];
    unsigned long long currentSpriteStartTime = 0;
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

        if (scheduledItems[0].show_duration == 0) {
            // we don't have a schedule? or no sprites in the schedule.
            continue;
        }

        if ((pdTICKS_TO_MS(xTaskGetTickCount()) - currentSpriteStartTime > (scheduledItems[currentlyDisplayingSprite].show_duration * 1000)) ||
            needToSkip) {
            if (scheduledItems[currentlyDisplayingSprite].is_pinned && !needToSkip) {
                currentSpriteStartTime = pdTICKS_TO_MS(xTaskGetTickCount());
                continue;
            }

            if (scheduledItems[currentlyDisplayingSprite + 1].is_skipped) {
                currentlyDisplayingSprite++;
            }

            if (scheduledItems[currentlyDisplayingSprite + 1].show_duration == 0) {
                currentlyDisplayingSprite = 0;
            } else {
                currentlyDisplayingSprite++;
            }

            workItem newWorkItem;
            newWorkItem.workItemType = WorkItemType::SHOW_SPRITE;
            newWorkItem.workItemInteger = 1;
            snprintf(newWorkItem.workItemString, 4, "%d", currentlyDisplayingSprite);
            xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));
            currentSpriteStartTime = pdTICKS_TO_MS(xTaskGetTickCount());

            needToSkip = false;
        }
    }
}

extern "C" void app_main(void) {
    /* Start the matrix */
    HUB75_I2S_CFG::i2s_pins _pins = {R1_PIN, G1_PIN, B1_PIN, R2_PIN, G2_PIN, B2_PIN, A_PIN, B_PIN, C_PIN, D_PIN, E_PIN, LAT_PIN, OE_PIN, CLK_PIN};
    HUB75_I2S_CFG mxconfig(64, 32, 1, _pins);
    matrix = new MatrixPanel_I2S_DMA(mxconfig);
    matrix->begin();

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

    xTaskCreatePinnedToCore(Worker_Task, "WorkerTask", 3500, NULL, 5, &workerTask, 1);
    xTaskCreatePinnedToCore(MqttMsg_Task, "MqttMsgTask", 3500, NULL, 5, &mqttMsgTask, 1);
    xTaskCreatePinnedToCore(Matrix_Task, "MatrixTask", 4000, NULL, 5, &matrixTask, 1);
    xTaskCreatePinnedToCore(Schedule_Task, "ScheduleTask", 3500, NULL, 5, &scheduleTask, 1);
    xTaskCreatePinnedToCore(SpriteDelivery_Task, "DeliveryTask", 3500, NULL, 5, &spriteDeliveryTask, 1);

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
    xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));

    ESP_ERROR_CHECK(esp_wifi_start());

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(300000));
        sprintf(tmpTopic, "$aws/things/%s/shadow/get", thing_name);
        esp_mqtt_client_publish(mqttClient, tmpTopic, "{}", 0, 0, false);
        ESP_LOGI(BOOT_TAG, "free heap: %" PRIu32 " int: %" PRIu32 " min: %" PRIu32 "", esp_get_free_heap_size(), esp_get_free_internal_heap_size(), esp_get_minimum_free_heap_size());
    }
}
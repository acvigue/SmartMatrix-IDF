#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <button.h>
#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_crt_bundle.h>
#include <esp_event.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
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
#include "semver.h"

WebPData webPData;
esp_mqtt_client_handle_t mqttClient;

scheduledItem *scheduledItems = nullptr;

QueueHandle_t xWorkerQueue, xMqttMessageQueue;
TaskHandle_t workerTask, mqttMsgTask, matrixTask, scheduleTask, otaTask;
TimerHandle_t tslTimer, brightnessTimer;

HUB75_I2S_CFG::i2s_pins _pins = {R1_PIN, G1_PIN, B1_PIN, R2_PIN, G2_PIN, B2_PIN, A_PIN, B_PIN, C_PIN, D_PIN, E_PIN, LAT_PIN, OE_PIN, CLK_PIN};
HUB75_I2S_CFG mxconfig(64, 32, 1, _pins);
MatrixPanel_I2S_DMA matrix = MatrixPanel_I2S_DMA(mxconfig);
tsl2561_t tslSensor;
button_t skip_button, pin_button;

char thing_name[18];
int currentlyDisplayingSprite, desiredBrightness, currentBrightness = 0;
bool wifiConnected, mqttConnected = false;

static void on_button(button_t *btn, button_state_t state) {
    if (!mqttConnected || !wifiConnected) {
        return;
    }
    if (state != BUTTON_PRESSED) {
        return;
    }

    if (btn == &pin_button) {
        // clear all pin states
        int i = 0;
        while (true) {
            if (scheduledItems[i].show_duration == 0) {
                break;
            }
            scheduledItems[i].is_pinned = false;
            i++;
        }

        ESP_LOGI(BUTTON_TAG, "pin button pressed, pinning current sprite");
        scheduledItems[currentlyDisplayingSprite].is_pinned = true;
    } else if (btn == &skip_button) {
        ESP_LOGI(BUTTON_TAG, "skip button pressed, notifying scheduler");
        xTaskNotify(scheduleTask, SCHEDULE_TASK_NOTIF_SKIP_TO_NEXT, eSetValueWithOverwrite);
    }
}

esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    static char *output_buffer;  // Buffer to store response of http request from event handler
    static int output_len;       // Stores number of bytes read
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(HTTP_TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(HTTP_TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(HTTP_TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(HTTP_TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(HTTP_TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            /*
             *  Check for chunked encoding is added as the URL for chunked encoding used in this example returns binary data.
             *  However, event handler can also be used in case chunked encoding is used.
             */
            if (!esp_http_client_is_chunked_response(evt->client)) {
                // If user_data buffer is configured, copy the response into the buffer
                int copy_len = 0;
                if (evt->user_data) {
                    copy_len = MIN(evt->data_len, (MAX_HTTP_OUTPUT_BUFFER - output_len));
                    if (copy_len) {
                        memcpy(evt->user_data + output_len, evt->data, copy_len);
                    }
                } else {
                    const int buffer_len = esp_http_client_get_content_length(evt->client);
                    if (output_buffer == NULL) {
                        output_buffer = (char *)malloc(buffer_len);
                        output_len = 0;
                        if (output_buffer == NULL) {
                            ESP_LOGE(HTTP_TAG, "Failed to allocate memory for output buffer");
                            return ESP_FAIL;
                        }
                    }
                    copy_len = MIN(evt->data_len, (buffer_len - output_len));
                    if (copy_len) {
                        memcpy(output_buffer + output_len, evt->data, copy_len);
                    }
                }
                output_len += copy_len;
            }

            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(HTTP_TAG, "HTTP_EVENT_ON_FINISH");
            if (output_buffer != NULL) {
                free(output_buffer);
                output_buffer = NULL;
            }
            output_len = 0;
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(HTTP_TAG, "HTTP_EVENT_DISCONNECTED");
            if (output_buffer != NULL) {
                free(output_buffer);
                output_buffer = NULL;
            }
            output_len = 0;
            break;
        case HTTP_EVENT_REDIRECT:
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    static char lastTopic[200];
    static char *dataBuf = nullptr;
    static bool hasConnected = false;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED: {
            mqttConnected = true;
            wifiConnected = true;
            ESP_LOGI(MQTT_TAG, "connected to broker..");

            // Device shadow topics.
            char tmpTopic[200];

            sprintf(tmpTopic, "smartmatrix/%s/schedule_delivery", thing_name);
            esp_mqtt_client_subscribe(event->client, tmpTopic, 1);

            sprintf(tmpTopic, "smartmatrix/%s/sprite_delivery", thing_name);
            esp_mqtt_client_subscribe(event->client, tmpTopic, 1);

            sprintf(tmpTopic, "smartmatrix/%s/command", thing_name);
            esp_mqtt_client_subscribe(event->client, tmpTopic, 1);

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
            mqttConnected = false;
            ESP_LOGI(MQTT_TAG, "mqtt disconnected");
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
                strcpy(newWorkItem.workItemString, "connect_wifi_failed");
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
                strcpy(newWorkItem.workItemString, "setup");
                xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));
                break;
            }
            case WIFI_PROV_CRED_RECV: {
                wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;

                workItem newWorkItem;
                newWorkItem.workItemType = WorkItemType::SHOW_SPRITE;
                strcpy(newWorkItem.workItemString, "connect_wifi");
                xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));
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
                wifiConnected = false;
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
            wifiConnected = true;
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

void OTA_Task(void *arg) {
    if (strlen(OTA_MANIFEST_URL) == 0) {
        ESP_LOGE(OTA_TAG, "skipping OTA task setup, no manifest given..");
        return;
    }
    ESP_LOGI(OTA_TAG, "ota update task started, manifest URL: %s", OTA_MANIFEST_URL);

    while (true) {
        if (wifiConnected) {
            vTaskDelay(pdMS_TO_TICKS(1000 * 60 * 5));
            ESP_LOGI(OTA_TAG, "ota update starting!");
            char http_response_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0};
            esp_http_client_config_t manifest_config = {.url = OTA_MANIFEST_URL,
                                                        .disable_auto_redirect = false,
                                                        .event_handler = http_event_handler,
                                                        .user_data = http_response_buffer,
                                                        .crt_bundle_attach = esp_crt_bundle_attach};
            esp_http_client_handle_t manifest_client = esp_http_client_init(&manifest_config);

            // GET
            esp_err_t err = esp_http_client_perform(manifest_client);
            if (err == ESP_OK) {
                ESP_LOGI(HTTP_TAG, "HTTP GET Status = %d, content_length = %" PRId64, esp_http_client_get_status_code(manifest_client),
                         esp_http_client_get_content_length(manifest_client));
            } else {
                ESP_LOGE(HTTP_TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
                if(err == ESP_ERR_HTTP_CONNECT) {
                    //socket error, only fixable by wlan disconnect.
                    ESP_LOGI(HTTP_TAG, "restarting wifi subsystem due to socket error.");
                }
            }

            esp_http_client_cleanup(manifest_client);

            cJSON *manifestDoc = cJSON_ParseWithLength(http_response_buffer, strlen(http_response_buffer));
            if (!cJSON_IsObject(manifestDoc)) {
                ESP_LOGE(OTA_TAG, "manifest document was not an object!");
                esp_wifi_disconnect();
                continue;
            }

            const char *otaType = cJSON_GetObjectItem(manifestDoc, "type")->valuestring;
            if (strcmp(otaType, "smartmatrix") != 0) {
                ESP_LOGE(OTA_TAG, "manifest device types are not equal, skipping update!");
                continue;
            }

            const esp_app_desc_t *app_desc = esp_app_get_description();
            const char *otaVersion = cJSON_GetObjectItem(manifestDoc, "version")->valuestring;
            const char *otaHost = cJSON_GetObjectItem(manifestDoc, "host")->valuestring;
            int otaPort = cJSON_GetObjectItem(manifestDoc, "port")->valueint;
            const char *otaBinPath = cJSON_GetObjectItem(manifestDoc, "bin")->valuestring;
            const char *otaFSPath = cJSON_GetObjectItem(manifestDoc, "spiffs")->valuestring;

            char binURL[200];
            snprintf(binURL, 200, "https://%s:%d%s", otaHost, otaPort, otaBinPath);

            semver_t current_version = {};
            semver_t compare_version = {};

            if (semver_parse(app_desc->version, &current_version) || semver_parse(otaVersion, &compare_version)) {
                ESP_LOGE(OTA_TAG, "could not parse version strings!");
                continue;
            }

            cJSON_Delete(manifestDoc);

            bool otaRequired = semver_compare(compare_version, current_version) > 0;

            semver_free(&current_version);
            semver_free(&compare_version);

            if (!otaRequired) {
                ESP_LOGI(OTA_TAG, "firmware (%s) is up to date, no OTA necessary..", app_desc->version);
                continue;
            }

            ESP_LOGI(OTA_TAG, "firmware (%s) is out of date, OTA updating to version %s", app_desc->version, otaVersion);
            ESP_LOGI(OTA_TAG, "built firmware binary URL: %s", binURL);

            esp_http_client_config_t config = {.url = binURL, .crt_bundle_attach = esp_crt_bundle_attach};
            esp_https_ota_config_t ota_config = {.http_config = &config, .partial_http_download = true};
            esp_err_t ret = esp_https_ota(&ota_config);
            if (ret == ESP_OK) {
                ESP_LOGI(OTA_TAG, "ota done, restarting");
            } else {
                ESP_LOGE(OTA_TAG, "ota failed, error code %d: %s", ret, esp_err_to_name(ret));
            }
            esp_restart();
        } else {
            vTaskDelay(pdMS_TO_TICKS(5000));
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
                } else if (strstr(currentMessage.topic, "command") != NULL) {
                    cJSON *commandDoc = cJSON_ParseWithLength(currentMessage.pMessage, currentMessage.messageLen);

                    if (commandDoc != nullptr) {
                        const char *type = cJSON_GetObjectItem(commandDoc, "type")->valuestring;
                        if (strcmp(type, "matrix_sleep") == 0) {
                            ESP_LOGI(MQTT_TASK_TAG, "sleeping");
                            xTaskNotify(matrixTask, MATRIX_TASK_NOTIF_SLEEP, eSetValueWithOverwrite);
                        } else if (strcmp(type, "matrix_wake") == 0) {
                            ESP_LOGI(MQTT_TASK_TAG, "waking up");
                            xTaskNotify(matrixTask, MATRIX_TASK_NOTIF_WAKE_UP, eSetValueWithOverwrite);
                        } else if (strcmp(type, "pin_sprite") == 0) {
                            // clear all pin states
                            int i = 0;
                            while (true) {
                                if (scheduledItems[i].show_duration == 0) {
                                    break;
                                }
                                scheduledItems[i].is_pinned = false;
                                i++;
                            }

                            // pin the mentioned sprite
                            int pinnedSprite = cJSON_GetObjectItem(commandDoc, "spriteID")->valueint;
                            scheduledItems[pinnedSprite].is_pinned = true;

                            // notify task
                            xTaskNotify(scheduleTask, SCHEDULE_TASK_NOTIF_SKIP_TO_PINNED, eSetValueWithOverwrite);
                        } else if (strcmp(type, "unpin_sprite") == 0) {
                            // clear all pin states
                            int i = 0;
                            while (true) {
                                if (scheduledItems[i].show_duration == 0) {
                                    break;
                                }
                                scheduledItems[i].is_pinned = false;
                                i++;
                            }

                            // notify task
                            xTaskNotify(scheduleTask, SCHEDULE_TASK_NOTIF_SKIP_TO_NEXT, eSetValueWithOverwrite);
                        }
                    }

                    cJSON_Delete(commandDoc);
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

                            if (scheduledItems[atoi(currentWorkItem.workItemString)].reported_error == false && mqttConnected) {
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

        if (isSleeping) {
            desiredBrightness = 0;
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
                                matrix.drawPixelRGB888(x, y, buf[px * 4], buf[px * 4 + 1], buf[px * 4 + 2]);
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
            } else if (notifiedValue == SCHEDULE_TASK_NOTIF_SKIP_TO_PINNED) {
                int i = 0;
                while (true) {
                    if (scheduledItems[i].show_duration == 0) {
                        i = -1;
                        break;
                    }

                    if (scheduledItems[i].is_pinned == true) {
                        break;
                    }
                    i++;
                }

                // i now contains (-1) for error, or the id of the sprite that is now pinned.
                if (i == -1) {
                    ESP_LOGE(SCHEDULE_TAG, "skipping sprite pinning logic as loop reported error");
                    continue;
                }

                currentlyDisplayingSprite = i;
                ESP_LOGI(SCHEDULE_TAG, "received pin request for sprite %d!", currentlyDisplayingSprite);

                // actually display the sprite!
                workItem newWorkItem;
                newWorkItem.workItemType = WorkItemType::SHOW_SPRITE;
                newWorkItem.workItemInteger = 1;
                snprintf(newWorkItem.workItemString, 4, "%d", currentlyDisplayingSprite);
                xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));
                currentSpriteStartTime = pdTICKS_TO_MS(xTaskGetTickCount());
            }
        }

        if (!mqttClient || !mqttConnected) {
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
                // the current sprite is pinned and is time for an update
                char resp[200];
                char tmpTopic[200];
                snprintf(tmpTopic, 200, "smartmatrix/%s/status", thing_name);
                const esp_app_desc_t *app_desc = esp_app_get_description();
                snprintf(resp, 200, "{\"type\": \"report\", \"currentSpriteID\":%d, \"nextSpriteID\": %d, \"appVersion\": \"%s\"}",
                         currentlyDisplayingSprite, currentlyDisplayingSprite, app_desc->version);
                esp_mqtt_client_publish(mqttClient, tmpTopic, resp, 0, 0, false);
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
            const esp_app_desc_t *app_desc = esp_app_get_description();
            snprintf(resp, 200, "{\"type\": \"report\", \"currentSpriteID\":%d, \"nextSpriteID\": %d, \"appVersion\": \"%s\"}",
                     currentlyDisplayingSprite, nextSpriteID, app_desc->version);
            esp_mqtt_client_publish(mqttClient, tmpTopic, resp, 0, 0, false);

            needToSkip = false;
        }
    }
}

extern "C" void app_main(void) {
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

    pin_button.gpio = IO_BTN_USER2;
    skip_button.gpio = IO_BTN_USER1;

    pin_button.pressed_level = 0;
    skip_button.pressed_level = 0;

    pin_button.internal_pull = false;
    skip_button.internal_pull = false;

    pin_button.callback = on_button;
    skip_button.callback = on_button;

    ESP_ERROR_CHECK(i2cdev_init());

    tsl2561_init_desc(&tslSensor, 0x39, (i2c_port_t)0, (gpio_num_t)7, (gpio_num_t)6);
    tsl2561_init(&tslSensor);

    /* Start the matrix */
    matrix.begin();
    currentBrightness = 0;
    desiredBrightness = 255;
    matrix.setBrightness8(0);

    xTaskCreatePinnedToCore(Worker_Task, "WorkerTask", 3500, NULL, 5, &workerTask, 1);
    xTaskCreatePinnedToCore(MqttMsg_Task, "MqttMsgTask", 10000, NULL, 5, &mqttMsgTask, 1);
    xTaskCreatePinnedToCore(Matrix_Task, "MatrixTask", 4000, NULL, 5, &matrixTask, 1);
    xTaskCreatePinnedToCore(Schedule_Task, "ScheduleTask", 3500, NULL, 5, &scheduleTask, 1);
    xTaskCreatePinnedToCore(OTA_Task, "OTA", 5000, NULL, 5, &otaTask, 0);

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

    // if both buttons are pressed upon startup, reset the device to factory defaults!
    if (gpio_get_level(IO_BTN_USER1) == 0 && gpio_get_level(IO_BTN_USER2) == 0) {
        while (gpio_get_level(IO_BTN_USER1) == 0 || gpio_get_level(IO_BTN_USER2) == 0) {
            ESP_LOGI(BOOT_TAG, "release button pls!");
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        ESP_LOGW(BOOT_TAG, "resetting provisioning status!");
        wifi_prov_mgr_config_t config = {.scheme = wifi_prov_scheme_ble, .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM};
        ESP_ERROR_CHECK(wifi_prov_mgr_init(config));
        ESP_ERROR_CHECK(wifi_prov_mgr_reset_provisioning());
        esp_restart();
    }

    ESP_ERROR_CHECK(button_init(&pin_button));
    ESP_ERROR_CHECK(button_init(&skip_button));

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
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_LOGI(BOOT_TAG, "free heap: %" PRIu32 " internal: %" PRIu32 " contig: %d", esp_get_free_heap_size(), esp_get_free_internal_heap_size(),
                 heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    }
}

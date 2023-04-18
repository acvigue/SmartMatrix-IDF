#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_event.h>
#include <esp_littlefs.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
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
#include "pins.h"

WebPData webPData;
esp_mqtt_client_handle_t mqttClient;

WebPAnimDecoder *dec = nullptr;
char *private_key = nullptr;
char *certificate = nullptr;

char device_id[7];
char thing_name[18];
char jsonBuf[8192];
static scheduledItem *scheduledItems = new scheduledItem[100];

QueueHandle_t xWorkerQueue;
TaskHandle_t workerTask, matrixTask;
MatrixPanel_I2S_DMA *matrix;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    static char lastTopic[100];
    static int failureCount = 0;
    char tmpTopic[100];
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

            // Request initial shadow
            sprintf(tmpTopic, "$aws/things/%s/shadow/get", thing_name);
            esp_mqtt_client_publish(event->client, tmpTopic, "{}", 0, 0, false);
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
                strcpy(lastTopic, event->topic);
            }

            if (strstr(lastTopic, "shadow") != NULL) {
                if (strstr(lastTopic, "get") != NULL) {
                    if (strstr(lastTopic, "accepted") != NULL) {
                        // We received a shadow from AWS
                        if (event->current_data_offset == 0) {
                            memset(jsonBuf, 0, sizeof(jsonBuf));
                        }

                        memcpy((void *)(jsonBuf + event->current_data_offset), event->data, event->data_len);
                        if (event->data_len + event->current_data_offset >= event->total_data_len) {
                            workItem newWorkItem;
                            newWorkItem.workItemType = WORKITEM_TYPE_HANDLE_DESIRED_SHADOW_UPDATE;
                            strcpy(newWorkItem.workItemString, "get");
                            xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));
                        }
                    } else if (strstr(lastTopic, "rejected") != NULL) {
                        // AWS rejected our get shadow request, generate and post the state.
                        ESP_LOGE(MQTT_TAG, "couldn't fetch shadow, creating one");

                        workItem newWorkItem;
                        newWorkItem.workItemType = WORKITEM_TYPE_UPDATE_REPORTED_SHADOW;
                        xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));
                    }
                } else if (strstr(lastTopic, "update") != NULL) {
                    if (strstr(lastTopic, "delta") != NULL) {
                        // We've received a change to our shadow
                        if (event->current_data_offset == 0) {
                            memset(jsonBuf, 0, sizeof(jsonBuf));
                        }

                        memcpy((void *)(jsonBuf + event->current_data_offset), event->data, event->data_len);
                        if (event->data_len + event->current_data_offset >= event->total_data_len) {
                            workItem newWorkItem;
                            newWorkItem.workItemType = WORKITEM_TYPE_HANDLE_DESIRED_SHADOW_UPDATE;
                            strcpy(newWorkItem.workItemString, "delta");
                            xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));
                        }
                    }
                }
            } else {
                ESP_LOGE(MQTT_TAG, "unknown focus!");
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
                newWorkItem.workItemType = WORKITEM_TYPE_SHOW_SPRITE;
                strcpy(newWorkItem.workItemString, "setup");
                xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));
                break;
        }
    } else if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START: {
                ESP_LOGI(WIFI_TAG, "STA started");

                workItem newWorkItem;
                newWorkItem.workItemType = WORKITEM_TYPE_SHOW_SPRITE;
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
                        ESP_LOGI(WIFI_TAG, "already provisioned, connecting..");
                        provisioning = false;
                        esp_wifi_connect();
                    } else {
                        ESP_LOGI(WIFI_TAG, "not provisioned, starting provisioner..");

                        workItem newWorkItem;
                        newWorkItem.workItemType = WORKITEM_TYPE_START_PROVISIONER;
                        xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));
                    }
                }
                break;
            }
            case WIFI_EVENT_STA_DISCONNECTED: {
                wifiConnectionAttempts++;
                ESP_LOGI(WIFI_TAG, "STA disconnected");
                esp_mqtt_client_stop(mqttClient);
                if (wifiConnectionAttempts > 5 && !provisioning) {
                    ESP_LOGI(WIFI_TAG, "failure count reached, restarting provisioner..");
                    provisioning = true;

                    workItem newWorkItem;
                    newWorkItem.workItemType = WORKITEM_TYPE_START_PROVISIONER;
                    xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));
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
            newWorkItem.workItemType = WORKITEM_TYPE_SHOW_SPRITE;
            strcpy(newWorkItem.workItemString, "connect_cloud");
            xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));

            /* Setup MQTT */
            const esp_mqtt_client_config_t mqtt_cfg = {.broker = {.address = "mqtts://afb1whot34whq-ats.iot.us-east-1.amazonaws.com:8883",
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

void Worker_Task(void *arg) {
    workItem currentWorkItem;
    while (1) {
        if (xWorkerQueue != NULL) {
            if (xQueueReceive(xWorkerQueue, &(currentWorkItem), pdMS_TO_TICKS(1000)) == pdPASS) {
                if (currentWorkItem.workItemType == WORKITEM_TYPE_HANDLE_DESIRED_SHADOW_UPDATE) {
                    ESP_LOGI(WORKER_TAG, "parsing desired shadow");
                    cJSON *shadowDoc = cJSON_Parse(jsonBuf);
                    cJSON *state = cJSON_GetObjectItem(shadowDoc, "state");

                    cJSON *schedule = nullptr;
                    if (strstr(currentWorkItem.workItemString, "get") != NULL) {
                        cJSON *desired = cJSON_GetObjectItem(state, "desired");
                        schedule = cJSON_GetObjectItem(desired, "schedule");
                    } else if (strstr(currentWorkItem.workItemString, "delta") != NULL) {
                        schedule = cJSON_GetObjectItem(state, "schedule");
                    } else {
                        ESP_LOGE(WORKER_TAG, "can't process shadow update request with invalid context %s", currentWorkItem.workItemString);
                        continue;
                    }

                    int itemCount = cJSON_GetArraySize(schedule);
                    for (int i = 0; i < itemCount; i++) {
                        cJSON *item = cJSON_GetArrayItem(schedule, i);
                        int itemDuration = cJSON_GetObjectItem(item, "duration")->valueint;
                        bool itemPinned = cJSON_GetObjectItem(item, "pinned")->valueint;
                        bool itemSkipped = cJSON_GetObjectItem(item, "skipped")->valueint;
                        scheduledItems[i].show_duration = itemDuration;
                        scheduledItems[i].is_pinned = itemPinned;
                        scheduledItems[i].is_skipped = itemSkipped;
                        strcpy(scheduledItems[i].data_md5, cJSON_GetObjectItem(item, "hash")->valuestring);
                    }
                    scheduledItems[itemCount].show_duration = 0;

                    cJSON_Delete(shadowDoc);

                    workItem newWorkItem;
                    newWorkItem.workItemType = WORKITEM_TYPE_UPDATE_REPORTED_SHADOW;
                    xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));
                } else if (currentWorkItem.workItemType == WORKITEM_TYPE_UPDATE_REPORTED_SHADOW) {
                    ESP_LOGI(WORKER_TAG, "updating reported shadow");
                    cJSON *state = cJSON_CreateObject();
                    cJSON *schedule = cJSON_CreateArray();
                    cJSON_AddItemToObject(state, "schedule", schedule);

                    for (int i = 0; i < 100; i++) {
                        if (scheduledItems[i].show_duration == 0) {
                            break;
                        }
                        ESP_LOGD(WORKER_TAG, "generating schedule item %d: %d, %s", i, scheduledItems[i].show_duration, scheduledItems[i].data_md5);
                        cJSON *scheduleItem = cJSON_CreateObject();
                        cJSON_AddItemToArray(schedule, scheduleItem);

                        cJSON_AddItemToObject(scheduleItem, "duration", cJSON_CreateNumber(scheduledItems[i].show_duration));
                        cJSON_AddItemToObject(scheduleItem, "skipped", cJSON_CreateBool(scheduledItems[i].is_skipped));
                        cJSON_AddItemToObject(scheduleItem, "pinned", cJSON_CreateBool(scheduledItems[i].is_pinned));
                        cJSON_AddItemToObject(scheduleItem, "hash", cJSON_CreateString(scheduledItems[i].data_md5));
                    }

                    char *string = NULL;
                    string = cJSON_Print(state);
                    cJSON_Delete(state);

                    char tmpTopic[100];
                    char *fullUpdateState = (char *)malloc(8192);
                    snprintf(fullUpdateState, 8192, "{\"state\":{\"reported\":%s}}", string);
                    sprintf(tmpTopic, "$aws/things/%s/shadow/update", thing_name);
                    esp_mqtt_client_publish(mqttClient, tmpTopic, fullUpdateState, 0, 0, false);
                    free(fullUpdateState);
                    free(string);
                } else if (currentWorkItem.workItemType == WORKITEM_TYPE_SHOW_SPRITE) {
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
                            ESP_LOGE(WORKER_TAG, "showing sprite %s", currentWorkItem.workItemString);
                        } else {
                            ESP_LOGE(WORKER_TAG, "file %s has invalid header!", filePath);
                        }
                    } else {
                        ESP_LOGE(WORKER_TAG, "couldn't find file %s", filePath);
                    }
                } else if (currentWorkItem.workItemType == WORKITEM_TYPE_START_PROVISIONER) {
                    ESP_LOGD(WORKER_TAG, "reached prov start request handler..");
                    wifi_prov_mgr_config_t config = {.scheme = wifi_prov_scheme_ble, .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM};
                    ESP_ERROR_CHECK(wifi_prov_mgr_init(config));
                    ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_0, NULL, thing_name, NULL));
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

    while (1) {
        uint32_t notifiedValue;
        if (xTaskNotifyWait(pdTRUE, pdTRUE, &notifiedValue, pdMS_TO_TICKS(30))) {
            if (notifiedValue == MATRIX_TASK_NOTIF_NOT_READY) {
                ESP_LOGI(MATRIX_TAG, "we are not ready to decode");
                WebPAnimDecoderDelete(dec);
                isReady = false;
            } else if (notifiedValue == MATRIX_TASK_NOTIF_READY) {
                ESP_LOGI(MATRIX_TAG, "we are ready to decode");
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
                        ESP_LOGI(MATRIX_TAG, "we cannot get the next frame!");
                        isReady = false;
                    }
                }
            }
        }
    }
}

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

extern "C" void app_main(void) {
    for (int i = 0; i < 100; i++) {
        scheduledItems[i].show_duration = 0;
        scheduledItems[i].is_pinned = false;
        scheduledItems[i].is_skipped = false;
        strcpy(scheduledItems[i].data_md5, "");
    }

    HUB75_I2S_CFG::i2s_pins _pins = {35,37,36,34,9,8,7,6,21,5,-1,4,2,1};
    HUB75_I2S_CFG mxconfig(64, 32, 1, _pins);
    matrix = new MatrixPanel_I2S_DMA(mxconfig);
    matrix->begin();

    /* Initialize NVS partition */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    xWorkerQueue = xQueueCreate(10, sizeof(workItem));

    if (xWorkerQueue == NULL) {
        return;
    }

    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/fs",
        .partition_label = "littlefs",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&conf));

    nvs_handle handle;
    nvs_open("certs", NVS_READONLY, &handle);

    private_key = nvs_load_value_if_exist(handle, "client_key");
    certificate = nvs_load_value_if_exist(handle, "client_cert");
    nvs_close(handle);

    // Check if both items have been correctly retrieved
    if (private_key == NULL || certificate == NULL) {
        ESP_LOGE(BOOT_TAG, "Private key and/or cert could not be loaded");
        return;
    }

    xTaskCreatePinnedToCore(Worker_Task, "WorkerTask", 5000, NULL, 5, &workerTask, 1);
    xTaskCreatePinnedToCore(Matrix_Task, "MatrixTask", 3500, NULL, 5, &matrixTask, 1);

    workItem newWorkItem;
    newWorkItem.workItemType = WORKITEM_TYPE_SHOW_SPRITE;
    strcpy(newWorkItem.workItemString, "connect_wifi");
    xQueueSend(xWorkerQueue, &newWorkItem, pdMS_TO_TICKS(1000));

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

    esp_netif_dns_info_t dns_info1;
    ip4_addr_t dns_server_ip1;
    IP4_ADDR(&dns_server_ip1, 1, 1, 1, 1);
    dns_info1.ip.u_addr.ip4.addr = dns_server_ip1.addr;
    esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_info1);

    esp_netif_dns_info_t dns_info2;
    ip4_addr_t dns_server_ip2;
    IP4_ADDR(&dns_server_ip2, 1, 0, 0, 1);
    dns_info2.ip.u_addr.ip4.addr = dns_server_ip2.addr;
    esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns_info2);

    uint8_t eth_mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
    snprintf(device_id, 7, "%02X%02X%02X", eth_mac[3], eth_mac[4], eth_mac[5]);
    snprintf(thing_name, 18, "SmartMatrix%s", device_id);

    ESP_ERROR_CHECK(esp_wifi_start());
}
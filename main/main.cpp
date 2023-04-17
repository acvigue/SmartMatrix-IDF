#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_event.h>
#include <esp_littlefs.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
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
WebPAnimDecoder *dec = nullptr;
char statusTopic[40];
char schedule[1024];

TaskHandle_t matrixTask;
esp_mqtt_client_handle_t mqttClient;

int currentlyShowingApplet = 0;
int currentAppletDuration = 0;
unsigned long currentAppletStartTime;
int scheduledAppletCount = 0;
static bool skipStates[100];
static int appletDurations[100];
static bool hasSentBoot = false;
static char lastTopic[50];
static FILE *pushingAppletFile;
static int pushingAppletID;

void workerTaskCode(void *arg);
void showApplet(const char *name);
void showNextApplet();
void startProvisioner();

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
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
            // esp_mqtt_client_subscribe(event->client, tmpTopic, 1);
            sprintf(tmpTopic, "smartmatrix/%s/schedule", device_id);
            // esp_mqtt_client_subscribe(event->client, tmpTopic, 1);
            sprintf(tmpTopic, "smartmatrix/%s/applet", device_id);
            // esp_mqtt_client_subscribe(event->client, tmpTopic, 1);
            sprintf(statusTopic, "smartmatrix/%s/status", device_id);

            if (!hasSentBoot) {
                const char *resp = "{\"type\":\"boot\"}";
                // esp_mqtt_client_publish(event->client, statusTopic, resp, 0,
                // 1,
                //                         0);
                hasSentBoot = true;
            }
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(MQTT_TAG, "mqtt disconnected");
            break;
        case MQTT_EVENT_DATA:
            if (event->topic_len != 0) {
                strcpy(lastTopic, event->topic);
            }
            if (strstr(lastTopic, "command")) {
                cJSON *root = cJSON_Parse(event->data);
                if (!cJSON_HasObjectItem(root, "command")) {
                    ESP_LOGE(MQTT_TAG, "command event had no command!");
                    break;
                }
                char *type = cJSON_GetObjectItem(root, "command")->valuestring;
                if (strcmp(type, "ping") == 0) {
                    const char *resp = "{\"type\":\"pong\"}";
                    esp_mqtt_client_publish(event->client, statusTopic, resp, 0,
                                            1, 0);
                } else if (strcmp(type, "send_app_graphic") == 0) {
                    cJSON *params = cJSON_GetObjectItem(root, "params");

                    fclose(pushingAppletFile);
                    pushingAppletID =
                        atoi(cJSON_GetObjectItem(params, "appid")->valuestring);

                    char tmpFileName[25];
                    sprintf(tmpFileName, "/littlefs/%d.webp.tmp",
                            pushingAppletID);
                    unlink(tmpFileName);

                    pushingAppletFile = fopen(tmpFileName, "w");
                    const char *resp =
                        "{\"type\":\"success\",\"info\":\"applet_update\","
                        "\"next\":\"send_chunk\"}";
                    esp_mqtt_client_publish(event->client, statusTopic, resp, 0,
                                            1, 0);
                } else if (strcmp(type, "app_graphic_stop") == 0) {
                    fclose(pushingAppletFile);
                    char tmpFileName[25];
                    sprintf(tmpFileName, "/littlefs/%d.webp.tmp",
                            pushingAppletID);

                    unlink(tmpFileName);
                    const char *resp =
                        "{\"type\":\"success\",\"info\":\"applet_update\","
                        "\"next\":\"send_next\"}";
                    esp_mqtt_client_publish(event->client, statusTopic, resp, 0,
                                            1, 0);
                } else if (strcmp(type, "app_graphic_sent") == 0) {
                    fclose(pushingAppletFile);

                    // Move temp file to real applet
                    char tmpFileName[25];
                    char realFileName[25];
                    sprintf(tmpFileName, "/littlefs/%d.webp.tmp",
                            pushingAppletID);
                    sprintf(realFileName, "/littlefs/%d.webp", pushingAppletID);

                    if (rename(tmpFileName, realFileName) == 0) {
                        const char *resp =
                            "{\"type\":\"success\",\"info\":\"applet_update\","
                            "\"next\":\"none\"}";
                        esp_mqtt_client_publish(event->client, statusTopic,
                                                resp, 0, 1, 0);
                    }
                } else {
                    ESP_LOGE(MQTT_TAG, "Received unknown command: %s", type);
                }
                cJSON_Delete(root);
            } else if (strstr(lastTopic, "schedule")) {
                strcpy(schedule, event->data);
            } else if (strstr(lastTopic, "applet")) {
                if (event->current_data_offset == 0) {
                    ESP_LOGI(MQTT_TAG, "receiving %d bytes for %d",
                             event->data_len, pushingAppletID);
                }
                fwrite(event->data, 1, event->data_len, pushingAppletFile);
                if (event->current_data_offset + event->data_len >=
                    event->total_data_len) {
                    ESP_LOGI(MQTT_TAG, "stored %d bytes for %d",
                             event->total_data_len, pushingAppletID);
                    const char *resp =
                        "{\"type\":\"success\",\"info\":\"applet_update\","
                        "\"next\":\"send_chunk\"}";
                    esp_mqtt_client_publish(event->client, statusTopic, resp, 0,
                                            1, 0);
                }
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
                showApplet("setup");
                break;
        }
    } else if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START: {
                ESP_LOGI(WIFI_TAG, "STA started");
                /* show connect wifi applet */
                showApplet("connect_wifi");

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
                        startProvisioner();
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
                    startProvisioner();
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

            showApplet("connect_cloud");

            esp_mqtt_client_start(mqttClient);
        }
    }
}

void Matrix_Task(void *arg) {
    bool isReady = false;
    unsigned long animStartTS = 0;
    int lastFrameTimestamp = 0;
    int currentFrame = 0;
    int errorCount = 0;

    HUB75_I2S_CFG::i2s_pins _pins = {R1_PIN, G1_PIN,  B1_PIN, R2_PIN, G2_PIN,
                                     B2_PIN, A_PIN,   B_PIN,  C_PIN,  D_PIN,
                                     E_PIN,  LAT_PIN, OE_PIN, CLK_PIN};
    HUB75_I2S_CFG mxconfig(64, 32, 1, _pins);
    MatrixPanel_I2S_DMA matrix = MatrixPanel_I2S_DMA(mxconfig);
    matrix.begin();

    while (1) {
        uint32_t notifiedValue;

        if (xTaskNotifyWait(pdTRUE, pdTRUE, &notifiedValue,
                            pdMS_TO_TICKS(30))) {
            if (notifiedValue == MATRIX_TASK_NOTIF_NOT_READY) {
                WebPAnimDecoderDelete(dec);
                isReady = false;
            } else if (notifiedValue == MATRIX_TASK_NOTIF_READY) {
                dec = WebPAnimDecoderNew(&webPData, NULL);
                if (dec == NULL) {
                    continue;
                }
                animStartTS = pdTICKS_TO_MS(xTaskGetTickCount());
                lastFrameTimestamp = 0;
                currentFrame = 0;
                errorCount = 0;
                isReady = true;
            }
        }

        if (isReady) {
            if (pdTICKS_TO_MS(xTaskGetTickCount()) - animStartTS >
                lastFrameTimestamp) {
                if (currentFrame == 0) {
                    animStartTS = pdTICKS_TO_MS(xTaskGetTickCount());
                    lastFrameTimestamp = 0;
                }

                bool hasMoreFrames = WebPAnimDecoderHasMoreFrames(dec);
                if (hasMoreFrames) {
                    uint8_t *buf;
                    if (WebPAnimDecoderGetNext(dec, &buf,
                                               &lastFrameTimestamp)) {
                        int px = 0;
                        for (int y = 0; y < MATRIX_HEIGHT; y++) {
                            for (int x = 0; x < MATRIX_WIDTH; x++) {
                                matrix.drawPixel(
                                    x, y,
                                    matrix.color565(buf[px * 4],
                                                    buf[px * 4 + 1],
                                                    buf[px * 4 + 2]));

                                px++;
                            }
                        }
                        currentFrame++;
                        if (!WebPAnimDecoderHasMoreFrames(dec)) {
                            currentFrame = 0;
                            WebPAnimDecoderReset(dec);
                        }
                    } else {
                        isReady = false;
                    }
                }
            }
        }
    }
}

char *read_file_from_littlefs(const char *fileName) {
    char fileName2[40];
    snprintf(fileName2, 40, "/littlefs/%s", fileName);
    FILE *f = fopen(fileName2, "r");
    if (f == NULL) {
        ESP_LOGE(BOOT_TAG, "couldn't find file %s", fileName);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    size_t fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *value = (char *)malloc(fileSize);

    fread((void *)value, 1, fileSize, f);
    fclose(f);

    return value;
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

    xTaskCreatePinnedToCore(Matrix_Task, "MatrixTask", 20000, NULL, 5,
                            &matrixTask, 1);

    /* show boot applet */
    showApplet("boot");

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

    /* Initialize WiFi, this will start the provisioner/mqtt client as
     * necessary */
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(wifi_mode_t::WIFI_MODE_STA));

    ESP_ERROR_CHECK(esp_wifi_start());

    char *private_key = read_file_from_littlefs("client.key");
    char *certificate = read_file_from_littlefs("client.crt");

    /* Setup MQTT */
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {.address = "mqtts://***REDACTED***",
                   .verification =
                       {
                           .crt_bundle_attach = esp_crt_bundle_attach,
                       }},
        .credentials = {.authentication = {
                            .certificate = certificate,
                            .key = private_key,
                        }}};

    mqttClient = esp_mqtt_client_init(&mqtt_cfg);

    esp_mqtt_client_register_event(mqttClient, MQTT_EVENT_ANY,
                                   mqtt_event_handler, NULL);
}

void showNextApplet() {
    currentlyShowingApplet++;
    if (currentlyShowingApplet >= scheduledAppletCount) {
        currentlyShowingApplet = 0;
    }
    char newAppletName[3];
    snprintf(newAppletName, 3, "%d", currentlyShowingApplet);

    /* show connect_cloud applet */
    showApplet(newAppletName);
    char resp[200];
    snprintf(resp, 200,
             "{\"type\":\"success\",\"info\":\"applet_displayed\","
             "\"appid\":%s}",
             newAppletName);
    esp_mqtt_client_publish(mqttClient, statusTopic, resp, 0, 1, 0);
    currentAppletStartTime = pdTICKS_TO_MS(xTaskGetTickCount());
}

void showApplet(const char *name) {
    char fileName[40];
    snprintf(fileName, 40, "/littlefs/%s.webp", name);
    FILE *f = fopen(fileName, "r");
    if (f == NULL) {
        ESP_LOGW("smx/dec", "couldn't find applet %s", fileName);
        return;
    }

    fseek(f, 0, SEEK_END);
    size_t fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    xTaskNotify(matrixTask, MATRIX_TASK_NOTIF_NOT_READY,
                eSetValueWithOverwrite);

    WebPDataClear(&webPData);

    webPData.bytes = (uint8_t *)WebPMalloc(fileSize);
    webPData.size = fileSize;

    fread((void *)webPData.bytes, 1, fileSize, f);
    fclose(f);

    xTaskNotify(matrixTask, MATRIX_TASK_NOTIF_READY, eSetValueWithOverwrite);
}

void startProvisioner() {
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
    ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_0, NULL,
                                                     service_name, NULL));
}
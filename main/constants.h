#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <stdio.h>

#define MFG_PARTITION_NAME "mfg_data"

/* Logger tags */
#define PROV_TAG "[smx/provisioner]"
#define BOOT_TAG "[smx/boot]"
#define WORKER_TAG "[smx/worker]"
#define MATRIX_TAG "[smx/matrix]"
#define MQTT_TAG "[smx/mqtt_ell]"
#define MQTT_TASK_TAG "[smx/mqtt_task]"
#define WIFI_TAG "[smx/wifi]"
#define HTTP_TAG "[smx/http]"
#define SCHEDULE_TAG "[smx/schedule]"
#define OTA_TAG "[smx/ota]"
#define BUTTON_TAG "[smx/button]"

/* Inter-task communication values */
#define MATRIX_TASK_NOTIF_READY 1
#define MATRIX_TASK_NOTIF_NOT_READY 2
#define MATRIX_TASK_NOTIF_SLEEP 3
#define MATRIX_TASK_NOTIF_WAKE_UP 4
#define SCHEDULE_TASK_NOTIF_SKIP_TO_NEXT 1
#define SCHEDULE_TASK_NOTIF_SKIP_TO_PINNED 2

enum WorkItemType {
    SHOW_SPRITE,
    UPDATE_REPORTED_SHADOW,
    MARK_JOB_COMPLETE,
    STORE_RECEIVED_SPRITE,
};

enum IoTJobOperation { SPRITE_DELIVERY, OTA_UPDATE };

#define R1_PIN 40
#define G1_PIN 39
#define B1_PIN 38
#define R2_PIN 46
#define G2_PIN 3
#define B2_PIN 9
#define A_PIN 21
#define B_PIN 47
#define C_PIN 48
#define D_PIN 45
#define E_PIN -1  // required for 1/32 scan panels, like 64x64. Any available pin would do, i.e. IO32
#define LAT_PIN 16
#define OE_PIN 17
#define CLK_PIN 18

#define IO_BTN_USER1 GPIO_NUM_11
#define IO_BTN_USER2 GPIO_NUM_12

#define STREAM_CHUNK_SIZE 30000
#define MAX_OPEN_STREAMS 20

typedef struct scheduledItem {
    int show_duration;
    bool is_pinned;
    bool is_skipped;
    bool reported_error;
    uint8_t *pData = nullptr;
    size_t dataLen;
} scheduledItem;

typedef struct workItem {
    WorkItemType workItemType;
    char workItemString[100];
    int workItemInteger;
    void *pArg;
} workItem;

typedef struct mqttMessage {
    char topic[200];
    const char *pMessage;
    size_t messageLen;
} mqttMessage;

#endif
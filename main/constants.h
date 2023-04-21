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

/* Inter-task communication values */
#define MATRIX_TASK_NOTIF_READY 1
#define MATRIX_TASK_NOTIF_NOT_READY 2

enum WorkItemType {
    SHOW_SPRITE,
    UPDATE_REPORTED_SHADOW,
    MARK_JOB_COMPLETE,
    REQUEST_STREAM_CHUNK,
    HANDLE_COMPLETE_STREAM_DATA,
};

enum IoTJobOperation { SPRITE_DELIVERY, OTA_UPDATE };

#define R1_PIN 35
#define G1_PIN 37
#define B1_PIN 36
#define R2_PIN 34
#define G2_PIN 9
#define B2_PIN 8
#define A_PIN 7
#define B_PIN 6
#define C_PIN 21
#define D_PIN 5
#define E_PIN -1  // required for 1/32 scan panels, like 64x64. Any available pin would do, i.e. IO32
#define LAT_PIN 4
#define OE_PIN 2
#define CLK_PIN 1

#define STREAM_CHUNK_SIZE 30000

typedef struct scheduledItem {
    int show_duration;
    bool is_pinned;
    bool is_skipped;
    bool reported_error;
} scheduledItem;

typedef struct workItem {
    WorkItemType workItemType;
    char workItemString[100];
    int workItemInteger;
} workItem;

typedef struct mqttMessage {
    char topic[200];
    const char *pMessage;
    size_t messageLen;
} mqttMessage;

typedef struct spriteDeliveryItem {
    char streamID[129];
    int spriteID;
    size_t spriteSize;
} spriteDeliveryItem;

#endif
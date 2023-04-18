#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <stdio.h>

/* Logger tags */
#define PROV_TAG "[smx/provisioner]"
#define BOOT_TAG "[smx/boot]"
#define WORKER_TAG "[smx/worker]"
#define MATRIX_TAG "[smx/matrix]"
#define MQTT_TAG "[smx/mqtt]"
#define WIFI_TAG "[smx/wifi]"
#define HTTP_TAG "[smx/http]"
#define SCHEDULE_TAG "[smx/schedule]"

/* Inter-task communication values */
#define MATRIX_TASK_NOTIF_READY 1
#define MATRIX_TASK_NOTIF_NOT_READY 2

#define WORKITEM_TYPE_HANDLE_DESIRED_SHADOW_UPDATE 1
#define WORKITEM_TYPE_SHOW_SPRITE 2
#define WORKITEM_TYPE_UPDATE_REPORTED_SHADOW 3
#define WORKITEM_TYPE_START_PROVISIONER 4

#define R1_PIN 35
#define G1_PIN 37
#define B1_PIN 36
#define R2_PIN 34
#define G2_PIN 9
#define B2_PIN 8
#define A_PIN  7
#define B_PIN  6
#define C_PIN  21
#define D_PIN  5
#define E_PIN  -1 // required for 1/32 scan panels, like 64x64. Any available pin would do, i.e. IO32
#define LAT_PIN 4
#define OE_PIN  2
#define CLK_PIN 1

typedef struct scheduledItem {
    int show_duration;
    bool is_pinned;
    bool is_skipped;
    char data_md5[33];
} scheduledItem;

typedef struct workItem
{
    uint8_t workItemType;
    char workItemString[200];
    int workItemInteger;
} workItem;

#endif
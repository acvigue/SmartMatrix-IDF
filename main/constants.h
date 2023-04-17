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
#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <stdio.h>

/* Logger tags */
#define PROV_TAG "[smx/provisioner]"
#define BOOT_TAG "[smx/boot]"
#define MATRIX_TAG "[smx/display]"
#define MQTT_TAG "[smx/mqtt]"
#define WIFI_TAG "[smx/wifi]"
#define WORKER_TAG "[smx/worker]"
#define HTTP_TAG "[smx/worker]"

/* Inter-task communication values */
#define WORKITEM_TYPE_SHOW_SPRITE 1
#define WORKITEM_TYPE_DOWNLOAD_SPRITE 2
#define MATRIX_TASK_NOTIF_READY 1
#define MATRIX_TASK_NOTIF_NOT_READY 2

struct workerQueueItem {
    uint8_t type;
    int numericParameter;
    char charParameter[300];
};

struct httpDownloadItem {
    int spriteID;
    bool shouldDownload;
    char receivedHash[33];
};

#endif
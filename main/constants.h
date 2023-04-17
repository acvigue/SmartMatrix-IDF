#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <stdio.h>

/* Logger tags */
#define PROV_TAG "[smx/provisioner]"
#define BOOT_TAG "[smx/boot]"
#define MATRIX_TAG "[smx/display]"
#define MQTT_TAG "[smx/mqtt]"
#define WIFI_TAG "[smx/wifi]"
#define HTTP_TAG "[smx/http]"
#define SCHEDULE_TAG "[smx/schedule]"

/* Inter-task communication values */
#define MATRIX_TASK_NOTIF_READY 1
#define MATRIX_TASK_NOTIF_NOT_READY 2

#endif
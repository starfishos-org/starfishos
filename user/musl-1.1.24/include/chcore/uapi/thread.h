#pragma once

#define THREAD_TYPE_USER     0   /* A normal user-level thread */
#define THREAD_TYPE_SHADOW   1   /* A shadow thread for serving IPC requests */
#define THREAD_TYPE_REGISTER 2   /* A register cb thread for serving IPC registration */
#define THREAD_TYPE_SERVICES 3   /* A system thread */

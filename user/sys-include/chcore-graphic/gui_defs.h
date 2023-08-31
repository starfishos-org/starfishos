#pragma once

#include <chcore/type.h>

enum GUI_REQ {
	GUI_CONN
};

enum GUI_CLIENT_TYPE {
	GUI_CLIENT_CHCORE,
	GUI_CLIENT_WAYLAND
};

/*
 * IPC request that GUI client sends to GUI server
 */
struct gui_request
{
	enum GUI_REQ req;
	enum GUI_CLIENT_TYPE client_type;
};

typedef struct
{
	u32 mods_depressed;
	u32 mods_latched;
	u32 mods_locked;
	u32 keys[6];
} raw_keyboard_event_t;

typedef struct
{
	u32 buttons;
	int x;
	int y;
	int scroll;
} raw_pointer_event_t;

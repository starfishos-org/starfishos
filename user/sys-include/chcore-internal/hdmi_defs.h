#pragma once

enum HDMI_REQ {
	HDMI_GET_FB
};

struct hdmi_request {
	enum HDMI_REQ req;
	int width;
	int height;
	int depth;
	int ret_width;
	int ret_height;
	int ret_depth;
	int ret_pitch;
};

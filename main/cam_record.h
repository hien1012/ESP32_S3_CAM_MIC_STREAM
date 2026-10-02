#pragma once
#include "esp_err.h"

esp_err_t cam_record_init(void);
void cam_stream_start(void);
void cam_stream_stop(void);

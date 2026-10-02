#pragma once
#include "esp_err.h"

esp_err_t audio_record_init(void);
void audio_stream_start(void);
void audio_stream_stop(void);

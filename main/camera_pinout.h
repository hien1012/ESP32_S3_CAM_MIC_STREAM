#pragma once

#define CAM_PIN_PWDN   -1
#define CAM_PIN_RESET  -1

#define CAM_PIN_XCLK   39

#define CAM_PIN_SIOD   2   // I2C SDA
#define CAM_PIN_SIOC   1   // I2C SCL

#define CAM_PIN_D0     47
#define CAM_PIN_D1     21
#define CAM_PIN_D2     13
#define CAM_PIN_D3     8
#define CAM_PIN_D4     18
#define CAM_PIN_D5     17
#define CAM_PIN_D6     16
#define CAM_PIN_D7     38

#define CAM_PIN_VSYNC  42
#define CAM_PIN_HREF   41
#define CAM_PIN_PCLK   40

#define CAM_IMAGE_WIDTH  640
#define CAM_IMAGE_HEIGHT 480
#define CAM_IMAGE_FRAME_BYTES (CAM_IMAGE_WIDTH * CAM_IMAGE_HEIGHT)  // 307200
#define CAM_TARGET_FPS   5
#define CAM_RECORD_DURATION_SEC 10

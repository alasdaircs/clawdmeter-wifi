#include "../../hal/touch_hal.h"
#include "../../hal/imu_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <TouchDrvCSTXXX.hpp>

static TouchDrvCST92xx touch;

static volatile bool     touch_data_ready = false;
static volatile bool     touch_pressed = false;
static volatile uint16_t touch_x = 0;
static volatile uint16_t touch_y = 0;

static void IRAM_ATTR touch_isr(void) {
    touch_data_ready = true;
}

void touch_hal_init(void) {
    touch.setPins(TP_RST, TP_INT);
    if (!touch.begin(Wire, CST9220_ADDR, IIC_SDA, IIC_SCL)) {
        Serial.println("Touch init failed");
        return;
    }
    touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
    touch.setSwapXY(true);
    touch.setMirrorXY(true, false);
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(TP_INT, touch_isr, FALLING);
    Serial.println("Touch init OK");
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    if (touch_data_ready) {
        touch_data_ready = false;
        int16_t tx[5], ty[5];
        uint8_t n = touch.getPoint(tx, ty, touch.getSupportTouchPoint());
        if (n > 0) {
            touch_pressed = true;
            touch_x = (uint16_t)tx[0];
            touch_y = (uint16_t)ty[0];
        } else {
            touch_pressed = false;
        }
    }
    // The controller reports panel-space coordinates, but IMU auto-rotation
    // renders the UI rotated via CPU pixel remapping (display.cpp's
    // rotate_strip). Apply the inverse of that transform so touch lands on
    // the widget the user sees — position-sensitive touch (settings slider/
    // switch) breaks in rotated orientations otherwise.
    //   display r=1 maps logical (x,y) -> panel (S-1-y, x)
    //   display r=2 maps logical (x,y) -> panel (S-1-x, S-1-y)
    //   display r=3 maps logical (x,y) -> panel (y, S-1-x)
    // The +1 offset: the controller's swap/mirror config (gotcha #7) was
    // tuned empirically with the device sitting in IMU quadrant 3, so the
    // reported coordinates already include one quadrant of compensation —
    // verified on hardware (without the offset, touch is uniformly 90° off).
    const uint16_t S = LCD_WIDTH;  // square panel: LCD_WIDTH == LCD_HEIGHT
    uint16_t px = touch_x, py = touch_y;
    switch ((imu_hal_rotation_quadrant() + 1) & 3) {
    case 1:  *x = py;          *y = S - 1 - px; break;
    case 2:  *x = S - 1 - px;  *y = S - 1 - py; break;
    case 3:  *x = S - 1 - py;  *y = px;         break;
    default: *x = px;          *y = py;         break;
    }
    *pressed = touch_pressed;
}

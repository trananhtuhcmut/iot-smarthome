#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <lvgl.h>

// Chan cam ung rieng biet tren ESP32-2432S028 (bus VSPI)
#define XPT2046_IRQ   36
#define XPT2046_MOSI  32
#define XPT2046_MISO  39
#define XPT2046_CLK   25
#define XPT2046_CS    33

static const uint16_t SCREEN_WIDTH  = 320;
static const uint16_t SCREEN_HEIGHT = 240;

TFT_eSPI tft = TFT_eSPI();
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[SCREEN_WIDTH * 10];
static lv_obj_t *lbl_status;
static int tap_count = 0;

void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)&color_p->full, w * h, true);
    tft.endWrite();

    lv_disp_flush_ready(disp);
}

void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
    if (touchscreen.tirqTouched() && touchscreen.touched()) {
        TS_Point p = touchscreen.getPoint();

        int x = map(p.x, 200, 3700, 0, SCREEN_WIDTH - 1);
        int y = map(p.y, 240, 3800, 0, SCREEN_HEIGHT - 1);
        x = constrain(x, 0, SCREEN_WIDTH - 1);
        y = constrain(y, 0, SCREEN_HEIGHT - 1);

        data->state = LV_INDEV_STATE_PR;
        data->point.x = x;
        data->point.y = y;

        Serial.printf("Touch -> raw(%d, %d) | screen(%d, %d)\n", p.x, p.y, x, y);
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

static void btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        tap_count++;
        lv_label_set_text_fmt(lbl_status, "Da cham nut: %d lan", tap_count);
    }
}

void setup() {
    Serial.begin(115200);

    // Bat den nen man hinh
    pinMode(21, OUTPUT);
    digitalWrite(21, HIGH);

    // Khoi tao man hinh TFT ngang 320x240
    tft.begin();
    tft.setRotation(1);
    tft.invertDisplay(false); // Doi thanh true neu mau bi am ban

    // Khoi tao cam ung XPT2046
    SPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
    touchscreen.begin();
    touchscreen.setRotation(1);

    // Khoi tao LVGL
    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, SCREEN_WIDTH * 10);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = SCREEN_WIDTH;
    disp_drv.ver_res = SCREEN_HEIGHT;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    // Giao dien test don gian
    lv_obj_t *title = lv_label_create(lv_scr_act());
    lv_label_set_text(title, "ESP32-2432S028 LVGL Ready");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *btn = lv_btn_create(lv_scr_act());
    lv_obj_set_size(btn, 180, 60);
    lv_obj_center(btn);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "TEST CHAM");
    lv_obj_center(btn_lbl);

    lbl_status = lv_label_create(lv_scr_act());
    lv_label_set_text(lbl_status, "Hay bam vao nut o giua");
    lv_obj_align(lbl_status, LV_ALIGN_BOTTOM_MID, 0, -28);

    Serial.println("LVGL CYD Init Done!");
}

void loop() {
    lv_tick_inc(5);
    lv_timer_handler();
    delay(5);
}
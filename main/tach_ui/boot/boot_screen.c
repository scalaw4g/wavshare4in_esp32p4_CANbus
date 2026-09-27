#include "boot_screen.h"
#include "ui.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

lv_obj_t *boot_screen = NULL;
static lv_obj_t *boot_logo;
static lv_obj_t *forward_button_label;
static SemaphoreHandle_t selection_semaphore;
static lv_timer_t *selection_timer;
static bool forward_mode_selected;

#define BOOT_SELECTION_TIMEOUT_MS 3500

/* ---------- Callbacks ---------- */

static void logo_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa(obj, v, 0);
}

static void select_forward_mode_cb(lv_event_t *event)
{
    forward_mode_selected = true;
    if (selection_timer) {
        lv_timer_del(selection_timer);
        selection_timer = NULL;
    }
    lv_obj_set_style_bg_color(lv_event_get_target(event), lv_color_hex(0xD53B35), LV_PART_MAIN);
    lv_label_set_text(forward_button_label, "CAN FORWARDING");
    xSemaphoreGive(selection_semaphore);
}

static void boot_selection_timeout_cb(lv_timer_t *timer)
{
    lv_timer_del(timer);
    selection_timer = NULL;
    forward_mode_selected = false;
    xSemaphoreGive(selection_semaphore);
    boot_finish(ui_Screen1);
}

/* ---------- Create Screen ---------- */
void boot_screen_create(void)
{
    selection_semaphore = xSemaphoreCreateBinary();
    configASSERT(selection_semaphore != NULL);
    forward_mode_selected = false;

    boot_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(boot_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(boot_screen, LV_OPA_COVER, 0);

    boot_logo = lv_img_create(boot_screen);
    lv_img_set_src(boot_logo, &ui_scumpunk);
    lv_obj_center(boot_logo);
    lv_obj_set_style_opa(boot_logo, LV_OPA_0, 0);

    lv_obj_t *forward_button = lv_btn_create(boot_screen);
    lv_obj_set_size(forward_button, 220, 64);
    lv_obj_align(forward_button, LV_ALIGN_BOTTOM_MID, 0, -32);
    lv_obj_set_style_bg_color(forward_button, lv_color_hex(0x55585E), LV_PART_MAIN);
    lv_obj_set_style_bg_color(forward_button, lv_color_hex(0xD53B35), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(forward_button, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(forward_button, 6, 0);
    lv_obj_add_event_cb(forward_button, select_forward_mode_cb, LV_EVENT_CLICKED, NULL);

    forward_button_label = lv_label_create(forward_button);
    lv_label_set_text(forward_button_label, "CAN FORWARD");
    lv_obj_center(forward_button_label);

    // Glow
    //lv_obj_set_style_shadow_color(
    //    boot_logo, lv_color_hex(0xFF1E1E), 0
    //);
}

/* ---------- Start Animation ---------- */
void boot_start(void)
{
    lv_scr_load(boot_screen);

    lv_anim_t a;

    lv_anim_init(&a);
    lv_anim_set_var(&a, boot_logo);
    lv_anim_set_exec_cb(&a, logo_opa_cb);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_time(&a, 500);
    lv_anim_set_delay(&a, 700);
    lv_anim_start(&a);

    selection_timer = lv_timer_create(boot_selection_timeout_cb,
                                      BOOT_SELECTION_TIMEOUT_MS, NULL);
}

bool boot_screen_wait_for_forward_mode(void)
{
    xSemaphoreTake(selection_semaphore, portMAX_DELAY);
    return forward_mode_selected;
}

/* ---------- Exit ---------- */
void boot_finish(lv_obj_t *next_screen)
{
    // Animate fade from boot screen → main UI
    lv_scr_load_anim(next_screen, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0, true);

    // The transition owns deletion of the previous screen.
    boot_screen = NULL;
}
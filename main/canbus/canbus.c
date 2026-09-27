#include "canbus.h"
#include "protocol_loader.h"

#include "driver/twai.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_spiffs.h"

static const char *TAG = "CANBUS";

static const int can_rates[] = {
    1000000,
    500000,
    250000,
    125000
};

#define NUM_RATES (sizeof(can_rates)/sizeof(can_rates[0]))
#define SLCAN_QUEUE_LENGTH 256
#define SLCAN_USB_TX_BUFFER_SIZE 8192
#define SLCAN_USB_RX_BUFFER_SIZE 1024

typedef struct {
    uint32_t identifier;
    uint8_t data[8];
    uint8_t data_length;
    bool extended;
    bool remote;
    uint16_t timestamp_ms;
} slcan_frame_t;

static QueueHandle_t slcan_tx_queue;
static portMUX_TYPE slcan_state_mux = portMUX_INITIALIZER_UNLOCKED;
static bool slcan_open;
static bool slcan_timestamps;
static uint8_t slcan_status_flags;
static int active_can_bitrate;


// =======================================================
// GPIO CONFIG
// =======================================================

#define CAN_TX GPIO_NUM_5
#define CAN_RX GPIO_NUM_4

// =======================================================
// GLOBAL DATA
// =======================================================

volatile can_dash_data_t can_data = {0};


// =======================================================
// CAN FRAME PROCESSOR
// =======================================================

void process_can_frame(uint32_t id, uint8_t *data){
    protocol_detect(id);

    if(!active_protocol)
        return;

    if(id >= CAN_ID_MAX)
        return;

    can_frame_def_t *frame = frame_lookup[id];

    if(!frame)
        return;

    for(int s=0;s<frame->signal_count;s++){
        can_signal_t *sig = &frame->signals[s];

        uint32_t raw = 0;

        if(sig->len==2){
            if(sig->endian==ENDIAN_BIG)
                raw=(data[sig->offset]<<8)|data[sig->offset+1];
            else
                raw=(data[sig->offset+1]<<8)|data[sig->offset];
        }
        else{
            raw=data[sig->offset];
        }

        if(sig->target)
            *sig->target = raw*sig->scale + sig->offset_val;
    }
}


void mount_fs() {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 10,
        .format_if_mount_failed = true
    };

    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));
}

static twai_timing_config_t get_timing(int bitrate){
    twai_timing_config_t t;

    switch (bitrate){
        case 1000000:
            t = (twai_timing_config_t)TWAI_TIMING_CONFIG_1MBITS();
            break;

        case 500000:
            t = (twai_timing_config_t)TWAI_TIMING_CONFIG_500KBITS();
            break;

        case 250000:
            t = (twai_timing_config_t)TWAI_TIMING_CONFIG_250KBITS();
            break;

        case 125000:
            t = (twai_timing_config_t)TWAI_TIMING_CONFIG_125KBITS();
            break;

        default:
            t = (twai_timing_config_t)TWAI_TIMING_CONFIG_500KBITS();
            break;
    }

    return t;
}


int detect_can_bitrate(bool listen_only)
{
    twai_general_config_t g_config =
        TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX, CAN_RX,
                                    listen_only ? TWAI_MODE_LISTEN_ONLY : TWAI_MODE_NORMAL);

    twai_filter_config_t f_config =
        TWAI_FILTER_CONFIG_ACCEPT_ALL();

    twai_message_t msg;

    for (int i = 0; i < NUM_RATES; i++){
        int rate = can_rates[i];

        twai_timing_config_t t_config = get_timing(rate);

        ESP_LOGI(TAG, "Trying CAN bitrate %d", rate);

        ESP_ERROR_CHECK(twai_driver_install(&g_config, &t_config, &f_config));
        ESP_ERROR_CHECK(twai_start());

        int frames = 0;
        int timeout = 20;

        while (timeout--){
            if (twai_receive(&msg, pdMS_TO_TICKS(10)) == ESP_OK){
                if (msg.rtr)
                    continue;

                frames++;

                if (frames >= 3){
                    ESP_LOGI(TAG, "Detected CAN bitrate %d", rate);

                    twai_stop();
                    twai_driver_uninstall();

                    return rate;
                }
            }
        }

        twai_stop();
        twai_driver_uninstall();
    }

    ESP_LOGW(TAG, "CAN bitrate detection failed, defaulting to 500k");

    return 500000;
}


// =======================================================
// CAN INIT
// =======================================================

static void slcan_set_status_flag(uint8_t flag)
{
    portENTER_CRITICAL(&slcan_state_mux);
    slcan_status_flags |= flag;
    portEXIT_CRITICAL(&slcan_state_mux);
}

static int slcan_bitrate_from_code(char code)
{
    switch (code) {
        case '4': return 125000;
        case '5': return 250000;
        case '6': return 500000;
        case '8': return 1000000;
        default: return 0;
    }
}

static void slcan_write(const char *data, size_t length)
{
    if (usb_serial_jtag_write_bytes(data, length, pdMS_TO_TICKS(50)) != (int)length)
        slcan_set_status_flag(0x01);
}

static void slcan_process_command(const char *command, size_t length)
{
    static const char ack[] = "\r";
    static const char nak[] = "\a";

    if (length == 0)
        return;

    switch (command[0]) {
        case 'O':
        case 'L':
            if (length != 1) {
                slcan_write(nak, sizeof(nak) - 1);
                return;
            }
            xQueueReset(slcan_tx_queue);
            portENTER_CRITICAL(&slcan_state_mux);
            slcan_open = true;
            portEXIT_CRITICAL(&slcan_state_mux);
            slcan_write(ack, sizeof(ack) - 1);
            break;

        case 'C':
            portENTER_CRITICAL(&slcan_state_mux);
            slcan_open = false;
            portEXIT_CRITICAL(&slcan_state_mux);
            xQueueReset(slcan_tx_queue);
            slcan_write(ack, sizeof(ack) - 1);
            break;

        case 'S':
            if (length == 2 && slcan_bitrate_from_code(command[1]) == active_can_bitrate)
                slcan_write(ack, sizeof(ack) - 1);
            else
                slcan_write(nak, sizeof(nak) - 1);
            break;

        case 'Z':
            if (length == 2 && (command[1] == '0' || command[1] == '1')) {
                portENTER_CRITICAL(&slcan_state_mux);
                slcan_timestamps = command[1] == '1';
                portEXIT_CRITICAL(&slcan_state_mux);
                slcan_write(ack, sizeof(ack) - 1);
            } else {
                slcan_write(nak, sizeof(nak) - 1);
            }
            break;

        case 'V':
            slcan_write("V0100\r", 6);
            break;

        case 'N':
            slcan_write("N0001\r", 6);
            break;

        case 'F': {
            char response[5];
            portENTER_CRITICAL(&slcan_state_mux);
            uint8_t status = slcan_status_flags;
            slcan_status_flags = 0;
            portEXIT_CRITICAL(&slcan_state_mux);
            int response_length = snprintf(response, sizeof(response), "F%02X\r", status);
            slcan_write(response, response_length);
            break;
        }

        case 'M':
        case 'm':
            slcan_write(ack, sizeof(ack) - 1);
            break;

        default:
            slcan_write(nak, sizeof(nak) - 1);
            break;
    }
}

static void slcan_control_task(void *arg)
{
    (void)arg;
    char command[16];
    size_t command_length = 0;
    bool command_overflow = false;
    uint8_t input[64];

    while (1) {
        int bytes_read = usb_serial_jtag_read_bytes(input, sizeof(input), pdMS_TO_TICKS(100));
        for (int i = 0; i < bytes_read; i++) {
            if (input[i] == '\r' || input[i] == '\n') {
                if (!command_overflow)
                    slcan_process_command(command, command_length);
                command_length = 0;
                command_overflow = false;
            } else if (command_length < sizeof(command)) {
                command[command_length++] = (char)input[i];
            } else {
                command_overflow = true;
            }
        }
    }
}

static void slcan_tx_task(void *arg)
{
    (void)arg;
    slcan_frame_t frame;
    char output[40];

    while (1) {
        if (xQueueReceive(slcan_tx_queue, &frame, portMAX_DELAY) != pdTRUE)
            continue;

        bool is_open;
        bool timestamps;
        portENTER_CRITICAL(&slcan_state_mux);
        is_open = slcan_open;
        timestamps = slcan_timestamps;
        portEXIT_CRITICAL(&slcan_state_mux);
        if (!is_open)
            continue;

        char frame_type = frame.remote ? (frame.extended ? 'R' : 'r')
                                       : (frame.extended ? 'T' : 't');
        int output_length;
        if (frame.extended)
            output_length = snprintf(output, sizeof(output), "%c%08" PRIX32 "%X",
                                     frame_type, frame.identifier, frame.data_length);
        else
            output_length = snprintf(output, sizeof(output), "%c%03" PRIX32 "%X",
                                     frame_type, frame.identifier, frame.data_length);
        if (output_length < 0 || (size_t)output_length >= sizeof(output)) {
            slcan_set_status_flag(0x01);
            continue;
        }

        size_t offset = output_length;
        if (!frame.remote) {
            for (uint8_t i = 0; i < frame.data_length; i++) {
                int written = snprintf(output + offset, sizeof(output) - offset,
                                       "%02X", frame.data[i]);
                if (written < 0 || (size_t)written >= sizeof(output) - offset) {
                    offset = 0;
                    break;
                }
                offset += written;
            }
        }

        if (offset == 0) {
            slcan_set_status_flag(0x01);
            continue;
        }

        if (timestamps) {
            int written = snprintf(output + offset, sizeof(output) - offset,
                                   "%04X", frame.timestamp_ms);
            if (written < 0 || (size_t)written >= sizeof(output) - offset) {
                slcan_set_status_flag(0x01);
                continue;
            }
            offset += written;
        }
        output[offset++] = '\r';

        if (usb_serial_jtag_write_bytes(output, offset, pdMS_TO_TICKS(20)) != (int)offset)
            slcan_set_status_flag(0x01);
    }
}

static void enqueue_slcan_frame(const twai_message_t *message)
{
    slcan_frame_t frame = {
        .identifier = message->identifier,
        .data_length = message->data_length_code > sizeof(frame.data)
            ? sizeof(frame.data)
            : message->data_length_code,
        .extended = message->extd,
        .remote = message->rtr,
        .timestamp_ms = (uint16_t)(esp_timer_get_time() / 1000)
    };
    if (!frame.remote)
        memcpy(frame.data, message->data, frame.data_length);

    if (xQueueSend(slcan_tx_queue, &frame, 0) != pdTRUE)
        slcan_set_status_flag(0x01);
}

void canbus_init(bool forward_mode)
{
    if (!forward_mode) {
        mount_fs();
        protocol_loader_init();
    }

    active_can_bitrate = detect_can_bitrate(forward_mode);

    twai_general_config_t g_config =
        TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX, CAN_RX,
                                    forward_mode ? TWAI_MODE_LISTEN_ONLY : TWAI_MODE_NORMAL);

    twai_timing_config_t t_config = get_timing(active_can_bitrate);

    twai_filter_config_t f_config =
        TWAI_FILTER_CONFIG_ACCEPT_ALL();

    ESP_ERROR_CHECK(twai_driver_install(&g_config, &t_config, &f_config));
    ESP_ERROR_CHECK(twai_start());

    ESP_LOGI(TAG, "CAN initialized at %d", active_can_bitrate);

    if (forward_mode) {
        slcan_tx_queue = xQueueCreate(SLCAN_QUEUE_LENGTH, sizeof(slcan_frame_t));
        ESP_ERROR_CHECK(slcan_tx_queue ? ESP_OK : ESP_ERR_NO_MEM);

        if (xTaskCreatePinnedToCore(slcan_tx_task, "slcan_tx", 3072,
                                    NULL, 8, NULL, 1) != pdPASS ||
            xTaskCreatePinnedToCore(slcan_control_task, "slcan_control", 3072,
                                    NULL, 9, NULL, 1) != pdPASS)
            ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
        ESP_LOGI(TAG, "SLCAN ready at %d bps", active_can_bitrate);
    }
}

void canbus_usb_init(void)
{
    usb_serial_jtag_driver_config_t usb_config = {
        .tx_buffer_size = SLCAN_USB_TX_BUFFER_SIZE,
        .rx_buffer_size = SLCAN_USB_RX_BUFFER_SIZE
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_config));
}


// =======================================================
// CAN RECEIVE TASK
// =======================================================

void canbus_task(void *arg){
    twai_message_t message;
    bool forward_mode = (bool)(uintptr_t)arg;

    while (1){
        if (twai_receive(&message, pdMS_TO_TICKS(10)) == ESP_OK){
            if (forward_mode) {
                bool is_open;
                portENTER_CRITICAL(&slcan_state_mux);
                is_open = slcan_open;
                portEXIT_CRITICAL(&slcan_state_mux);
                if (is_open)
                    enqueue_slcan_frame(&message);
            } else if (!message.extd && !message.rtr) {
                process_can_frame(message.identifier, message.data);
            }
        }
    }
}
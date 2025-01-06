#include <string.h>
#include <stdint.h>

#include "esp_log.h"

#include "driver/gpio.h"
#include "driver/rmt_tx.h"

/*
 * This section is needed for reconfiguring the Tx GPIO just before transmission to output mode
 * and then back to input mode after transmission. This is necessary to prevent interference with
 * other transmissions. Reconfiguring using new Rx RMT channels each time would be too time-consuming.
 */
#include "driver/rmt_types.h"                      // Include RMT driver types
#include "../src/rmt_private.h"                    // Include private RMT definitions
#include "soc/rmt_periph.h"                        // Required for accessing rmt_signal_conn_t and rmt_periph_signals.
extern const rmt_signal_conn_t rmt_periph_signals; // External declaration of RMT peripheral signals

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pd_config.h"
#include "pd_coding.h"
#include "pd_rx.h"
#include "pd_tx.h"
#include "pd.h"
#include "crc32.h"

#define TAG "PD-TX"

#define PD_TX_FREQ (10000000)
#define PD_TX_SYMS (64)
#define PD_TX_SHORT_DURATION (16) /* 1.6us */

static rmt_channel_handle_t pd_tx_chan;
static rmt_encoder_handle_t pd_tx_encoder;

static volatile bool pd_tx_ongoing_flag = false;

bool IRAM_ATTR pd_tx_ongoing()
{
    return pd_tx_ongoing_flag;
}

void IRAM_ATTR pd_tx_active()
{
#ifdef GPIO_CC1
    gpio_set_drive_capability(GPIO_CC1, GPIO_DRIVE_CAP_0);
#endif
    gpio_set_drive_capability(GPIO_TX, GPIO_DRIVE_CAP_0);
    esp_rom_gpio_connect_out_signal(
        GPIO_TX,
        rmt_periph_signals.groups[pd_tx_chan->group->group_id].channels[pd_tx_chan->channel_id + RMT_TX_CHANNEL_OFFSET_IN_GROUP].tx_sig,
        true, false);
#ifdef GPIO_CC1
    gpio_set_direction(GPIO_CC1, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_CC1, 0);
    /* we now have configured the CC1 pin and TX pin to drive against each other.
    with these drive capabilities, we get a voltage divider to approx 1.7V, which is closer to
    the expected 1.1V on the PD lines */
#else
    /* we now have a weak TX pin driving 3.3V on the PD bus line. also works, its just far from specs. */
#endif
}

void IRAM_ATTR pd_tx_inactive()
{
    /* now configure the TX pin back to an input so communication isn't blocked. order is chosen that there are less spikes. */
#ifdef GPIO_CC1
    gpio_set_direction(GPIO_CC1, GPIO_MODE_INPUT);
#endif
    gpio_set_direction(GPIO_TX, GPIO_MODE_INPUT);
#ifdef GPIO_CC1
    gpio_set_pull_mode(GPIO_CC1, GPIO_FLOATING);
#endif
    gpio_set_pull_mode(GPIO_TX, GPIO_FLOATING);
}

static IRAM_ATTR void add_bit(pd_tx_ctx_t *ctx, rmt_symbol_word_t *symbols, size_t *symbols_used, bool bit, uint32_t short_duration)
{
    /*
     * In this encoding scheme:
     * - A 0-bit creates two equally long pulses of the same level.
     * - A 1-bit creates an alternating bit pair.
     */
    symbols[*symbols_used].level0 = ctx->level;
    symbols[*symbols_used].duration0 = short_duration;
    symbols[*symbols_used].duration1 = short_duration;
    if (!bit)
    {
        ctx->level = !ctx->level;
    }
    symbols[*symbols_used].level1 = !ctx->level;

    (*symbols_used)++;
}

static IRAM_ATTR void add_half(pd_tx_ctx_t *ctx, rmt_symbol_word_t *symbols, size_t *symbols_used, uint8_t half)
{
    for (int pos = 0; pos < 5; pos++)
    {
        add_bit(ctx, symbols, symbols_used, half & 1, PD_TX_SHORT_DURATION);
        half >>= 1;
    }
}

static IRAM_ATTR size_t pd_tx_enc_cbr(const void *data, size_t data_size,
                                      size_t symbols_written, size_t symbols_free,
                                      rmt_symbol_word_t *symbols, bool *done, void *arg)
{
    size_t symbols_used = 0;
    pd_tx_ctx_t *ctx = (pd_tx_ctx_t *)arg;

    if (symbols_written == 0)
    {
        /* reset all states */
        memset(ctx, 0, sizeof(pd_tx_ctx_t));
        ctx->state = PD_TX_PATTERN;
        ctx->level = true;
    }

    /* We need a minimum of 10 symbol spaces per byte */
    bool loop = true;
    while (loop)
    {
        if (symbols_used >= symbols_free)
        {
            break;
        }

        switch (ctx->state)
        {
        case PD_TX_PATTERN:
        {
            if (ctx->sync_bits >= 64)
            {
                ctx->state = PD_TX_SYNC;
                break;
            }
            add_bit(ctx, symbols, &symbols_used, ctx->sync_bits & 1, PD_TX_SHORT_DURATION);
            ctx->sync_bits++;
            break;
        }

        case PD_TX_SYNC:
        {
            if (ctx->sync_symbols >= 4)
            {
                ctx->state = PD_TX_DATA;
                break;
            }
            uint8_t sync_symbols[] = {SYNC_1, SYNC_1, SYNC_1, SYNC_2};

            add_half(ctx, symbols, &symbols_used, line_code_encode[sync_symbols[ctx->sync_symbols]]);
            ctx->sync_symbols++;
            break;
        }

        case PD_TX_DATA:
        {
            if (ctx->data_pos >= data_size)
            {
                ctx->state = PD_TX_EOP;
                break;
            }
            if ((symbols_used + 10) > symbols_free)
            {
                loop = false;
                break;
            }

            /* if all bytes transmitted, finish */
            uint8_t data_byte = ((uint8_t *)data)[ctx->data_pos];
            uint8_t half_lower = line_code_encode[data_byte & 0x0F];
            uint8_t half_upper = line_code_encode[data_byte >> 4];

            add_half(ctx, symbols, &symbols_used, half_lower);
            add_half(ctx, symbols, &symbols_used, half_upper);

            ctx->data_pos++;
            break;
        }

        case PD_TX_EOP:
        {
            if (ctx->eop_symbols)
            {
                ctx->state = PD_TX_DONE;
                break;
            }
            add_half(ctx, symbols, &symbols_used, line_code_encode[EOP]);
            /* create a longer final edge */
            add_bit(ctx, symbols, &symbols_used, 0, 2 * PD_TX_SHORT_DURATION);
            ctx->eop_symbols++;
            break;
        }

        case PD_TX_DONE:
        {
            /* not documented good enough IMO - only set done when no symbols got enqueued */
            loop = false;
            *done = !symbols_used;
            break;
        }
        }
    }

    if (symbols_written == 0)
    {
        /* first transmission starts now */
        pd_tx_active();
    }

    return symbols_used;
}

bool IRAM_ATTR pd_tx_done_cbr(rmt_channel_handle_t tx_chan, const rmt_tx_done_event_data_t *edata, void *user_ctx)
{
    pd_tx_inactive();
    pd_tx_ongoing_flag = false;
    return false;
}

void pd_tx_init()
{
    const rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = PD_TX_FREQ,
        .mem_block_symbols = PD_TX_SYMS,
        .gpio_num = GPIO_TX,
        .trans_queue_depth = 10,
        .flags.io_loop_back = true,
        .flags.invert_out = false,
        .flags.with_dma = false,
        .flags.io_od_mode = false,
    };

    const rmt_tx_event_callbacks_t cbr = {
        .on_trans_done = &pd_tx_done_cbr,
    };

    const rmt_simple_encoder_config_t encoder_cfg = {
        .callback = pd_tx_enc_cbr,
        .min_chunk_size = 10,
        .arg = calloc(1, sizeof(pd_tx_ctx_t)),
    };

    ESP_LOGI(TAG, "  Init TX channel");
    ESP_LOGI(TAG, "    Register channel");
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &pd_tx_chan));
    ESP_LOGI(TAG, "    Register callback");
    ESP_ERROR_CHECK(rmt_tx_register_event_callbacks(pd_tx_chan, &cbr, NULL));
    ESP_LOGI(TAG, "    Create encoder");
    ESP_ERROR_CHECK(rmt_new_simple_encoder(&encoder_cfg, &pd_tx_encoder));
    ESP_LOGI(TAG, "    Enable channel");
    ESP_ERROR_CHECK(rmt_enable(pd_tx_chan));

    gpio_set_direction(GPIO_TX, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_TX, GPIO_FLOATING);
    ESP_LOGI(TAG, "    Done");
}

void IRAM_ATTR pd_tx_start(const uint8_t *data, size_t length)
{
    rmt_transmit_config_t config = {
        .loop_count = 0,
    };

    pd_tx_ongoing_flag = true;
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(pd_tx_chan, 100 / portMAX_DELAY));
    ESP_ERROR_CHECK(rmt_transmit(pd_tx_chan, pd_tx_encoder, data, length, &config));
}

void pd_tx(const uint8_t *data, size_t length)
{
    /* check for CC line being in use */
    while (pd_rx_ongoing() || pd_tx_ongoing())
    {
        vPortYield();
    }

    pd_tx_start(data, length);
}

uint16_t IRAM_ATTR pd_tx_header(
    uint8_t extended,
    uint8_t num_data_objects,
    uint8_t message_id,
    uint8_t power_role,
    uint8_t spec_revision,
    uint8_t data_role,
    uint8_t message_type)
{
    uint16_t header = 0;

    /* Construct the 16-bit header */
    header |= (extended & 0x01) << 15;
    header |= (num_data_objects & 0x07) << 12;
    header |= (message_id & 0x07) << 9;
    header |= (power_role & 0x01) << 8;
    header |= (spec_revision & 0x03) << 6;
    header |= (data_role & 0x01) << 5;
    header |= (message_type & 0x1F);

    return header;
}

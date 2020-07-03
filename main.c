#include <stdbool.h>
#include <stdint.h>
#include "nrf.h"
#include "nrf_gpiote.h"
#include "nrf_gpio.h"
#include "boards.h"
#include "nrf_drv_ppi.h"
#include "nrf_drv_timer.h"
#include "nrf_drv_gpiote.h"
#include "app_error.h"

/// Added in
#include "nrf_drv_pwm.h"

#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"
#include "nrf_temp.h"
#include "nrf_drv_saadc.h"
#include "app_timer.h"
#include "nrf_drv_clock.h"
#include "nrfx_rtc.h"
#include "nrf_drv_rtc.h"
#include "nrfx_power.h"
#include "app_timer.h"



#define SAMPLES_IN_BUFFER 1
////

#ifdef BSP_LED_0
    #define GPIO_OUTPUT_PIN_NUMBER BSP_LED_0  /**< Pin number for output. */
#endif

#define ADC_REF_VOLTAGE_IN_MILLIVOLTS     600      /**< Reference voltage (in milli volts) used by ADC while doing conversion. */
#define ADC_PRE_SCALING_COMPENSATION      6        /**< The ADC is configured to use VDD with 1/3 prescaling as input. And hence the result of conversion is to be multiplied by 3 to get the actual value of the battery voltage.*/
#define ADC_RES_8BIT                     1023   

#define ADC_RESULT_IN_MILLI_VOLTS(ADC_VALUE)\
        ((((ADC_VALUE) * ADC_REF_VOLTAGE_IN_MILLIVOLTS) / ADC_RES_8BIT) * ADC_PRE_SCALING_COMPENSATION)


static nrf_saadc_value_t     m_buffer_pool[2][SAMPLES_IN_BUFFER];
static nrf_saadc_value_t     m_adc_value;
static uint32_t              m_adc_evt_counter;
uint16_t voltage;

static nrf_drv_pwm_t m_pwm0 = NRF_DRV_PWM_INSTANCE(0); // Added in

APP_TIMER_DEF(temp_timer_id);     /**< Handler for repeated timer - TEMP. */
APP_TIMER_DEF(saadc_timer_id);  /**< Handler for repeated timer - SAADC. */


uint32_t b_period[3] = {32768,32768/2,3277};
uint32_t period_counter = 0;
int32_t volatile temp;

static nrf_drv_rtc_t timer = NRF_DRV_RTC_INSTANCE(0);

void rtc_handler(nrf_drv_rtc_int_type_t int_type){
    nrf_drv_rtc_counter_clear(&timer);
    nrf_drv_rtc_int_enable(&timer, NRF_RTC_INT_COMPARE0_MASK);
    nrf_drv_rtc_cc_set(&timer,0,b_period[period_counter],true);
}


static void led_blinking_setup()
{
    uint32_t compare_evt_addr;
    uint32_t gpiote_task_addr;
    nrf_ppi_channel_t ppi_channel;
    ret_code_t err_code;
    nrf_drv_gpiote_out_config_t config = GPIOTE_CONFIG_OUT_TASK_TOGGLE(false);

    err_code = nrf_drv_gpiote_out_init(GPIO_OUTPUT_PIN_NUMBER, &config);
    APP_ERROR_CHECK(err_code);


    err_code = nrf_drv_rtc_cc_set(&timer,0,32768,true);
    APP_ERROR_CHECK(err_code);

    err_code = nrf_drv_ppi_channel_alloc(&ppi_channel);
    APP_ERROR_CHECK(err_code);

    compare_evt_addr = nrf_drv_rtc_event_address_get(&timer, NRF_RTC_EVENT_COMPARE_0 );
    //compare_evt_addr = 0x4000B000 + 0x140;
    gpiote_task_addr = nrf_drv_gpiote_out_task_addr_get(GPIO_OUTPUT_PIN_NUMBER);

    err_code = nrf_drv_ppi_channel_assign(ppi_channel, compare_evt_addr, gpiote_task_addr);
    APP_ERROR_CHECK(err_code);

    err_code = nrf_drv_ppi_channel_enable(ppi_channel);
    APP_ERROR_CHECK(err_code);

    nrf_drv_gpiote_out_task_enable(GPIO_OUTPUT_PIN_NUMBER);
}

static void temp_setup()
{
    // TEMP
    nrf_temp_init();
    NRFX_IRQ_ENABLE(TEMP_IRQn);
    NRF_TEMP->INTENSET = 0x01;
    NRF_TEMP->EVENTS_DATARDY = 0;
}

// Button 1 interrupt routine
void in_pin_handler(nrf_drv_gpiote_pin_t pin, nrf_gpiote_polarity_t action)
{
    // Change blinking period here
    period_counter++;
    if(period_counter == 3)
    period_counter=0;
    nrf_drv_rtc_counter_clear(&timer);
    nrf_drv_rtc_cc_set(&timer, 0, b_period[period_counter], true);
    
    NRF_LOG_INFO("Blinking frequency changed");
    NRF_LOG_FLUSH();
}


// Button 2 interrupt routine
void in_pin_handler2(nrf_drv_gpiote_pin_t pin, nrf_gpiote_polarity_t action)
{
    // Change duty cycle value
    // Calling function for PWM sequence step
    nrfx_pwm_step(&m_pwm0);
    NRF_LOG_INFO("PWM duty cycle changed");
    NRF_LOG_FLUSH();
//    NRF_TEMP->TASKS_START = 1;
}

// Button interrupt init
static void gpio_init(void)
{
    ret_code_t err_code;

    nrf_drv_gpiote_in_config_t in_config = NRFX_GPIOTE_CONFIG_IN_SENSE_HITOLO(true);
    in_config.pull = NRF_GPIO_PIN_PULLUP;
    
    //Button1
    err_code = nrf_drv_gpiote_in_init(BUTTON_1, &in_config, in_pin_handler);
    APP_ERROR_CHECK(err_code);

    nrf_drv_gpiote_in_event_enable(BUTTON_1, true);

    // Button 2
    err_code = nrf_drv_gpiote_in_init(BUTTON_2, &in_config, in_pin_handler2);
    APP_ERROR_CHECK(err_code);

    nrf_drv_gpiote_in_event_enable(BUTTON_2, true);
}

static void demo3(void)
{

    nrf_drv_pwm_config_t const config0 =
    {
        .output_pins =
        {
            BSP_LED_1 | NRF_DRV_PWM_PIN_INVERTED, // channel 0
            NRF_DRV_PWM_PIN_NOT_USED,             // channel 1
            NRF_DRV_PWM_PIN_NOT_USED,             // channel 2
            NRF_DRV_PWM_PIN_NOT_USED,             // channel 3
        },
        .irq_priority = APP_IRQ_PRIORITY_LOWEST,
        .base_clock   = NRF_PWM_CLK_500kHz,
        .count_mode   = NRF_PWM_MODE_UP,
        .top_value    = 10000,
        .load_mode    = NRF_PWM_LOAD_COMMON,
        .step_mode    = NRF_PWM_STEP_TRIGGERED
    };
    APP_ERROR_CHECK(nrf_drv_pwm_init(&m_pwm0, &config0, NULL));

    // This array cannot be allocated on stack (hence "static") and it must
    // be in RAM (hence no "const", though its content is not changed).
    // Additional config0.top_value is added at the end of the array, because when last step is played it auto jumps to first step.
    static uint16_t /*const*/ seq_values[] = {config0.top_value, config0.top_value*0.75, config0.top_value*0.5, config0.top_value*0.25, config0.top_value};
    nrf_pwm_sequence_t const seq =
    {
        .values.p_common = seq_values,
        .length          = NRF_PWM_VALUES_LENGTH(seq_values),
        .repeats         = 0,
        .end_delay       = 0
    };

    (void)nrf_drv_pwm_simple_playback(&m_pwm0, &seq, 1, NRF_DRV_PWM_FLAG_LOOP);
}



void TEMP_IRQHandler(void)
{// Temperature sensor interrupt handler
    temp = (nrf_temp_read() / 4);
    NRF_LOG_INFO("Actual temperature: %d", (int)temp);
    NRF_TEMP->EVENTS_DATARDY = 0;
    NRF_TEMP->TASKS_STOP = 1;   // Need to stop temp manually according to documentation
    NRF_LOG_FLUSH();
}




void saadc_callback(nrf_drv_saadc_evt_t const * p_event)
{
    if (p_event->type == NRF_DRV_SAADC_EVT_DONE)
    {
        ret_code_t err_code;
        

        err_code = nrf_drv_saadc_buffer_convert(p_event->data.done.p_buffer, SAMPLES_IN_BUFFER);
        APP_ERROR_CHECK(err_code);

        int i;
        NRF_LOG_INFO("ADC event number: %d", (int)m_adc_evt_counter);
        for (i = 0; i < SAMPLES_IN_BUFFER; i++)
        {
            NRF_LOG_INFO("ADC Value: %d mV", ADC_RESULT_IN_MILLI_VOLTS(p_event->data.done.p_buffer[i]));
        }
        m_adc_evt_counter++;
        NRF_LOG_FLUSH();
    }
}

void saadc_init(void)
{
    ret_code_t err_code;
    nrf_saadc_channel_config_t channel_config =
        NRF_DRV_SAADC_DEFAULT_CHANNEL_CONFIG_SE(NRF_SAADC_INPUT_AIN0);

    err_code = nrf_drv_saadc_init(NULL, saadc_callback);
    APP_ERROR_CHECK(err_code);

    err_code = nrf_drv_saadc_channel_init(0, &channel_config);
    APP_ERROR_CHECK(err_code);

    err_code = nrf_drv_saadc_buffer_convert(m_buffer_pool[0], SAMPLES_IN_BUFFER);
    APP_ERROR_CHECK(err_code);


}


static void lfclk_config(void)
{
    ret_code_t err_code = nrf_drv_clock_init();
    APP_ERROR_CHECK(err_code);

    nrf_drv_clock_lfclk_request(NULL);
}

static void temp_timer_h(void * p_context)
{
    NRF_TEMP->TASKS_START = 1;
}

static void saadc_timer_h(void * p_context)
{
    nrf_drv_saadc_sample();
}


static void create_timers()
{
    ret_code_t err_code;

    // Create timers
    err_code = app_timer_create(&temp_timer_id,
                                APP_TIMER_MODE_REPEATED,
                                temp_timer_h);
    APP_ERROR_CHECK(err_code);

    err_code = app_timer_create(&saadc_timer_id,
                                APP_TIMER_MODE_REPEATED,
                                saadc_timer_h);
    APP_ERROR_CHECK(err_code);
}





/**
 * @brief Function for application main entry.
 */
int main(void)

{
    APP_ERROR_CHECK(NRF_LOG_INIT(NULL));
    NRF_LOG_DEFAULT_BACKENDS_INIT();
    NRF_LOG_INFO("Demo Started.");
    NRF_LOG_FLUSH();

    ret_code_t err_code;

    lfclk_config();

    app_timer_init();

    err_code = nrf_drv_ppi_init();
    APP_ERROR_CHECK(err_code);

    err_code = nrf_drv_gpiote_init();
    APP_ERROR_CHECK(err_code);

    nrf_drv_rtc_config_t config = NRF_DRV_RTC_DEFAULT_CONFIG;
    err_code = nrf_drv_rtc_init(&timer, &config, rtc_handler);
    APP_ERROR_CHECK(err_code);

    nrfx_rtc_int_enable(&timer,NRFX_RTC_INT_COMPARE0);

    create_timers();

    // Setup PPI channel with event from TIMER compare and task GPIOTE pin toggle.
    led_blinking_setup();
    // Setup PPI channel with event from TIMER1 compare and task TEMP 
    temp_setup();

    saadc_init();
    // Enable timer
    err_code = app_timer_start(temp_timer_id, APP_TIMER_TICKS(2000), NULL);
    APP_ERROR_CHECK(err_code);
    err_code = app_timer_start(saadc_timer_id, APP_TIMER_TICKS(2000), NULL);
    APP_ERROR_CHECK(err_code);
    nrf_drv_rtc_enable(&timer);

    // Enable button-press detection button1
    gpio_init();

    demo3();



    

    while (true)
    {
        
    }
}


/** @} */

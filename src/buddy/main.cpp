#include "main.h"
#include "main.hpp"
#include "platform.h"
#include <device/board.h>
#include "config_features.h"
#include "cmsis_os.h"
#include "fatfs.h"
#include "usb_device.h"
#include "usb_host.h"
#include "buffered_serial.hpp"
#include "bsod.h"
#include <config_store/store_instance.hpp>
#include <option/buddy_enable_connect.h>
#if BUDDY_ENABLE_CONNECT()
    #include "connect/run.hpp"
#endif

#include "sys.h"
#include "app.h"
#include "wdt.h"
#include <crash_dump/dump.hpp>
#include "timer_defaults.h"
#include "tick_timer_api.h"
#include "thread_measurement.h"
#include "metric_handlers.h"
#include "hwio_pindef.h"
#include "gui.hpp"
#include "config_buddy_2209_02.h"
#include "crc32.h"
#include "w25x.h"
#include "timing.h"
#include "filesystem.h"
#include "adc.hpp"
#include "logging.h"
#include "common/disable_interrupts.h"
#include <option/has_accelerometer.h>
#include <option/has_puppies.h>
#include <option/has_puppies_bootloader.h>
#include <option/filament_sensor.h>
#include <option/has_gui.h>
#include <option/has_mmu2.h>
#include "tasks.hpp"
#include <appmain.hpp>
#include "safe_state.h"
#include <espif.h>
#include "sound.hpp"

#if HAS_PUPPIES()
    #include "puppies/PuppyBus.hpp"
    #include "puppies/puppy_task.hpp"
#endif

#if ENABLED(POWER_PANIC)
    #include "power_panic.hpp"
#endif

#include <option/buddy_enable_wui.h>
#if BUDDY_ENABLE_WUI()
    #include "wui.h"
    #if USE_ASYNCIO
        #include <transfers/async_io.hpp>
    #endif
#endif

#if (BOARD_IS_XBUDDY)
    #include "hw_configuration.hpp"
#endif

using namespace crash_dump;

LOG_COMPONENT_REF(Buddy);

osThreadId defaultTaskHandle;
osThreadId displayTaskHandle;
osThreadId connectTaskHandle;

#if HAS_ACCELEROMETER()
LIS2DH accelerometer(10);
#endif

unsigned HAL_RCC_CSR = 0;
int HAL_GPIO_Initialized = 0;
int HAL_ADC_Initialized = 0;
int HAL_PWM_Initialized = 0;
int HAL_SPI_Initialized = 0;

void SystemClock_Config(void);
void StartDefaultTask(void const *argument);
void StartDisplayTask(void const *argument);
void StartConnectTask(void const *argument);
#if USE_ASYNCIO
void StartAsyncIoTask(void const *argument);
#endif
void StartESPTask(void const *argument);
void iwdg_warning_cb(void);

#if (BOARD_IS_BUDDY)
uartrxbuff_t uart1rxbuff;
static uint8_t uart1rx_data[32];
#endif

extern "C" void main_cpp(void) {
    // save and clear reset flags
    HAL_RCC_CSR = RCC->CSR;
    __HAL_RCC_CLEAR_RESET_FLAGS();

    // Initialize sound instance now with its default settings
    // to be able to service sound related calls from interrupts.
    Sound::getInstance();

    hw_gpio_init();
    hw_dma_init();

    // ADC/DMA
    hw_adc1_init();
    adcDma1.init();
#ifdef HAS_ADC3
    hw_adc3_init();
    adcDma3.init();
#endif

#if BOARD_IS_BUDDY || BOARD_IS_XBUDDY
    hw_tim1_init();
#endif
    hw_tim14_init();

    SPI_INIT(flash);
    // initialize SPI flash
    w25x_spi_assign(&SPI_HANDLE_FOR(flash));
    if (!w25x_init())
        bsod("failed to initialize ext flash");

    /*
     * If we have BSOD or red screen we want to have as small boot process as we can.
     * We want to init just xflash, display and start gui task to display the bsod or redscreen
     */
    if ((dump_is_valid() && !dump_is_displayed()) || (message_get_type() != MsgType::EMPTY && !message_is_displayed())) {
        hwio_safe_state();
        init_error_screen();
        return;
    }
    bsod_mark_shown(); // BSOD would be shown, allow new BSOD dump

    logging_init();
    TaskDeps::components_init();

#if (BOARD_IS_BUDDY)
    hw_uart1_init();
#endif

#if BOARD_IS_BUDDY || BOARD_IS_XBUDDY
    hw_tim3_init();
#endif

#if HAS_GUI()
    SPI_INIT(lcd);
#endif

#if BOARD_IS_XBUDDY || BOARD_IS_XLBUDDY
    I2C_INIT(usbc);
#endif

#if (BOARD_IS_XBUDDY || BOARD_IS_XLBUDDY)
    I2C_INIT(touch);
#endif

#if (BOARD_IS_XBUDDY)
    SPI_INIT(extconn);
    SPI_INIT(accelerometer);
#endif

#if defined(spi_tmc)
    SPI_INIT(tmc);
#elif defined(uart_tmc)
    UART_INIT(tmc);
#else
    #error Do not know how to init TMC communication channel
#endif

#if BUDDY_ENABLE_WUI()
    UART_INIT(esp);
#endif

#if HAS_MMU2()
    UART_INIT(mmu);
#endif

#if HAS_GUI() && !(BOARD_IS_XLBUDDY)
    hw_tim2_init(); // TIM2 is used to generate buzzer PWM. Not needed without display.
#endif

#if BOARD_IS_XLBUDDY
    hw_init_spi_side_leds();
#endif

#if HAS_PUPPIES()
    UART_INIT(puppies);
    buddy::puppies::PuppyBus::Open();
#endif

    hw_rtc_init();
    hw_rng_init();

    MX_USB_HOST_Init();

    MX_FATFS_Init();

    usb_device_init();

    HAL_GPIO_Initialized = 1;
    HAL_ADC_Initialized = 1;
    HAL_PWM_Initialized = 1;
    HAL_SPI_Initialized = 1;

    config_store_ns::InitResult status = config_store_init_result();
    if (status == config_store_ns::InitResult::cold_start || status == config_store_ns::InitResult::migrated_from_old) {
        // this means we are either starting from defaults or after a FW upgrade -> invalidate the XFLASH dump, since it is not relevant anymore
        dump_reset();
    }

    // Restore sound settings from eeprom
    Sound::getInstance().restore_from_eeprom();

    wdt_iwdg_warning_cb = iwdg_warning_cb;

#if (BOARD_IS_BUDDY)
    buddy::hw::BufferedSerial::uart2.Open();
#endif

#if (BOARD_IS_BUDDY)
    uartrxbuff_init(&uart1rxbuff, &hdma_usart1_rx, sizeof(uart1rx_data), uart1rx_data);
    HAL_UART_Receive_DMA(&huart1, uart1rxbuff.buffer, uart1rxbuff.buffer_size);
    uartrxbuff_reset(&uart1rxbuff);
#endif

#if (BOARD_IS_XBUDDY)
    #if !HAS_PUPPIES()
    buddy::hw::BufferedSerial::uart6.Open();
    #endif
#endif

    filesystem_init();

    static metric_handler_t *handlers[] = {
        &metric_handler_syslog,
        &metric_handler_info_screen,
        NULL
    };
    metric_system_init(handlers);

#if BUDDY_ENABLE_WUI()
    espif_init_hw();
#endif

    osThreadDef(defaultTask, StartDefaultTask, TASK_PRIORITY_DEFAULT_TASK, 0, 1024);
    defaultTaskHandle = osThreadCreate(osThread(defaultTask), NULL);

    if (option::has_gui) {
        osThreadDef(displayTask, StartDisplayTask, TASK_PRIORITY_DISPLAY_TASK, 0, 1024 + 256 + 128);
        displayTaskHandle = osThreadCreate(osThread(displayTask), NULL);
    }

#if ENABLED(POWER_PANIC)
    power_panic::check_ac_fault_at_startup();
    /* definition and creation of acFaultTask */
    osThreadDef(acFaultTask, power_panic::ac_fault_task_main, TASK_PRIORITY_AC_FAULT, 0, 80);
    power_panic::ac_fault_task = osThreadCreate(osThread(acFaultTask), NULL);
#endif

#if HAS_PUPPIES()
    buddy::puppies::start_puppy_task();
#endif

#if BUDDY_ENABLE_WUI()
    #if USE_ASYNCIO
    osThreadCCMDef(asyncIoTask, StartAsyncIoTask, TASK_PRIORITY_ASYNCIO, 0, 256);
    osThreadCreate(osThread(asyncIoTask), nullptr);
    #endif

    TaskDeps::wait(TaskDeps::Tasks::network);
    start_network_task();
#endif

#if BUDDY_ENABLE_CONNECT()
    #if !BUDDY_ENABLE_WUI()
        // FIXME: We should be able to split networking to the lower-level network part and the Link part. Currently, both are done through WUI.
        #error "Can't have connect without WUI"
    #endif
    /* definition and creation of connectTask */
    osThreadDef(connectTask, StartConnectTask, TASK_PRIORITY_CONNECT, 0, 2304);
    connectTaskHandle = osThreadCreate(osThread(connectTask), NULL);
#endif

    if constexpr (option::filament_sensor != option::FilamentSensor::no) {
        /* definition and creation of measurementTask */
        osThreadDef(measurementTask, StartMeasurementTask, TASK_PRIORITY_MEASUREMENT_TASK, 0, 512);
        osThreadCreate(osThread(measurementTask), NULL);
    }
}

#ifdef USE_ST7789
extern void st7789v_spi_tx_complete(void);
#endif

void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi) {

#if HAS_GUI() && defined(USE_ST7789)
    if (hspi == st7789v_config.phspi) {
        st7789v_spi_tx_complete();
    }
#endif

#if HAS_GUI() && defined(USE_ILI9488)
    if (hspi == ili9488_config.phspi) {
        ili9488_spi_tx_complete();
    }
#endif

    if (hspi == &SPI_HANDLE_FOR(flash)) {
        w25x_spi_transfer_complete_callback();
    }
}

void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi) {

#if HAS_GUI() && defined(USE_ILI9488)
    if (hspi == ili9488_config.phspi) {
        ili9488_spi_rx_complete();
    }
#endif

    if (hspi == &SPI_HANDLE_FOR(flash)) {
        w25x_spi_receive_complete_callback();
    }

#if HAS_ACCELEROMETER()
    if (hspi == &SPI_HANDLE_FOR(accelerometer)) {
        accelerometer.spiReceiveCompleteCallback();
    }
#endif
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart) {
#if (BOARD_IS_BUDDY)
    if (huart == &huart2) {
        buddy::hw::BufferedSerial::uart2.WriteFinishedISR();
    }
#endif

#if HAS_PUPPIES()
    if (huart == &UART_HANDLE_FOR(puppies)) {
        buddy::puppies::PuppyBus::bufferedSerial.WriteFinishedISR();
    }
#endif

#if (BOARD_IS_XBUDDY)
    #if !HAS_PUPPIES()
    if (huart == &huart6) {
        //        log_debug(Buddy, "HAL_UART6_TxCpltCallback");
        buddy::hw::BufferedSerial::uart6.WriteFinishedISR();
        #if HAS_MMU2()
                // instruct the RS485 converter, that we have finished sending data and from now on we are expecting a response from the MMU
                // set to high in hwio_pindef.h
                // buddy::hw::RS485FlowControl.write(buddy::hw::Pin::State::high);
        #endif
    }
    #endif
#endif
}

void HAL_UART_RxHalfCpltCallback(UART_HandleTypeDef *huart) {

#if (BOARD_IS_BUDDY)
    if (huart == &huart2) {
        buddy::hw::BufferedSerial::uart2.FirstHalfReachedISR();
    }
#endif

#if HAS_PUPPIES()
    if (huart == &UART_HANDLE_FOR(puppies)) {
        buddy::puppies::PuppyBus::bufferedSerial.FirstHalfReachedISR();
    }
#endif

#if (BOARD_IS_XBUDDY)
    #if !HAS_PUPPIES()
    if (huart == &huart6) {
        buddy::hw::BufferedSerial::uart6.FirstHalfReachedISR();
    }
    #endif
#endif
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
#if (BOARD_IS_BUDDY)
    if (huart == &huart2) {
        buddy::hw::BufferedSerial::uart2.SecondHalfReachedISR();
    }
#endif

#if HAS_PUPPIES()
    if (huart == &UART_HANDLE_FOR(puppies)) {
        buddy::puppies::PuppyBus::bufferedSerial.SecondHalfReachedISR();
    }
#endif

#if (BOARD_IS_XBUDDY)
    #if !HAS_PUPPIES()
    if (huart == &huart6) {
        buddy::hw::BufferedSerial::uart6.SecondHalfReachedISR();
    }
    #endif
#endif
}

void StartDefaultTask([[maybe_unused]] void const *argument) {
    app_startup();

    app_run();
    for (;;) {
        osDelay(1);
    }
}

void StartDisplayTask([[maybe_unused]] void const *argument) {
    gui_run();
    for (;;) {
        osDelay(1);
    }
}

void StartErrorDisplayTask([[maybe_unused]] void const *argument) {
    gui_error_run();
    for (;;) {
        osDelay(1);
    }
}

#if BUDDY_ENABLE_WUI() && USE_ASYNCIO
void StartAsyncIoTask([[maybe_unused]] void const *argument) {
    async_io::run();
}
#endif

#if BUDDY_ENABLE_CONNECT()
void StartConnectTask([[maybe_unused]] void const *argument) {
    connect_client::run();
}
#endif

/**
 * @brief  Period elapsed callback in non blocking mode
 * @note   This function is called  when TIM6 interrupt took place, inside
 * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
 * a global variable "uwTick" used as application time base.
 * @param  htim : TIM handle
 * @retval None
 */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim) {
    if (htim->Instance == TIM14) {
        app_tim14_tick();
    } else if (htim->Instance == TICK_TIMER) {
        app_tick_timer_overflow();
    }
}

/**
 * @brief  This function is executed in case of error occurrence.
 * @retval None
 */
void Error_Handler(void) {
    /* User can add his own implementation to report the HAL error return state */
    app_error();
}

void system_core_error_handler() {
    app_error();
}

void iwdg_warning_cb(void) {
#ifdef _DEBUG
    ///@note Watchdog is disabled in debug,
    /// so this will be helpful only if it is manually enabled or ifdef commented out.

    // Breakpoint if debugger is connected
    if (CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) {
        __BKPT(0);
    }
#endif /*_DEBUG*/
    DUMP_IWDGW_TO_CCRAM(0x10);
    wdt_iwdg_refresh();
    save_dump();
#ifndef _DEBUG
    while (true)
        ;
#else
    sys_reset();
#endif
}

static uint32_t _spi_prescaler(int prescaler_num) {
    switch (prescaler_num) {
    case 0:
        return SPI_BAUDRATEPRESCALER_2;   // 0x00000000U
    case 1:
        return SPI_BAUDRATEPRESCALER_4;   // 0x00000008U
    case 2:
        return SPI_BAUDRATEPRESCALER_8;   // 0x00000010U
    case 3:
        return SPI_BAUDRATEPRESCALER_16;  // 0x00000018U
    case 4:
        return SPI_BAUDRATEPRESCALER_32;  // 0x00000020U
    case 5:
        return SPI_BAUDRATEPRESCALER_64;  // 0x00000028U
    case 6:
        return SPI_BAUDRATEPRESCALER_128; // 0x00000030U
    case 7:
        return SPI_BAUDRATEPRESCALER_256; // 0x00000038U
    }
    return SPI_BAUDRATEPRESCALER_2;
}

void spi_set_prescaler(SPI_HandleTypeDef *hspi, int prescaler_num) {
    buddy::DisableInterrupts disable_interrupts;
    HAL_SPI_DeInit(hspi);
    hspi->Init.BaudRatePrescaler = _spi_prescaler(prescaler_num);
    HAL_SPI_Init(hspi);
}

void init_error_screen() {
    if constexpr (option::has_gui) {
        // init lcd spi and timer for buzzer
        SPI_INIT(lcd);
#if !(BOARD_IS_XLBUDDY && _DEBUG)
        hw_tim2_init(); // TIM2 is used to generate buzzer PWM. Not needed without display.
#endif

        init_only_littlefs();

        osThreadDef(displayTask, StartErrorDisplayTask, TASK_PRIORITY_DISPLAY_TASK, 0, 1024 + 256);
        displayTaskHandle = osThreadCreate(osThread(displayTask), NULL);
    }
}

static void enable_trap_on_division_by_zero() {
    SCB->CCR |= SCB_CCR_DIV_0_TRP_Msk;
}

static void enable_backup_domain() {
    // this allows us to use the RTC->BKPXX registers
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();
}

static void enable_segger_sysview() {
    // enable the cycle counter for correct time reporting
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    SEGGER_SYSVIEW_Conf();
}

static void enable_dfu_entry() {
#ifdef BUDDY_ENABLE_DFU_ENTRY
    // check whether user requested to enter the DFU mode
    // this has to be checked after having
    //  1) initialized access to the backup domain
    //  2) having initialized related clocks (SystemClock_Config)
    if (sys_dfu_requested())
        sys_dfu_boot_enter();
#endif
}

static void eeprom_init_i2c() {
    I2C_INIT(eeprom);
}

namespace {
/// The entrypoint of the startup task
///
/// WARNING
/// The C++ runtime isn't initialized at the beginning of this function
/// and initializing it is the main priority here.
/// So first, we have to get the EEPROM ready, then we call libc_init_array
/// and that is the time everything is ready for us to switch to C++ context.
extern "C" void startup_task(void const *) {
    // init crc32 module. We need crc in eeprom_init
    crc32_init();

    // init communication with eeprom
    eeprom_init_i2c();

    // init eeprom module itself
    taskENTER_CRITICAL();
    init_config_store();
    taskEXIT_CRITICAL();

// must do this before timer 1, timer 1 interrupt calls Configuration
// also must be before initializing global variables
#if BOARD_IS_XBUDDY
    buddy::hw::Configuration::Instance();
#endif

    // init global variables and call constructors
    extern void __libc_init_array(void);
    __libc_init_array();

    // call the main main() function
    main_cpp();

    // terminate this thread (release its resources), we are done
    osThreadTerminate(osThreadGetId());
}
} // namespace

/// The entrypoint of our firmware
///
/// Do not do anything here that isn't essential to starting the RTOS
/// That is our one and only priority.
///
/// WARNING
/// The C++ runtime hasn't been initialized yet (together with C's constructors).
/// So make sure you don't do anything that is dependent on it.
int main() {
    // initialize FPU, vector table & external memory
    SystemInit();

    // initialize HAL
    HAL_Init();

    // configure system clock and timing
    system_core_init();
    tick_timer_init();

    // other MCU setup
    enable_trap_on_division_by_zero();
    enable_backup_domain();
    enable_segger_sysview();
    enable_dfu_entry();

    // define the startup task
    osThreadDef(startup, startup_task, TASK_PRIORITY_STARTUP, 0, 1024 + 512 + 256);
    osThreadCreate(osThread(startup), NULL);

    // start the RTOS with the single startup task
    osKernelStart();
}

#ifdef USE_FULL_ASSERT
/**
 * @brief  Reports the name of the source file and the source line number
 *         where the assert_param error has occurred.
 * @param  file: pointer to the source file name
 * @param  line: assert_param error line source number
 * @retval None
 */
void assert_failed(uint8_t *file, uint32_t line) {
    /* User can add his own implementation to report the file name and line number,
     tex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
    app_assert(file, line);
}
#endif /* USE_FULL_ASSERT */

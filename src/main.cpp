#include <FreeRTOS.h>
#include <task.h>
#include <libraries/bsp/bsp.h>
#include <legacy/nrf_drv_clock.h>
#include <libraries/timer/app_timer.h>
#include <libraries/gpiote/app_gpiote.h>
#include <DisplayApp/DisplayApp.h>
#include <softdevice/common/nrf_sdh.h>
#include <softdevice/common/nrf_sdh_freertos.h>
#include <hal/nrf_rtc.h>
#include <timers.h>
#include <libraries/log/nrf_log.h>
#include <ble/ble_services/ble_cts_c/ble_cts_c.h>
#include <Components/DateTime/DateTimeController.h>
#include "BLE/BleManager.h"
#include "Components/Battery/BatteryController.h"
#include "Components/Ble/BleController.h"
#include "../drivers/Cst816s.h"
#include <drivers/St7789.h>
#include <drivers/SpiMaster.h>
#include <Components/Gfx/Gfx.h>

#if NRF_LOG_ENABLED
#include "Logging/NrfLogger.h"
Pinetime::Logging::NrfLogger logger;
#else
#include "Logging/DummyLogger.h"
Pinetime::Logging::DummyLogger logger;
#endif

std::unique_ptr<Pinetime::Drivers::SpiMaster> spi;
std::unique_ptr<Pinetime::Drivers::St7789> lcd;
std::unique_ptr<Pinetime::Components::Gfx> gfx;
std::unique_ptr<Pinetime::Drivers::Cst816S> touchPanel;

static constexpr uint8_t pinSpiSck = 2;
static constexpr uint8_t pinSpiMosi = 3;
static constexpr uint8_t pinSpiMiso = 4;
static constexpr uint8_t pinSpiCsn = 25;
static constexpr uint8_t pinLcdDataCommand = 18;


std::unique_ptr<Pinetime::Applications::DisplayApp> displayApp;
TaskHandle_t systemThread;
bool isSleeping = false;
TimerHandle_t debounceTimer;
Pinetime::Controllers::Battery batteryController;
Pinetime::Controllers::Ble bleController;
Pinetime::Controllers::DateTime dateTimeController;


void ble_manager_set_ble_connection_callback(void (*connection)());
void ble_manager_set_ble_disconnection_callback(void (*disconnection)());
static constexpr uint8_t pinButton = 13;
static constexpr uint8_t pinTouchIrq = 28;
QueueHandle_t systemTaksMsgQueue;
enum class SystemTaskMessages {GoToSleep, GoToRunning};
void SystemTask_PushMessage(SystemTaskMessages message);

void nrfx_gpiote_evt_handler(nrfx_gpiote_pin_t pin, nrf_gpiote_polarity_t action) {
  if(pin == pinTouchIrq) {
    displayApp->PushMessage(Pinetime::Applications::DisplayApp::Messages::TouchEvent);
    if(!isSleeping) return;
  }

  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  xTimerStartFromISR(debounceTimer, &xHigherPriorityTaskWoken);
  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void DebounceTimerCallback(TimerHandle_t xTimer) {
  xTimerStop(xTimer, 0);
  if(isSleeping) {
    SystemTask_PushMessage(SystemTaskMessages::GoToRunning);
    displayApp->PushMessage(Pinetime::Applications::DisplayApp::Messages::GoToRunning);
    isSleeping = false;
    batteryController.Update();
    displayApp->PushMessage(Pinetime::Applications::DisplayApp::Messages::UpdateBatteryLevel);
  }
  else {
    SystemTask_PushMessage(SystemTaskMessages::GoToSleep);
    displayApp->PushMessage(Pinetime::Applications::DisplayApp::Messages::GoToSleep);
    isSleeping = true;
  }
}

void SystemTask_PushMessage(SystemTaskMessages message) {
  BaseType_t xHigherPriorityTaskWoken;
  xHigherPriorityTaskWoken = pdFALSE;
  xQueueSendFromISR(systemTaksMsgQueue, &message, &xHigherPriorityTaskWoken);
  if (xHigherPriorityTaskWoken) {
    /* Actual macro used here is port specific. */
    // TODO : should I do something here?
  }
}

// TODO The whole SystemTask should go in its own class
// BUT... it has to work with pure C callback (nrfx_gpiote_evt_handler) and i've still not found
// a good design for that (the callback does not allow to pass a pointer to an instance...)
void SystemTask(void *) {
  APP_GPIOTE_INIT(2);
  bool erase_bonds=false;
  nrf_sdh_freertos_init(ble_manager_start_advertising, &erase_bonds);

  spi.reset(new Pinetime::Drivers::SpiMaster {Pinetime::Drivers::SpiMaster::SpiModule::SPI0,  {
          Pinetime::Drivers::SpiMaster::BitOrder::Msb_Lsb,
          Pinetime::Drivers::SpiMaster::Modes::Mode3,
          Pinetime::Drivers::SpiMaster::Frequencies::Freq8Mhz,
          pinSpiSck,
          pinSpiMosi,
          pinSpiMiso,
          pinSpiCsn
  }});

  lcd.reset(new Pinetime::Drivers::St7789(*spi, pinLcdDataCommand));
  gfx.reset(new Pinetime::Components::Gfx(*lcd));
  touchPanel.reset(new Pinetime::Drivers::Cst816S());

  spi->Init();
  lcd->Init();
  touchPanel->Init();
  batteryController.Init();

  displayApp.reset(new Pinetime::Applications::DisplayApp(*lcd, *gfx, *touchPanel, batteryController, bleController, dateTimeController));
  displayApp->Start();

  batteryController.Update();
  displayApp->PushMessage(Pinetime::Applications::DisplayApp::Messages::UpdateBatteryLevel);

  debounceTimer = xTimerCreate ("debounceTimer", 200, pdFALSE, (void *) 0, DebounceTimerCallback);

  nrf_gpio_cfg_sense_input(pinButton, (nrf_gpio_pin_pull_t)GPIO_PIN_CNF_PULL_Pulldown, (nrf_gpio_pin_sense_t)GPIO_PIN_CNF_SENSE_High);
  nrf_gpio_cfg_output(15);
  nrf_gpio_pin_set(15);

  nrfx_gpiote_in_config_t pinConfig;
  pinConfig.skip_gpio_setup = true;
  pinConfig.hi_accuracy = false;
  pinConfig.is_watcher = false;
  pinConfig.sense = (nrf_gpiote_polarity_t)NRF_GPIOTE_POLARITY_HITOLO;
  pinConfig.pull = (nrf_gpio_pin_pull_t)GPIO_PIN_CNF_PULL_Pulldown;

  nrfx_gpiote_in_init(pinButton, &pinConfig, nrfx_gpiote_evt_handler);

  nrf_gpio_cfg_sense_input(pinTouchIrq, (nrf_gpio_pin_pull_t)GPIO_PIN_CNF_PULL_Pullup, (nrf_gpio_pin_sense_t)GPIO_PIN_CNF_SENSE_Low);

  pinConfig.skip_gpio_setup = true;
  pinConfig.hi_accuracy = false;
  pinConfig.is_watcher = false;
  pinConfig.sense = (nrf_gpiote_polarity_t)NRF_GPIOTE_POLARITY_HITOLO;
  pinConfig.pull = (nrf_gpio_pin_pull_t)GPIO_PIN_CNF_PULL_Pullup;

  nrfx_gpiote_in_init(pinTouchIrq, &pinConfig, nrfx_gpiote_evt_handler);

  systemTaksMsgQueue = xQueueCreate(10, 1);
  bool systemTaskSleeping = false;

  while(true) {
    uint8_t msg;

    if (xQueueReceive(systemTaksMsgQueue, &msg, systemTaskSleeping?3600000 : 1000)) {
      SystemTaskMessages message = static_cast<SystemTaskMessages >(msg);
      switch(message) {
        case SystemTaskMessages::GoToRunning: systemTaskSleeping = false; break;
        case SystemTaskMessages::GoToSleep: systemTaskSleeping = true; break;
        default: break;
      }
    }
    uint32_t systick_counter = nrf_rtc_counter_get(portNRF_RTC_REG);
    dateTimeController.UpdateTime(systick_counter);
  }
}

void OnBleConnection() {
  bleController.Connect();
  displayApp->PushMessage(Pinetime::Applications::DisplayApp::Messages::UpdateBleConnection);
}

void OnBleDisconnection() {
  bleController.Disconnect();
  displayApp->PushMessage(Pinetime::Applications::DisplayApp::Messages::UpdateBleConnection);
}

void OnNewTime(current_time_char_t* currentTime) {
  auto dayOfWeek = currentTime->exact_time_256.day_date_time.day_of_week;
  auto year = currentTime->exact_time_256.day_date_time.date_time.year;
  auto month = currentTime->exact_time_256.day_date_time.date_time.month;
  auto day = currentTime->exact_time_256.day_date_time.date_time.day;
  auto hour = currentTime->exact_time_256.day_date_time.date_time.hours;
  auto minute = currentTime->exact_time_256.day_date_time.date_time.minutes;
  auto second = currentTime->exact_time_256.day_date_time.date_time.seconds;

  dateTimeController.SetTime(year, month, day,
                             dayOfWeek, hour, minute, second, nrf_rtc_counter_get(portNRF_RTC_REG));
}

void SPIM0_SPIS0_TWIM0_TWIS0_SPI0_TWI0_IRQHandler(void) {
  if(((NRF_SPIM0->INTENSET & (1<<6)) != 0) && NRF_SPIM0->EVENTS_END == 1) {
    NRF_SPIM0->EVENTS_END = 0;
    spi->OnEndEvent(*gfx);
  }

  if(((NRF_SPIM0->INTENSET & (1<<19)) != 0) && NRF_SPIM0->EVENTS_STARTED == 1) {
    NRF_SPIM0->EVENTS_STARTED = 0;
    spi->OnStartedEvent(*gfx);
  }

  if(((NRF_SPIM0->INTENSET & (1<<1)) != 0) && NRF_SPIM0->EVENTS_STOPPED == 1) {
    NRF_SPIM0->EVENTS_STOPPED = 0;
  }
}
int main(void) {
  logger.Init();
  nrf_drv_clock_init();

  if (pdPASS != xTaskCreate(SystemTask, "MAIN", 256, nullptr, 0, &systemThread))
    APP_ERROR_HANDLER(NRF_ERROR_NO_MEM);

  ble_manager_init();
  ble_manager_set_new_time_callback(OnNewTime);
  ble_manager_set_ble_connection_callback(OnBleConnection);
  ble_manager_set_ble_disconnection_callback(OnBleDisconnection);

  vTaskStartScheduler();

  for (;;) {
    APP_ERROR_HANDLER(NRF_ERROR_FORBIDDEN);
  }
}





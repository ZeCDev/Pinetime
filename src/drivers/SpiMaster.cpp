#include <hal/nrf_gpio.h>
#include <hal/nrf_spim.h>
#include "SpiMaster.h"
#include <algorithm>
using namespace Pinetime::Drivers;

SpiMaster::SpiMaster(const SpiMaster::SpiModule spi, const SpiMaster::Parameters &params) :
        spi{spi}, params{params} {

}

bool SpiMaster::Init() {
  /* Configure GPIO pins used for pselsck, pselmosi, pselmiso and pselss for SPI0 */
  nrf_gpio_pin_set(params.pinSCK);
  nrf_gpio_cfg_output(params.pinSCK);
  nrf_gpio_pin_clear(params.pinMOSI);
  nrf_gpio_cfg_output(params.pinMOSI);
  nrf_gpio_cfg_input(params.pinMISO, NRF_GPIO_PIN_NOPULL);
  nrf_gpio_cfg_output(params.pinCSN);
  pinCsn = params.pinCSN;

  switch(spi) {
    case SpiModule::SPI0: spiBaseAddress = NRF_SPIM0; break;
    case SpiModule::SPI1: spiBaseAddress = NRF_SPIM1; break;
    default: return false;
  }

  /* Configure pins, frequency and mode */
  spiBaseAddress->PSELSCK  = params.pinSCK;
  spiBaseAddress->PSELMOSI = params.pinMOSI;
  spiBaseAddress->PSELMISO = params.pinMISO;
  nrf_gpio_pin_set(pinCsn); /* disable Set slave select (inactive high) */

  uint32_t frequency;
  switch(params.Frequency) {
    case Frequencies::Freq8Mhz: frequency = 0x80000000; break;
    default: return false;
  }
  spiBaseAddress->FREQUENCY = frequency;

  uint32_t regConfig = 0;
  switch(params.bitOrder) {
    case BitOrder::Msb_Lsb: break;
    case BitOrder::Lsb_Msb: regConfig = 1;
    default: return false;
  }
  switch(params.mode) {
    case Modes::Mode0: break;
    case Modes::Mode1: regConfig |= (0x01 << 1); break;
    case Modes::Mode2: regConfig |= (0x02 << 1); break;
    case Modes::Mode3: regConfig |= (0x03 << 1); break;
    default: return false;
  }

  spiBaseAddress->CONFIG = regConfig;
  spiBaseAddress->EVENTS_ENDRX = 0;
  spiBaseAddress->EVENTS_ENDTX = 0;
  spiBaseAddress->EVENTS_END = 0;

  spiBaseAddress->INTENSET = ((unsigned)1 << (unsigned)6);
  spiBaseAddress->INTENSET = ((unsigned)1 << (unsigned)1);
  spiBaseAddress->INTENSET = ((unsigned)1 << (unsigned)19);

  spiBaseAddress->ENABLE = (SPIM_ENABLE_ENABLE_Enabled << SPIM_ENABLE_ENABLE_Pos);

  NRFX_IRQ_PRIORITY_SET(SPIM0_SPIS0_TWIM0_TWIS0_SPI0_TWI0_IRQn,2);
  NRFX_IRQ_ENABLE(SPIM0_SPIS0_TWIM0_TWIS0_SPI0_TWI0_IRQn);
  return true;
}


void SpiMaster::SetupWorkaroundForFtpan58(NRF_SPIM_Type *spim, uint32_t ppi_channel, uint32_t gpiote_channel) {
  // Create an event when SCK toggles.
  NRF_GPIOTE->CONFIG[gpiote_channel] = (GPIOTE_CONFIG_MODE_Event << GPIOTE_CONFIG_MODE_Pos) |
                                       (spim->PSEL.SCK << GPIOTE_CONFIG_PSEL_Pos) |
                                       (GPIOTE_CONFIG_POLARITY_Toggle << GPIOTE_CONFIG_POLARITY_Pos);

  // Stop the spim instance when SCK toggles.
  NRF_PPI->CH[ppi_channel].EEP = (uint32_t) &NRF_GPIOTE->EVENTS_IN[gpiote_channel];
  NRF_PPI->CH[ppi_channel].TEP = (uint32_t) &spim->TASKS_STOP;
  NRF_PPI->CHENSET = 1U << ppi_channel;

  // Disable IRQ
  spim->INTENCLR = (1<<6);
  spim->INTENCLR = (1<<1);
  spim->INTENCLR = (1<<19);
}

void SpiMaster::DisableWorkaroundForFtpan58(NRF_SPIM_Type *spim, uint32_t ppi_channel, uint32_t gpiote_channel) {
  NRF_GPIOTE->CONFIG[gpiote_channel] = 0;
  NRF_PPI->CH[ppi_channel].EEP = 0;
  NRF_PPI->CH[ppi_channel].TEP = 0;
  NRF_PPI->CHENSET = ppi_channel;
  spim->INTENSET = (1<<6);
  spim->INTENSET = (1<<1);
  spim->INTENSET = (1<<19);
}

void SpiMaster::OnEndEvent(BufferProvider& provider) {
  if(!busy) return;

  auto s = currentBufferSize;
  if(s > 0) {
    auto currentSize = std::min((size_t) 255, s);
    PrepareTx(currentBufferAddr, currentSize);
    currentBufferAddr += currentSize;
    currentBufferSize -= currentSize;

    spiBaseAddress->TASKS_START = 1;
  } else {
    uint8_t* buffer = nullptr;
    size_t size = 0;
    if(provider.GetNextBuffer(&buffer, size)) {
      currentBufferAddr = (uint32_t) buffer;
      currentBufferSize = size;
      auto s = currentBufferSize;
      auto currentSize = std::min((size_t)255, s);
      PrepareTx(currentBufferAddr, currentSize);
      currentBufferAddr += currentSize;
      currentBufferSize -= currentSize;

      spiBaseAddress->TASKS_START = 1;
    } else {
      busy = false;
      nrf_gpio_pin_set(pinCsn);
    }
  }
}

void SpiMaster::OnStartedEvent(BufferProvider& provider) {
  if(!busy) return;
}

void SpiMaster::PrepareTx(const volatile uint32_t bufferAddress, const volatile size_t size) {
  spiBaseAddress->TXD.PTR = bufferAddress;
  spiBaseAddress->TXD.MAXCNT = size;
  spiBaseAddress->TXD.LIST = 0;
  spiBaseAddress->RXD.PTR = 0;
  spiBaseAddress->RXD.MAXCNT = 0;
  spiBaseAddress->RXD.LIST = 0;
  spiBaseAddress->EVENTS_END = 0;
}

bool SpiMaster::Write(const uint8_t *data, size_t size) {
  if(data == nullptr) return false;

  while(busy) {
    asm("nop");
  }

  if(size == 1) {
    SetupWorkaroundForFtpan58(spiBaseAddress, 0,0);
  } else {
    DisableWorkaroundForFtpan58(spiBaseAddress, 0, 0);
  }

  nrf_gpio_pin_clear(pinCsn);

  currentBufferAddr = (uint32_t)data;
  currentBufferSize = size;
  busy = true;

  auto currentSize = std::min((size_t)255, (size_t)currentBufferSize);
  PrepareTx(currentBufferAddr, currentSize);
  currentBufferSize -= currentSize;
  currentBufferAddr += currentSize;
  spiBaseAddress->TASKS_START = 1;

  if(size == 1) {
    while (spiBaseAddress->EVENTS_END == 0);
    busy = false;
  }

  return true;
}

void SpiMaster::Sleep() {
  while(spiBaseAddress->ENABLE != 0) {
    spiBaseAddress->ENABLE = (SPIM_ENABLE_ENABLE_Disabled << SPIM_ENABLE_ENABLE_Pos);
  }
  nrf_gpio_cfg_default(params.pinSCK);
  nrf_gpio_cfg_default(params.pinMOSI);
  nrf_gpio_cfg_default(params.pinMISO);
  nrf_gpio_cfg_default(params.pinCSN);
}

void SpiMaster::Wakeup() {
  Init();
}



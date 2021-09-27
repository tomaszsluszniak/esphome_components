#include "cse7761.h"

#include "esphome/core/log.h"

namespace esphome {
namespace cse7761 {

static const char* const TAG = "cse7761.sensor";

/*********************************************************************************************\
 * CSE7761 - Energy  (Sonoff Dual R3 Pow)
 *
 * Based on Tasmota source code
 * See https://github.com/arendst/Tasmota/discussions/10793
 * https://github.com/arendst/Tasmota/blob/development/tasmota/xnrg_19_cse7761.ino
\*********************************************************************************************/

#define CSE7761_UREF 42563  // RmsUc
#define CSE7761_IREF 52241  // RmsIAC
#define CSE7761_PREF 44513  // PowerPAC
#define CSE7761_FREF \
  3579545  // System clock (3.579545MHz) as used in frequency calculation

#define CSE7761_REG_SYSCON 0x00   // (2) System Control Register (0x0A04)
#define CSE7761_REG_EMUCON 0x01   // (2) Metering control register (0x0000)
#define CSE7761_REG_EMUCON2 0x13  // (2) Metering control register 2 (0x0001)
#define CSE7761_REG_PULSE1SEL \
  0x1D  // (2) Pin function output select register (0x3210)

#define CSE7761_REG_UFREQ 0x23  // (2) Voltage Frequency (0x0000)
#define CSE7761_REG_RMSIA \
  0x24  // (3) The effective value of channel A current (0x000000)
#define CSE7761_REG_RMSIB \
  0x25  // (3) The effective value of channel B current (0x000000)
#define CSE7761_REG_RMSU 0x26  // (3) Voltage RMS (0x000000)
#define CSE7761_REG_POWERFACTOR \
  0x27  // (3) Power factor register, select by command: channel A Power factor
        // or channel B power factor (0x7FFFFF)
#define CSE7761_REG_POWERPA \
  0x2C  // (4) Channel A active power, update rate 27.2Hz (0x00000000)
#define CSE7761_REG_POWERPB \
  0x2D  // (4) Channel B active power, update rate 27.2Hz (0x00000000)
#define CSE7761_REG_SYSSTATUS 0x43  // (1) System status register

#define CSE7761_REG_COEFFOFFSET \
  0x6E  // (2) Coefficient checksum offset (0xFFFF)
#define CSE7761_REG_COEFFCHKSUM 0x6F  // (2) Coefficient checksum
#define CSE7761_REG_RMSIAC \
  0x70  // (2) Channel A effective current conversion coefficient
#define CSE7761_REG_RMSIBC \
  0x71  // (2) Channel B effective current conversion coefficient
#define CSE7761_REG_RMSUC 0x72  // (2) Effective voltage conversion coefficient
#define CSE7761_REG_POWERPAC \
  0x73  // (2) Channel A active power conversion coefficient
#define CSE7761_REG_POWERPBC \
  0x74  // (2) Channel B active power conversion coefficient
#define CSE7761_REG_POWERSC 0x75  // (2) Apparent power conversion coefficient
#define CSE7761_REG_ENERGYAC \
  0x76  // (2) Channel A energy conversion coefficient
#define CSE7761_REG_ENERGYBC \
  0x77  // (2) Channel B energy conversion coefficient

#define CSE7761_SPECIAL_COMMAND 0xEA  // Start special command
#define CSE7761_CMD_RESET \
  0x96  // Reset command, after receiving the command, the chip resets
#define CSE7761_CMD_CHAN_A_SELECT \
  0x5A  // Current channel A setting command, which specifies the current used
        // to calculate apparent power,
        //   Power factor, phase angle, instantaneous active power,
        //   instantaneous apparent power and The channel indicated by the
        //   signal of power overload is channel A
#define CSE7761_CMD_CHAN_B_SELECT \
  0xA5  // Current channel B setting command, which specifies the current used
        // to calculate apparent power,
        //   Power factor, phase angle, instantaneous active power,
        //   instantaneous apparent power and The channel indicated by the
        //   signal of power overload is channel B
#define CSE7761_CMD_CLOSE_WRITE 0xDC   // Close write operation
#define CSE7761_CMD_ENABLE_WRITE 0xE5  // Enable write operation

enum CSE7761 {
  RmsIAC,
  RmsIBC,
  RmsUC,
  PowerPAC,
  PowerPBC,
  PowerSC,
  EnergyAC,
  EnergyBC
};

struct {
  uint32_t frequency = 0;
  uint32_t voltage_rms = 0;
  uint32_t current_rms[2] = {0};
  uint32_t energy[2] = {0};
  uint32_t active_power[2] = {0};
  uint16_t coefficient[8] = {0};
  uint8_t energy_update = 0;
  uint8_t init = 4;
  uint8_t ready = 0;
} CSE7761Data;

long last_init = 0;

inline int32_t TimeDifference(uint32_t prev, uint32_t next) {
  return ((int32_t)(next - prev));
}

int32_t TimePassedSince(uint32_t timestamp) {
  // Compute the number of milliSeconds passed since timestamp given.
  // Note: value can be negative if the timestamp has not yet been reached.
  return TimeDifference(timestamp, millis());
}

bool TimeReached(uint32_t timer) {
  // Check if a certain timeout has been reached.
  const long passed = TimePassedSince(timer);
  return (passed >= 0);
}

uint32_t Cse7761Ref(uint32_t unit) {
  switch (unit) {
    case RmsUC:
      return 0x400000 * 100 / CSE7761Data.coefficient[RmsUC];
    case RmsIAC:
      return (0x800000 * 100 / CSE7761Data.coefficient[RmsIAC]) *
             10;  // Stay within 32 bits
    case PowerPAC:
      return 0x80000000 / CSE7761Data.coefficient[PowerPAC];
  }
  return 0;
}

void CSE7761Sensor::setup() { ESP_LOGCONFIG(TAG, "Setting up CSE7761..."); }

void CSE7761Sensor::loop() {
  if (CSE7761Data.init && millis() > last_init + 1000) {
    last_init = millis();
    if (3 == CSE7761Data.init) {
      Cse7761Write(CSE7761_SPECIAL_COMMAND, CSE7761_CMD_RESET);
    } else if (2 == CSE7761Data.init) {
      uint16_t syscon = Cse7761Read(0x00, 2);  // Default 0x0A04
      if ((0x0A04 == syscon) && Cse7761ChipInit()) {
        CSE7761Data.ready = 1;
      }
    } else if (1 == CSE7761Data.init) {
      if (1 == CSE7761Data.ready) {
        Cse7761Write(CSE7761_SPECIAL_COMMAND, CSE7761_CMD_CLOSE_WRITE);
        ESP_LOGD(TAG, "C61: CSE7761 found");
        CSE7761Data.ready = 2;
      }
    }
    CSE7761Data.init--;
  }
}

void CSE7761Sensor::dump_config() {
  ESP_LOGCONFIG(TAG, "CSE7761Sensor:");
  if (this->is_failed()) {
    ESP_LOGE(TAG, "Communication with CSE7761Sensor failed!");
  }
}

float CSE7761Sensor::get_setup_priority() const {
  return setup_priority::HARDWARE;
}

void CSE7761Sensor::update() {
  if (2 == CSE7761Data.ready) {
    this->Cse7761GetData();
  }
}

void CSE7761Sensor::Cse7761Write(uint32_t reg, uint32_t data) {
  uint8_t buffer[5];

  buffer[0] = 0xA5;
  buffer[1] = reg;
  uint32_t len = 2;
  if (data) {
    if (data < 0xFF) {
      buffer[2] = data & 0xFF;
      len = 3;
    } else {
      buffer[2] = (data >> 8) & 0xFF;
      buffer[3] = data & 0xFF;
      len = 4;
    }
    uint8_t crc = 0;
    for (uint32_t i = 0; i < len; i++) {
      crc += buffer[i];
    }
    buffer[len] = ~crc;
    len++;
  }

  this->write_array(buffer, len);
}

bool CSE7761Sensor::Cse7761ReadOnce(uint32_t reg, uint32_t size,
                                    uint32_t* value) {
  while (this->available()) {
    this->read();
  }

  this->Cse7761Write(reg, 0);

  uint8_t buffer[8] = {0};
  uint32_t rcvd = 0;
  uint32_t timeout = millis() + 3;

  while (!TimeReached(timeout) && (rcvd <= size)) {
    int value = this->read();
    if ((value > -1) && (rcvd < sizeof(buffer) - 1)) {
      buffer[rcvd++] = value;
    }
  }

  if (!rcvd) {
    ESP_LOGD(TAG, PSTR("C61: Rx none"));
    return false;
  }
  if (rcvd > 5) {
    ESP_LOGD(TAG, PSTR("C61: Rx overflow"));
    return false;
  }

  rcvd--;
  uint32_t result = 0;
  uint8_t crc = 0xA5 + reg;
  for (uint32_t i = 0; i < rcvd; i++) {
    result = (result << 8) | buffer[i];
    crc += buffer[i];
  }
  crc = ~crc;
  if (crc != buffer[rcvd]) {
    return false;
  }

  *value = result;
  return true;
}

uint32_t CSE7761Sensor::Cse7761Read(uint32_t reg, uint32_t size) {
  bool result = false;  // Start loop
  uint32_t retry = 3;   // Retry up to three times
  uint32_t value = 0;   // Default no value
  while (!result && retry) {
    retry--;
    result = this->Cse7761ReadOnce(reg, size, &value);
  }
  return value;
}

uint32_t CSE7761Sensor::Cse7761ReadFallback(uint32_t reg, uint32_t prev,
                                            uint32_t size) {
  uint32_t value = Cse7761Read(reg, size);
  if (!value) {  // Error so use previous value read
    value = prev;
  }
  return value;
}

bool CSE7761Sensor::Cse7761ChipInit(void) {
  uint16_t calc_chksum = 0xFFFF;
  for (uint32_t i = 0; i < 8; i++) {
    CSE7761Data.coefficient[i] = Cse7761Read(CSE7761_REG_RMSIAC + i, 2);
    calc_chksum += CSE7761Data.coefficient[i];
  }
  calc_chksum = ~calc_chksum;
  //  uint16_t dummy = Cse7761Read(CSE7761_REG_COEFFOFFSET, 2);
  uint16_t coeff_chksum = Cse7761Read(CSE7761_REG_COEFFCHKSUM, 2);
  if ((calc_chksum != coeff_chksum) || (!calc_chksum)) {
    ESP_LOGD(TAG, PSTR("C61: Default calibration"));
    CSE7761Data.coefficient[RmsIAC] = CSE7761_IREF;
    //    CSE7761Data.coefficient[RmsIBC] = 0xCC05;
    CSE7761Data.coefficient[RmsUC] = CSE7761_UREF;
    CSE7761Data.coefficient[PowerPAC] = CSE7761_PREF;
    //    CSE7761Data.coefficient[PowerPBC] = 0xADD7;
  }

  Cse7761Write(CSE7761_SPECIAL_COMMAND, CSE7761_CMD_ENABLE_WRITE);

  //  delay(8);  // Exception on ESP8266
  //  uint32_t timeout = millis() + 8;
  //  while (!TimeReached(timeout)) { }

  uint8_t sys_status = Cse7761Read(CSE7761_REG_SYSSTATUS, 1);
  if (sys_status & 0x10) {  // Write enable to protected registers (WREN)
                            /*
                                System Control Register (SYSCON)  Addr:0x00  Default value: 0x0A04
                                Bit    name               Function description
                                15-11  NC                 -, the default is 1
                                10     ADC2ON
                                                          =1, means ADC current channel B is on (Sonoff
                               Dual R3 Pow)                         =0, means ADC current channel B is closed                         9      NC                         -, the
                               default is 1.                         8-6    PGAIB[2:0]         Current channel B analog gain
                               selection highest bit                         =1XX, PGA of current channel B=16 (Sonoff Dual R3
                               Pow)                         =011, PGA of current channel B=8                         =010, PGA of current channel B=4
                                                          =001, PGA of current channel B=2
                                                          =000, PGA of current channel B=1
                                5-3    PGAU[2:0]          Highest bit of voltage channel analog gain
                               selection                         =1XX, PGA of voltage U=16                         =011, PGA of voltage U=8                         =010, PGA of
                               voltage U=4                         =001, PGA of voltage U=2                         =000, PGA of voltage U=1 (Sonoff
                               Dual R3 Pow)                         2-0    PGAIA[2:0]         Current channel A analog gain
                               selection highest bit                         =1XX, PGA of current channel A=16 (Sonoff Dual R3
                               Pow)                         =011, PGA of current channel A=8                         =010, PGA of current channel A=4
                                                          =001, PGA of current channel A=2
                                                          =000, PGA of current channel A=1
                            */
    Cse7761Write(CSE7761_REG_SYSCON | 0x80, 0xFF04);

    /*
        Energy Measure Control Register (EMUCON)  Addr:0x01  Default value:
       0x0000 Bit    name               Function description 15-14
       Tsensor_Step[1:0]  Measurement steps of temperature sensor: =2'b00 The
       first step of temperature sensor measurement, the Offset of OP1 and OP2
       is +/+. (Sonoff Dual R3 Pow) =2'b01 The second step of temperature sensor
       measurement, the Offset of OP1 and OP2 is +/-. =2'b10 The third step of
       temperature sensor measurement, the Offset of OP1 and OP2 is -/+. =2'b11
       The fourth step of temperature sensor measurement, the Offset of OP1 and
       OP2 is -/-. After measuring these four results and averaging, the AD
       value of the current measured temperature can be obtained. 13 tensor_en
       Temperature measurement module control =0 when the temperature
       measurement module is closed; (Sonoff Dual R3 Pow) =1 when the
       temperature measurement module is turned on; 12     comp_off Comparator
       module close signal: =0 when the comparator module is in working state =1
       when the comparator module is off (Sonoff Dual R3 Pow) 11-10  Pmode[1:0]
       Selection of active energy calculation method: Pmode =00, both positive
       and negative active energy participate in the accumulation, the
       accumulation method is algebraic sum mode, the reverse REVQ symbol
       indicates to active power; (Sonoff Dual R3 Pow) Pmode = 01, only
       accumulate positive active energy; Pmode = 10, both positive and negative
       active energy participate in the accumulation, and the accumulation
       method is absolute value method. No reverse active power indication;
                                  Pmode =11, reserved, the mode is the same as
       Pmode =00 9      NC                 - 8      ZXD1               The
       initial value of ZX output is 0, and different waveforms are output
       according to the configuration of ZXD1 and ZXD0: =0, it means that the ZX
       output changes only at the selected zero-crossing point (Sonoff Dual R3
       Pow) =1, indicating that the ZX output changes at both the positive and
       negative zero crossings 7      ZXD0 =0, indicates that the positive
       zero-crossing point is selected as the zero-crossing detection signal
       (Sonoff Dual R3 Pow) =1, indicating that the negative zero-crossing point
       is selected as the zero-crossing detection signal 6      HPFIBOFF =0,
       enable current channel B digital high-pass filter (Sonoff Dual R3 Pow)
                                  =1, turn off the digital high-pass filter of
       current channel B 5      HPFIAOFF =0, enable current channel A digital
       high-pass filter (Sonoff Dual R3 Pow) =1, turn off the digital high-pass
       filter of current channel A 4      HPFUOFF =0, enable U channel digital
       high pass filter (Sonoff Dual R3 Pow) =1, turn off the U channel digital
       high-pass filter 3-2    NC                 - 1      PBRUN =1, enable PFB
       pulse output and active energy register accumulation; (Sonoff Dual R3
       Pow) =0 (default), turn off PFB pulse output and active energy register
       accumulation. 0      PARUN =1, enable PFA pulse output and active energy
       register accumulation; (Sonoff Dual R3 Pow) =0 (default), turn off PFA
       pulse output and active energy register accumulation.
    */
    //    Cse7761Write(CSE7761_REG_EMUCON | 0x80, 0x1003);
    Cse7761Write(CSE7761_REG_EMUCON | 0x80,
                 0x1183);  // Tasmota enable zero cross detection on both
                           // positive and negative signal

    /*
        Energy Measure Control Register (EMUCON2)  Addr: 0x13  Default value:
       0x0001 Bit    name               Function description 15-13  NC - 12
       SDOCmos =1, SDO pin CMOS open-drain output =0, SDO pin CMOS output
       (Sonoff Dual R3 Pow) 11     EPB_CB             Energy_PB clear signal
       control, the default is 0, and it needs to be configured to 1 in UART
       mode. Clear after reading is not supported in UART mode =1, Energy_PB
       will not be cleared after reading; (Sonoff Dual R3 Pow) =0, Energy_PB is
       cleared after reading; 10     EPA_CB             Energy_PA clear signal
       control, the default is 0, it needs to be configured to 1 in UART mode,
                                    Clear after reading is not supported in UART
       mode =1, Energy_PA will not be cleared after reading; (Sonoff Dual R3
       Pow) =0, Energy_PA is cleared after reading; 9-8    DUPSEL[1:0] Average
       register update frequency control =00, Update frequency 3.4Hz =01, Update
       frequency 6.8Hz =10, Update frequency 13.65Hz =11, Update
       frequency 27.3Hz (Sonoff Dual R3 Pow) 7      CHS_IB             Current
       channel B measurement selection signal =1, measure the current of channel
       B (Sonoff Dual R3 Pow) =0, measure the internal temperature of the chip
        6      PfactorEN          Power factor function enable
                                  =1, turn on the power factor output function
       (Sonoff Dual R3 Pow) =0, turn off the power factor output function 5
       WaveEN             Waveform data, instantaneous data output enable signal
                                  =1, turn on the waveform data output function
       (Tasmota add frequency) =0, turn off the waveform data output function
       (Sonoff Dual R3 Pow) 4      SAGEN              Voltage drop detection
       enable signal, WaveEN=1 must be configured first =1, turn on the voltage
       drop detection function =0, turn off the voltage drop detection function
       (Sonoff Dual R3 Pow) 3      OverEN             Overvoltage, overcurrent,
       and overload detection enable signal, WaveEN=1 must be configured first
                                  =1, turn on the overvoltage, overcurrent, and
       overload detection functions =0, turn off the overvoltage, overcurrent,
       and overload detection functions (Sonoff Dual R3 Pow) 2      ZxEN
       Zero-crossing detection, phase angle, voltage frequency measurement
       enable signal =1, turn on the zero-crossing detection, phase angle, and
       voltage frequency measurement functions (Tasmota add frequency) =0,
       disable zero-crossing detection, phase angle, voltage frequency
       measurement functions (Sonoff Dual R3 Pow) 1      PeakEN             Peak
       detect enable signal =1, turn on the peak detection function =0, turn off
       the peak detection function (Sonoff Dual R3 Pow) 0      NC Default is 1
    */
    Cse7761Write(CSE7761_REG_EMUCON2 | 0x80, 0x0FC1);  // Sonoff Dual R3 Pow
                                                       /*
                                                           Pin function output selection register (PULSE1SEL)  Addr: 0x1D  Default
                                                          value: 0x3210                                                    Bit    name               Function description                                                    15-13  NC                                                    -
                                                           12     SDOCmos
                                                                                     =1, SDO pin CMOS open-drain output
                                                           15-12  NC                 NC, the default value is 4'b0011
                                                           11-8   NC                 NC, the default value is 4'b0010
                                                           7-4    P2Sel              Pulse2 Pin output function selection, see the
                                                          table below                                                    3-0    P1Sel              Pulse1 Pin output function
                                                          selection, see the table below                                                    Table Pulsex function output selection
                                                          list                                                    Pxsel  Select description                                                    0000   Output of energy metering
                                                          calibration pulse PFA                                                    0001   The output of the energy metering
                                                          calibration pulse PFB                                                    0010   Comparator indication signal comp_sign                                                    0011
                                                          Interrupt signal IRQ output (the default is high level, if it is an
                                                          interrupt, set to 0)                                                    0100   Signal indication of power overload: only PA
                                                          or PB can be selected                                                    0101   Channel A negative power indicator signal
                                                           0110   Channel B negative power indicator signal
                                                           0111   Instantaneous value update interrupt output
                                                           1000   Average update interrupt output
                                                           1001   Voltage channel zero-crossing signal output (Tasmota add
                                                          zero-cross detection)                                                    1010   Current channel A zero-crossing signal
                                                          output                                                    1011   Current channel B zero crossing signal output                                                    1100                                                    Voltage
                                                          channel overvoltage indication signal output                                                    1101   Voltage channel
                                                          undervoltage indication signal output                                                    1110   Current channel A
                                                          overcurrent signal indication output                                                    1111   Current channel B overcurrent
                                                          signal indication output
                                                       */
    Cse7761Write(CSE7761_REG_PULSE1SEL | 0x80, 0x3290);
  } else {
    ESP_LOGD(TAG, PSTR("C61: Write failed"));
    return false;
  }
  return true;
}

void CSE7761Sensor::Cse7761GetData(void) {
  // The effective value of current and voltage Rms is a 24-bit signed number,
  // the highest bit is 0 for valid data,
  //   and when the highest bit is 1, the reading will be processed as zero
  // The active power parameter PowerA/B is in two’s complement format, 32-bit
  // data, the highest bit is Sign bit.
  uint32_t value =
      Cse7761ReadFallback(CSE7761_REG_RMSU, CSE7761Data.voltage_rms, 3);
  CSE7761Data.voltage_rms = (value >= 0x800000) ? 0 : value;

  value = Cse7761ReadFallback(CSE7761_REG_RMSIA, CSE7761Data.current_rms[0], 3);
  CSE7761Data.current_rms[0] = ((value >= 0x800000) || (value < 1600))
                                   ? 0
                                   : value;  // No load threshold of 10mA
  value =
      Cse7761ReadFallback(CSE7761_REG_POWERPA, CSE7761Data.active_power[0], 4);
  CSE7761Data.active_power[0] = (0 == CSE7761Data.current_rms[0]) ? 0
                                : (value & 0x80000000)            ? (~value) + 1
                                                                  : value;

  value = Cse7761ReadFallback(CSE7761_REG_RMSIB, CSE7761Data.current_rms[1], 3);
  CSE7761Data.current_rms[1] = ((value >= 0x800000) || (value < 1600))
                                   ? 0
                                   : value;  // No load threshold of 10mA
  value =
      Cse7761ReadFallback(CSE7761_REG_POWERPB, CSE7761Data.active_power[1], 4);
  CSE7761Data.active_power[1] = (0 == CSE7761Data.current_rms[1]) ? 0
                                : (value & 0x80000000)            ? (~value) + 1
                                                                  : value;

  ESP_LOGD(TAG, PSTR("C61: F%d, U%d, I%d/%d, P%d/%d"), CSE7761Data.frequency,
           CSE7761Data.voltage_rms, CSE7761Data.current_rms[0],
           CSE7761Data.current_rms[1], CSE7761Data.active_power[0],
           CSE7761Data.active_power[1]);

  // convert values and publish to sensors

  float voltage = (float)CSE7761Data.voltage_rms / Cse7761Ref(RmsUC);
  if (this->voltage_sensor_ != nullptr) {
    this->voltage_sensor_->publish_state(voltage);
  }

  for (uint32_t channel = 0; channel < 2; channel++) {
    // Active power = PowerPA * PowerPAC * 1000 / 0x80000000
    float activePower =
        (float)CSE7761Data.active_power[channel] / Cse7761Ref(PowerPAC);  // W
    float amps =
        (float)CSE7761Data.current_rms[channel] / Cse7761Ref(RmsIAC);  // A
    ESP_LOGD(TAG, PSTR("C61: Channel %d power %f W, current %f A"), channel,
             activePower, amps);
    if (channel == 0) {
      if (this->power_sensor_1_ != nullptr) {
        this->power_sensor_1_->publish_state(activePower);
      }
      if (this->current_sensor_1_ != nullptr) {
        this->current_sensor_1_->publish_state(amps);
      }
    } else if (channel == 1) {
      if (this->power_sensor_2_ != nullptr) {
        this->power_sensor_2_->publish_state(activePower);
      }
      if (this->current_sensor_2_ != nullptr) {
        this->current_sensor_2_->publish_state(amps);
      }
    }
  }
}

}  // namespace cse7761
}  // namespace esphome
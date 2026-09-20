// AXP2101 power configuration for the ESP32-S3-Touch-AMOLED-2.06.
//
// The externally powered OTG lead supplies the audio interface directly. These
// limits keep the board tolerant of USB transients while its own battery powers
// the ESP32-S3 and AMOLED.
//
// The Gea runtime brings the measurement channels up and reads the gauge
// through the same chip; what is set here is what this pedal's wiring needs and
// no board can be given by default. Both reach the PMU over the shared I2C bus,
// and the settings survive either order: the driver's own writes are
// read-modify-write, so enabling the channels does not undo the thermistor
// channel being switched off here.
//
// Only a board that carries the PMIC gets any of this. A bare module has no
// AXP2101 to configure, the framework links its own absent-power backend in
// place of the chip driver, and naming the driver here would fail to link
// rather than degrade. The board declares which it is; a target that says
// nothing is the AMOLED, whose PMIC is wired in.
#if !defined(GEA_BOARD_HAS_POWER) || GEA_BOARD_HAS_POWER

#include "power/axp2101/axp2101.h"

#include "esp_err.h"
#include "esp_log.h"

#include "driver/i2c_master.h"
#include "i2c.h"

namespace {

namespace axp = gea::chips::axp2101;

constexpr const char* kTag = "pmu";
constexpr int kI2cTimeoutMs = 200;

class I2cRegisterBus final : public axp::RegisterBus {
  public:
    esp_err_t attach() {
        const auto bus = gea::platform::i2c::Bus::primary();
        if (!bus.available()) {
            return ESP_FAIL;
        }
        i2c_device_config_t config = {};
        config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        config.device_address = axp::kI2cAddress;
        config.scl_speed_hz = 100000;
        return i2c_master_bus_add_device(static_cast<i2c_master_bus_handle_t>(bus.nativeHandle()),
                                         &config, &device_);
    }

    bool writeRegister(uint8_t reg, uint8_t value) override {
        const uint8_t payload[2] = {reg, value};
        return i2c_master_transmit(device_, payload, sizeof(payload), kI2cTimeoutMs) == ESP_OK;
    }

    bool readRegister(uint8_t reg, uint8_t& value) override {
        return i2c_master_transmit_receive(device_, &reg, 1, &value, 1, kI2cTimeoutMs) == ESP_OK;
    }

  private:
    i2c_master_dev_handle_t device_ = nullptr;
};

I2cRegisterBus pmu_bus;
axp::PowerManagementUnit PMU(pmu_bus);

} // namespace

esp_err_t pmu_init() {
    const esp_err_t added = pmu_bus.attach();
    if (added != ESP_OK) {
        ESP_LOGE(kTag, "AXP2101 device add failed: %s", esp_err_to_name(added));
        return added;
    }

    // The whole point: stop the PMU from browning out when the USB host port
    // feeds a bus-powered device.
    if (!PMU.setVbusCurrentLimit(axp::VbusCurrentLimit::mA2000)) {
        ESP_LOGE(kTag, "AXP2101 not responding");
        return ESP_FAIL;
    }
    // Tolerate deep VBUS sag during a downstream device's inrush instead of
    // declaring the supply lost.
    PMU.setVbusVoltageLimit(3880);
    PMU.setSystemPowerDownVoltage(2600);
    // The Y-lead's charger feeds the audio interface and this VBUS pin from one
    // wire; at the default charge current the PMU's own draw can sag the lead
    // enough to reset the interface when streaming starts. Keep the battery
    // topping up gently instead of competing for the supply.
    PMU.setChargeCurrent(axp::ChargeCurrent::mA100);
    // No battery temperature sensor on this board; leaving TS measurement on
    // disturbs charging logic.
    PMU.setBatteryTemperatureMeasure(false);
    PMU.setVbusVoltageMeasure(true);
    PMU.setSystemVoltageMeasure(true);
    PMU.clearInterrupts();

    axp::VbusCurrentLimit limit = axp::VbusCurrentLimit::mA100;
    PMU.vbusCurrentLimit(limit);
    ESP_LOGI(kTag, "AXP2101 ready: VBUS limit=%u (5=2A), VBUS=%d mV, VSYS=%d mV",
             static_cast<unsigned>(limit), PMU.vbusMillivolts(), PMU.systemMillivolts());
    return ESP_OK;
}

#else

#include "esp_err.h"

// Nothing to configure, and nothing failed: main only warns when this does not
// return ESP_OK, and a module without a PMIC has no power setup to get wrong.
esp_err_t pmu_init() {
    return ESP_OK;
}

#endif

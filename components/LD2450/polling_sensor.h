#pragma once
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/sensor/sensor.h"

namespace esphome::ld2450
{
    /**
     * @brief Simple polling sensor which publishes it's values on a regular basis.
     * Additionally, this sensor converts the output according to the desired unit of measurement.
     */
    class PollingSensor : public sensor::Sensor, public PollingComponent
    {
    public:
        void setup() override
        {
            // Determine unit conversion
            const StringRef unit = this->get_unit_of_measurement_ref();
            if (unit == "m")
                conversion_factor_ = 0.001f;
            else if (unit == "cm")
                conversion_factor_ = 0.1f;
        }

        void update() override
        {
            const float current = this->get_raw_state();
            if (current != value_ && !(std::isnan(current) && std::isnan(value_)))
                publish_state(value_);
        }

        /**
         * Gets the unit conversion factor of this sensor.
         * @param new value (distance in mm)
         */
        void set_value(float new_value)
        {
            value_ = new_value * conversion_factor_;
        }

    private:
        /// @brief conversion factor applied to input values
        float conversion_factor_ = 1;

        /// @brief Value of this sensor (un-published)
        float value_ = 0;
    };
} // namespace esphome::ld2450
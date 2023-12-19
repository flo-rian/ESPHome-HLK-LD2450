#include "LD2450.h"
#include "esphome/core/log.h"

namespace esphome::ld2450
{

    static const char *TAG = "LD2450";

    void LD2450::setup()
    {

        check_uart_settings(256000, 1, uart::UART_CONFIG_PARITY_NONE, 8);

        // Fill target list with mock targets if not present
        for (int i = targets_.size(); i < 3; i++)
        {
            Target *new_target = new Target();
            targets_.push_back(new_target);
        }

        for (int i = 0; i < targets_.size(); i++)
        {
            Target *target = targets_[i];
            // Generate Names if not present
            if (target->get_name() == nullptr)
            {
                std::string name = std::string("Target ").append(std::to_string(i + 1));
                char *cstr = new char[name.length() + 1];
                std::strcpy(cstr, name.c_str());
                target->set_name(cstr);
            }

            target->set_fast_off_detection(fast_off_detection_);
        }

#ifdef USE_BINARY_SENSOR
        if (occupancy_binary_sensor_ != nullptr)
            occupancy_binary_sensor_->publish_initial_state(false);
#endif
    }

    void LD2450::dump_config()
    {
        ESP_LOGCONFIG(TAG, "LD2450 Hub: %s", name_);
        ESP_LOGCONFIG(TAG, "  fast_off_detection: %s", fast_off_detection_ ? "True" : "False");
        ESP_LOGCONFIG(TAG, "  flip_x_axis: %s", flip_x_axis_ ? "True" : "False");
        ESP_LOGCONFIG(TAG, "  max_detection_distance: %i mm", max_detection_distance_);
        ESP_LOGCONFIG(TAG, "  max_distance_margin: %i mm", max_distance_margin_);
#ifdef USE_BINARY_SENSOR
        LOG_BINARY_SENSOR("  ", "OccupancyBinarySensor", occupancy_binary_sensor_);
#endif
#ifdef USE_NUMBER
        LOG_NUMBER("  ", "MaxDistanceNumber", max_distance_number_);
#endif
        ESP_LOGCONFIG(TAG, "Zones:");
        if (zones_.size() > 0)
        {
            for (Zone *zone : zones_)
            {
                zone->dump_config();
            }
        }

        // Read and log Firmware-version
        log_sensor_version();
    }

    const uint8_t update_header[4] = {0xAA, 0xFF, 0x03, 0x00};
    const uint8_t config_header[4] = {0xFD, 0xFC, 0xFB, 0xFA};
    void LD2450::loop()
    {
        // Process command queue
        if (command_queue_.size() > 0)
        {
            // Inject enter config mode command if not in mode
            if (!configuration_mode_ && command_queue_.front()[0] != COMMAND_ENTER_CONFIG)
            {
                command_queue_.insert(command_queue_.begin(), {COMMAND_ENTER_CONFIG, 0x00, 0x01, 0x00});
            }

            // Wait before retransmitting
            if (millis() - command_last_sent_ > COMMAND_RETRY_DELAY)
            {

                // Remove command form queue after max retries
                if (command_send_retries_ >= COMMAND_MAX_RETRIES)
                {
                    command_queue_.erase(command_queue_.begin());
                    command_send_retries_ = 0;
                    ESP_LOGW(TAG, "Sending command timed out! Is the sensor connected?");
                }
                else
                {
                    std::vector<uint8_t> command = command_queue_.front();
                    write_command(&command[0], command.size());
                    command_last_sent_ = millis();
                    command_send_retries_++;
                }
            }
        }
        else if (configuration_mode_)
        {
            // Inject leave config command after clearing the queue
            command_queue_.push_back({COMMAND_LEAVE_CONFIG, 0x00});
        }

        // Skip stream until start of message and parse header
        while (!peek_status_ && available() >= 4)
        {
            // Try to read the header and abort on mismatch
            const uint8_t *header;
            bool skip = false;
            uint8_t target;
            if (peek() == update_header[0])
            {
                header = update_header;
                target = 1;
            }
            else
            {
                header = config_header;
                target = 2;
            }

            for (int i = 0; i < 4 && !skip; i++)
            {
                if (read() != header[i])
                    skip = true;
            }
            if (skip)
                continue;

            // Flag successful header reading
            peek_status_ = target;
        }

        if (peek_status_ == 1 && available() >= 28)
        {
            uint8_t msg[26] = {0x00};
            read_array(msg, 26);
            peek_status_ = 0;

            // Skip invalid messages
            if (msg[24] != 0x55 || msg[25] != 0xCC)
                return;

            process_message(msg, 24);
        }
        if (peek_status_ == 2 && (available() >= 2 || configuration_message_length_ > 0))
        {
            if (configuration_message_length_ == 0)
            {
                // Read message content length
                uint8_t content_length[2];
                read_array(content_length, 2);
                configuration_message_length_ = content_length[1] << 8 | content_length[0];
            }

            // Wait until message and frame end are available
            if (available() >= configuration_message_length_ + 4)
            {
                uint8_t msg[configuration_message_length_ + 4] = {0x00};
                read_array(msg, configuration_message_length_ + 4);

                // Assert frame end read correctly
                if (msg[configuration_message_length_] == 0x04 && msg[configuration_message_length_ + 1] == 0x03 && msg[configuration_message_length_ + 2] == 0x02 && msg[configuration_message_length_ + 3] == 0x01)
                {
                    process_config_message(msg, configuration_message_length_);
                }
                configuration_message_length_ = 0;
                peek_status_ = 0;
            }
        }
    }

    void LD2450::process_message(uint8_t *msg, int len)
    {
        for (int i = 0; i < 3; i++)
        {
            int offset = 8 * i;

            int16_t x = msg[offset + 1] << 8 | msg[offset + 0];
            if (msg[offset + 1] & 0x80)
                x = -x + 0x8000;
            int16_t y = (msg[offset + 3] << 8 | msg[offset + 2]);
            if (y != 0)
                y -= 0x8000;
            int speed = msg[offset + 5] << 8 | msg[offset + 4];
            if (msg[offset + 5] & 0x80)
                speed = -speed + 0x8000;
            int distance_resolution = msg[offset + 7] << 8 | msg[offset + 6];

            // Flip x axis if required
            x = x * (flip_x_axis_ ? -1 : 1);

            targets_[i]->update_values(x, y, speed, distance_resolution);

            // Filter targets further than max detection distance
            if (y <= max_detection_distance_ || (targets_[i]->is_present() && y <= max_detection_distance_ + max_distance_margin_))
                targets_[i]->update_values(x, y, speed, distance_resolution);
            else if (y >= max_detection_distance_ + max_distance_margin_)
                targets_[i]->clear();
        }

        int target_count = 0;
        for (Target *target : targets_)
        {
            target_count += target->is_present();
        }
        is_occupied_ = target_count > 0;

#ifdef USE_BINARY_SENSOR
        if (occupancy_binary_sensor_ != nullptr && occupancy_binary_sensor_->state != is_occupied_)
            occupancy_binary_sensor_->publish_state(is_occupied_);
#endif
#ifdef USE_SENSOR
        if (target_count_sensor_ != nullptr && target_count_sensor_->state != target_count)
            target_count_sensor_->publish_state(target_count);
#endif

        // Update zones and related components
        for (Zone *zone : zones_)
        {
            zone->update(targets_);
        }
    }

    void LD2450::process_config_message(uint8_t *msg, int len)
    {
        // Remove command from Queue upon receiving acknowledgement
        std::vector<uint8_t> command = command_queue_.front();
        if (command_queue_.size() > 0 && command[0] == msg[0] && msg[1] == 0x01)
        {
            command_queue_.erase(command_queue_.begin());
            command_send_retries_ = 0;
            command_last_sent_ = 0;
        }

        if (msg[0] == COMMAND_ENTER_CONFIG && msg[1] == true)
        {
            configuration_mode_ = true;
        }

        if (msg[0] == COMMAND_LEAVE_CONFIG && msg[1] == true)
        {
            configuration_mode_ = false;
        }

        if (msg[0] == COMMAND_READ_VERSION && msg[1] == true)
        {
            ESP_LOGI(TAG, "Sensor Firmware-Version: V%X.%02X.%02X%02X%02X%02X", msg[7], msg[6], msg[11], msg[10], msg[9], msg[8]);
        }
    }

    void LD2450::write_command(uint8_t *msg, int len)
    {
        // Write frame header
        write_array({0xFD, 0xFC, 0xFB, 0xFA});

        // Write message length
        write(static_cast<uint8_t>(len));
        write(static_cast<uint8_t>(len << 8));

        // Write message content
        write_array(msg, len);

        // Write frame end
        write_array({0x04, 0x03, 0x02, 0x01});

        flush();
    }
}
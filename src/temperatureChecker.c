/*
 * MIT License
 *
 * Copyright (c) 2017, Alexander Solncev
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
*/

#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "owb.h"
#include "owb_rmt.h"
#include "ds18b20.h"
#include "config/config.h"
#include "temperatureChecker.h"
#include "publicQueues.h"

static Temperature_info temperatures;

xSemaphoreHandle temperatureMutex()
{
    static xSemaphoreHandle pwm_mutex = NULL;
    if (pwm_mutex == NULL) {
        pwm_mutex = xSemaphoreCreateMutex();
    }
    return pwm_mutex;
}

Temperature_info getTemperatures(){
  Temperature_info result;
  if (xSemaphoreTake(temperatureMutex(), portMAX_DELAY) == pdTRUE) {
      result = temperatures;
      xSemaphoreGive(temperatureMutex());
  }
  return result;
}

bool setTemperature(uint32_t device_index, float temperature){
  if (xSemaphoreTake(temperatureMutex(), portMAX_DELAY) == pdTRUE) {
      if(device_index == 0){
          temperatures.temperatureFirst = temperature;
      }else if(device_index == 1){
          temperatures.temperatureSecond = temperature;
      }else if(device_index == 2){
          temperatures.temperatureThird = temperature;
      }
      xSemaphoreGive(temperatureMutex());
  }
  return true;
}

void checkingTemperaturesTask(void* pvParameters)
{
    esp_log_level_set("*", ESP_LOG_INFO);

    // Stable readings require a brief period before communication
    vTaskDelay(2000.0 / portTICK_PERIOD_MS);

    // Create a 1-Wire bus
    OneWireBus* owb;
    owb_rmt_driver_info rmt_driver_info;
    owb = owb_rmt_initialize(&rmt_driver_info, DS18B20_GPIO_PIN, RMT_CHANNEL_1, RMT_CHANNEL_0);

    owb_use_crc(owb, true); // enable CRC check for ROM code

    // Find all connected devices
    printf("Find devices:\n");
    OneWireBus_ROMCode device_rom_codes[DS18B20_DEVICES_QUANTITY] = { 0 };
    int num_devices = 0;
    OneWireBus_SearchState search_state = { 0 };
    bool found = false;
    owb_search_first(owb, &search_state, &found);
    while (found) {
        char rom_code_s[17];
        owb_string_from_rom_code(search_state.rom_code, rom_code_s, sizeof(rom_code_s));
        printf("  %d : %s\n", num_devices, rom_code_s);
        device_rom_codes[num_devices] = search_state.rom_code;
        ++num_devices;
        owb_search_next(owb, &search_state, &found);
    }

    printf("Found %d devices\n", num_devices);

    // Known ROM code (LSB first):
    OneWireBus_ROMCode known_device = {
        .fields.family = { 0x28 },
        .fields.serial_number = { 0xee, 0xcc, 0x87, 0x2e, 0x16, 0x01 },
        .fields.crc = { 0x00 },
    };
    char rom_code_s[17];
    owb_string_from_rom_code(known_device, rom_code_s, sizeof(rom_code_s));
    bool is_present = false;
    owb_verify_rom(owb, known_device, &is_present);
    printf("Device %s is %s\n", rom_code_s, is_present ? "present" : "not present");

    // Create a DS18B20 device on the 1-Wire bus
    DS18B20_Info* devices[DS18B20_DEVICES_QUANTITY] = { 0 };

    for (int i = 0; i < num_devices; ++i) {
        DS18B20_Info* ds18b20_info = ds18b20_malloc(); // heap allocation
        devices[i] = ds18b20_info;
        if (num_devices == 1) {
            printf("Single device optimisations enabled\n");
            ds18b20_init_solo(ds18b20_info, owb); // only one device on bus
        }
        else {
            ds18b20_init(ds18b20_info, owb, device_rom_codes[i]); // associate with bus and device
        }
        ds18b20_use_crc(ds18b20_info, true); // enable CRC check for temperature readings
        ds18b20_set_resolution(ds18b20_info, DS18B20_RESOLUTION);
    }

    // Read temperatures more efficiently by starting conversions on all devices at the same time
    int crc_errors[DS18B20_DEVICES_QUANTITY] = { 0 };
    if (num_devices > 0) {
        while (1) {
            TickType_t start_ticks = xTaskGetTickCount();

            ds18b20_convert_all(owb);

            // In this application all devices use the same resolution,
            // so use the first device to determine the delay
            ds18b20_wait_for_conversion(devices[0]);

            // Read the results immediately after conversion otherwise it may fail
            // (using printf before reading may take too long)
            float temps[DS18B20_DEVICES_QUANTITY] = { 0 };
            for (int i = 0; i < num_devices; ++i) {
                temps[i] = ds18b20_read_temp(devices[i]);
            }

            // Send results in a separate loop, after all have been read
            for (int i = 0; i < num_devices; ++i) {
                if (temps[i] == DS18B20_INVALID_READING) {
                    ++crc_errors[i];
                }
                setTemperature(i, temps[i]);
            }

            // Make up periodic delay to approximately one sample period per measurement
            if ((xTaskGetTickCount() - start_ticks) < (DS18B20_CHECK_PERIOD / portTICK_PERIOD_MS)) {
                vTaskDelay(DS18B20_CHECK_PERIOD / portTICK_PERIOD_MS - (xTaskGetTickCount() - start_ticks));
            }
        }
    }

    printf("No devices found.\n");
    vTaskDelete( NULL );
}

void startCheckingTemperatures(int priority)
{
    xTaskCreate(checkingTemperaturesTask, "dallas_checking", STACK_SIZE, NULL, priority, NULL);
}

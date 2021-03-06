#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/task.h"

#include "sdkconfig.h" // generated by "make menuconfig"

#include "bme280.h"

#define TAG "BME280"

#define I2C_MASTER_ACK 0
#define I2C_MASTER_NACK 1

#define i2c_num I2C_NUM_0

#define WRITE_BIT                           I2C_MASTER_WRITE /*!< I2C master write */
#define READ_BIT                            I2C_MASTER_READ  /*!< I2C master read */
#define ACK_CHECK_EN                        0x1              /*!< I2C master will check ack from slave*/
#define ACK_CHECK_DIS                       0x0              /*!< I2C master will not check ack from slave */
#define ACK_VAL                             0x0              /*!< I2C ack value */
#define NACK_VAL                            0x1              /*!< I2C nack value */
#define LAST_NACK_VAL                       0x2              /*!< I2C last_nack value */


/**
 * @brief i2c master initialization
 */
esp_err_t i2c_master_init(const int sda_pin, const int scl_pin)
{
    int i2c_master_port = I2C_NUM_0;
    i2c_config_t conf;
		conf.mode = I2C_MODE_MASTER,
		conf.sda_io_num = sda_pin,
		conf.scl_io_num = scl_pin,
    conf.sda_pullup_en = 1;
    conf.scl_pullup_en = 1;
    #ifdef CONFIG_TARGET_DEVICE_ESP32
    ESP_ERROR_CHECK(i2c_driver_install(i2c_master_port, conf.mode, 0, 0, 0));
    #else //CONFIG_TARGET_DEVICE_ESP32
    ESP_ERROR_CHECK(i2c_driver_install(i2c_master_port, conf.mode));
    #endif //CONFIG_TARGET_DEVICE_ESP32
    ESP_ERROR_CHECK(i2c_param_config(i2c_master_port, &conf));
    return ESP_OK;
}

/**
 * @brief test code to write mpu6050
 *
 * 1. send data
 * ___________________________________________________________________________________________________
 * | start | slave_addr + wr_bit + ack | write reg_address + ack | write data_len byte + ack  | stop |
 * --------|---------------------------|-------------------------|----------------------------|------|
 *
 * @param i2c_num I2C port number
 * @param reg_address slave reg address
 * @param data data to send
 * @param data_len data length
 *
 * @return
 *     - ESP_OK Success
 *     - ESP_ERR_INVALID_ARG Parameter error
 *     - ESP_FAIL Sending command error, slave doesn't ACK the transfer.
 *     - ESP_ERR_INVALID_STATE I2C driver not installed or not in master mode.
 *     - ESP_ERR_TIMEOUT Operation timeout because the bus is busy.
 */
s8 BME280_I2C_bus_write(u8 dev_addr, u8 reg_address, u8 *data, u8 data_len)
{
    int ret;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, dev_addr << 1 | WRITE_BIT, ACK_CHECK_EN);
    i2c_master_write_byte(cmd, reg_address, ACK_CHECK_EN);
    i2c_master_write(cmd, data, data_len, ACK_CHECK_EN);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(i2c_num, cmd, 10 / portTICK_RATE_MS);
    i2c_cmd_link_delete(cmd);

    return ret;
}

/**
 * @brief test code to read mpu6050
 *
 * 1. send reg address
 * ______________________________________________________________________
 * | start | slave_addr + wr_bit + ack | write reg_address + ack | stop |
 * --------|---------------------------|-------------------------|------|
 *
 * 2. read data
 * ___________________________________________________________________________________
 * | start | slave_addr + wr_bit + ack | read data_len byte + ack(last nack)  | stop |
 * --------|---------------------------|--------------------------------------|------|
 *
 * @param i2c_num I2C port number
 * @param reg_address slave reg address
 * @param data data to read
 * @param data_len data length
 *
 * @return
 *     - ESP_OK Success
 *     - ESP_ERR_INVALID_ARG Parameter error
 *     - ESP_FAIL Sending command error, slave doesn't ACK the transfer.
 *     - ESP_ERR_INVALID_STATE I2C driver not installed or not in master mode.
 *     - ESP_ERR_TIMEOUT Operation timeout because the bus is busy.
 */
s8 BME280_I2C_bus_read(u8 dev_addr, u8 reg_address, u8 *data, u8 data_len)
{
    int ret;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, dev_addr << 1 | WRITE_BIT, ACK_CHECK_EN);
    i2c_master_write_byte(cmd, reg_address, ACK_CHECK_EN);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(i2c_num, cmd, 10 / portTICK_RATE_MS);
    i2c_cmd_link_delete(cmd);

    if (ret != ESP_OK) {
        return ret;
    }

    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, dev_addr << 1 | READ_BIT, ACK_CHECK_EN);
    i2c_master_read(cmd, data, data_len, LAST_NACK_VAL);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(i2c_num, cmd, 10 / portTICK_RATE_MS);
    i2c_cmd_link_delete(cmd);

    return ret;
}

void BME280_delay_msek(u32 msek)
{
	vTaskDelay(msek / portTICK_RATE_MS);
}


esp_err_t BME280_I2C_init(struct bme280_t *bme280, const int sda_pin, const int scl_pin)
{
  ESP_ERROR_CHECK(i2c_master_init(sda_pin, scl_pin));

	s32 com_rslt = 0;
	com_rslt = bme280_init(bme280);
	com_rslt += bme280_set_oversamp_pressure(BME280_OVERSAMP_16X);
	com_rslt += bme280_set_oversamp_temperature(BME280_OVERSAMP_2X);
	com_rslt += bme280_set_oversamp_humidity(BME280_OVERSAMP_1X);

	com_rslt += bme280_set_standby_durn(BME280_STANDBY_TIME_1_MS);
	com_rslt += bme280_set_filter(BME280_FILTER_COEFF_16);

	com_rslt += bme280_set_power_mode(BME280_NORMAL_MODE);
  if (com_rslt) {
    return ESP_FAIL;
  }
  return ESP_OK;
}




esp_err_t bme_read_data(int32_t *temperature, int32_t *pressure, int32_t *humidity)
{
  s32 v_uncomp_pressure_s32;
  s32 v_uncomp_temperature_s32;
  s32 v_uncomp_humidity_s32;
  s32 com_rslt;
  com_rslt = bme280_read_uncomp_pressure_temperature_humidity(
	  &v_uncomp_pressure_s32, &v_uncomp_temperature_s32, &v_uncomp_humidity_s32);
  
  if (com_rslt == BME280_SUCCESS) {
    ESP_LOGI(TAG, "%d.%02d degC / %d hPa / %d.%03d %%",
             bme280_compensate_temperature_int32(v_uncomp_temperature_s32)/100,
             bme280_compensate_temperature_int32(v_uncomp_temperature_s32)%100,
             bme280_compensate_pressure_int32(v_uncomp_pressure_s32)/100, // Pa -> hPa
             bme280_compensate_humidity_int32(v_uncomp_humidity_s32)/1000,
             bme280_compensate_humidity_int32(v_uncomp_humidity_s32)%1000);
    *temperature = bme280_compensate_temperature_int32(v_uncomp_temperature_s32);
    *pressure = bme280_compensate_pressure_int32(v_uncomp_pressure_s32)/100;
    *humidity = bme280_compensate_humidity_int32(v_uncomp_humidity_s32);
    return ESP_OK;
  } else {
    ESP_LOGE(TAG, "measure error. code: %d", com_rslt);
  }
  return ESP_FAIL;
}

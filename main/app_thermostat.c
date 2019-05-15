#include "esp_system.h"
#ifdef CONFIG_MQTT_THERMOSTAT

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#include <string.h>

#include "app_main.h"
#include "app_relay.h"
#include "app_thermostat.h"
#include "app_nvs.h"

bool thermostatEnabled = false;
bool heatingEnabled = false;
int targetTemperature=23*10; //30 degrees
int targetTemperatureSensibility=5; //0.5 degrees

const char * targetTemperatureTAG="targetTemp";
const char * targetTemperatureSensibilityTAG="tgtTempSens";

extern int32_t wtemperature;
int32_t wtemperature_1 = 0;
int32_t wtemperature_2 = 0;
extern EventGroupHandle_t mqtt_event_group;
extern const int MQTT_INIT_FINISHED_BIT;
extern const int MQTT_PUBLISHED_BIT;
extern QueueHandle_t thermostatQueue;


static const char *TAG = "APP_THERMOSTAT";

void publish_thermostat_cfg(esp_mqtt_client_handle_t client)
{
  if (xEventGroupGetBits(mqtt_event_group) & MQTT_INIT_FINISHED_BIT)
    {
      const char * connect_topic = CONFIG_MQTT_DEVICE_TYPE "/" CONFIG_MQTT_CLIENT_ID "/evt/thermostat/cfg";
      char data[256];
      memset(data,0,256);

      sprintf(data, "{\"targetTemperature\":%02f, \"targetTemperatureSensibility\":%02f}", targetTemperature/10., targetTemperatureSensibility/10.);
      xEventGroupClearBits(mqtt_event_group, MQTT_PUBLISHED_BIT);
      int msg_id = esp_mqtt_client_publish(client, connect_topic, data,strlen(data), 1, 1);
      if (msg_id > 0) {
        ESP_LOGI(TAG, "sent publish thermostat data successful, msg_id=%d", msg_id);
        EventBits_t bits = xEventGroupWaitBits(mqtt_event_group, MQTT_PUBLISHED_BIT, false, true, MQTT_FLAG_TIMEOUT);
        if (bits & MQTT_PUBLISHED_BIT) {
          ESP_LOGI(TAG, "publish ack received, msg_id=%d", msg_id);
        } else {
          ESP_LOGW(TAG, "publish ack not received, msg_id=%d", msg_id);
        }
      } else {
        ESP_LOGI(TAG, "failed to publish thermostat data, msg_id=%d", msg_id);
      }
    }
}

void publish_thermostat_state(esp_mqtt_client_handle_t client)
{
  if (xEventGroupGetBits(mqtt_event_group) & MQTT_INIT_FINISHED_BIT)
    {
      const char * connect_topic = CONFIG_MQTT_DEVICE_TYPE "/" CONFIG_MQTT_CLIENT_ID "/evt/thermostat/state";
      char data[256];
      memset(data,0,256);

      sprintf(data, "{\"thermostatState\":%d, \"heatingState\":%d}",
              thermostatEnabled, heatingEnabled);
      xEventGroupClearBits(mqtt_event_group, MQTT_PUBLISHED_BIT);
      int msg_id = esp_mqtt_client_publish(client, connect_topic, data,strlen(data), 1, 1);
      if (msg_id > 0) {
        ESP_LOGI(TAG, "sent publish thermostat state successful, msg_id=%d", msg_id);
        EventBits_t bits = xEventGroupWaitBits(mqtt_event_group, MQTT_PUBLISHED_BIT, false, true, MQTT_FLAG_TIMEOUT);
        if (bits & MQTT_PUBLISHED_BIT) {
          ESP_LOGI(TAG, "publish ack received, msg_id=%d", msg_id);
        } else {
          ESP_LOGW(TAG, "publish ack not received, msg_id=%d", msg_id);
        }
      } else {
        ESP_LOGI(TAG, "failed to publish thermostat state, msg_id=%d", msg_id);
      }
    }
}

void publish_thermostat_data(esp_mqtt_client_handle_t client)
{
  publish_thermostat_state(client);
  publish_thermostat_cfg(client);
}


void disableThermostat(esp_mqtt_client_handle_t client)
{
  thermostatEnabled=false;
  update_relay_state(CONFIG_MQTT_THERMOSTAT_RELAY_ID, 0, client);
  publish_thermostat_state(client);
  ESP_LOGI(TAG, "thermostat disabled");
}

void enableThermostat(esp_mqtt_client_handle_t client)
{
  thermostatEnabled=true;
  update_relay_state(CONFIG_MQTT_THERMOSTAT_RELAY_ID, 1, client);
  publish_thermostat_state(client);
  ESP_LOGI(TAG, "thermostat disabled");
}


void update_thermostat(esp_mqtt_client_handle_t client)
{

  ESP_LOGI(TAG, "heat state is %d", thermostatEnabled);
  ESP_LOGI(TAG, "wtemperature is %d", wtemperature);
  ESP_LOGI(TAG, "targetTemperature is %d", targetTemperature);
  ESP_LOGI(TAG, "targetTemperatureSensibility is %d", targetTemperatureSensibility);
  if ( wtemperature == 0 )
    {
      ESP_LOGI(TAG, "sensor is not reporting, no thermostat handling");
      if (thermostatEnabled==true) {
        ESP_LOGI(TAG, "stop thermostat as sensor is not reporting");
        disableThermostat(client);
      }
      return;
    }

  if (thermostatEnabled==true && wtemperature > targetTemperature + targetTemperatureSensibility)
    {
      disableThermostat(client);
    }


  if (thermostatEnabled==false && wtemperature < targetTemperature - targetTemperatureSensibility /*&& (getTime -savedTime) > toggleProtection/tick /*/)
    {
      enableThermostat(client);
    }

  if (wtemperature_2 && wtemperature_1 && wtemperature) {//three consecutive valid readings
    if (!heatingEnabled && wtemperature_2 < wtemperature_1 && wtemperature_1 < wtemperature) { //heating is enabled
      heatingEnabled = true;
      publish_thermostat_state(client);
    }

    if (heatingEnabled && wtemperature_2 >= wtemperature_1 && wtemperature_1 >= wtemperature) { //heating is disabled
      heatingEnabled = false;
      publish_thermostat_state(client);

    }
  }
  wtemperature_2 = wtemperature_1;
  wtemperature_1 = wtemperature;
}

void handle_thermostat_cmd_task(void* pvParameters)
{
  esp_mqtt_client_handle_t client = (esp_mqtt_client_handle_t) pvParameters;
  struct ThermostatMessage t;
  while(1) {
    if( xQueueReceive( thermostatQueue, &t , portMAX_DELAY) )
      {
        bool updated = false;
        if (t.targetTemperature && targetTemperature != t.targetTemperature * 10) {
          targetTemperature=t.targetTemperature*10;
          esp_err_t err = write_nvs_integer(targetTemperatureTAG, targetTemperature);
          ESP_ERROR_CHECK( err );
          updated = true;
        }
        if (t.targetTemperatureSensibility && targetTemperatureSensibility != t.targetTemperatureSensibility * 10) {
          targetTemperatureSensibility=t.targetTemperatureSensibility*10;
          esp_err_t err = write_nvs_integer(targetTemperatureSensibilityTAG, targetTemperatureSensibility);
          ESP_ERROR_CHECK( err );
          updated = true;
        }
        if (updated) {
          update_thermostat(client);
          publish_thermostat_cfg(client);
        }
      }
  }
}
#endif // CONFIG_MQTT_THERMOSTAT

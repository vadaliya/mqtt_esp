#include "app_ota.h"

/* OTA example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

extern QueueHandle_t otaQueue;

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

#include "nvs.h"
#include "nvs_flash.h"

#define EXAMPLE_SERVER_IP "sw.iot.cipex.ro"
#define EXAMPLE_SERVER_PORT "8910"
#define EXAMPLE_FILENAME "/"CONFIG_MQTT_CLIENT_ID".bin"
#define BUFFSIZE 1500
#define TEXT_BUFFSIZE 1024

static const char *TAG = "ota";
/*an ota data write buffer ready to write to the flash*/
static char ota_write_data[BUFFSIZE + 1] = { 0 };
/*an packet receive buffer*/
static char text[BUFFSIZE + 1] = { 0 };
/* an image total length*/
static int binary_file_length = 0;
/*socket id*/
static int socket_id = -1;

extern EventGroupHandle_t mqtt_event_group;
extern const int CONNECTED_BIT;
extern const int INIT_FINISHED_BIT;


/*read buffer by byte still delim ,return read bytes counts*/
static int read_until(char *buffer, char delim, int len)
{
  //  /*TODO: delim check,buffer check,further: do an buffer length limited*/
  int i = 0;
  while (buffer[i] != delim && i < len) {
    ++i;
  }
  return i + 1;
}

/* resolve a packet from http socket
 * return true if packet including \r\n\r\n that means http packet header finished,start to receive packet body
 * otherwise return false
 * */
static bool read_past_http_header(char text[], int total_len, esp_ota_handle_t update_handle)
{
  /* i means current position */
  int i = 0, i_read_len = 0;
  while (text[i] != 0 && i < total_len) {
    i_read_len = read_until(&text[i], '\n', total_len);
    // if we resolve \r\n line,we think packet header is finished
    if (i_read_len == 2) {
      int i_write_len = total_len - (i + 2);
      memset(ota_write_data, 0, BUFFSIZE);
      /*copy first http packet body to write buffer*/
      memcpy(ota_write_data, &(text[i + 2]), i_write_len);

      esp_err_t err = esp_ota_write( update_handle, (const void *)ota_write_data, i_write_len);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error: esp_ota_write failed! err=0x%x", err);
        return false;
      } else {
        ESP_LOGI(TAG, "esp_ota_write header OK");
        binary_file_length += i_write_len;
      }
      return true;
    }
    i += i_read_len;
  }
  return false;
}

int hostname_to_ip(char * hostname , char* ip)
{
	struct hostent *he;
	struct in_addr **addr_list;
	int i;
		
	if ( (he = gethostbyname( hostname ) ) == NULL)
    {
      // get the host info
      ESP_LOGI(TAG, "gethostbyname");
      return 1;
    }

	addr_list = (struct in_addr **) he->h_addr_list;
	
	for(i = 0; addr_list[i] != NULL; i++)
    {
      //Return the first one;
      strcpy(ip , inet_ntoa(*addr_list[i]) );
      return 0;
    }
	
	return 1;
}

static bool connect_to_http_server()
{
  ESP_LOGI(TAG, "Server IP: %s Server Port:%s", EXAMPLE_SERVER_IP, EXAMPLE_SERVER_PORT);

  int  http_connect_flag = -1;
  struct sockaddr_in sock_info;

  socket_id = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_id == -1) {
    ESP_LOGE(TAG, "Create socket failed!");
    return false;
  }

  char ip[16];
  hostname_to_ip(EXAMPLE_SERVER_IP, ip);
  // set connect info
  memset(&sock_info, 0, sizeof(struct sockaddr_in));
  sock_info.sin_family = AF_INET;
  sock_info.sin_addr.s_addr = inet_addr(ip);
  sock_info.sin_port = htons(atoi(EXAMPLE_SERVER_PORT));

  // connect to http server
  http_connect_flag = connect(socket_id, (struct sockaddr *)&sock_info, sizeof(sock_info));
  if (http_connect_flag == -1) {
    ESP_LOGE(TAG, "Connect to server failed! errno=%d", errno);
    close(socket_id);
    return false;
  } else {
    ESP_LOGI(TAG, "Connected to server");
    return true;
  }
  return false;
}

void handle_ota_update_task(void *pvParameters)
{

  MQTTClient* client = (MQTTClient*) pvParameters;

  esp_err_t err;
  /* update handle : set by esp_ota_begin(), must be freed via esp_ota_end() */
  esp_ota_handle_t update_handle = 0 ;
  const esp_partition_t *update_partition = NULL;

  ESP_LOGI(TAG, "Starting OTA example...");

  const esp_partition_t *configured = esp_ota_get_boot_partition();
  const esp_partition_t *running = esp_ota_get_running_partition();

  if (configured != running) {
    ESP_LOGW(TAG, "Configured OTA boot partition at offset 0x%08x, but running from offset 0x%08x",
             configured->address, running->address);
    ESP_LOGW(TAG, "(This can happen if either the OTA boot data or preferred boot image become corrupted somehow.)");
  }
  ESP_LOGI(TAG, "Running partition type %d subtype %d (offset 0x%08x)",
           running->type, running->subtype, running->address);

  /* Wait for the callback to set the CONNECTED_BIT in the
     event group.
  */
  struct OtaMessage o;
  while(1) {

    if( xQueueReceive( otaQueue, &o , portMAX_DELAY) )
      {
        ESP_LOGI(TAG, "OTA cmd received....");
        publish_ota_data(client, OTA_ONGOING);

        /*connect to http server*/
        if (connect_to_http_server()) {
          ESP_LOGI(TAG, "Connected to http server");
        } else {
          ESP_LOGE(TAG, "Connect to http server failed!");
          publish_ota_data(client, OTA_FAILED);
          continue;
        }

        /*send GET request to http server*/
        const char *GET_FORMAT =
          "GET %s HTTP/1.0\r\n"
          "Host: %s:%s\r\n"
          "User-Agent: esp-idf/1.0 esp32\r\n\r\n";

        char *http_request = NULL;
        int get_len = asprintf(&http_request, GET_FORMAT, EXAMPLE_FILENAME, EXAMPLE_SERVER_IP, EXAMPLE_SERVER_PORT);
        if (get_len < 0) {
          ESP_LOGE(TAG, "Failed to allocate memory for GET request buffer");
          close(socket_id);
          publish_ota_data(client, OTA_FAILED);
          continue;
        }
        int res = send(socket_id, http_request, get_len, 0);
        free(http_request);

        if (res < 0) {
          ESP_LOGE(TAG, "Send GET request to server failed");
          close(socket_id);
          publish_ota_data(client, OTA_FAILED);
          continue;
        } else {
          ESP_LOGI(TAG, "Send GET request to server succeeded, file: %s", EXAMPLE_FILENAME);
        }

        update_partition = esp_ota_get_next_update_partition(NULL);
        ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%x",
                 update_partition->subtype, update_partition->address);
        assert(update_partition != NULL);

        err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
        if (err != ESP_OK) {
          ESP_LOGE(TAG, "esp_ota_begin failed, error=%d", err);
          close(socket_id);
          publish_ota_data(client, OTA_FAILED);
          continue;
        }
        ESP_LOGI(TAG, "esp_ota_begin succeeded");

        bool resp_body_start = false, flag = true;
        /*deal with all receive packet*/
        while (flag) {
          memset(text, 0, TEXT_BUFFSIZE);
          memset(ota_write_data, 0, BUFFSIZE);
          int buff_len = recv(socket_id, text, TEXT_BUFFSIZE, 0);
          if (buff_len < 0) { /*receive error*/
            ESP_LOGE(TAG, "Error: receive data error! errno=%d", errno);
            close(socket_id);
            publish_ota_data(client, OTA_FAILED);
            continue;
          } else if (buff_len > 0 && !resp_body_start) { /*deal with response header*/
            memcpy(ota_write_data, text, buff_len);
            resp_body_start = read_past_http_header(text, buff_len, update_handle);
          } else if (buff_len > 0 && resp_body_start) { /*deal with response body*/
            memcpy(ota_write_data, text, buff_len);
            err = esp_ota_write( update_handle, (const void *)ota_write_data, buff_len);
            if (err != ESP_OK) {
              ESP_LOGE(TAG, "Error: esp_ota_write failed! err=0x%x", err);
              close(socket_id);
              publish_ota_data(client, OTA_FAILED);
              continue;
            }
            binary_file_length += buff_len;
            ESP_LOGI(TAG, "Have written image length %d", binary_file_length);
          } else if (buff_len == 0) {  /*packet over*/
            flag = false;
            ESP_LOGI(TAG, "Connection closed, all packets received");
            close(socket_id);
            publish_ota_data(client, OTA_FAILED);
            continue;
          } else {
            ESP_LOGE(TAG, "Unexpected recv result");
          }
        }

        ESP_LOGI(TAG, "Total Write binary data length : %d", binary_file_length);

        if (esp_ota_end(update_handle) != ESP_OK) {
          ESP_LOGE(TAG, "esp_ota_end failed!");
          close(socket_id);
          publish_ota_data(client, OTA_FAILED);
          continue;
        }
        err = esp_ota_set_boot_partition(update_partition);
        if (err != ESP_OK) {
          ESP_LOGE(TAG, "esp_ota_set_boot_partition failed! err=0x%x", err);
          close(socket_id);
          publish_ota_data(client, OTA_FAILED);
          continue;
        }
        ESP_LOGI(TAG, "Prepare to restart system in 10 seconds!");
        publish_ota_data(client, OTA_SUCCESFULL);
        vTaskDelay(10000 / portTICK_PERIOD_MS);
        esp_restart();
      }
  }
}



void publish_ota_data(MQTTClient* pclient, int status)
{
  if (xEventGroupGetBits(mqtt_event_group) & INIT_FINISHED_BIT)
    {
      const char * connect_topic = CONFIG_MQTT_DEVICE_TYPE "/" CONFIG_MQTT_CLIENT_ID "/evt/ota";
      char data[256];
      memset(data,0,256);

      sprintf(data, "{\"status\":%d}", status);

      MQTTMessage message;
      message.qos = QOS1;
      message.retained = 1;
      message.payload = data;
      message.payloadlen = strlen(data);

      int rc = MQTTPublish(pclient, connect_topic, &message);
      if (rc == 0) {
        ESP_LOGI(TAG, "sent publish ota successful, rc=%d", rc);
      } else {
        ESP_LOGI(TAG, "failed to publish ota, rc=%d", rc);
      }
    }
}

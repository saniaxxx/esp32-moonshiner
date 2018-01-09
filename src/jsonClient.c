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


#include "jsonClient.h"
#include "operationModes.h"
#include "temperatureChecker.h"
#include "config/config.h"
#include "publicQueues.h"
#include "paramsStorage.h"
#include "cJSON.h"

bool sendTempToClient()
{
    portBASE_TYPE xStatus;
    Temperature_info tempinfo;
    xStatus = xQueueReceive(Temperatures_queue, &tempinfo, 0);
    if (xStatus == pdTRUE) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "temp", tempinfo.temperature);
        xQueueSend(Json_outgoing_queue, &root, 0);
        return 1;
    }
    return 0;
}

void sendAckToClient(esp_err_t err)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "error", err);
    xQueueSend(Json_outgoing_queue, &root, 0);
}

void handleClientMessage(bool (*changeWorkingMode)(OperationMode)){
  cJSON* root = NULL;
  if (xQueueReceive(Json_incoming_queue, &root, 100 / portTICK_PERIOD_MS) == pdTRUE) {
      MessageType message_type = (MessageType)cJSON_GetObjectItem(root, "type")->valueint;
      if (message_type == MessageTypeChangeMode) {
          OperationMode mode = (OperationMode)cJSON_GetObjectItem(root, "mode")->valueint;
          if (changeWorkingMode(mode)) {
              sendAckToClient(ESP_OK);
          }else{
              sendAckToClient(ESP_FAIL);
          }
      }else if(message_type == MessageTypeGetPreParameter){
          PreParameter parameter = (PreParameter)cJSON_GetObjectItem(root, "parameter")->valueint;
          esp_err_t err;
          uint32_t value = getPreParameter(parameter, &err);
          cJSON* root = cJSON_CreateObject();
          cJSON_AddNumberToObject(root, "value", value);
          xQueueSend(Json_outgoing_queue, &root, 0);
      }else if(message_type == MessageTypeSetPreParameter){
          PreParameter parameter = (PreParameter)cJSON_GetObjectItem(root, "parameter")->valueint;
          uint32_t value = (uint32_t)cJSON_GetObjectItem(root, "value")->valueint;
          esp_err_t err;
          setPreParameter(parameter, value, &err);
          sendAckToClient(err);
      }
  }
}
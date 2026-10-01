#pragma once
#include "esp_http_server.h"

esp_err_t pedalboard_ir_upload(httpd_req_t* request);
esp_err_t pedalboard_ir_download(httpd_req_t* request);
esp_err_t pedalboard_ir_list(httpd_req_t* request);

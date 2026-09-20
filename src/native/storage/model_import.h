#pragma once
#include "esp_http_server.h"
esp_err_t coyopedal_model_import(httpd_req_t* request);
esp_err_t coyopedal_model_download(httpd_req_t* request);

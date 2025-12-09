#include "web_ui_devices.h"

#include "esp_log.h"

static const char *TAG = "web_ui_devices";

extern const uint8_t _binary_devices_wizard_css_start[] asm("_binary_devices_wizard_css_start");
extern const uint8_t _binary_devices_wizard_css_end[] asm("_binary_devices_wizard_css_end");
extern const uint8_t _binary_devices_wizard_js_start[] asm("_binary_devices_wizard_js_start");
extern const uint8_t _binary_devices_wizard_js_end[] asm("_binary_devices_wizard_js_end");
extern const uint8_t _binary_devices_editor_css_start[] asm("_binary_devices_editor_css_start");
extern const uint8_t _binary_devices_editor_css_end[] asm("_binary_devices_editor_css_end");
extern const uint8_t _binary_devices_editor_js_start[] asm("_binary_devices_editor_js_start");
extern const uint8_t _binary_devices_editor_js_end[] asm("_binary_devices_editor_js_end");

static esp_err_t devices_css_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css");
    const size_t len = _binary_devices_wizard_css_end - _binary_devices_wizard_css_start;
    const size_t send_len = (len > 0) ? len - 1 : 0;
    return httpd_resp_send(req, (const char *)_binary_devices_wizard_css_start, send_len);
}

static esp_err_t devices_js_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    const size_t len = _binary_devices_wizard_js_end - _binary_devices_wizard_js_start;
    const size_t send_len = (len > 0) ? len - 1 : 0;
    return httpd_resp_send(req, (const char *)_binary_devices_wizard_js_start, send_len);
}

static esp_err_t editor_css_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css");
    const size_t len = _binary_devices_editor_css_end - _binary_devices_editor_css_start;
    const size_t send_len = (len > 0) ? len - 1 : 0;
    return httpd_resp_send(req, (const char *)_binary_devices_editor_css_start, send_len);
}

static esp_err_t editor_js_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    const size_t len = _binary_devices_editor_js_end - _binary_devices_editor_js_start;
    const size_t send_len = (len > 0) ? len - 1 : 0;
    return httpd_resp_send(req, (const char *)_binary_devices_editor_js_start, send_len);
}

esp_err_t web_ui_devices_register_assets(httpd_handle_t server)
{
    const httpd_uri_t css_uri = {
        .uri = "/ui/devices.css",
        .method = HTTP_GET,
        .handler = devices_css_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t js_uri = {
        .uri = "/ui/devices.js",
        .method = HTTP_GET,
        .handler = devices_js_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t editor_css = {
        .uri = "/ui/devices_editor.css",
        .method = HTTP_GET,
        .handler = editor_css_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t editor_js = {
        .uri = "/ui/devices_editor.js",
        .method = HTTP_GET,
        .handler = editor_js_handler,
        .user_ctx = NULL,
    };

    esp_err_t err = httpd_register_uri_handler(server, &css_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register css failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &js_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register js failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &editor_css);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register editor css failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &editor_js);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register editor js failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

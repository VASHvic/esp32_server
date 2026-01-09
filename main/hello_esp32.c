#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "soc/rtc.h"
#include "driver/gpio.h"
#include "config.h"


static const char *TAG = "ESP32_WebServer";
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define LED_PIN        GPIO_NUM_21
static int s_retry_num = 0;
#define MAX_RETRY 5

static bool led_state = false;

void led_init(void)
{
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED_PIN),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(LED_PIN, 0);
    led_state = false;
}

void led_set_state(bool state)
{
    led_state = state;
    gpio_set_level(LED_PIN, state ? 1 : 0);
    ESP_LOGI(TAG, "LED %s", state ? "ENCENDIDO" : "APAGADO");
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Reintentando conexión WiFi...");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "Fallo al conectar al AP");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "IP asignada: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Inicialización WiFi completada.");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Conectado al AP SSID:%s", WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Fallo al conectar al SSID:%s", WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "Evento inesperado");
    }
}

float get_chip_temperature(void)
{
    // TODO
    return 45.0 + (esp_timer_get_time() % 10000000) / 1000000.0;
}

void get_system_info_json(char* buffer, size_t buffer_size)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    
    wifi_ap_record_t ap_info;
    esp_wifi_sta_get_ap_info(&ap_info);
    
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(netif, &ip_info);
    
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t min_free_heap = esp_get_minimum_free_heap_size();
    
    // Uptime
    int64_t uptime_us = esp_timer_get_time();
    int uptime_seconds = (int)(uptime_us / 1000000);
    int hours = uptime_seconds / 3600;
    int minutes = (uptime_seconds % 3600) / 60;
    int seconds = uptime_seconds % 60;
    
    float temperature = get_chip_temperature();
    
    rtc_cpu_freq_config_t freq_config;
    rtc_clk_cpu_freq_get_config(&freq_config);
    uint32_t cpu_freq_mhz = freq_config.freq_mhz;
    
    snprintf(buffer, buffer_size,
        "{\n"
        "  \"chip\": {\n"
        "    \"model\": \"ESP32\",\n"
        "    \"cores\": %d,\n"
        "    \"revision\": %d,\n"
        "    \"frequency\": %lu\n"
        "  },\n"
        "  \"temperature\": %.2f,\n"
        "  \"wifi\": {\n"
        "    \"ssid\": \"%s\",\n"
        "    \"rssi\": %d,\n"
        "    \"ip\": \"" IPSTR "\",\n"
        "    \"gateway\": \"" IPSTR "\",\n"
        "    \"netmask\": \"" IPSTR "\"\n"
        "  },\n"
        "  \"memory\": {\n"
        "    \"free_heap\": %lu,\n"
        "    \"min_free_heap\": %lu,\n"
        "    \"free_heap_mb\": %.2f\n"
        "  },\n"
        "  \"uptime\": {\n"
        "    \"seconds\": %d,\n"
        "    \"formatted\": \"%02d:%02d:%02d\"\n"
        "  },\n"
        "  \"led\": {\n"
        "    \"state\": %s\n"
        "  }\n"
        "}",
        chip_info.cores,
        chip_info.revision,
        cpu_freq_mhz,
        temperature,
        (char*)ap_info.ssid,
        ap_info.rssi,
        IP2STR(&ip_info.ip),
        IP2STR(&ip_info.gw),
        IP2STR(&ip_info.netmask),
        (unsigned long)free_heap,
        (unsigned long)min_free_heap,
        free_heap / (1024.0 * 1024.0),
        uptime_seconds,
        hours, minutes, seconds,
        led_state ? "true" : "false"
    );
}

static const char* html_page = 
"<!DOCTYPE html>\n"
"<html lang='es'>\n"
"<head>\n"
"    <meta charset='UTF-8'>\n"
"    <meta name='viewport' content='width=device-width, initial-scale=1.0'>\n"
"    <title>ESP32 Monitor</title>\n"
"    <style>\n"
"        * { margin: 0; padding: 0; box-sizing: border-box; }\n"
"        body {\n"
"            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;\n"
"            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);\n"
"            min-height: 100vh;\n"
"            padding: 20px;\n"
"            color: #333;\n"
"        }\n"
"        .container {\n"
"            max-width: 1200px;\n"
"            margin: 0 auto;\n"
"        }\n"
"        h1 {\n"
"            text-align: center;\n"
"            color: white;\n"
"            margin-bottom: 30px;\n"
"            font-size: 2.5em;\n"
"            text-shadow: 2px 2px 4px rgba(0,0,0,0.3);\n"
"        }\n"
"        .status {\n"
"            text-align: center;\n"
"            color: #fff;\n"
"            margin-bottom: 20px;\n"
"            font-size: 1.1em;\n"
"        }\n"
"        .status.connected { color: #4ade80; }\n"
"        .status.error { color: #f87171; }\n"
"        .grid {\n"
"            display: grid;\n"
"            grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));\n"
"            gap: 20px;\n"
"            margin-bottom: 20px;\n"
"        }\n"
"        .card {\n"
"            background: rgba(255, 255, 255, 0.95);\n"
"            border-radius: 15px;\n"
"            padding: 25px;\n"
"            box-shadow: 0 8px 32px rgba(0,0,0,0.1);\n"
"            backdrop-filter: blur(10px);\n"
"            transition: transform 0.3s ease, box-shadow 0.3s ease;\n"
"        }\n"
"        .card:hover {\n"
"            transform: translateY(-5px);\n"
"            box-shadow: 0 12px 48px rgba(0,0,0,0.15);\n"
"        }\n"
"        .card-title {\n"
"            font-size: 1.3em;\n"
"            font-weight: 600;\n"
"            margin-bottom: 15px;\n"
"            color: #667eea;\n"
"            border-bottom: 2px solid #667eea;\n"
"            padding-bottom: 10px;\n"
"        }\n"
"        .metric {\n"
"            display: flex;\n"
"            justify-content: space-between;\n"
"            padding: 10px 0;\n"
"            border-bottom: 1px solid #e5e7eb;\n"
"        }\n"
"        .metric:last-child { border-bottom: none; }\n"
"        .metric-label {\n"
"            font-weight: 500;\n"
"            color: #6b7280;\n"
"        }\n"
"        .metric-value {\n"
"            font-weight: 600;\n"
"            color: #1f2937;\n"
"        }\n"
"        .temp-value {\n"
"            font-size: 2em;\n"
"            text-align: center;\n"
"            color: #f59e0b;\n"
"            margin: 10px 0;\n"
"        }\n"
"        .btn {\n"
"            display: block;\n"
"            width: 100%;\n"
"            padding: 15px;\n"
"            color: white;\n"
"            border: none;\n"
"            border-radius: 10px;\n"
"            font-size: 1.1em;\n"
"            font-weight: 600;\n"
"            cursor: pointer;\n"
"            transition: all 0.3s ease;\n"
"            margin-bottom: 10px;\n"
"        }\n"
"        .btn-restart {\n"
"            background: linear-gradient(135deg, #f87171 0%, #dc2626 100%);\n"
"            box-shadow: 0 4px 15px rgba(248, 113, 113, 0.3);\n"
"        }\n"
"        .btn-led-on {\n"
"            background: linear-gradient(135deg, #4ade80 0%, #22c55e 100%);\n"
"            box-shadow: 0 4px 15px rgba(74, 222, 128, 0.3);\n"
"        }\n"
"        .btn-led-off {\n"
"            background: linear-gradient(135deg, #94a3b8 0%, #64748b 100%);\n"
"            box-shadow: 0 4px 15px rgba(148, 163, 184, 0.3);\n"
"        }\n"
"        .btn:hover {\n"
"            transform: translateY(-2px);\n"
"        }\n"
"        .btn:active {\n"
"            transform: translateY(0);\n"
"        }\n"
"        .led-status {\n"
"            display: flex;\n"
"            align-items: center;\n"
"            justify-content: center;\n"
"            margin: 15px 0;\n"
"            font-size: 1.2em;\n"
"        }\n"
"        .led-indicator {\n"
"            width: 20px;\n"
"            height: 20px;\n"
"            border-radius: 50%;\n"
"            margin-left: 10px;\n"
"            transition: all 0.3s ease;\n"
"        }\n"
"        .led-indicator.on {\n"
"            background-color: #22c55e;\n"
"            box-shadow: 0 0 20px #22c55e;\n"
"        }\n"
"        .led-indicator.off {\n"
"            background-color: #64748b;\n"
"            box-shadow: 0 0 5px #64748b;\n"
"        }\n"
"        @media (max-width: 768px) {\n"
"            h1 { font-size: 1.8em; }\n"
"            .grid { grid-template-columns: 1fr; }\n"
"        }\n"
"    </style>\n"
"</head>\n"
"<body>\n"
"    <div class='container'>\n"
"        <h1>🔧 ESP32 Monitor de Sistema</h1>\n"
"        <div class='status' id='status'>🔄 Cargando datos...</div>\n"
"        \n"
"        <div class='grid'>\n"
"            <div class='card'>\n"
"                <div class='card-title'>🌡️ Temperatura</div>\n"
"                <div class='temp-value' id='temp'>--°C</div>\n"
"            </div>\n"
"            \n"
"            <div class='card'>\n"
"                <div class='card-title'>📡 WiFi</div>\n"
"                <div class='metric'>\n"
"                    <span class='metric-label'>SSID:</span>\n"
"                    <span class='metric-value' id='ssid'>--</span>\n"
"                </div>\n"
"                <div class='metric'>\n"
"                    <span class='metric-label'>IP:</span>\n"
"                    <span class='metric-value' id='ip'>--</span>\n"
"                </div>\n"
"                <div class='metric'>\n"
"                    <span class='metric-label'>RSSI:</span>\n"
"                    <span class='metric-value' id='rssi'>--</span>\n"
"                </div>\n"
"                <div class='metric'>\n"
"                    <span class='metric-label'>Gateway:</span>\n"
"                    <span class='metric-value' id='gateway'>--</span>\n"
"                </div>\n"
"            </div>\n"
"            \n"
"            <div class='card'>\n"
"                <div class='card-title'>💾 Memoria</div>\n"
"                <div class='metric'>\n"
"                    <span class='metric-label'>Heap Libre:</span>\n"
"                    <span class='metric-value' id='heap'>--</span>\n"
"                </div>\n"
"                <div class='metric'>\n"
"                    <span class='metric-label'>Heap Mínimo:</span>\n"
"                    <span class='metric-value' id='minheap'>--</span>\n"
"                </div>\n"
"            </div>\n"
"            \n"
"            <div class='card'>\n"
"                <div class='card-title'>⏱️ Uptime</div>\n"
"                <div class='temp-value' id='uptime'>00:00:00</div>\n"
"            </div>\n"
"            \n"
"            <div class='card'>\n"
"                <div class='card-title'>🔌 Chip Info</div>\n"
"                <div class='metric'>\n"
"                    <span class='metric-label'>Modelo:</span>\n"
"                    <span class='metric-value' id='model'>--</span>\n"
"                </div>\n"
"                <div class='metric'>\n"
"                    <span class='metric-label'>Núcleos:</span>\n"
"                    <span class='metric-value' id='cores'>--</span>\n"
"                </div>\n"
"                <div class='metric'>\n"
"                    <span class='metric-label'>Frecuencia:</span>\n"
"                    <span class='metric-value' id='freq'>--</span>\n"
"                </div>\n"
"                <div class='metric'>\n"
"                    <span class='metric-label'>Revisión:</span>\n"
"                    <span class='metric-value' id='revision'>--</span>\n"
"                </div>\n"
"            </div>\n"
"            \n"
"            <div class='card'>\n"
"                <div class='card-title'>💡 Control LED (Pin 21)</div>\n"
"                <div class='led-status'>\n"
"                    <span>Estado:</span>\n"
"                    <div class='led-indicator off' id='led-indicator'></div>\n"
"                </div>\n"
"                <button class='btn btn-led-on' onclick='toggleLED(true)'>🔆 Encender LED</button>\n"
"                <button class='btn btn-led-off' onclick='toggleLED(false)'>🔅 Apagar LED</button>\n"
"            </div>\n"
"            \n"
"            <div class='card'>\n"
"                <div class='card-title'>⚡ Control</div>\n"
"                <button class='btn btn-restart' onclick='restartESP()'>Reiniciar ESP32</button>\n"
"            </div>\n"
"        </div>\n"
"    </div>\n"
"    \n"
"    <script>\n"
"        async function fetchData() {\n"
"            try {\n"
"                const response = await fetch('/api/data');\n"
"                const data = await response.json();\n"
"                \n"
"                document.getElementById('status').textContent = '✅ Conectado';\n"
"                document.getElementById('status').className = 'status connected';\n"
"                \n"
"                document.getElementById('temp').textContent = data.temperature.toFixed(1) + '°C';\n"
"                document.getElementById('ssid').textContent = data.wifi.ssid;\n"
"                document.getElementById('ip').textContent = data.wifi.ip;\n"
"                document.getElementById('rssi').textContent = data.wifi.rssi + ' dBm';\n"
"                document.getElementById('gateway').textContent = data.wifi.gateway;\n"
"                document.getElementById('heap').textContent = (data.memory.free_heap / 1024).toFixed(2) + ' KB';\n"
"                document.getElementById('minheap').textContent = (data.memory.min_free_heap / 1024).toFixed(2) + ' KB';\n"
"                document.getElementById('uptime').textContent = data.uptime.formatted;\n"
"                document.getElementById('model').textContent = data.chip.model;\n"
"                document.getElementById('cores').textContent = data.chip.cores;\n"
"                document.getElementById('freq').textContent = data.chip.frequency + ' MHz';\n"
"                document.getElementById('revision').textContent = data.chip.revision;\n"
"                \n"
"                // Actualizar estado del LED\n"
"                const ledIndicator = document.getElementById('led-indicator');\n"
"                if (data.led.state) {\n"
"                    ledIndicator.className = 'led-indicator on';\n"
"                } else {\n"
"                    ledIndicator.className = 'led-indicator off';\n"
"                }\n"
"            } catch (error) {\n"
"                document.getElementById('status').textContent = '❌ Error de conexión';\n"
"                document.getElementById('status').className = 'status error';\n"
"                console.error('Error:', error);\n"
"            }\n"
"        }\n"
"        \n"
"        async function toggleLED(state) {\n"
"            try {\n"
"                const response = await fetch('/api/led', {\n"
"                    method: 'POST',\n"
"                    headers: {\n"
"                        'Content-Type': 'application/json'\n"
"                    },\n"
"                    body: JSON.stringify({ state: state })\n"
"                });\n"
"                \n"
"                if (response.ok) {\n"
"                    // Actualizar inmediatamente la UI\n"
"                    const ledIndicator = document.getElementById('led-indicator');\n"
"                    if (state) {\n"
"                        ledIndicator.className = 'led-indicator on';\n"
"                    } else {\n"
"                        ledIndicator.className = 'led-indicator off';\n"
"                    }\n"
"                    // Actualizar todos los datos\n"
"                    fetchData();\n"
"                }\n"
"            } catch (error) {\n"
"                console.error('Error al controlar LED:', error);\n"
"            }\n"
"        }\n"
"        \n"
"        async function restartESP() {\n"
"            if (confirm('¿Estás seguro de que quieres reiniciar el ESP32?')) {\n"
"                try {\n"
"                    await fetch('/api/restart', { method: 'POST' });\n"
"                    document.getElementById('status').textContent = '🔄 Reiniciando...';\n"
"                    document.getElementById('status').className = 'status';\n"
"                } catch (error) {\n"
"                    console.error('Error al reiniciar:', error);\n"
"                }\n"
"            }\n"
"        }\n"
"        \n"
"        // Cargar datos inicialmente\n"
"        fetchData();\n"
"        \n"
"        // Actualizar cada 5 segundos\n"
"        setInterval(fetchData, 5000);\n"
"    </script>\n"
"</body>\n"
"</html>";

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page, strlen(html_page));
    return ESP_OK;
}

static esp_err_t data_handler(httpd_req_t *req)
{
    char json_buffer[1024];
    get_system_info_json(json_buffer, sizeof(json_buffer));
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_buffer, strlen(json_buffer));
    return ESP_OK;
}

static esp_err_t led_handler(httpd_req_t *req)
{
    char content[100];
    int ret = httpd_req_recv(req, content, sizeof(content));
    
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    
    content[ret] = '\0';
    
    bool new_state = false;
    if (strstr(content, "\"state\":true") != NULL || strstr(content, "\"state\": true") != NULL) {
        new_state = true;
    }
    
    led_set_state(new_state);
    
    char response[50];
    snprintf(response, sizeof(response), "{\"status\":\"ok\",\"led_state\":%s}", new_state ? "true" : "false");
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

static esp_err_t restart_handler(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "{\"status\":\"restarting\"}");
    
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    esp_restart();
    
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Iniciando servidor HTTP en puerto: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = root_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &root);

        httpd_uri_t data = {
            .uri       = "/api/data",
            .method    = HTTP_GET,
            .handler   = data_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &data);

        httpd_uri_t led = {
            .uri       = "/api/led",
            .method    = HTTP_POST,
            .handler   = led_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &led);

        httpd_uri_t restart = {
            .uri       = "/api/restart",
            .method    = HTTP_POST,
            .handler   = restart_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &restart);

        return server;
    }

    ESP_LOGI(TAG, "Error al iniciar servidor HTTP");
    return NULL;
}

void app_main(void)
{
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "  ESP32 Web Server - Monitor de Sistema");
    ESP_LOGI(TAG, "===========================================");

    ESP_LOGI(TAG, "Inicializando LED en pin 21...");
    led_init();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Inicializando WiFi...");
    wifi_init_sta();

    ESP_LOGI(TAG, "Iniciando servidor web...");
    httpd_handle_t server = start_webserver();
    
    if (server != NULL) {
        ESP_LOGI(TAG, "===========================================");
        ESP_LOGI(TAG, "  Servidor web iniciado correctamente");
        ESP_LOGI(TAG, "  Accede desde tu navegador a la IP mostrada");
        ESP_LOGI(TAG, "===========================================");
    } else {
        ESP_LOGI(TAG, "Error: No se pudo iniciar el servidor web");
    }

    while (1) {
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
}
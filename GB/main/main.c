#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "rom/ets_sys.h"

/* Pinos */
#define DHT11_PIN       4
#define LED_RED_PIN     2
#define BTN1_PIN        19
#define BTN2_PIN        21

#define BTN_SILENCIA_MS 30000

static const char *TAG = "MONITOR";

/* Limites de alerta — alteráveis em runtime via terminal (task_console) */
static volatile float g_temp_limite = 25.0f;
static volatile float g_umid_limite = 80.0f;

/* Estrutura de dados do sensor */
typedef struct {
    float temperature;
    float humidity;
    bool  valida;
} sensor_data_t;

/* Handles globais */
static QueueHandle_t fila_leds;
static QueueHandle_t fila_http;
static TaskHandle_t  xSensorTask;

/* --- Driver DHT11 ---
   Protocolo single-wire: 40 bits = umidade (2B) + temperatura (2B) + checksum (1B).
   Retorno: 0 = OK, -1 = timeout, -2 = checksum inválido */
static int dht11_read(sensor_data_t *out)
{
    uint8_t data[5] = {0};
    int timeout;

    gpio_set_direction(DHT11_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(DHT11_PIN, 0);
    vTaskDelay(20 / portTICK_PERIOD_MS);
    gpio_set_level(DHT11_PIN, 1);
    ets_delay_us(30);
    gpio_set_direction(DHT11_PIN, GPIO_MODE_INPUT);

    timeout = 0;
    while (gpio_get_level(DHT11_PIN) == 1) { if (++timeout > 100) return -1; ets_delay_us(1); }
    timeout = 0;
    while (gpio_get_level(DHT11_PIN) == 0) { if (++timeout > 100) return -1; ets_delay_us(1); }
    timeout = 0;
    while (gpio_get_level(DHT11_PIN) == 1) { if (++timeout > 100) return -1; ets_delay_us(1); }

    for (int i = 0; i < 40; i++) {
        timeout = 0;
        while (gpio_get_level(DHT11_PIN) == 0) { if (++timeout > 60) return -1; ets_delay_us(1); }
        ets_delay_us(35);
        if (gpio_get_level(DHT11_PIN) == 1) data[i / 8] |= (1 << (7 - (i % 8)));
        timeout = 0;
        while (gpio_get_level(DHT11_PIN) == 1) { if (++timeout > 80) return -1; ets_delay_us(1); }
    }

    if (data[4] != ((data[0] + data[1] + data[2] + data[3]) & 0xFF)) return -2;

    out->humidity    = (float)data[0] + data[1] * 0.1f;
    out->temperature = (float)data[2] + data[3] * 0.1f;

    return 0;
}

/* --- task_sensor ---
   Lê o DHT11 a cada 3s e distribui para fila_leds e fila_http.
   O BTN2 pode acordar antes dos 3s via xTaskNotifyGive. */
static void task_sensor(void *arg)
{
    while (1) {
        sensor_data_t leitura = { .valida = false };

        int ret = dht11_read(&leitura);

        if (ret == 0) {
            leitura.valida = true;
            ESP_LOGI(TAG, "Temperatura: %.1f C  |  Umidade: %.1f %%",
                     leitura.temperature, leitura.humidity);
        } else {
            ESP_LOGW(TAG, "Falha na leitura (erro %d)", ret);
        }

        xQueueSend(fila_leds, &leitura, 0);
        xQueueSend(fila_http, &leitura, 0);

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000));
    }
}

/* --- task_leds_botoes ---
   Gerencia o LED e os botões. Espera até 50ms por dado novo para checar botões. */
static void task_leds_botoes(void *arg)
{
    TickType_t silenciar_ate = 0;
    int btn1_prev = 1, btn2_prev = 1;

    while (1) {
        sensor_data_t leitura;
        bool tem_dado = (xQueueReceive(fila_leds, &leitura, pdMS_TO_TICKS(50)) == pdTRUE);

        TickType_t agora = xTaskGetTickCount() * portTICK_PERIOD_MS;

        /* BTN1: silencia o LED por 30s */
        int btn1 = gpio_get_level(BTN1_PIN);
        if (btn1 == 0 && btn1_prev == 1) {
            silenciar_ate = agora + BTN_SILENCIA_MS;
            gpio_set_level(LED_RED_PIN, 0);
            ESP_LOGI(TAG, "Alerta silenciado por %d segundos", BTN_SILENCIA_MS / 1000);
        }
        btn1_prev = btn1;

        /* BTN2: força leitura imediata */
        int btn2 = gpio_get_level(BTN2_PIN);
        if (btn2 == 0 && btn2_prev == 1) {
            ESP_LOGI(TAG, "Leitura forcada pelo botao");
            xTaskNotifyGive(xSensorTask);
        }
        btn2_prev = btn2;

        if (tem_dado && leitura.valida) {
            bool temp_alta  = (leitura.temperature >= g_temp_limite);
            bool umid_alta  = (leitura.humidity    >= g_umid_limite);
            bool em_alerta  = temp_alta || umid_alta;
            bool silenciado = (agora < silenciar_ate);

            gpio_set_level(LED_RED_PIN, (em_alerta && !silenciado) ? 1 : 0);

            if (em_alerta && !silenciado) {
                if (temp_alta)
                    ESP_LOGW(TAG, "ALERTA — Temperatura: %.1f C (limite: %.1f C)",
                             leitura.temperature, g_temp_limite);
                if (umid_alta)
                    ESP_LOGW(TAG, "ALERTA — Umidade: %.1f %% (limite: %.1f %%)",
                             leitura.humidity, g_umid_limite);
            }
        }
    }
}

/* --- task_console ---
   Comandos via monitor serial: "temp <val>", "umid <val>", "limites" */
static void task_console(void *arg)
{
    char linha[64];
    int  idx = 0;

    ESP_LOGI(TAG, "Console pronto. Comandos: 'temp <val>', 'umid <val>', 'limites'");

    while (1) {
        int c = getchar();

        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (c == '\n' || c == '\r') {
            if (idx == 0) continue;

            linha[idx] = '\0';
            idx = 0;

            float val;

            if (sscanf(linha, "temp %f", &val) == 1) {
                g_temp_limite = val;
                ESP_LOGI(TAG, "Limite de temperatura atualizado para %.1f C", g_temp_limite);

            } else if (sscanf(linha, "umid %f", &val) == 1) {
                g_umid_limite = val;
                ESP_LOGI(TAG, "Limite de umidade atualizado para %.1f %%", g_umid_limite);

            } else if (linha[0] == 'l') {
                ESP_LOGI(TAG, "Limites atuais — temp: %.1f C  |  umid: %.1f %%",
                         g_temp_limite, g_umid_limite);

            } else {
                ESP_LOGW(TAG, "Comando desconhecido. Use: 'temp <val>', 'umid <val>', 'limites'");
            }

            continue;
        }

        if (idx < (int)sizeof(linha) - 1) {
            linha[idx++] = (char)c;
        }
    }
}

/* --- task_http ---
   Stub: aguarda dados na fila_http. Colega implementa o envio aqui.
   Dados disponíveis: leitura.temperature, leitura.humidity */
static void task_http(void *arg)
{
    sensor_data_t leitura;

    while (1) {
        if (xQueueReceive(fila_http, &leitura, portMAX_DELAY) != pdTRUE) continue;
        if (!leitura.valida) continue;

        /* TODO: colega implementa o envio HTTP aqui */
    }
}

void app_main(void)
{
    gpio_set_pull_mode(DHT11_PIN, GPIO_PULLUP_ONLY);

    gpio_set_direction(LED_RED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_RED_PIN, 0);

    gpio_set_direction(BTN1_PIN, GPIO_MODE_INPUT);
    gpio_set_direction(BTN2_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN1_PIN, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(BTN2_PIN, GPIO_PULLUP_ONLY);

    ESP_LOGI(TAG, "Monitor iniciado — sensor:GPIO%d  LED_R:GPIO%d  BTN1:GPIO%d  BTN2:GPIO%d",
             DHT11_PIN, LED_RED_PIN, BTN1_PIN, BTN2_PIN);

    fila_leds = xQueueCreate(5, sizeof(sensor_data_t));
    fila_http = xQueueCreate(5, sizeof(sensor_data_t));

    xTaskCreate(task_sensor,      "sensor",      2048, NULL, 2, &xSensorTask);
    xTaskCreate(task_leds_botoes, "leds_botoes", 2048, NULL, 3, NULL);
    xTaskCreate(task_console,     "console",     3072, NULL, 1, NULL);
    xTaskCreate(task_http,        "http",        4096, NULL, 1, NULL);
}

/* ============================================================================
 *  BIBLIOTECAS
 * ============================================================================ */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"          /* xQueueCreate / xQueueSend / xQueueReceive */
#include "driver/gpio.h"
#include "esp_log.h"
#include "rom/ets_sys.h"

/* ============================================================================
 *  CONFIGURAÇÃO DE PINOS E LIMITES DE ALERTA
 * ============================================================================ */

#define DHT22_PIN       4
#define LED_RED_PIN     2
#define LED_GREEN_PIN   5
#define BTN1_PIN        19
#define BTN2_PIN        21

#define TEMP_LIMITE     23.8f
#define BTN_SILENCIA_MS 30000

static const char *TAG = "MONITOR";

/* ============================================================================
 *  ESTRUTURA DE DADOS DO SENSOR
 * ============================================================================ */
typedef struct {
    float temperature;
    float humidity;
    bool  valida;   /* false = falha na leitura, HTTP/LEDs devem ignorar */
} sensor_data_t;

/* ============================================================================
 *  HANDLES GLOBAIS
 *  Filas que fazem o papel de demux: uma leitura do sensor chega às duas tasks.
 * ============================================================================ */
static QueueHandle_t fila_leds;   /* sensor → task_leds_botoes */
static QueueHandle_t fila_http;   /* sensor → task_http         */
static TaskHandle_t  xSensorTask; /* usado pelo BTN2 para acordar a task_sensor */

/* ============================================================================
 *  DRIVER DHT11
 *
 *  Protocolo single-wire: 40 bits = umidade (2B) + temperatura (2B) + checksum (1B).
 *  Retorno:  0 = OK,  -1 = timeout,  -2 = checksum inválido
 * ============================================================================ */
static int dht11_read(sensor_data_t *out)
{
    uint8_t data[5] = {0};
    int timeout;

    /* Sinal de início: ESP32 puxa LOW por 20ms, depois solta */
    gpio_set_direction(DHT22_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(DHT22_PIN, 0);
    vTaskDelay(20 / portTICK_PERIOD_MS);
    gpio_set_level(DHT22_PIN, 1);
    ets_delay_us(30);
    gpio_set_direction(DHT22_PIN, GPIO_MODE_INPUT);

    /* Resposta do sensor: LOW 80µs → HIGH 80µs */
    timeout = 0;
    while (gpio_get_level(DHT22_PIN) == 1) { if (++timeout > 100) return -1; ets_delay_us(1); }
    timeout = 0;
    while (gpio_get_level(DHT22_PIN) == 0) { if (++timeout > 100) return -1; ets_delay_us(1); }
    timeout = 0;
    while (gpio_get_level(DHT22_PIN) == 1) { if (++timeout > 100) return -1; ets_delay_us(1); }

    /* Leitura dos 40 bits: amostra em 35µs após a borda de subida */
    for (int i = 0; i < 40; i++) {
        timeout = 0;
        while (gpio_get_level(DHT22_PIN) == 0) { if (++timeout > 60) return -1; ets_delay_us(1); }
        ets_delay_us(35);
        if (gpio_get_level(DHT22_PIN) == 1) data[i / 8] |= (1 << (7 - (i % 8)));
        timeout = 0;
        while (gpio_get_level(DHT22_PIN) == 1) { if (++timeout > 80) return -1; ets_delay_us(1); }
    }

    if (data[4] != ((data[0] + data[1] + data[2] + data[3]) & 0xFF)) return -2;

    out->humidity    = (float)data[0] + data[1] * 0.1f;
    out->temperature = (float)data[2] + data[3] * 0.1f;

    return 0;
}

/* ============================================================================
 *  TASK: SENSOR
 *
 *  Lê o DHT11 a cada 3 segundos e distribui o resultado para as duas filas
 *  (demux). O BTN2 pode acordar esta task antes dos 3s via xTaskNotifyGive.
 * ============================================================================ */
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

        /* Demux: envia a mesma leitura para cada consumidor */
        xQueueSend(fila_leds, &leitura, 0);
        xQueueSend(fila_http, &leitura, 0);

        /* Aguarda 3s OU sinal do BTN2 para leitura imediata */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000));
    }
}

/* ============================================================================
 *  TASK: LEDs E BOTÕES
 *
 *  Gerencia os LEDs com base nos dados recebidos da fila e monitora os botões.
 *  Espera até 50ms por dado novo para poder checar os botões com frequência.
 * ============================================================================ */
static void task_leds_botoes(void *arg)
{
    TickType_t silenciar_ate = 0;
    int btn1_prev = 1, btn2_prev = 1;

    while (1) {
        sensor_data_t leitura;
        bool tem_dado = (xQueueReceive(fila_leds, &leitura, pdMS_TO_TICKS(50)) == pdTRUE);

        TickType_t agora = xTaskGetTickCount() * portTICK_PERIOD_MS;

        /* BTN1: silencia o LED vermelho por 30s */
        int btn1 = gpio_get_level(BTN1_PIN);
        if (btn1 == 0 && btn1_prev == 1) {
            silenciar_ate = agora + BTN_SILENCIA_MS;
            gpio_set_level(LED_RED_PIN, 0);
            ESP_LOGI(TAG, "Alerta silenciado por %d segundos", BTN_SILENCIA_MS / 1000);
        }
        btn1_prev = btn1;

        /* BTN2: acorda a task_sensor para leitura imediata */
        int btn2 = gpio_get_level(BTN2_PIN);
        if (btn2 == 0 && btn2_prev == 1) {
            ESP_LOGI(TAG, "Leitura forcada pelo botao");
            xTaskNotifyGive(xSensorTask);
        }
        btn2_prev = btn2;

        /* Atualiza LEDs somente quando chegou dado novo */
        if (tem_dado) {
            if (leitura.valida) {
                gpio_set_level(LED_GREEN_PIN, 1);

                bool em_alerta  = (leitura.temperature >= TEMP_LIMITE);
                bool silenciado = (agora < silenciar_ate);

                gpio_set_level(LED_RED_PIN, (em_alerta && !silenciado) ? 1 : 0);

                if (em_alerta && !silenciado) {
                    ESP_LOGW(TAG, "ALERTA — Temperatura: %.1f C acima do limite de %.1f C",
                             leitura.temperature, TEMP_LIMITE);
                }
            } else {
                gpio_set_level(LED_GREEN_PIN, 0);
            }
        }
    }
}

/* ============================================================================
 *  TASK: HTTP  (stub — a ser implementado pelo colega)
 *
 *  Fica bloqueada esperando dados na fila_http. Quando chega uma leitura
 *  válida, envia para o servidor via HTTP.
 *
 *  O colega só precisa substituir o ESP_LOGI pelo código de envio HTTP —
 *  a fila, o Wi-Fi init e a estrutura da task já estão prontos.
 * ============================================================================ */
static void task_http(void *arg)
{
    sensor_data_t leitura;

    while (1) {
        if (xQueueReceive(fila_http, &leitura, portMAX_DELAY) != pdTRUE) continue;
        if (!leitura.valida) continue;

        /* TODO: colega implementa o envio HTTP aqui.
           Dados disponíveis: leitura.temperature, leitura.humidity */
        ESP_LOGI(TAG, "[HTTP] temp=%.1f C  hum=%.1f %% — envio pendente",
                 leitura.temperature, leitura.humidity);
    }
}

/* ============================================================================
 *  PONTO DE ENTRADA
 * ============================================================================ */
void app_main(void)
{
    /* Configuração dos pinos */
    gpio_set_pull_mode(DHT22_PIN, GPIO_PULLUP_ONLY);

    gpio_set_direction(LED_RED_PIN,   GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_GREEN_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_RED_PIN,   0);
    gpio_set_level(LED_GREEN_PIN, 0);

    gpio_set_direction(BTN1_PIN, GPIO_MODE_INPUT);
    gpio_set_direction(BTN2_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN1_PIN, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(BTN2_PIN, GPIO_PULLUP_ONLY);

    ESP_LOGI(TAG, "Monitor iniciado — sensor:GPIO%d  LED_R:GPIO%d  LED_G:GPIO%d  BTN1:GPIO%d  BTN2:GPIO%d",
             DHT22_PIN, LED_RED_PIN, LED_GREEN_PIN, BTN1_PIN, BTN2_PIN);

    /* Cria as filas do demux (capacidade: 5 leituras cada) */
    fila_leds = xQueueCreate(5, sizeof(sensor_data_t));
    fila_http = xQueueCreate(5, sizeof(sensor_data_t));

    /* Cria as tasks
       Prioridade 3 → leds_botoes (mais urgente: resposta aos botões)
       Prioridade 2 → sensor
       Prioridade 1 → http (pode aguardar) */
    xTaskCreate(task_sensor,      "sensor",      2048, NULL, 2, &xSensorTask);
    xTaskCreate(task_leds_botoes, "leds_botoes", 2048, NULL, 3, NULL);
    xTaskCreate(task_http,        "http",        4096, NULL, 1, NULL);
}

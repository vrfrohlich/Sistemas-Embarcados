#include "http_client.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"  // grupos de eventos para sinalizar quando WiFi conectou
#include "esp_wifi.h"               // funções de WiFi do ESP-IDF
#include "esp_event.h"              // sistema de eventos (WiFi conectou, IP obtido, etc)
#include "esp_log.h"                // ESP_LOGI, ESP_LOGE para logs no monitor serial
#include "nvs_flash.h"              // armazenamento não-volátil, obrigatório para WiFi
#include "esp_http_client.h"        // cliente HTTP do ESP-IDF

// ── Bits usados no grupo de eventos ──────────────────────────────────────────
// O EventGroup é um "placar de flags" — cada bit representa um estado.
// Quando o WiFi conecta e pega IP, acendemos o bit CONNECTED.
// Se falhar todas as tentativas, acendemos o bit FAIL.
// O wifi_init() fica bloqueado esperando um desses dois bits acender.
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAXTENTATIVAS 5        // quantas vezes tenta reconectar antes de desistir

static const char *TAG = "HTTP";   // prefixo que aparece nos logs: I (HTTP): ...
static EventGroupHandle_t wifi_event_group; // handle do grupo de eventos
static int tentativas = 0;         // contador de tentativas de reconexão

// ── Callback de eventos WiFi ──────────────────────────────────────────────────
// Esta função é chamada automaticamente pelo sistema sempre que algo acontece
// no WiFi. O ESP-IDF usa um sistema de eventos — você registra um handler
// e ele é chamado nos momentos certos, sem você precisar ficar verificando.
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // O WiFi inicializou — tenta conectar imediatamente
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        // Desconectou — tenta reconectar até atingir o limite
        if (tentativas < WIFI_MAXTENTATIVAS) {
            esp_wifi_connect();
            tentativas++;
            ESP_LOGW(TAG, "Reconectando... (%d/%d)", tentativas, WIFI_MAXTENTATIVAS);
        } else {
            // Esgotou as tentativas — sinaliza falha para o wifi_init() desbloquear
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "Falha ao conectar no WiFi.");
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        // Conectou E pegou um IP — agora sim está pronto para usar a rede
        // Sem IP não adianta estar conectado, não dá para mandar HTTP
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "IP obtido: " IPSTR, IP2STR(&event->ip_info.ip));
        tentativas = 0; // reseta o contador caso desconecte e reconecte depois
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// ── Inicialização do WiFi ─────────────────────────────────────────────────────
// Configura e conecta o ESP32 à rede WiFi.
// Bloqueia a tarefa até conectar ou falhar — só retorna quando sabe o resultado.
bool http_wifi_init(void)
{
    // Cria o grupo de eventos que vai sinalizar conectado/falhou
    wifi_event_group = xEventGroupCreate();

    // Inicializa a pilha de rede TCP/IP (lwIP) — necessário antes de qualquer WiFi
    ESP_ERROR_CHECK(esp_netif_init());

    // Cria o loop de eventos padrão — é ele que chama o wifi_event_handler
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Cria a interface de rede no modo Station (STA = cliente WiFi, não ponto de acesso)
    esp_netif_create_default_wifi_sta();

    // Inicializa o driver WiFi com configurações padrão
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Registra o handler para eventos WiFi (conectou, desconectou, etc)
    esp_event_handler_instance_t inst_wifi, inst_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &inst_wifi));

    // Registra o handler para o evento de IP obtido (evento separado do WiFi)
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &inst_ip));

    // Define as credenciais da rede — SSID e senha declarados no http_client.h
    wifi_config_t wifi_config = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };

    // Coloca o WiFi em modo Station e aplica a configuração
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    // Liga o WiFi — isso dispara o evento STA_START, que chama esp_wifi_connect()
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Conectando ao WiFi: %s", WIFI_SSID);

    // Bloqueia aqui até um dos bits acender (CONNECTED ou FAIL)
    // portMAX_DELAY = espera para sempre, sem timeout
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi conectado!");
        return true;
    } else {
        ESP_LOGE(TAG, "Nao foi possivel conectar.");
        return false;
    }
}

// ── Envio HTTP ────────────────────────────────────────────────────────────────
// Monta um JSON com temperatura e umidade e envia via HTTP POST para o servidor.
// É chamada pela task_http a cada leitura válida do sensor.
void http_enviar(float temperatura, float umidade)
{
    // Monta o JSON no formato que o Node-RED espera
    // Ex: {"temperatura": 19.5, "umidade": 87.0}
    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"temperatura\": %.1f, \"umidade\": %.1f}",
             temperatura, umidade);

    // Configura o cliente HTTP:
    // - url: endereço do servidor Node-RED (IP do PC + porta + rota)
    // - method: POST — envia dados para o servidor (GET seria para buscar)
    esp_http_client_config_t config = {
        .url    = SERVIDOR_URL,
        .method = HTTP_METHOD_POST,
    };

    // Inicializa o cliente com a configuração acima
    esp_http_client_handle_t client = esp_http_client_init(&config);

    // Define o cabeçalho Content-Type para avisar ao servidor que o corpo é JSON
    // Sem isso o Node-RED não sabe como interpretar os dados recebidos
    esp_http_client_set_header(client, "Content-Type", "application/json");

    // Define o corpo da requisição (o JSON) e seu tamanho
    esp_http_client_set_post_field(client, payload, strlen(payload));

    // Executa a requisição: abre conexão, envia, recebe resposta, fecha
    // Esta chamada bloqueia até ter uma resposta (ou timeout)
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        // esp_http_client_get_status_code retorna o código HTTP (200, 404, etc)
        // 200 = OK, significa que o Node-RED recebeu e processou corretamente
        ESP_LOGI(TAG, "Enviado! Status: %d | %s",
                 esp_http_client_get_status_code(client), payload);
    } else {
        // esp_err_to_name converte o código de erro em texto legível
        ESP_LOGE(TAG, "Erro ao enviar: %s", esp_err_to_name(err));
    }

    // Libera a memória alocada pelo cliente — importante para não vazar memória
    // Como isso é chamado a cada 3s, sem cleanup o heap esgota rapidamente
    esp_http_client_cleanup(client);
}
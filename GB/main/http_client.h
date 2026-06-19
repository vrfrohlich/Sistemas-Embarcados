#pragma once

#include <stdbool.h>

// Credenciais WiFi — altere aqui
#define WIFI_SSID      "iPhone de Vítor"
#define WIFI_PASSWORD  "gremiomaiordosul"
#define SERVIDOR_URL   "http://172.20.10.6:1880/dados"

// Conecta ao WiFi. Retorna true se conectou, false se falhou.
bool http_wifi_init(void);

// Envia temperatura e umidade via HTTP POST para SERVIDOR_URL.
void http_enviar(float temperatura, float umidade);
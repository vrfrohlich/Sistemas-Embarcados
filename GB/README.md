# Projeto GB — Monitor de Temperatura e Umidade

Firmware para ESP32 que lê temperatura e umidade do sensor DHT11 a cada 3 segundos,
aciona alertas visuais via LED, envia os dados para um servidor Node-RED via HTTP POST
e permite ajustar os limites de alerta em tempo real pelo monitor serial.

---

## Funcionalidades

| Funcionalidade | Descrição |
|----------------|-----------|
| Leitura do DHT11 | Temperatura e umidade a cada 3 s (protocolo single-wire) |
| Alerta visual | LED vermelho acende quando temperatura ou umidade supera o limite |
| Silenciar alerta | BTN1 desliga o LED por 30 s |
| Leitura imediata | BTN2 força uma leitura antes dos 3 s |
| Ajuste de limites | Comandos via monitor serial sem reiniciar o firmware |
| Envio HTTP | POST com JSON para servidor Node-RED a cada leitura válida |

---

## Mapeamento de Pinos

| GPIO | Tipo | Componente |
|------|------|------------|
| 4 | Entrada/Saída | DHT11 — pino DATA |
| 2 | Saída | LED vermelho (alerta) |
| 19 | Entrada (pull-up) | BTN1 — silencia alerta 30 s |
| 21 | Entrada (pull-up) | BTN2 — força leitura imediata |

---

## Esquema de Ligação

```
ESP32           DHT11
3.3V  ──────── VCC  (pino 1)
GPIO4 ──┬───── DATA (pino 2)
        │
       [4.7kΩ]   ← resistor pull-up obrigatório
        │
3.3V  ──┘
GND   ──────── GND  (pino 4)

ESP32           LED vermelho
GPIO2 ──[330Ω]── anodo (+)
GND   ────────── catodo (-)

ESP32           Botões (normalmente abertos)
GPIO19 ── BTN1 ── GND   (silencia alerta)
GPIO21 ── BTN2 ── GND   (leitura imediata)
```

> O DHT11 exige um resistor de pull-up de **4.7 kΩ** entre DATA e VCC.
> Os botões usam pull-up interno do ESP32 — nível baixo = pressionado.

---

## Arquitetura de Software (FreeRTOS)

```
DHT11
  │
  ▼
task_sensor  (prio 2) ─── lê DHT11 a cada 3 s
  │                        BTN2 acorda antes via xTaskNotifyGive
  ├──[fila_leds]──► task_leds_botoes (prio 3) — LED + BTN1 + BTN2
  └──[fila_http]──► task_http        (prio 1) — HTTP POST Node-RED

task_console (prio 1) ─── comandos via serial (ajuste de limites)
```

| Task | Prioridade | Stack | Responsabilidade |
|------|-----------|-------|-----------------|
| `task_sensor` | 2 | 2048 | Driver DHT11, distribui para filas |
| `task_leds_botoes` | 3 | 2048 | Controla LED e lê botões |
| `task_http` | 1 | 4096 | Envia dados via HTTP POST |
| `task_console` | 1 | 3072 | Comandos via monitor serial |

### Região Crítica (Mutex)

As variáveis `g_temp_limite` e `g_umid_limite` são compartilhadas entre
`task_console` (escrita) e `task_leds_botoes` (leitura). O acesso é protegido
por `g_limites_mutex` (FreeRTOS mutex) para garantir consistência sem uso de `volatile`.

---

## Console Serial — Comandos

Com o monitor serial aberto, envie:

| Comando | Exemplo | Ação |
|---------|---------|------|
| `temp <valor>` | `temp 30.5` | Atualiza o limite de temperatura (°C) |
| `umid <valor>` | `umid 75.0` | Atualiza o limite de umidade (%) |
| `limites` | `limites` | Exibe os limites atuais |

Limites padrão ao ligar: **temperatura 25 °C** e **umidade 80 %**.

---

## Integração HTTP — Node-RED

A cada leitura válida, o firmware envia um HTTP POST com o corpo:

```json
{"temperatura": 25.3, "umidade": 61.0}
```

Configure o endpoint e as credenciais WiFi em [main/http_client.h](main/http_client.h):

```c
#define WIFI_SSID      "SuaRede"
#define WIFI_PASSWORD  "SuaSenha"
#define SERVIDOR_URL   "http://<IP_DO_PC>:1880/dados"
```

> O Node-RED deve ter um fluxo escutando na rota `/dados` (HTTP In → processamento → resposta).

---

## Como Funciona o Protocolo DHT11

A comunicação usa um único fio (single-wire, 40 bits = umidade 2 B + temperatura 2 B + checksum 1 B):

1. ESP32 puxa a linha para LOW por 20 ms — sinal de início
2. ESP32 solta a linha; DHT11 responde com LOW 80 µs → HIGH 80 µs
3. DHT11 transmite 40 bits: Bit 0 = HIGH ~28 µs / Bit 1 = HIGH ~70 µs
4. Checksum valida a integridade dos 4 bytes de dados

---

## Compilar e Gravar

Abra o terminal ESP-IDF no VS Code (`Ctrl+Shift+P` → **ESP-IDF: Open ESP-IDF Terminal**):

```bash
idf.py build flash monitor
```

Com porta específica:

```bash
idf.py -p COM3 build flash monitor
```

> Para descobrir a porta: **Gerenciador de Dispositivos → Portas (COM e LPT)**

Saída esperada no monitor serial:

```
I (HTTP): IP obtido: 172.20.10.X
I (MONITOR): Monitor iniciado — sensor:GPIO4  LED_R:GPIO2  BTN1:GPIO19  BTN2:GPIO21
I (MONITOR): Temperatura: 25.0 C  |  Umidade: 61.0 %
I (HTTP): Enviado! Status: 200 | {"temperatura": 25.0, "umidade": 61.0}
```

---

## Estrutura do Projeto

```
gb/
├── main/
│   ├── main.c          # Tasks FreeRTOS, driver DHT11, lógica de alerta e console
│   ├── http_client.c   # WiFi init + envio HTTP POST (Node-RED)
│   ├── http_client.h   # Credenciais WiFi, URL do servidor, declarações
│   └── CMakeLists.txt  # Registro do componente e dependências
├── CMakeLists.txt      # Configuração do projeto ESP-IDF
├── sdkconfig           # Configuração gerada pelo menuconfig
└── .vscode/
    └── settings.json   # Configurações do VS Code / ESP-IDF
```

---

## Dependências

- [ESP-IDF v6.0.1](https://docs.espressif.com/projects/esp-idf/en/latest/)
- Componentes ESP-IDF: `nvs_flash`, `esp_wifi`, `esp_event`, `esp_netif`, `esp_http_client`, `esp_driver_gpio`
- Hardware: ESP32 DevKit, DHT11, LED, 2 botões, resistores (4.7 kΩ e 330 Ω)

# Pendências — Projeto GB Monitor de Temperatura

---

## Status Atual

| Item | Status |
|------|--------|
| Estrutura do projeto ESP-IDF | ✅ Feito |
| Driver DHT11 (protocolo single-wire) | ✅ Feito |
| LEDs verde/vermelho + lógica de alerta | ✅ Feito |
| BTN1 (silencia alerta 30s) e BTN2 (leitura imediata) | ✅ Feito |
| Arquitetura multi-task FreeRTOS | ✅ Feito |
| Demux via filas (fila_leds + fila_http) | ✅ Feito |
| Stub da task_http pronto para o colega | ✅ Feito |
| Montagem física do circuito | ⏳ Pendente |
| Validação no hardware real | ⏳ Pendente |
| Wi-Fi init + envio HTTP (colega) | ⏳ Pendente |
| Relatório / documentação para entrega | ⏳ Pendente |

---

## Pendências

### Colega (HTTP)
- [ ] Adicionar `esp_wifi` e `esp_http_client` no `CMakeLists.txt` do componente
- [ ] Implementar Wi-Fi init (nvs_flash, esp_netif, modo station, credenciais)
- [ ] Substituir o `ESP_LOGI` placeholder na `task_http` pelo envio HTTP real
- [ ] Definir endpoint do servidor e formato do payload (JSON recomendado)

### Hardware
- [ ] Montar o circuito físico na protoboard (ESP32 + DHT11 + resistor 4.7 kΩ + LEDs + botões)
- [ ] Confirmar qual GPIO será usado (código usa GPIO 4 — alterar `DHT22_PIN` se necessário)
- [ ] Verificar versão do ESP32 disponível (DevKit ou outra variante)

### Firmware
- [ ] Gerar o `sdkconfig` com `idf.py build` (necessário na primeira compilação)
- [ ] Validar leituras no hardware real e ajustar timeouts do driver se necessário

### Entrega / Documentação
- [ ] Criar esquema elétrico (Fritzing, KiCad ou desenho manual)
- [ ] Redigir relatório (objetivo, metodologia, resultados, conclusão)
- [ ] Registrar evidências de funcionamento (print do monitor serial ou vídeo)

---

## Arquitetura das Tasks

```
DHT11
  │
  ▼
task_sensor (prio 2)
  │
  ├──[fila_leds]──► task_leds_botoes (prio 3)  — LEDs + BTN1/BTN2
  └──[fila_http]──► task_http        (prio 1)  — envio HTTP (colega)
```

O BTN2 envia `xTaskNotifyGive` para a `task_sensor`, acordando-a antes dos 3s.

---

## O que o colega precisa para implementar o HTTP

1. Localizar o comentário `/* TODO */` na função `task_http` em `main.c`
2. Neste ponto os dados já chegam prontos em `leitura.temperature` e `leitura.humidity`
3. Adicionar as dependências no `main/CMakeLists.txt`:
   ```cmake
   idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_http_client nvs_flash)
   ```

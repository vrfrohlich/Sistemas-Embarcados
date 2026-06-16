# Projeto GB — Monitor de Temperatura

Firmware para ESP32 que lê temperatura e umidade do sensor DHT11 e exibe os valores
via serial a cada 3 segundos.

---

## Comportamento

| Situação | Resultado |
|----------|-----------|
| Leitura bem-sucedida | `Temperatura: XX.X C  \|  Umidade: XX.X %` no monitor serial |
| Falha na leitura | Aviso de erro e nova tentativa em 3 s |

---

## Mapeamento de Pinos

| GPIO | Tipo | Componente |
|------|------|------------|
| 4 | Entrada/Saída | DHT11 — pino DATA |

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
```

> O DHT11 exige um resistor de pull-up de **4.7 kΩ** entre DATA e VCC.
> Sem ele a comunicação falha de forma intermitente.

---

## Como Funciona o Protocolo DHT11

A comunicação usa um único fio (single-wire):

1. ESP32 puxa a linha para LOW por 20 ms — sinal de início
2. ESP32 solta a linha; DHT11 responde com LOW 80 µs → HIGH 80 µs
3. DHT11 transmite 40 bits (umidade + temperatura + checksum)
4. Bit 0: HIGH por ~28 µs / Bit 1: HIGH por ~70 µs

---

## Como Validar

Após gravar, abra o monitor serial para ver as leituras em tempo real:

```bash
idf.py -p COM3 flash monitor
```

Saída esperada no terminal:
```
I (1234) DHT11: Monitor de Temperatura iniciado — DHT11 no GPIO4
I (4567) DHT11: Temperatura: 25.0 C  |  Umidade: 60.0 %
I (7890) DHT11: Temperatura: 25.0 C  |  Umidade: 60.0 %
```

> Para descobrir a porta COM: **Gerenciador de Dispositivos → Portas (COM e LPT)**

---

## Compilar e Gravar

Abrir terminal ESP-IDF no VS Code (`Ctrl+Shift+P` → **ESP-IDF: Open ESP-IDF Terminal**):

```bash
idf.py build flash monitor
```

Com porta específica:
```bash
idf.py -p COM3 build flash monitor
```

---

## Estrutura do Projeto

```
Projeto GB - Monitor de temperatura/
├── main/
│   ├── main.c          # Driver DHT11 + leitura periódica
│   └── CMakeLists.txt  # Registro do componente
├── CMakeLists.txt      # Configuração do projeto ESP-IDF
├── README.md
└── .vscode/
    ├── settings.json          # Configurações do VS Code / clangd
    └── c_cpp_properties.json  # IntelliSense C/C++
```

---

## Dependências

- [ESP-IDF v6.0.1](https://docs.espressif.com/projects/esp-idf/en/latest/)
- Placa: ESP32 (qualquer variante DevKit)
- Sensor: DHT11

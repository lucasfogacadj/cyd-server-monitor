# esp32-server-monitor

Monitor de saúde do servidor numa telinha **ESP32-2432S028R "Cheap Yellow Display"**
(2.8", 320x240, ST7789, CH340).

HUD escuro com acento ciano, números em 7 segmentos e barras de LED:

```
 ▌S E R V I D O R              up 4h 15m  ◉
 ─────────────────────────────────────────
 ┐CPU                ┌ ┐TEMP           ┌
       ┌─┐ ┌─┐            ┌─┐ ┌─┐
       ╶─┤ │ │ %          ╵ │ ╵ │  C
       └─┘ └─┘            └─┘ └─┘
 ┘                  └ ┘               └
 ┌─────────────────────────────────────┐
 │▁▂▃▅▆▅▃▂▁▁▂▄▆▇▆▄▂▁▁▂▃▄▅▄▃▂▁▁▂▃▄▆▇▆▅▃▂│
 └─────────────────────────────────────┘
 RAM  ▮▮▮▮▮▮▮▯▯▯▯▯▯▯   3.0/5.6G     54%
 SWAP ▮▮▯▯▯▯▯▯▯▯▯▯▯▯   1.4/10.0G    14%
 /    ▮▮▮▮▮▮▯▯▯▯▯▯▯▯   25.8/55.5G   46%
 BKP  ▮▮▮▮▮▯▯▯▯▯▯▯▯▯   746/1877G    40%
 ─────────────────────────────────────────
 LOAD  6.92  6.36  4.96          2 CORES
```

Os números grandes são desenhados sobre um `888` apagado, então os segmentos
não acesos continuam visíveis em tom escuro — como num instrumento de verdade.

## Arquitetura

```
node-exporter ──> Prometheus ──> tft-dashboard-api ──(WiFi/HTTP)──> CYD
                                  JSON ~370 bytes
```

O ESP32 **puxa** um JSON compacto e desenha nativamente. O USB serve só como
alimentação e para regravar o firmware.

**Por que não renderizar PNG do Grafana** (o `grafana-image-renderer` já está na
stack):

- 320x240 é pequeno demais — um painel do Grafana reduzido a esse tamanho fica
  ilegível: eixos, legendas e labels viram borrão.
- Cada render sobe um Chromium headless (pico de ~300MB + segundos de CPU) num
  host que roda com ~150MB livres e load ~3 em 2 cores.
- Este ESP32 **não tem PSRAM**; decodificar JPEG 320x240 disputa DRAM com o
  resto do firmware.
- Payload: ~30-60KB de JPEG por frame contra **370 bytes** de JSON.

## Estrutura do repo

```
platformio.ini        projeto PlatformIO (firmware) -- fica na raiz por convencao
src/main.cpp          firmware da telinha
api/                  o servico que serve o JSON
Dockerfile            imagem do tft-dashboard-api
docker-compose.yml    stack do lado servidor
```

O dump do firmware original de fábrica fica em `backup/`, fora do git: são 4MB
de binário proprietário, específicos de uma unidade.

## Lado servidor

| item | valor |
|---|---|
| serviço | `tft-dashboard-api` (stack própria, neste repo) |
| código | `api/tft_dashboard_api.py` |
| endpoint | `http://<host>:9881/status.json` |
| health | `http://<host>:9881/healthz` |

Stdlib pura em `python:3.12-alpine` — não há nada para instalar. Faz cache de 10s
para que o poll da telinha não vire 12 queries no Prometheus a cada refresh.

```bash
cp .env.example .env     # ajuste PROM_URL e DOCKER_NETWORK se precisar
docker compose up -d --build
docker compose logs -f
```

O serviço se anexa a uma rede docker **externa** (`DOCKER_NETWORK`, padrão
`grafana_default`) para alcançar o Prometheus pelo nome. A rede precisa existir
antes; este compose não a cria nem a destrói.

| variável | padrão | para quê |
|---|---|---|
| `PROM_URL` | `http://prometheus:9090` | de onde os dados vêm |
| `DOCKER_NETWORK` | `grafana_default` | rede externa a ser usada |
| `TFT_API_PORT` | `9881` | porta publicada no host |
| `CACHE_TTL` | `10` | segundos de cache do JSON |
| `DISK_MOUNTS` | `/,/mnt/backup` | mountpoints, na ordem da tela |
| `TEMP_CHIP` | `platform_coretemp_0` | chip em `node_hwmon_temp_celsius` |

Este mesmo JSON também alimenta o
[relógio WFD PUNKCYBER](https://github.com/lucasfogacadj/wfd-punkcyber-clock).

### Formato do JSON

```json
{"ok":true,"ts":1789154800,"up":14397,"cpu":60.2,"cores":2,
 "load":[3.9,3.05,2.57],"temp":79.0,"mem":[3.1,5.6],"swap":[1.0,10.0],
 "disk":[["/",22.7,55.5],["/mnt/backup",746.1,1876.7]],
 "spark":[47,48,48,...]}
```

Chaves curtas de propósito. `mem`/`swap`/`disk` são `[usado, total]` em GiB;
`spark` são 60 inteiros 0-100 (CPU% da última hora, um ponto por minuto).
Qualquer campo pode vir `null` — o firmware desenha `--` nesse caso.

**Sem taxa de rede**: o `node-exporter` desta stack roda na própria netns do
container (sem `network_mode: host`), então `node_network_*` só reporta o `eth0`
dele, nunca o `enp2s0` do host. Expor isso mostraria zero permanente.

## Firmware

WiFi e URL da API são configurados por **portal captivo** — não há credencial no
código. No primeiro boot a placa sobe o AP `CYD-Monitor`; conecte-se a ele pelo
celular e a página de configuração abre sozinha. Além da rede, há um campo para
a URL do `status.json`.

### Compilar e gravar

```bash
PIO=/tmp/.../esp-venv/bin/pio        # ou instale: pip install platformio
cd <raiz deste repo>                  # o projeto PlatformIO vive na raiz

$PIO run -e cyd                       # compila
$PIO run -e cyd -t upload             # grava
$PIO device monitor                   # serial 115200
```

**Nunca rode o `pio` com `sudo`.** Ele deixa os artefatos em `.pio/`
pertencendo ao root e a próxima compilação como usuário normal morre com
`PermissionError: ... .sconsign312.tmp`. Se acontecer:

```bash
sudo chown -R "$USER:$USER" .
```

`casaos` já foi adicionado ao grupo `dialout` (vale a partir do próximo login).
Se a porta voltar a dar permissão negada antes disso, ou depois de desconectar e
reconectar o USB:

```bash
sudo chmod 666 /dev/ttyUSB0
```

## Se a imagem sair errada

As cores foram confirmadas corretas em 2026-09-11, então o auto-teste de boot
vem **desligado** (`-DSHOW_BOOT_SELFTEST=0`). Se precisar rediagnosticar a tela,
ponha `1` e regrave: o boot passa a mostrar 5s de fundo preto com quatro barras
que trazem o próprio nome escrito dentro. Compare o que aparece com o que está
escrito e ajuste `build_flags` no `platformio.ini`:

| sintoma | causa | correção |
|---|---|---|
| Barra "VERMELHO" aparece azul (e vice-versa) | ordem dos canais | trocar `-DTFT_RGB_ORDER=TFT_BGR` por `TFT_RGB` |
| Fundo branco, cores todas negativas | inversão | `-DTFT_INVERSION_OFF=1` — **foi o caso desta placa** |
| Tela toda branca / apagada | driver errado | trocar `-DST7789_DRIVER=1` por `-DILI9341_DRIVER=1` (muitas CYD usam ILI9341) |
| Imagem de lado ou espelhada | rotação | `tft.setRotation(3)` em `main.cpp` no lugar de `1` |
| Pixels sujos / chuvisco | SPI rápido demais | baixar `-DSPI_FREQUENCY` para `40000000` |
| Nada acende | backlight | confirmar `-DTFT_BL=21`; em algumas revisões é `27` |

Depois que estiver certo, troque `-DSHOW_BOOT_SELFTEST=1` para `0` e regrave
para o monitor subir direto.

### Pinout da CYD (referência)

```
TFT (HSPI)                 Touch XPT2046 (VSPI)
  MISO 12   CS  15           CLK  25   MOSI 32
  MOSI 13   DC   2           CS   33   MISO 39
  SCLK 14   RST -1           IRQ  36
  BL   21

LED RGB (ativo em LOW): R=4  G=16  B=17
```

O firmware original de servidor não usa o touch. O firmware solar usa os pinos
acima e calibra a tela no primeiro início.

## Segunda CYD: geração solar

O ambiente `solar` compila um firmware independente para a segunda placa. A
primeira continua com `pio run -e cyd`; a nova usa `pio run -e solar` e
`pio run -e solar -t upload`. Na primeira inicialização, toque os três alvos
para calibrar a tela. Em seguida, configure WiFi e URL no AP `CYD-Solar`; a URL
padrão é `http://192.168.3.17:9881/solar.json`.

A tela **Agora** mostra potência instantânea em W, energia do dia, mês, ano e
acumulada em kWh. A tela **24 horas**
mostra a potência e permite tocar numa barra para ver o valor. Os botões
inferiores alternam telas e atualizam a leitura. O serviço expõe os dados em
`/solar.json` e os mantém em cache por `CACHE_TTL` segundos.
Segure **ATUALIZAR** por cinco segundos para refazer a calibração do toque.
As consultas HTTP rodam em uma tarefa separada para manter o toque responsivo
mesmo se a rede estiver lenta. O histórico mantém 49 pontos separados por
30 minutos; os horários sem telemetria permanecem vazios. O estado **NOITE**
é informado pela série de luz solar do SolisCloud, sem inventar potência zero
quando o datalogger deixa de reportar.

Configure no `.env` as expressões PromQL que retornam **uma única série**:

| variável | unidade esperada | obrigatória |
|---|---|---|
| `SOLAR_POWER_QUERY` | W instantâneos | sim |
| `SOLAR_TODAY_QUERY` | kWh gerados hoje | recomendada |
| `SOLAR_TOTAL_QUERY` | kWh acumulados | recomendada |
| `SOLAR_MONTH_QUERY` | kWh do mês | recomendada |
| `SOLAR_YEAR_QUERY` | kWh do ano | recomendada |
| `SOLAR_DAYLIGHT_QUERY` | 1 durante o dia, 0 à noite | recomendada |
| `SOLAR_LIVE_QUERY` | 1 com telemetria atual, 0 sem sinal | recomendada |
| `SOLAR_GRID_QUERY` | W da rede, sinal conforme a métrica | não |
| `SOLAR_LOAD_QUERY` | W consumidos | não |

`SOLAR_POWER_SCALE` multiplica as três potências; `SOLAR_ENERGY_SCALE`
multiplica as quatro energias. Use `1000` quando a origem estiver em kW ou MWh;
use `0.001` quando estiver em mW ou Wh. Sem expressão ou sem dados, a tela
mostra `--`, sem confundir ausência de leitura com zero. Após editar o `.env`,
recrie o serviço com `docker compose up -d --build` e confira
`http://<host>:9881/solar.json`.

## Notas de implementação

- **A faixa sob os números é o histórico de CPU de 1 hora** (60 pontos, um por
  minuto): área preenchida em tom frio com o topo aceso na cor do nível, sobre
  uma grade de 25/50/75%.
- A **fonte 7 do TFT_eSPI (7 segmentos) só contém `1234567890:-.`** — sem `%` e
  sem `C`; a fonte 6 tem `apm` além dos dígitos, mas também não tem `%`. Por
  isso cada valor grande é desenhado em duas partes: número na fonte 7 e
  unidade na fonte 4 ao lado. Medido no dispositivo: `"888"` = 96px de largura
  por 48px de altura.
- O efeito de segmento apagado vem de desenhar `888` em `C_GHOST` e depois o
  valor por cima **com fundo transparente** — no TFT_eSPI isso é
  `setTextColor(cor)` com um argumento só, que faz `textcolor == textbgcolor` e
  ativa o modo transparente no `drawChar`.
- Os números grandes e o sparkline são desenhados em **sprites** (~33KB no
  total) e depois empurrados de uma vez, o que elimina a piscada do repinte.
  Sobram ~262KB de heap. As linhas de barra usam `setTextPadding`, que apaga o
  texto anterior sem `fillRect`. O layout estático é desenhado uma única vez.
- Acima de **85 °C** este N4000 entra em throttle térmico — daí o vermelho nesse
  limiar. Em repouso ele já fica em ~75-80 °C.
- O `LOAD` é colorido pela razão `load1 / nº de cores` (2 aqui): amarelo a partir
  de 1.0, vermelho a partir de 2.0.

<p align="center">
  <img src="resources/icon/streamline.svg" width="96" alt="">
</p>

<h1 align="center">Streamline</h1>

<p align="center">
  Transmita uma janela ou a tela inteira do Windows direto para seus amigos —
  P2P via WebRTC, com baixa latência, sem servidor próprio e sem relay.
</p>

<p align="center">
  <a href="https://github.com/ldrizze/streamline/actions/workflows/build.yml"><img src="https://github.com/ldrizze/streamline/actions/workflows/build.yml/badge.svg" alt="build"></a>
  <a href="https://github.com/ldrizze/streamline/releases"><img src="https://img.shields.io/github/v/release/ldrizze/streamline?include_prereleases" alt="release"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/licen%C3%A7a-GPL--3.0-blue" alt="GPL-3.0"></a>
</p>

---

## Destaques

- **Janela ou tela inteira.** Janelas são capturadas com Windows.Graphics.Capture, mesmo
  cobertas por outras e acompanhando redimensionamentos; a tela, com Desktop Duplication.
- **Áudio certo.** Ao transmitir uma janela, vai só o som daquele aplicativo (loopback por
  processo). Ao transmitir a tela, vai o som do sistema **sem o Discord**, para quem está
  na call não ouvir a própria voz de volta.
- **Codecs livres de royalties.** Vídeo AV1 por hardware em GPUs AMD (AMF) com VP8 por
  software como alternativa; áudio Opus. No espectador, AV1 por GPU (D3D11) ou software
  (dav1d).
- **Sem servidor para manter.** A sinalização usa brokers MQTT públicos em paralelo, com
  criptografia ponta a ponta por sala. A mídia vai direto entre os PCs.
- **Um código de convite e pronto.** O transmissor copia o convite; o espectador cola.
- **Portátil ou instalado.** Instalador por usuário (sem administrador) ou zip portátil;
  não precisa instalar GStreamer nem MSYS2 no PC de destino.

## Como usar

1. Baixe a [última versão](https://github.com/ldrizze/streamline/releases) — o instalador
   `Streamline-Setup-*.exe` ou o zip portátil.
2. **Quem transmite:** clique em **Transmitir**, escolha uma tela ou janela e clique em
   **Copiar** para pegar o código de convite.
3. **Quem assiste:** clique em **Assistir** e cole o código.

A barra de status mostra fps, kbps, nível de áudio e quantos espectadores estão
conectados. Transmissor e espectadores precisam usar a mesma versão (enquanto 0.x).

## Requisitos

- Windows 10 2004 (build 19041) ou mais novo, 64 bits — necessário para o loopback de
  áudio por processo.
- Para AV1 por hardware: GPU AMD com encoder AV1 (ex.: RX 7000). Sem ela, o Streamline usa
  VP8 por software automaticamente.

## Como funciona

```
 Transmissor                                                     Espectador
 ┌───────────────────────────────────────┐                ┌────────────────────────┐
 │ WGC / Desktop Duplication ─► AV1/VP8 ─┤                │                        │
 │                                       ├─► webrtcbin ═══╪═► webrtcbin ─► decode  │
 │ WASAPI loopback (por processo) ► Opus ┤   (1 por       │   ─► d3d11videosink    │
 │                                       │   espectador)  │                        │
 └───────────────────┬───────────────────┘   mídia P2P    └───────────┬────────────┘
                     │        (SRTP/DTLS, STUN público, sem TURN)     │
                     └──────► brokers MQTT públicos (wss) ◄───────────┘
                          sinalização cifrada: SDP + candidatos ICE
```

- **Pipeline** (GStreamer): a captura gera texturas D3D11 que, no caminho AV1, vão ao
  encoder sem passar pela CPU; a saída passa por `tee`s que alimentam o preview local e um `webrtcbin` por
  espectador.
- **Sinalização:** o convite são 16 bytes aleatórios (22 caracteres base64url). Dele
  derivam, via BLAKE2b, o tópico MQTT e uma chave XChaCha20-Poly1305 exclusiva da sala
  (libsodium). Os brokers (`broker.emqx.io`, `broker.hivemq.com`, `test.mosquitto.org`)
  não veem o código, a chave nem o conteúdo; mensagens forjadas, repetidas ou antigas são
  descartadas.
- **Rede:** só STUN público, **sem servidor de relay (TURN), por escolha**: a mídia nunca
  passa por terceiros.

## Limitações conhecidas

- Se os dois lados estiverem atrás de NAT restritivo (CGNAT/NAT simétrico), a conexão P2P
  não se estabelece — não há relay.
- Os brokers MQTT públicos são serviços de teste gratuitos, sem garantia de
  disponibilidade; eles veem o IP de quem conecta e o volume/horário das mensagens (não o
  conteúdo).
- Conexão entre redes diferentes ainda não foi testada.
- O bitrate do AV1 da AMF oscila bastante (picos acima do alvo de 8 Mbps).
- No Windows 10, a borda amarela de captura do Windows não pode ser removida.
- O Discord é detectado ao iniciar a transmissão; se for aberto depois, o áudio dele entra
  até a próxima transmissão.
- Executáveis não assinados: o SmartScreen avisa na primeira execução.

## Compilando

O projeto é C++20 compilado com **MSYS2 UCRT64** (GCC + CMake + Ninja). Instale as
dependências no shell UCRT64:

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,gstreamer,gst-plugins-base,gst-plugins-good,gst-plugins-bad,gst-plugins-rs,libnice,libsoup3,glib-networking,json-glib,libsodium,wxwidgets3.3-msw}
```

Compile e gere a pasta `dist/` autocontida (app + DLLs + plugins GStreamer):

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix dist
bash tools/collect-licenses.sh dist
```

O instalador é gerado com Inno Setup 6 a partir de `packaging/streamline.iss`. O CI
([build.yml](.github/workflows/build.yml)) faz tudo isso a cada push e publica uma Release
com instalador e zip em tags `v*`.

### Linha de comando

| Opção | O que faz |
| --- | --- |
| `--check` | Mostra a versão do GStreamer e confere os elementos necessários e os opcionais de GPU |
| `--check-package` | Verifica se todos os plugins empacotados carregam (usado no CI) |
| `--list-sources` | Lista monitores, janelas capturáveis e o Discord detectado |
| `--brokers=url1,url2` | Usa outros brokers MQTT (WebSocket) |
| `--test-host=monitor\|<pid>` | Transmite a tela ou a janela do PID e encerra com o resultado |
| `--test-watch=<convite>` | Assiste à sala e encerra com sucesso se chegarem quadros |
| `--room=<convite>` | Convite fixo para `--test-host` |
| `--test-seconds=N` | Duração dos testes (padrão 6) |

### Estrutura

```
src/app/       interface wxWidgets: janela principal e seletor de fonte
src/capture/   enumeração de janelas/monitores e captura WGC
src/host/      pipeline de captura/encode e uma sessão WebRTC por espectador
src/viewer/    recepção, decodificação e renderização
src/common/    runtime GStreamer, cliente MQTT e canal de sala cifrado
packaging/     instalador Inno Setup e licenças de terceiros
```

## Licença

[GPL-3.0](LICENSE). O pacote distribuído inclui GStreamer, wxWidgets e outras bibliotecas
sob suas próprias licenças, reunidas em `dist/` pelo `tools/collect-licenses.sh`.

Mudanças entre versões: [CHANGELOG.md](CHANGELOG.md).

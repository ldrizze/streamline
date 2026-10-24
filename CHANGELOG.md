# Changelog

Todas as mudanças relevantes do Streamline. Formato baseado em
[Keep a Changelog](https://keepachangelog.com/pt-BR/1.1.0/); versões seguem
[SemVer](https://semver.org/lang/pt-BR/). Enquanto a versão for 0.x, o protocolo de
sinalização e o comportamento podem mudar entre versões menores.

## [Não lançado]

### Adicionado
- Ao transmitir a tela toda, o áudio do Discord (estável, PTB ou Canary) fica de fora
  da transmissão, para quem está na call não ouvir a própria voz de volta. Opção
  **Não transmitir o áudio do Discord** no seletor, ligada por padrão.
  `--list-sources` mostra o PID do Discord detectado. Com o Discord aberto, o próprio
  Streamline deixa de ser excluído da captura (o Windows só exclui uma árvore de
  processos por vez): um espectador rodando no mesmo PC do transmissor causaria eco.

## [0.2.0] - 2026-10-24

Sinalização sem servidor próprio. **Incompatível com a 0.1.0**: transmissor e
espectadores precisam estar na mesma versão.

### Adicionado
- Sinalização pelos brokers MQTT públicos (`broker.emqx.io`, `broker.hivemq.com`,
  `test.mosquitto.org`) via WebSocket seguro, usados ao mesmo tempo: se um cair, os
  outros seguem. Não é preciso rodar nem configurar servidor algum.
- Criptografia ponta a ponta da sinalização: cada sala tem um código de convite
  aleatório de 16 bytes (22 caracteres) do qual derivam o tópico MQTT e uma chave
  XChaCha20-Poly1305 exclusiva da sala. Os brokers não veem o código, a chave nem o
  conteúdo (SDP, IPs). Mensagens forjadas, repetidas ou antigas são descartadas.
- Botão **Copiar** para o código de convite; o espectador só cola o código.
- Saída de participantes detectada também quando o app fecha sem avisar (last will
  MQTT, também cifrado).
- `--brokers=url1,url2` para escolher outros brokers.
- Ícone próprio do aplicativo (ondas de transmissão), exibido no executável, na
  barra de título, na barra de tarefas e no instalador.

### Alterado
- O código da sala deixou de ser escolhível/curto (6 letras) e passou a ser o
  convite aleatório acima.
- O vídeo começa mais rápido para um espectador novo: um keyframe é pedido quando a
  conexão P2P se estabelece.

### Removido
- `streamline-signal` (servidor de sinalização próprio), o campo "Servidor" e a
  opção `--server`. Atualizar pelo instalador remove o executável antigo.

### Limitações conhecidas
- Os brokers MQTT públicos são serviços de teste gratuitos, sem garantia de
  disponibilidade. Eles enxergam o IP de quem se conecta e o volume/horário das
  mensagens (não o conteúdo).
- Sem servidor de relay (TURN), por escolha: se os dois lados estiverem atrás de
  NAT restritivo (CGNAT/NAT simétrico), a conexão P2P não se estabelece.
- Conexão entre redes diferentes ainda não testada.
- O bitrate do AV1 da AMF oscila bastante (picos acima do alvo de 8 Mbps).
- No Windows 10 a borda amarela de captura do Windows não pode ser removida.
- Executáveis não assinados: o SmartScreen avisa na primeira execução.
- Requer Windows 10 2004 (build 19041) ou mais novo, 64 bits.

## [0.1.0] - 2026-10-24

Primeira versão de desenvolvimento.

### Adicionado
- Transmissão P2P via WebRTC de uma **janela** ou da **tela inteira** para um ou
  mais espectadores, com sala identificada por um código de 6 caracteres.
- Captura de janela com Windows.Graphics.Capture, mesmo coberta por outras janelas,
  acompanhando redimensionamentos; captura de tela com Desktop Duplication.
- Áudio **somente do aplicativo capturado** (loopback por processo); em modo tela,
  o som do sistema exceto o do próprio Streamline.
- Vídeo AV1 por hardware em GPUs AMD (AMF), com VP8 por software como alternativa;
  áudio Opus. Decodificação AV1 por GPU (D3D11) ou software (dav1d).
- Preview local, estatísticas (fps, kbps, nível de áudio, espectadores).
- `streamline-signal`: servidor de sinalização WebSocket com salas.
- Instalador por usuário (sem administrador) e versão portátil em zip.
- Diagnóstico: `streamline --check`, `--check-package` e `--list-sources`.

### Limitações conhecidas
- Testado apenas com transmissor e espectador no mesmo computador; conexão entre
  redes diferentes depende de STUN público e ainda não há servidor TURN.
- O servidor de sinalização precisa ser executado manualmente (padrão
  `ws://127.0.0.1:8443/ws`).
- O bitrate do AV1 da AMF oscila bastante (picos acima do alvo de 8 Mbps).
- No Windows 10 a borda amarela de captura do Windows não pode ser removida.
- Executáveis não assinados: o SmartScreen avisa na primeira execução.
- Requer Windows 10 2004 (build 19041) ou mais novo, 64 bits.

[Não lançado]: https://github.com/ldrizze/streamline/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/ldrizze/streamline/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/ldrizze/streamline/releases/tag/v0.1.0

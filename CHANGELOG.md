# Changelog

Todas as mudanças relevantes do Streamline. Formato baseado em
[Keep a Changelog](https://keepachangelog.com/pt-BR/1.1.0/); versões seguem
[SemVer](https://semver.org/lang/pt-BR/). Enquanto a versão for 0.x, o protocolo de
sinalização e o comportamento podem mudar entre versões menores.

## [Não lançado]

### Adicionado
- Ícone próprio do aplicativo (ondas de transmissão), exibido no executável, na
  barra de título e na barra de tarefas.

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

[Não lançado]: https://github.com/ldrizze/streamline/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/ldrizze/streamline/releases/tag/v0.1.0

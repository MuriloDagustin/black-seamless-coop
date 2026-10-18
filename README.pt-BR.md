# Black Seamless Co-op

🇺🇸 [Read in English](README.md)

Jogue a campanha do **Black** (Criterion, 2006) junto com até **8 jogadores**, no PC.

Este é um mod para a [BLACK-XBERecomp](https://github.com/TGP482/BLACK-XBERecomp), uma port nativa e não
oficial do jogo original do Xbox para PC (uma recompilação estática feita com o
[XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp)). O Black nunca teve multiplayer: tudo aqui foi
construído em cima do próprio código do jogo, então espere algumas arestas.

> **Você precisa da sua própria cópia do Black (Xbox, versão USA).** Este repositório tem só o código do
> co-op. Ele não tem arquivos do jogo nem código da port ou do recompilador.

---

## Começo rápido

1. **Instale as ferramentas** (uma vez só): Windows 10/11 64 bits, [git](https://git-scm.com/download/win),
   [Python 3](https://www.python.org) e Visual Studio 2022 ou mais novo com *Desenvolvimento para desktop
   com C++*, *Ferramentas Clang do C++* e *Ferramentas CMake do C++*.
2. **Baixe este repositório** (botão verde *Code* → *Download ZIP*, ou `git clone`).
3. **Rode o setup** com a sua imagem de disco do Black:
   ```
   setup.bat black-recomp "C:\caminho\para\Black.iso"
   ```
   Ele baixa a port e o recompilador nas versões em que este mod foi testado, aplica o mod e extrai o seu
   jogo em `black-recomp\assets`.
4. **Compile**: rode `black-recomp\build.bat`. A primeira compilação recompila o jogo inteiro e demora.
5. **Jogue**: `black-recomp\out\build\release\BLACK.exe`.

Todos os jogadores precisam da **mesma build**. O jeito mais fácil: uma pessoa compila e passa a pasta
`out\build\release` (o `BLACK.exe` e as DLLs dele, sem o jogo) para os outros. Cada jogador coloca a
**própria** cópia do jogo numa pasta `assets` ao lado do `BLACK.exe`: o arquivo `.iso` ou os arquivos do
disco extraídos.

## Jogando junto

Aperte **F10 → CO-OP** no jogo:

- **Um jogador é o host.** O jogo dele roda os inimigos e a missão; todos os outros seguem.
- **Os outros entram**: escolhem o host na lista (mesma rede) ou digitam o IP do host.
- Quem entra é levado para a missão, a dificuldade e o progresso do host automaticamente, até no meio de
  uma fase.

Pela internet o host precisa estar acessível na **porta UDP 45410**: um redirecionamento de porta no
roteador do host (com o `BLACK.exe` liberado no firewall do Windows), ou uma rede virtual como Tailscale ou
ZeroTier.

**Salas online por código** (sem redirecionar porta) funcionam pelo Epic Online Services, mas só numa build
feita com a sua própria conta gratuita de desenvolvedor da Epic e o SDK do EOS. Veja
[docs/online.md](docs/online.md) (em inglês).

### Controles

| | Teclado / mouse | Controle |
|---|---|---|
| Menu F10: co-op, configurações, teclas | **F10** | |
| Correr | **Shift** andando para frente | |
| "Me tira daqui": ir até um parceiro | **F8** | **L3 + R3** segurados por um segundo |
| Caído: assistir o próximo parceiro | **clique**, **Espaço** ou **→** (**←** o anterior) | **A** ou o direcional |

A velocidade da corrida fica ao lado das teclas no menu F10 (**SPRINT SPEED**, de 100% a 250%, 140% por
padrão).

### Regras do co-op

- **Morrer não é o fim.** Quem morre fica *caído*: a missão continua e ele assiste um parceiro vivo. Ele
  volta quando o time chega na próxima área ou completa um objetivo. A missão só falha quando **todos**
  estão caídos.
- **As áreas carregam para todos.** Quem chega primeiro na porta da próxima área espera lá até os outros
  chegarem. Se você se perder ou travar, aperte **F8**.
- **Progresso compartilhado.** Objetivos, itens secundários (chantagem, inteligência...), missão falhada,
  continuar, reiniciar e fim de fase valem para todos.
- **O progresso de todos é salvo.** Depois de uma missão, o jogo de cada um mostra as próprias telas de
  save; quem entrou passa por elas antes de seguir o host para a próxima missão. Sem perfil ainda, o jogo
  pede um nome nessa hora.

## O que funciona

| | |
|---|---|
| Até 8 jogadores | sessão rodada pelo host, um nick em cima de cada parceiro |
| Parceiros | cada um aparece como um soldado que anda, corre, agacha e mira como ele |
| Tiros | as balas dos parceiros acertam, quebram e matam na sua tela também, com clarão, rastro e estampido |
| Inimigos | rodados pelo host; eles veem, ouvem e atiram em todos os jogadores |
| Mundo | mortes, caixas, barris, cenário destrutível, granadas, foguetes e gatilhos de área são iguais para todos |
| Fluxo da fase | áreas, objetivos, missão falhada / continuar / reiniciar / concluída juntos |
| Vidas | caído, assistir um parceiro, reviver no próximo checkpoint |
| Pausa | um parceiro no menu de pausa continua visível, marcado PAUSED |

Ainda não: cutscenes sincronizadas, veículos.

## Problemas?

- **Não conecta:** todos precisam rodar a mesma build. Confira o redirecionamento de porta e o firewall do
  host (UDP 45410), ou use uma rede virtual.
- **O jogo fechou:** mande o `logs\black.log` (ao lado do `BLACK.exe`) e o arquivo mais novo da pasta
  `CrashDumps`, se tiver. O log da sessão anterior fica guardado como `logs\black.prev.log`.
- **Travou, ou a próxima área não carregou:** aperte **F8** para ir até um parceiro.

---

Para os detalhes técnicos (o que o setup faz, linha de comando, canal de teste e como o mod funciona), veja a
seção *For developers* do [README em inglês](README.md#for-developers).

## Licença

O código deste repositório é MIT (veja `LICENSE`). Ela vale só para os arquivos deste repositório: a port, o
recompilador e o jogo pertencem aos seus autores.

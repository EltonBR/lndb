# Arquitetura e código do LNDB

## Escopo

LNDB é composto por dois executáveis C11 que se comunicam diretamente por TCP:
`lndb`, executado no computador de administração, e `lndbd`, executado no
Linux embarcado. Não há servidor intermediário. A implementação usa APIs
POSIX/Linux e libc; não requer OpenSSL, SSH, FTP, `tar` nem bibliotecas de
terceiros.

O código comum não depende de uma arquitetura específica. A primeira imagem
alvo deste projeto é o Linux customizado 4.1.51 da FiberHome AN5506-01-A:
BMIPS4350 MIPS32 big-endian, ABI o32, BusyBox e musl. O `build.sh` usa o SDK
`mips32--musl--stable-2018.02-2` e gera executáveis estáticos por padrão. O
mesmo código pode ser compilado nativamente ou com toolchains para outros
Linux, desde que forneçam as interfaces POSIX/Linux usadas pelo projeto.

## Mapa dos arquivos

| Caminho | Responsabilidade |
|---|---|
| `client/main.c` | Entrada CLI, parsing de opções e despacho dos comandos. |
| `client/connection.c` | Parsing de endpoints, conexão TCP com timeout e handshake/autenticação por API key. |
| `client/config_store.c` | Caminho padrão, criação privada da configuração do cliente e atualização atômica da lista de chaves. |
| `client/discovery.c` | Parsing de faixas IPv4, sondagem concorrente, identificação/autenticação de serviços e mapa local de IDs. |
| `client/shell.c` | Terminal interativo, execução de comandos e envio de scripts por stdin. |
| `client/transfer.c` | Envio/recepção de arquivos e árvores no cliente; valida caminhos relativos e trata arquivos locais sem seguir symlinks. |
| `client/transfer.h` | Interface da transferência do cliente. |
| `daemon/main.c` | Inicialização/configuração do servidor, listener, limite de filhos e encerramento. |
| `daemon/session.c` | Greeting, timeout/autenticação e dispatch autenticado por conexão. |
| `daemon/blink_queue.c` | Fila limitada, comunicação com worker e ciclo de vida do processo de blink. |
| `daemon/gpio.c` | Controla exclusivamente GPIO15 via `/sys/class/gpio`; o worker executa um pedido por vez e restaura estado/direção anteriores. |

O daemon mantém um worker de blink e uma fila limitada a 16 pedidos em
execução/espera. O pedido do cliente termina quando o daemon confirma a entrada
na fila. Ao receber SIGTERM/SIGINT, o worker conclui a sequência ativa para
restaurar GPIO15 e descarta os pedidos ainda pendentes.
| `daemon/shell.c` | Shell interativo com PTY e shell não interativo com pipes separados para stdin/stdout/stderr; limpa grupos de processos em desconexões. |
| `daemon/transfer.c` | Percorre árvores remotas, recebe envios, produz downloads, valida caminhos e rejeita symlinks/arquivos especiais. |
| `common/file_wire.c` | Codificação/decodificação compartilhada dos metadados `FILE_META`. |
| `common/protocol.c` | Leitura/escrita completa de streams, framing, limites de payload e conversão de inteiros para network byte order. |
| `common/config.c` | Parser INI, validação dos campos e geração inicial da configuração do daemon com chave em `/dev/urandom`. |
| `include/lndb_protocol.h` | Constantes, tipos de mensagem e API de framing; fonte de verdade para os números do protocolo. |
| `include/lndb_config.h` | Estruturas de configuração e interfaces compartilhadas. |
| `tests/local_test.c` | Testes em diretórios temporários para configuração, framing, metadados e push/pull recursivos, incluindo falha remota e interrupção. |
| `tests/network_test.c` | Integração TCP local com daemon real: autenticação, rejeição de chave, shell/exit code e timeout de autenticação. |
| `docs/PROTOCOL.md` | Formato LNDB1 e payloads das mensagens. |
| `docs/USAGE.md` | Guia de compilação, configuração, execução e comandos. |

## Caminho de uma conexão

1. O daemon aceita TCP e envia um frame `HELLO` vazio, que identifica o
   protocolo `LNDB`, versão 1.
2. O cliente envia um frame `AUTH` contendo a API key como texto hexadecimal.
3. O daemon responde `AUTH_OK` quando a chave corresponde. Em caso de erro,
   envia `ERROR` com `unauthorized` e fecha a conexão.
4. Só depois de `AUTH_OK` o cliente envia `SHELL_REQUEST` ou `FILE_REQUEST`.
5. Cada conexão trata uma operação; não há multiplexação de operações.

`lndb_client_connect_auth()` em `client/connection.c` abre uma conexão por chave configurada e
experimenta as chaves na ordem do arquivo. `add device` usa o mesmo handshake
com a chave fornecida na linha de comando; só depois da confirmação do daemon
atualiza `api_key_list`. A substituição do arquivo do cliente é feita por
arquivo temporário e `rename()`, com modo `0600`.

## Processos e concorrência

`lndbd` roda em foreground. Para cada conexão TCP aceita, faz `fork()` e deixa
o filho autenticar e atender a operação, enquanto o processo pai volta ao
`poll()` do socket de escuta. O pai verifica filhos encerrados com
`waitpid(..., WNOHANG)` e, ao receber SIGINT/SIGTERM, fecha o listener, envia
SIGTERM aos filhos registrados e aguarda sua saída. O número de clientes
simultâneos rastreados pelo daemon é limitado a 128; conexões excedentes são
fechadas sem criar filhos não rastreados. O filho aplica um prazo absoluto de
4 segundos ao greeting e à autenticação. Depois de autenticado, o prazo deixa
de se aplicar para não limitar shells e transferências longas.

Não há pool de threads, daemonização ou dependência de systemd. As operações
ativas usam memória em blocos; a transferência não carrega um arquivo inteiro
na memória.

## Shell

- Sem argumentos e com terminal local, o cliente pede modo interativo. O
  daemon abre um pseudo-terminal, inicia `/bin/sh`, conecta os três streams ao
  PTY e aplica redimensionamentos recebidos. O cliente coloca o terminal local
  em modo raw e restaura `termios` e o handler de SIGWINCH ao sair.
- Com argumentos, o cliente os concatena com espaços e o daemon executa
  `/bin/sh -c <texto>`, sem PTY. Os argumentos não são enviados como `argv`
  remoto independente; metacaracteres e aspas são interpretados pelo shell
  remoto. stdout e stderr são frames separados e `SHELL_EXIT` contém o status.
- Com stdin redirecionado e um `/dev/tty` local disponível, o cliente envia o
  script em `SHELL_STDIN` e `CLOSE` usando o modo 2. O daemon o grava em arquivo
  temporário e executa `/bin/sh <arquivo>` sob PTY; assim, `read` e programas
  interativos recebem respostas do terminal local. O temporário é limitado a
  16 MiB e removido ao terminar.
- Sem terminal de controle, stdin redirecionado usa o modo 1: o script é
  encaminhado até EOF para `/bin/sh`, com stdout e stderr separados. Args
  explícitos como `shell sh -s < script.sh` continuam nesse modo.
- O EOF encerra o envio sem exigir newline final no arquivo. `/bin/sh` processa
  a última linha ainda que o último byte não seja `\n` (por exemplo, um arquivo
  de dois bytes `ls`). Um comando sem saída visível, como `ls` em diretório vazio
  ou contendo somente nomes ocultos, ainda foi executado; o exit code remoto
  permite distinguir falha de sucesso.

No modo interativo, `Ctrl+C` é processado pelo PTY remoto. Em modo não
interativo, stdin pode ser um pipe/arquivo, por exemplo `shell sh -s < script`.

## Transferência

`FILE_REQUEST` seleciona upload (cliente para daemon) ou download. Metadados
identificam arquivo/diretório, permissões básicas, tamanho e caminho relativo.
Dados de arquivos seguem em frames `FILE_DATA` limitados pelo tamanho máximo
de frame; `FILE_END` termina cada arquivo e `FILE_DONE` informa o resultado da
operação.

Os receptores criam diretórios necessários e gravam arquivos primeiro em
temporários no mesmo diretório do destino, aplicam permissões e substituem o
destino por `rename()` quando a transferência termina. Uma desconexão ou erro
remove o temporário de arquivo em andamento. Diretórios já criados antes de
uma falha podem permanecer. Links simbólicos e arquivos especiais são
rejeitados. Caminhos relativos recebidos não podem ser absolutos nem conter
componentes `.` ou `..`.

## Discovery

`lndb_client_devices()` em `client/discovery.c` interpreta a faixa IPv4, abre sockets não
bloqueantes em lotes limitados por `concurrency`, e espera cada lote até
`connect_timeout_ms`, mantendo sockets lentos pendentes até o prazo do lote.
Para identificar LNDB, a rotina espera um `HELLO` válido
e tenta as API keys configuradas. Mostra `device` para autenticação aceita e
`unauthorized` para LNDB identificado sem chave válida; serviços que não
respondem com o protocolo não aparecem. A porta de discovery é `[client]
port`.

Os tipos `DISCOVER`/`DISCOVER_REPLY` estão reservados no cabeçalho do
protocolo, mas a versão atual faz a descoberta sondando a porta TCP e usando o
handshake normal. Não existe broadcast/multicast de descoberta.

## Configuração e dados sensíveis

Os esquemas completos dos arquivos, valores default e gramática aceita pelo
parser estão em [`CONFIGURATION.md`](CONFIGURATION.md).

O daemon usa `/etc/lndbd/lndbd.conf` por padrão. Na primeira execução, cria o
diretório pai e um arquivo `0600` com bind, porta e chave de 32 bytes obtida de
`/dev/urandom`. Configuração existente inválida causa erro; a chave não é
regenerada. `--config PATH` escolhe outro arquivo.

O cliente usa `${XDG_CONFIG_HOME:-~/.config}/lndb/lndb.conf`, criado com modo
`0600` na primeira execução, independentemente do local do executável. `add
device` atualiza essa configuração privada; `--config PATH` seleciona outro
arquivo explicitamente. A lista pode estar vazia antes do primeiro cadastro.
As chaves são validadas como 64 caracteres hexadecimais.

LNDB1 não cifra credenciais, comandos, saída de terminal nem arquivos. A API
key é somente controle de acesso básico para rede confiável. O programa não
registra a chave intencionalmente. Restrinja as permissões dos arquivos de
configuração e o acesso de rede à porta do daemon.

## AN5506-01-A e inicialização

O alvo desta versão para a ONU é o firmware **custom Linux 4.1.51** descrito
em [`../FIBERHOME-AN5506-01-A-PLATFORM.md`](../FIBERHOME-AN5506-01-A-PLATFORM.md),
não o firmware OEM Linux 3.4.11-rt19. O binário MIPS deve ser ELF32 big-endian,
ABI o32, compatível com a CPU e com o userland musl do firmware escolhido.

No rootfs custom descrito no documento, `/start-on-boot.sh` é chamado pelo
`/sbin/init` depois de `/etc/init.d/rcS`. Isso é um detalhe dessa imagem, não
um mecanismo de instalação do LNDB. O projeto não copia executáveis para a
ONU, não altera scripts de boot e não escreve firmware, kernel, CFE ou MTD.
Consulte o [guia de uso](USAGE.md) para execução manual e um exemplo opcional
de inicialização nesse firmware. Não aplique esse caminho ao firmware OEM.

## Build e verificação

O Makefile suporta `CC`, `CPPFLAGS`, `CFLAGS`, `LDFLAGS`, `LDLIBS` e
`BUILD_DIR`. Use um diretório diferente por toolchain para não misturar objetos
de host e alvo. `make test` executa testes locais em diretórios temporários;
não valida a rede real, a PTY na ONU nem a instalação no hardware.

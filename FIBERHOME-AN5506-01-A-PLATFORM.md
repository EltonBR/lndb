# Especificação da plataforma: FiberHome AN5506-01-A / BCM68385

Este documento reúne as características de hardware e software observadas na
ONU FiberHome AN5506-01-A com SoC BCM68385, incluindo boot, memória, flash,
rede, GPIO e SPI. É uma referência geral da plataforma, independente de uma
ferramenta ou aplicação específica.

## Identificação do equipamento

| Propriedade | Plataforma atual |
|---|---|
| Equipamento | ONU FiberHome AN5506-01-A |
| Board ID informado pelo CFE | `968385SFU_ND` |
| SoC | Broadcom BCM68385 B0, também identificado nas fontes da plataforma como BCM96838 / BCM6838 Viper |
| CPU | 2 × Broadcom BMIPS4350, SMP |
| Arquitetura da CPU | MIPS de 32 bits, big-endian, ABI o32 |
| Linux | Linux customizado 4.1.51 |
| Userspace | BusyBox `ash` e musl |
| Interface Ethernet | `eth0`, driver Ethernet BCM68385 customizado |
| Console local | `ttyS0`, 115200 baud |

A configuração de hardware do CFE, o MAC base e a versão do CFE podem variar
entre unidades. Use o Board ID para identificar a família, não uma ONU
específica. Obtenha o MAC de cada unidade em runtime ou na etiqueta do
equipamento.

## Memória e processador

- DDR instalada: **128 MiB**. O CFE informa clock DDR de **333 MHz**.
- O kernel customizado mantém as reservas originais do datapath: 4 MiB para
  MC e 20 MiB para TM. Assim, cerca de **104 MiB** são gerenciados como RAM do
  Linux; o kernel e as alocações em runtime reduzem a quantidade imediatamente
  livre.
- O código-fonte atual do kernel seleciona uma configuração de CPU de 600 MHz,
  enquanto a leitura OTP de uma unidade capturada informa 240 MHz. O clock
  efetivo aparece no log de boot e pode variar conforme a imagem e a unidade.
- A plataforma tem dois núcleos BMIPS4350. Um boot de recovery capturado
  informou Linux 4.1.51 e `uname -m` como `mips`.

## NAND e mapa MTD

O equipamento tem **128 MiB de NAND bruta**. Os logs do kernel capturados
informam páginas de 2 KiB, blocos de apagamento de 128 KiB, ECC BCH-4 e OOB de
64 bytes após a configuração do controlador. O ID NAND informado é
`c8d18095`; o log do controlador identifica o chip como desconhecido. Portanto,
softwares da plataforma devem consultar a geometria MTD do kernel em vez de
depender de um modelo NAND codificado.

O mapa de partições confirmado é:

| MTD | Nome | Offset na flash | Tamanho | Uso atual / mountpoint |
|---:|---|---:|---:|---|
| 0 | `rootfs` | `0x00020000` | `0x01460000` (20,375 MiB) | Raiz inicializável do slot 0 |
| 1 | `rootfs_update` | `0x01480000` | `0x01460000` (20,375 MiB) | Raiz inicializável do slot 1 |
| 2 | `app_basic` | `0x028e0000` | `0x01460000` (20,375 MiB) | JFFS2 persistente, `/opt` |
| 3 | `app_basic_update` | `0x03d40000` | `0x01460000` (20,375 MiB) | JFFS2 persistente, `/home` |
| 4 | `app_extend` | `0x051a0000` | `0x01460000` (20,375 MiB) | JFFS2 persistente, `/srv` |
| 5 | `app_extend_update` | `0x06600000` | `0x01460000` (20,375 MiB) | JFFS2 persistente, `/var/lib/custom` |
| 6 | `data` | `0x07a60000` | `0x004a0000` (4,625 MiB) | Dados persistentes, `/state` |
| 7 | `nvram` | `0x00000000` | `0x00020000` (128 KiB) | Partição identificada como NVRAM; preservar em operações comuns de arquivos |

O último MiB da NAND é reservado pelo controlador para as tabelas de blocos
ruins. O Linux expõe dispositivos MTD de caractere (`/dev/mtdN` e
`/dev/mtdNro`, somente leitura); no sistema capturado não havia dispositivos de
bloco. Consulte `/proc/mtd` e `/proc/mounts` em runtime. Não presuma que
`/dev/mtdblockN` exista.

Os slots 0 e 1 têm sistemas de arquivos raiz inicializáveis independentes. A
seleção `latest/previous` do CFE é diferente do número físico MTD. Cada imagem
de raiz contém um arquivo CFERAM com nome de sequência próprio (`cferam.002`
no slot 0 e `cferam.003` no slot 1). O CFE principal é um artefato separado;
alterar o CFERAM dentro do rootfs não altera a versão do CFE mostrada no
prompt serial. Quando os dois slots customizados estão presentes, o layout
atual do projeto associa `latest` ao slot 1 e `previous` ao slot 0.

O sistema de arquivos raiz customizado é JFFS2 gravável. `mtd2`–`mtd6` são
volumes de dados compartilhados pelos dois slots customizados; a instalação
inicializa `mtd2`–`mtd5`, mas preserva `mtd6` e o outro slot raiz. Para
transferências comuns, use caminhos do sistema de arquivos e não trate
gravações MTD brutas como operações comuns de cópia de arquivos.

## GPIO e sinais da placa

O controlador GPIO integrado ao SoC está no endereço KSEG1 `0xb4e00100` e
registra 74 linhas, numeradas de GPIO 0 a 73. O driver atual expõe o chip pela
interface GPIO do Linux (`/sys/class/gpio`), mas permite requisitar somente as
linhas 15 e 48. As demais linhas permanecem indisponíveis pelo driver até que
seus usos e o pinmux sejam mapeados.

| Linha / sinal | Função identificada | Polaridade / direção | Estado de suporte |
|---|---|---|---|
| GPIO 15 | LED PON/GPON | Ativo baixo, saída | Pode ser lido e controlado pelo driver atual; teste em hardware validado |
| GPIO 48 | Botão/reset | Ativo baixo, entrada | Pode ser lido; `1` com botão solto e `0` pressionado |
| Canal LED 0 | `OpticalLinkFail` | Ativo baixo, controlador de LED dedicado | Não é uma GPIO discreta |
| GPIOs 0–14, 16–47, 49–73 | Não mapeados individualmente aqui | Desconhecida | Não requisitáveis pelo driver atual |

Os registradores de direção ficam nos offsets `0x00`, `0x04` e `0x08`; os
registradores de dados ficam em `0x2c`, `0x30` e `0x34`. As linhas são
organizadas em três bancos de 32 bits. O bit de direção em `1` seleciona
saída. O usuário confirmou funcionamento de GPIO, LEDs e botões no firmware
customizado, mas o mapeamento completo dos sinais ainda não está consolidado.

## SPI

O kernel customizado atualmente compilado não habilita o subsistema SPI
(`CONFIG_SPI` está desativado) e não há evidência de barramento SPI Linux ou
dispositivo `/dev/spidev*` no sistema em execução. Portanto, SPI não está
disponível como interface Linux genérica nessa imagem.

O prompt do CFE inclui um comando `spi`, descrito como acesso legado ao switch
externo. Esse comando pertence ao ambiente do bootloader; sua presença não
confirma um controlador SPI utilizável pelo Linux, nem documenta pinagem,
frequência, chip select ou periféricos acessíveis no firmware customizado. A
NAND principal é exposta pelo controlador NAND e pela interface MTD, não como
um dispositivo SPI comum. Até que controlador, pinmux e periféricos sejam
identificados e validados, não se deve presumir que SPI esteja disponível.

## Rede de gerenciamento

A imagem customizada configura IPv4 estático em `eth0`:

| Configuração | Valor |
|---|---|
| ONU 1 | `192.168.10.10/24` |
| ONU 2 | `192.168.10.11/24` |
| ONU 3 | `192.168.10.12/24` |
| Máscara de sub-rede | `255.255.255.0` |
| Gateway padrão | `192.168.10.1` |
| Link Ethernet | Autonegociação; PHY1 / EMAC0, até 1000 Mbps full duplex |

Regra operacional de endereçamento: a ONU de número `N` (começando em 1) usa
`192.168.10.(9 + N)`. Continue a sequência até `.254`; reserve `.0`, `.1`
(gateway) e `.255`. A configuração atual da imagem usa um único IP estático
padrão, `192.168.10.10`; ela **não** detecta o número da ONU nem incrementa o
endereço automaticamente. Configure um IP exclusivo em cada unidade antes de
conectar várias ONUs à mesma rede de camada 2.

Não é necessário DHCP nessa rede de gerenciamento. O computador de
gerenciamento precisa ter um endereço em `192.168.10.0/24` e conseguir
alcançar a ONU diretamente ou por uma rota. Em uma conexão Ethernet direta na
mesma sub-rede, o tráfego entre os dois equipamentos não precisa de gateway.

Acesso padrão informado para as ONUs atualmente flasheadas:

| Serviço | Porta | Acesso observado | Observações |
|---|---:|---|---|
| Telnet | TCP/23 | Conecta diretamente ao shell root, sem pedir usuário ou senha | Comportamento reportado pelo usuário no firmware instalado |
| FTP | TCP/21 | Usuário `root`, senha vazia | Serviço BusyBox, raiz FTP em `/`, escrita habilitada |

Esses são os padrões observados na imagem atualmente instalada; não são
credenciais OEM nem garantias para builds futuros. No checkout do projeto,
`start-telnet.sh` inicia o `telnetd` com `/bin/login`, enquanto a entrada root
em `/etc/shadow` tem senha vazia. Assim, a configuração versionada pode exibir
um prompt de login mesmo com autenticação sem senha. Confirme o comportamento
do Telnet após mudanças no rootfs. O script FTP usa `ftpd -w` e serve `/`.

## Caminhos e shell em runtime

- O PID 1 é o `/sbin/init` customizado; o shell local de recovery é `/bin/sh`
  no console serial.
- Após executar `/etc/init.d/rcS`, o init chama `/start-on-boot.sh` se o arquivo
  existir e for executável. Esse script inicia Telnet e FTP. O cliente e o
  servidor NBD estão explicitamente fora da inicialização automática. O init
  mantém o shell UART disponível mesmo se a rotina de inicialização reportar
  falha.
- Nas ONUs atualmente flasheadas, o Telnet abre shell root diretamente; o FTP
  usa `root` com senha vazia. O FTP está enraizado em `/` e permite escrita.
  Veja a diferença entre o comportamento observado e a configuração
  versionada na seção de rede.
- `/tmp`, `/run` e `/var` voláteis usam tmpfs. No instalador em RAM, `/tmp` tem
  modo `1777`; o sistema customizado cria os mounts voláteis durante o boot.
- Os caminhos persistentes são `/opt`, `/home`, `/srv`, `/var/lib/custom` e
  `/state`, conforme o mapa acima.
- O kernel customizado expõe uma interface de rede Linux, `eth0`, pelo caminho
  integrado PHY1/EMAC0. A interface e o tráfego IP bidirecional foram
  validados no equipamento.
- O init monta um PTY em `/dev/pts`.

## Descoberta de informações em runtime

Estas interfaces fornecem informações úteis em runtime e devem ser
consultadas quando disponíveis, em vez de depender somente de valores de
build:

| Informação | Fonte em runtime |
|---|---|
| Kernel / arquitetura | `uname -a`, `uname -m` |
| Modelo e frequência da CPU | `/proc/cpuinfo`, log de boot em `dmesg` |
| Memória | `/proc/meminfo` |
| MAC e estado do link Ethernet | `/sys/class/net/eth0/address`, `/sys/class/net/eth0/operstate`, `/sys/class/net/eth0/speed` quando disponíveis |
| Endereços IPv4 e rotas | `/proc/net/route`, `ifconfig eth0` |
| Geometria e nomes da flash | `/proc/mtd` |
| Sistemas de arquivos montados | `/proc/mounts` |
| Detalhes da placa / SoC | `dmesg` |

## Evidências e limites

Os dados de hardware acima vêm da saída de boot do CFE, dos logs do recovery
Linux 4.1.51, de `/proc/mtd`, de `/proc/mounts` e da documentação da plataforma
customizada em `bcm6838-kernel/docs/`. Os padrões de rede e os serviços vêm dos
scripts de inicialização do rootfs customizado. Memória disponível, clock
efetivo da CPU, velocidade do link, MAC, versão do CFE e configuração da placa
podem variar por imagem e por unidade física; softwares da plataforma devem
informar os valores obtidos em runtime quando disponíveis.

Referências do repositório usadas na preparação desta especificação:

- `bcm6838-kernel/docs/INSTALLER.md` — slots de boot, offsets MTD, mounts
  compartilhados e configuração de rede do instalador.
- `bcm6838-kernel/docs/ethernet/memory.md` — mapa de RAM física e reservas
  MC/TM.
- `bcm6838-kernel/docs/ethernet/link.md` — comportamento do link PHY1/EMAC0.
- `bcm6838-kernel/docs/GPIO.md` — GPIO 15/48, LED PON e botão de reset.
- `bcm6838-kernel/config/phy-link-minimal.config` — estado de configuração
  atual do subsistema SPI no kernel customizado.
- `bcm6838-kernel/rootfs/overlay/scripts/network-config.sh` — configuração
  IPv4 estática padrão.
- `bcm6838-kernel/rootfs/overlay/scripts/start-telnet.sh` e
  `start-ftp.sh` — inicialização dos serviços remotos no rootfs versionado.
- `bcm6838-kernel/rootfs/overlay/start-on-boot.sh` e `sbin/init` — sequência de
  inicialização pós-`rcS`.
- `bcm6838-kernel/rootfs/overlay/etc/passwd` e `etc/shadow` — conta root e
  configuração local da senha no rootfs versionado.

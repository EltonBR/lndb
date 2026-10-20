# Guia de utilização

Os formatos completos dos arquivos do cliente e do daemon, seus defaults,
limites e permissões estão em [Configuração](CONFIGURATION.md).

Este guia cobre o uso do LNDB em duas partes: preparar e executar o daemon no
dispositivo Linux e usar o cliente no computador. A comunicação é direta entre
os dois por TCP. As credenciais e os dados trafegam sem criptografia; use uma
rede confiável e controlada.

## 1. Compilar

Para compilar para o sistema host:

```sh
make
```

Os executáveis ficam em `build/lndb` e `build/lndbd`. É possível compilar um
componente apenas com `make client` ou `make daemon`; `make clean` remove o
diretório de build selecionado.

Para preparar as ferramentas no Debian/Ubuntu e compilar para a FiberHome
AN5506-01-A custom Linux 4.1.51:

```sh
./install-deps.sh
./build.sh daemon
```

O resultado é `build-mips/lndbd`, MIPS32 big-endian/musl estático por padrão.
Para compilar o cliente também, rode `./build.sh` ou `./build.sh client`.
`BUILD_DIR`, `CC` e `JOBS` podem selecionar diretório, compilador e
paralelismo. O alvo MIPS desta ONU é específico; para outros dispositivos,
selecione o compilador e `BUILD_DIR` apropriados, por exemplo:

```sh
make CC=mips-linux-musl-gcc BUILD_DIR=build-outro-mips daemon
```

`install-deps.sh` instala dependências de build/runtime do SDK no host. Ele
não instala pacotes na ONU. O Linux do dispositivo precisa oferecer os
syscalls e recursos requeridos, incluindo TCP, `fork`, pipes e, para shell
interativo, PTYs com `/dev/pts` montado.

## 2. Iniciar o daemon

O daemon roda em foreground por padrão. Como root no dispositivo, inicie-o
assim:

```sh
./lndbd
```

Por padrão, ele escuta em `0.0.0.0:5555`. Na primeira execução, cria
`/etc/lndbd/lndbd.conf`, gera uma API key aleatória de 32 bytes (64 caracteres
hexadecimais) e define modo `0600`. Confirme a criação e leia a chave por um
canal administrativo confiável:

```sh
cat /etc/lndbd/lndbd.conf
```

O daemon não imprime a chave como parte do log de inicialização. Guarde-a
como segredo e evite copiar a saída para logs compartilhados. Nas próximas
execuções, o daemon reutiliza a configuração existente. Se o arquivo estiver
inválido, corrija-o manualmente; não será criada uma chave substituta.

Para configuração em outro caminho:

```sh
./lndbd --config /caminho/lndbd.conf
```

Configuração do servidor:

```ini
[server]
bind = 0.0.0.0
port = 5555

[auth]
api_key = 64_caracteres_hexadecimais
```

O daemon atende conexões independentes por processos filhos, suporta até 128
clientes rastreados simultaneamente e encerra os filhos ao receber SIGINT ou
SIGTERM. Para pará-lo no foreground, use `Ctrl+C`; como serviço externo,
envie SIGTERM ao processo.

## 3. Instalação e boot na AN5506-01-A custom

Para a AN5506-01-A descrita neste repositório, use o binário produzido por
`./build.sh daemon`. Copie-o ao filesystem do firmware custom Linux 4.1.51
usando um canal administrativo já disponível e instale, por exemplo, em
`/opt/lndb/lndbd`. `/opt` é um caminho persistente nessa imagem custom segundo
o [documento da plataforma](../FIBERHOME-AN5506-01-A-PLATFORM.md). Permita a
execução do binário:

```sh
mkdir -p /opt/lndb
chmod 0755 /opt/lndb/lndbd
```

Execute uma vez para gerar a configuração padrão, ou passe `--config` para
outro caminho gravável e persistente:

```sh
/opt/lndb/lndbd
```

Para iniciar automaticamente **somente no firmware custom documentado**, o
`/sbin/init` chama `/start-on-boot.sh` se ele existir e for executável, depois
de `/etc/init.d/rcS`. Após fazer backup do script atual e verificar que esse é
de fato o firmware inicializado, pode-se adicionar a ele uma linha como:

```sh
/opt/lndb/lndbd --config /etc/lndbd/lndbd.conf >/tmp/lndbd.log 2>&1 &
```

O exemplo pressupõe que a configuração já foi criada, que o filesystem contém
o binário e que `/tmp` é gravável. `/tmp` é volátil nessa imagem; o arquivo de
log é perdido no reboot. A inicialização não é feita pelo LNDB e o exemplo não
se aplica ao firmware OEM. Não grave MTD, não altere bootloader e não reinicie
a ONU como parte da instalação do LNDB.

Para instalar ou atualizar automaticamente uma ONU por IP, use o instalador do
repositório:

```sh
./install-onu.sh 192.168.10.29
```

O script compila `lndbd` para MIPS quando o binário `build-mips/lndbd` está
ausente ou desatualizado. Em seguida, valida por Telnet que o alvo é o firmware
custom BCM68385/MIPS/Linux 4.1.51 e que `/opt` e `/start-on-boot.sh` são
graváveis. Transfere o binário por FTP, instala em `/opt/lndb/lndbd`, inicia ou
reinicia o daemon e garante uma única entrada de inicialização no
`/start-on-boot.sh`. A configuração existente e a API key são preservadas; se
a configuração ainda não existir, o primeiro início do daemon a cria. Na
primeira atualização, o binário anterior é preservado em
`/opt/lndb/lndbd.previous`; se o script precisar acrescentar a entrada de boot,
salva o script anterior em `/start-on-boot.sh.pre-lndb`.

O instalador requer Python 3 no computador e acesso FTP/Telnet root conforme a
imagem custom atual (senha root vazia). Ele aceita um endereço IPv4 por
execução, não reinicia a ONU e não grava MTD, bootloader ou firmware. A
verificação recusa kernels, arquiteturas e modelos incompatíveis; não use no
firmware OEM Linux 3.4.11-rt19. Restrinja FTP e Telnet à rede de administração.

## 4. Configurar o cliente

Na primeira execução, o cliente cria
`${XDG_CONFIG_HOME:-~/.config}/lndb/lndb.conf` com modo `0600` e lista de
chaves vazia, independentemente do diretório atual ou do local do executável.
Assim, `lndb add device` grava a chave sem root e sem alterar `/etc`. Um
arquivo existente nunca é substituído automaticamente.

O arquivo criado terá estes valores iniciais:

```ini
[client]
port = 5555
connect_timeout_ms = 500

[discovery]
network = 192.168.10.*
concurrency = 32

[auth]
api_key_list =
```

Para criar ou editar a configuração antes da primeira execução:

```sh
umask 077
mkdir -p "${XDG_CONFIG_HOME:-$HOME/.config}/lndb"
cat > "${XDG_CONFIG_HOME:-$HOME/.config}/lndb/lndb.conf" <<'EOF'
[client]
port = 5555
connect_timeout_ms = 500

[discovery]
network = 192.168.10.*
concurrency = 32

[auth]
api_key_list =
EOF
chmod 0600 "${XDG_CONFIG_HOME:-$HOME/.config}/lndb/lndb.conf"
```

Também é possível escolher um caminho explícito em qualquer comando usando
`--config FILE` antes do comando. `make install` instala apenas os executáveis;
a configuração ativa é criada privadamente no XDG do usuário na primeira
execução.

Campos da configuração:

| Campo | Uso | Valor padrão |
|---|---|---|
| `[client] port` | Porta padrão para discovery e operações sem `:PORT`. | `5555` |
| `connect_timeout_ms` | Prazo de conexão e handshake LNDB por endpoint, e janela TCP por lote de discovery, em ms. Após autenticar, shell e transferências não herdam esse prazo. | `500` |
| `[discovery] network` | Faixa IPv4 varrida por `devices`. | `192.168.10.*` |
| `concurrency` | Máximo de conexões não bloqueantes simultâneas na varredura. | `32` |
| `[auth] api_key_list` | API keys hexadecimais separadas por vírgulas. Pode estar vazia antes do primeiro cadastro. | vazio |

As formas de faixa aceitas são `192.168.10.*`, `192.168.10.x`, CIDR IPv4
(por exemplo `192.168.10.0/24`) e intervalo inclusivo (por exemplo
`192.168.10.2-192.168.10.30`). A porta usada na varredura é `[client] port`.

## 5. Cadastrar e validar a chave

Use a chave obtida da configuração do daemon. O comando verifica a chave no
dispositivo antes de salvar. Exemplo:

```sh
build/lndb add device 192.168.10.29 <API_KEY>
```

Para outra configuração ou porta:

```sh
build/lndb --config ./lndb.conf add device 192.168.10.29:8989 <API_KEY>
```

Após autenticação aceita, o cliente acrescenta a chave em `api_key_list` e
substitui o arquivo de forma atômica, com modo `0600`. Uma chave rejeitada não
é adicionada. Se ela já estiver na lista, o comando valida a chave e não cria
duplicata. Não há associação persistente entre IP e chave; em cada conexão, o
cliente tenta as chaves cadastradas até uma ser aceita.

## 6. Encontrar dispositivos

```sh
build/lndb devices
```

Saída típica:

```text
List of LNDB devices

1    192.168.10.29    device
2    192.168.10.30    unauthorized
```

`device` indica que uma chave configurada autenticou. `unauthorized` indica
que há um serviço LNDB, mas nenhuma das chaves do arquivo foi aceita.
Serviços que não respondem como LNDB não são listados. O uso de IP direto não
exige executar `devices`; para usar um ID numérico, execute `devices` primeiro.

O último resultado da descoberta é salvo em `lndb.conf.devices`, ao lado da
configuração do cliente, com permissão `0600`. IDs apontam para esse resultado
até a próxima execução de `devices`:

```sh
build/lndb 1 shell
build/lndb 1 push ./arquivo /tmp/
build/lndb 1 pull /tmp/logfile.log .
```

Discovery é IPv4 e testa a porta configurada. A porta pode ser diferente por
equipamento nos comandos individuais, mas não há lista de portas por IP para
discovery nesta versão.

## 7. Shell

Shell interativo, com PTY remoto:

```sh
build/lndb 192.168.10.29 shell
```

Ctrl+C, Ctrl+D, sequências de terminal e redimensionamento são encaminhados ao
PTY remoto. O cliente restaura o terminal local ao terminar.

Comando não interativo:

```sh
build/lndb 192.168.10.29 shell uname -a
build/lndb 192.168.10.29 shell "ps | grep init"
build/lndb 192.168.10.29 shell "exit 7"
```

O cliente junta os argumentos após `shell` com espaços. O dispositivo executa
o texto com `/bin/sh -c`; metacaracteres, redirecionamentos e aspas são
interpretados pelo shell remoto. O status de saída remoto é o status de saída
do cliente. Essa modalidade não aloca PTY e mantém stdout e stderr separados.

Para enviar um script pela entrada padrão:

```sh
build/lndb 192.168.10.29 shell < script.sh
build/lndb 192.168.10.29 shell sh -s < script.sh
```

`shell < script.sh` mostra stdout e stderr do script. Se o cliente tiver um
terminal de controle disponível, o script roda em um PTY remoto e pode pedir
respostas pelo terminal. Sem terminal de controle, stdin é somente o arquivo
redirecionado e o script roda sem interação. A forma explícita `shell sh -s`
também permanece não interativa.

O arquivo pode terminar sem newline: o shell remoto executa a última linha ao
chegar ao EOF. Por exemplo, um arquivo contendo somente os bytes `ls` executa
esse comando mesmo sem `\n` no final. Se `ls` não imprimir nada, verifique o
diretório remoto atual e use `ls -la` para incluir arquivos ocultos.

Para reproduzir o caso sem newline:

```sh
printf 'ls' > script.sh
build/lndb 192.168.10.29 shell < script.sh
```

## 8. Piscar o LED PON

Este comando é uma extensão específica da FiberHome AN5506-01-A com firmware
custom Linux 4.1.51. Ele controla o LED PON ativo em nível baixo no GPIO15;
não é uma operação genérica de Linux nem deve ser usada em outros modelos ou
no firmware OEM. O suporte vem habilitado por padrão.

```sh
build/lndb 192.168.10.29 blink 100 10
build/lndb 1 blink 100 10
```

`100` é o intervalo em milissegundos e `10` é o número de ciclos completos.
Cada fase acesa e apagada dura o intervalo indicado; portanto, `blink 100 10`
fica na fila para aproximadamente 2 segundos de execução. O daemon processa os
pedidos em ordem FIFO e aceita até 16 requests pendentes, contando o que está
em execução. O cliente encerra assim que o daemon confirma o enfileiramento;
ele não espera o piscar terminar. Se a fila estiver cheia, o cliente recebe um
erro. Falhas de GPIO que ocorram depois do aceite são registradas no log do
daemon. Ao encerrar o daemon, a sequência ativa termina e os pedidos restantes
na fila são descartados.
Pedidos são processados em ordem FIFO. O intervalo permitido é 1..60000 ms, a
quantidade 1..1000, e a duração total não pode exceder 120 segundos. O daemon
usa GPIO15 em `/sys/class/gpio`, restaura o nível e a direção anteriores e
desexporta o pino somente se ele próprio o exportou. Essa operação exige que
`lndbd` tenha permissão de root no dispositivo e que o driver GPIO esperado
esteja disponível. Outros dispositivos Linux podem responder com erro.

Para compilar cliente e daemon sem essa operação, use `WITH_PON_BLINK=0`. Isso
define `LNDB_ENABLE_PON_BLINK=0`; escolha outro diretório de build para não
reutilizar objetos compilados com a opção anterior:

```sh
make BUILD_DIR=build-no-blink WITH_PON_BLINK=0
```

Para compilar o daemon MIPS sem a fila nem a implementação específica de GPIO:

```sh
WITH_PON_BLINK=0 BUILD_DIR=build-mips-no-blink ./build.sh daemon
```

Nesse build, `blink` não aparece na ajuda do cliente e o daemon responde
`unsupported operation` se receber essa requisição. Os identificadores de
mensagem permanecem reservados no protocolo LNDB1 para manter seus valores
estáveis.

## 9. Enviar e receber arquivos

Upload de arquivo ou árvore:

```sh
build/lndb 192.168.10.29 push ./arquivo.bin /tmp/
build/lndb 192.168.10.29 push ./projeto /opt/
build/lndb 192.168.10.29 push ./projeto/ /opt/
```

Se o destino remoto for diretório existente, uma árvore é criada dentro dele
com o nome-base da origem (ex.: `/opt/projeto`). Se o destino não existir, ele
se torna a raiz da cópia. Barra final ou prefixo `./` na origem não mudam o
resultado. Diretórios são recursivos.

Download:

```sh
build/lndb 192.168.10.29 pull /tmp/logfile.log .
build/lndb 192.168.10.29 pull /opt/projeto ./
```

Se o destino local for um diretório existente, o nome-base da origem é criado
dentro dele. Se o destino local não existir, ele é usado como caminho raiz da
cópia e a raiz da árvore transferida é colocada nele. Arquivos vazios e dados
binários são suportados.

As permissões Unix básicas (bits `rwx`) são preservadas. O receptor usa arquivo
temporário para cada arquivo e o renomeia ao terminar; em falha, a árvore pode
conter diretórios já criados, mas o arquivo parcial temporário é removido.
Links simbólicos e arquivos especiais são recusados em vez de seguidos.

## 9. Portas e endpoints

O valor de `[client] port` aplica-se por padrão. Para usar outra porta em uma
ONU específica, informe-a no endpoint:

```sh
build/lndb 192.168.10.29:8989 shell
build/lndb 192.168.10.29:8989 push ./arquivo /tmp/
build/lndb 192.168.10.29:8989 pull /tmp/arquivo .
```

Para IPv6 com porta explícita, use colchetes, por exemplo
`build/lndb '[fe80::1]:5555' shell`.

## 10. Problemas comuns

| Sintoma | Verificações |
|---|---|
| `connection failed or not an LNDB service` | Confirme o IP, a rota, a porta, que `lndbd` está rodando e que o firewall permite TCP. Confirme que o servidor é LNDB/versão 1. |
| `unauthorized` / chave rejeitada | Confira a chave completa de 64 caracteres na configuração do daemon; execute `add device` com essa chave e o endpoint/porta corretos. O comando não mostra a chave em sua saída. |
| Configuração inválida | Verifique nome de seção, campos, porta, timeout, faixa e formato hexadecimal das chaves. O daemon não substitui automaticamente uma configuração inválida. |
| Shell interativo sem terminal ou erro de PTY | Verifique se o cliente tem TTY e se o Linux remoto tem suporte PTY e `/dev/pts` montado. Use shell não interativo para comandos sem terminal. |
| Transferência recusada | Confira permissões do destino e espaço livre. Confirme também que a origem existe e não é symlink ou arquivo especial. |

## Compatibilidade específica da ONU

O caminho de boot e os mountpoints variam por firmware. Para a imagem custom
documentada neste repositório, `/opt` é persistente e
`/start-on-boot.sh` é chamado pelo init. Isso não vale automaticamente para o
firmware OEM Linux 3.4.11-rt19. Confirme o firmware em execução antes de
seguir qualquer instrução de instalação persistente. Consulte
[`FIBERHOME-AN5506-01-A-PLATFORM.md`](../FIBERHOME-AN5506-01-A-PLATFORM.md) e
[`ARCHITECTURE.md`](ARCHITECTURE.md).

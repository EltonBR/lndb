# Configuração do LNDB

Este documento descreve os arquivos lidos ou mantidos pelo cliente e pelo
daemon. Os arquivos usam formato INI simples: uma seção por linha entre
colchetes e atribuições `nome = valor`. Espaços no início e no fim da linha,
do nome e do valor são removidos. Linhas vazias e linhas cujo primeiro
caractere não branco seja `#` ou `;` são ignoradas. Seções e nomes de campos
são sensíveis a maiúsculas/minúsculas. Não use aspas nem comentários no fim de
uma atribuição. Seções/campos desconhecidos e campos repetidos são erros.

## Configuração do daemon: `lndbd.conf`

Sem `--config`, `lndbd` lê `/etc/lndbd/lndbd.conf`. Se o arquivo ainda não
existir, cria os diretórios pais ausentes (modo solicitado `0755`) e gera uma
chave aleatória de 32 bytes com `/dev/urandom`. O arquivo novo recebe modo
`0600`, bind `0.0.0.0` e porta `5555`. O daemon inicia usando essa configuração.
Nas execuções seguintes, reutiliza a chave existente. Um arquivo existente
inválido causa erro e nunca é substituído por uma chave nova.

Exemplo completo:

```ini
[server]
bind = 0.0.0.0
port = 5555

[auth]
api_key = 64_caracteres_hexadecimais
```

| Campo | Obrigatório | Default se omitido | Valores aceitos |
|---|---:|---|---|
| `[server] bind` | não | `0.0.0.0` | Endereço/interface aceito pelo sistema para `bind(2)`; não pode ser vazio. |
| `[server] port` | não | `5555` | Inteiro decimal de `1` a `65535`. |
| `[auth] api_key` | sim | — | Exatamente 64 caracteres hexadecimais. A comparação de autenticação é sensível a maiúsculas/minúsculas. |

O caminho pode ser alterado com `lndbd --config FILE`. Os diretórios pais
ausentes desse caminho também são criados. O modo `0600` é aplicado quando o
arquivo é criado; permissões de arquivos já existentes não são verificadas nem
alteradas automaticamente. Proteja manualmente qualquer arquivo que contenha
a chave.

## Configuração do cliente: `lndb.conf`

Sem `--config`, o cliente usa:

```text
${XDG_CONFIG_HOME}/lndb/lndb.conf   se XDG_CONFIG_HOME for absoluto
${HOME}/.config/lndb/lndb.conf      caso contrário
```

O diretório padrão é criado com modo solicitado `0700`, e o arquivo inicial
com `0600`. A localização independe do diretório atual e do diretório do
executável. A primeira execução cria uma lista de chaves vazia. Um `--config
FILE` seleciona outro arquivo; se ele não existir, o cliente cria o arquivo
inicial, desde que o diretório pai já exista. O cliente não cria o diretório
pai de um caminho explícito.

Exemplo:

```ini
[client]
port = 5555
connect_timeout_ms = 500

[discovery]
network = 192.168.10.*
concurrency = 32

[auth]
api_key_list = chave_hexadecimal_1,chave_hexadecimal_2
```

| Campo | Obrigatório | Default se omitido | Valores aceitos |
|---|---:|---|---|
| `[client] port` | não | `5555` | Inteiro decimal de `1` a `65535`; porta padrão para discovery e endpoints sem `:PORT`. |
| `[client] connect_timeout_ms` | não | `500` | Inteiro decimal de `1` a `600000`; prazo de conexão e handshake LNDB para cada endpoint, além da janela TCP de cada lote do discovery. Após autenticação, shell e transferências não herdam esse prazo. |
| `[discovery] network` | não | `192.168.10.*` | Faixa IPv4 não vazia, validada quando `devices` é executado. Aceita `192.168.10.*`, `192.168.10.x`, CIDR ou intervalo inclusivo `IP-INICIAL-IP-FINAL`. |
| `[discovery] concurrency` | não | `32` | Inteiro decimal de `1` a `4096`; máximo de conexões de discovery simultâneas. |
| `[auth] api_key_list` | sim | — | Lista vazia ou chaves de 64 caracteres hexadecimais, separadas por vírgulas. |

`api_key_list =` pode ficar vazio antes de cadastrar dispositivos. Use uma
chave por item, sem vírgulas extras. O parser atual ignora itens vazios
resultantes de vírgulas repetidas ou no fim da linha. Para autenticar, o
cliente testa as chaves na ordem em que aparecem; não guarda associação entre
IP e chave. Chaves hexadecimais são comparadas como texto e diferenciam
maiúsculas de minúsculas.

O campo `network` é validado ao executar `lndb devices`, não ao carregar o
arquivo. Uma faixa inválida pode, portanto, não impedir comandos que não usam
discovery. `HOST:PORT` substitui a porta configurada para uma operação
individual; IPv6 com porta deve ser escrito como `[ENDEREÇO]:PORTA`.

`lndb add device HOST[:PORT] API_KEY` autentica primeiro e só então acrescenta
a chave a `api_key_list`. O cliente substitui o arquivo atomicamente e define
modo `0600`; uma chave rejeitada não é salva. Arquivos existentes não têm as
permissões corrigidas durante a leitura, então defina `0600` manualmente se
necessário. O comando preserva as demais linhas do arquivo e exige que o
campo `api_key_list` esteja presente.

## Mapa dos IDs de discovery

Depois de `lndb devices`, o cliente mantém `lndb.conf.devices` ao lado do
arquivo de configuração selecionado (por exemplo,
`lndb.conf.devices`). Esse arquivo auxiliar contém ID, endereço IPv4 e porta
da última descoberta, recebe modo `0600` e é substituído atomicamente. Ele não
é uma lista de credenciais: `lndb 1 shell` resolve o ID usando esse mapa, e
cada conexão ainda tenta as chaves do `lndb.conf`. Rode `lndb devices` para
atualizar os IDs.

## Segurança

As configurações contêm credenciais em texto puro. LNDB1 também transmite
credenciais, comandos, terminal e arquivos sem criptografia. Restrinja o
acesso aos arquivos e use a ferramenta somente em redes confiáveis.

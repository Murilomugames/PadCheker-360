# PadCheck 360

**Versao 0.2.0** — aplicativo homebrew open source para testar controles de Xbox 360 em consoles com RGH/JTAG.

O PadCheck 360 usa o projeto aberto [xsdk](https://github.com/xenosworks/xsdk) para criar um titulo `.xex`. Este pacote contem o codigo-fonte; nao inclui um executavel `.xex` compilado ou testado em console real.

## Novidades da versão 0.2.0

- Leitura simultanea dos quatro slots de controle: P1, P2, P3 e P4.
- Painel detalhado por jogador. Use o direcional esquerdo/direito do primeiro controle conectado (normalmente P1) para trocar o jogador exibido.
- Teste de 14 botoes digitais, LT e RT, com percentual de entradas acionadas.
- Valores ao vivo dos dois analogicos, incluindo porcentagem, centro calibrado e diferenca em relacao ao centro.
- Verificacao de movimento nos analogicos: segure START por aproximadamente 0,75 segundo com os dois sticks soltos, solte START e nao toque nos sticks durante a amostra de aproximadamente 3 segundos.
- Pico de movimento da amostra exibido separadamente para o analógico esquerdo e direito.
- Teste dos motores de vibracao: segure Y para o motor esquerdo e X para o motor direito. Cada jogador controla a vibracao do proprio controle.
- Layout adaptado para telas 720p e telas menores.
- Fechamento com START + BACK por aproximadamente 2 segundos em qualquer controle conectado.

## O que o teste de drift significa

O aplicativo registra a posicao de repouso quando START e mantido por cerca de 45 quadros. Depois que START e solto, acompanha os quatro eixos por 180 quadros e guarda a maior diferenca em relacao ao centro capturado.

O limite inicial configurado e **3500 unidades brutas**. Um valor acima disso e mostrado como **movimento detectado / possivel drift**. Isso nao prova que o controle esta com defeito: tocar no analógico durante a amostra, vibracao ou ruido do sensor tambem pode alterar a leitura. Para um teste melhor, deixe o controle apoiado e nao toque nos sticks durante a amostra.

A calibracao e por controle. Repita o procedimento para cada jogador. Para mudar o limite, edite `DRIFT_THRESHOLD` em `main.c`.

## Requisitos

- Xbox 360 com RGH/JTAG e dashboard que permita iniciar homebrew `.xex` sem assinatura, como Aurora ou XeXMenu.
- PC com Linux Debian ou Windows 10/11 com WSL 2 (Debian).
- xsdk e sua toolchain GCC para `powerpc-xsdk-elf`.
- CMake 3.25 ou mais recente e Ninja.
- Um a quatro controles conectados ao Xbox 360.

O xsdk e um projeto separado, distribuido sob MIT. Consulte o README e a documentacao dele: <https://github.com/xenosworks/xsdk>.

## Compilar no Windows com WSL 2

### 1. Instalar Debian e os pacotes

No PowerShell do Windows:

```powershell
wsl --install -d Debian
```

Abra Debian e instale as dependencias:

```bash
sudo apt update
sudo apt install -y build-essential bison flex texinfo libgmp-dev libmpfr-dev libmpc-dev curl xz-utils patch python3 cmake ninja-build git
```

### 2. Baixar e compilar o xsdk

```bash
git clone https://github.com/xenosworks/xsdk.git ~/xsdk
cd ~/xsdk
PREFIX=$HOME/xsdk-gcc WORK=$HOME/xsdk-gcc-work JOBS=$(nproc) tools/toolchain/build.sh
make -j$(nproc)
```

A construcao da toolchain leva espaco em disco e pode demorar. Siga o guia atual do xsdk caso algum comando tenha mudado:
<https://github.com/xenosworks/xsdk/blob/main/docs/GETTING_STARTED.md>.

### 3. Copiar e compilar o PadCheck

Copie a pasta `padcheck360-v0.2` para sua pasta pessoal no Debian. Depois, dentro do Debian, rode:

```bash
cd ~/padcheck360-v0.2
chmod +x ./build-xex.sh
./build-xex.sh
```

O script verifica se o xsdk, a biblioteca e o compilador estao instalados, configura o CMake e compila o aplicativo. Se voce instalou o xsdk em outro local, defina `XSDK_DIR` e `XSDK_TOOLCHAIN`, por exemplo:

```bash
XSDK_DIR=$HOME/xsdk XSDK_TOOLCHAIN=$HOME/xsdk-gcc ./build-xex.sh
```

O arquivo gerado deve ficar em:

```text
build/padcheck360.xex
```

### 4. Copiar para o Xbox 360

Copie `padcheck360.xex` para o HD ou pendrive FAT32. Pela Aurora ou XeXMenu, abra o arquivo para iniciar o aplicativo. Tambem e possivel usar FTP se o servidor FTP da dashboard estiver ativado.


## Compilar sem instalar Debian no seu PC (GitHub Actions)

Este pacote inclui um fluxo de compilacao na nuvem em `.github/workflows/build-xex.yml`. Assim, voce nao precisa instalar Debian, WSL ou o compilador no Windows: o GitHub Actions usa uma maquina Ubuntu temporaria para tentar compilar o `.xex`.

1. Crie um repositorio no GitHub e envie todos os arquivos desta pasta para ele, mantendo a pasta `.github/workflows`.
2. Abra a aba **Actions** do repositorio.
3. Selecione **Build PadCheck 360 XEX** e clique em **Run workflow** (ou envie uma alteracao na branch `main`/`master`, que tambem inicia a compilacao).
4. Quando a execucao terminar com sucesso, abra a execucao e baixe o artefato **PadCheck360-v0.2.0-XEX** na secao **Artifacts**.

O GitHub executa o compilador em Linux na nuvem; a diferenca e que voce nao instala nem configura Debian no seu proprio computador. A primeira compilacao precisa baixar e compilar a toolchain, por isso pode demorar. O artefato fica disponivel por 14 dias.

**Importante:** este fluxo foi preparado, mas ainda nao foi executado no seu repositorio GitHub. Portanto, nao posso garantir que a primeira compilacao passe sem ajustes. Mesmo que compile, o `.xex` ainda precisa ser testado no Xbox 360 RGH real.

## Como usar

1. Ligue os controles que deseja testar. O topo da tela indica P1, P2, P3 e P4 como online/offline.
2. Use o direcional esquerdo/direito do primeiro controle conectado para escolher qual jogador aparece no painel detalhado.
3. Aperte cada botao do controle selecionado. O indicador muda para `OK` depois que o botao for detectado.
4. Aperte LT e RT. Cada gatilho conta como testado depois de ultrapassar 32/255; o aplicativo tambem guarda o valor maximo observado.
5. Para testar vibracao, segure Y (motor esquerdo) e X (motor direito). Solte para parar.
6. Para testar o centro dos analogicos, deixe os dois sticks em repouso, segure START por aproximadamente 0,75 segundo, solte START e nao toque nos sticks ate a amostra terminar.
7. Repita o teste de drift para cada jogador.
8. Para sair, segure START + BACK por aproximadamente 2 segundos em qualquer controle conectado.

## Como interpretar o relatorio

- `Entradas testadas`: quantidade de botoes e gatilhos diferentes que ja foram acionados pelo menos uma vez. O total e 16 (14 botoes digitais + 2 gatilhos).
- `Centro estavel durante esta amostra`: nenhum eixo ultrapassou o limite configurado durante a amostra. Isso nao garante que o controle esteja perfeito.
- `Movimento detectado - verifique possivel drift`: algum eixo passou do limite. Repita o teste sem tocar no controle para descartar movimento acidental.
- A barra e os valores dos eixos mostram a posicao atual, nao uma nota de qualidade.

## Limitacoes conhecidas

- O botao Guide/Home e reservado ao sistema do Xbox e nao faz parte da lista de 16 entradas.
- A navegacao entre jogadores usa o direcional do primeiro controle conectado. Pelo menos um controle precisa estar conectado para trocar o painel detalhado.
- O relatorio e exibido na tela; esta versao nao exporta arquivo de resultados.
- O limite de drift e uma heuristica simples configuravel, nao um diagnostico eletrico ou uma medicao certificada.
- O codigo precisa ser compilado com uma versao compativel do xsdk e testado em hardware real. Nao afirmamos que exista um `.xex` validado nesta entrega.

## Estrutura

```text
padcheck360-v0.2/
  main.c
  CMakeLists.txt
  README.md
  CHANGELOG.md
  LICENSE
  .gitignore
```

## Codigo aberto

Este projeto e distribuido sob a licenca MIT (veja `LICENSE`). O xsdk e uma dependencia separada e mantem sua propria licenca e avisos.

- xsdk: <https://github.com/xenosworks/xsdk>
- Guia de inicio do xsdk: <https://github.com/xenosworks/xsdk/blob/main/docs/GETTING_STARTED.md>
- Guia de API do xsdk: <https://github.com/xenosworks/xsdk/blob/main/docs/GUIDE.md>

PadCheck 360 nao e afiliado, patrocinado ou endossado pela Microsoft. Xbox e Xbox 360 sao marcas da Microsoft.

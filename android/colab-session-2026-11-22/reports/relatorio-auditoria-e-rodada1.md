# GoW Ascension Recomp — Android Port
## Colab como estação de processamento: auditoria e rodada 1

Data: 22/11/2026 · Commit avaliado: `dc38f15` (branch `android/lift-pipeline` = `colab/lift-preflight`)

**Conclusão principal:** o único trabalho realmente pesado do projeto é compilar o código liftado do jogo, e ele depende do EBOOT. Todo o resto que existe hoje sem o jogo roda neste Colab em cerca de 12 minutos de CPU no total.

## 1. Ambiente

| Item | Valor |
|---|---|
| Máquina | Google Colab, 2 vCPU Xeon 2,2 GHz, 12,67 GB de RAM, **sem swap**, 108 GB de disco, Ubuntu 24.04 |
| Repositório | branch `colab/lift-preflight` = `android/lift-pipeline` = commit `dc38f15`; working tree limpo |
| Isolamento | tudo rodou a partir de um snapshot `git archive` em `/root/work/colab-station`; o repositório não foi alterado; sem commit, merge ou push |
| Ferramentas instaladas | Python 3.13 em venv (capstone, pycryptodome, construct, tabulate, tqdm), ninja 1.11, cmake 3.31, clang 18, qemu-user 8.2, Android NDK r29 (29.0.14206865), SDL3 3.4.12 para Android arm64 |
| Persistência | tudo em `/root/work/colab-station` some quando a VM é reciclada; resultados empacotados em `results-wave1-dc38f15.tar.gz` (3,8 MB, nada derivado do jogo) |

## 2. Pipeline de lift (`android/lift`)

| Etapa | Script | Precisa do EBOOT | Retomável |
|---|---|---|---|
| Selftest (testes do lifter e 1.202 casos de conformidade PPU) | `selftest.sh` | não | sim |
| 1. Extração dos job modules SPU | `prepare.sh` | sim | sim |
| 2. Tabela de patches | `prepare.sh` | sim | refaz sempre (é barato) |
| 3. Tradução PPU (19.199 funções, ~1,3 GB de C++ em ~12 arquivos) e hooks | `prepare.sh` | sim | **não**: só roda se `ppu_recomp.h` não existir, e esse header é gravado antes dos `.cpp` |
| 4. Extração dos programas SPU | `prepare.sh` | sim | refaz sempre |
| 5. Tradução SPU | `prepare.sh` | sim | sim |
| 6. Compilação arm64 (`libgow_ppu.a`, `libgow_spu.a`) | `build_lifted.sh` | precisa do lift | sim, via ninja, se o diretório de build persistir |

Requisitos do README para compilar o lift: 16 GB de RAM ou mais (32 GB é o confortável), 8 núcleos ou mais, 40 GB de disco, e 2 a 4 GB de RAM por arquivo PPU.

## 3. Inventário de tarefas sem o EBOOT

| Categoria | Tarefas |
|---|---|
| Pesadas em CPU | análise estática (~15 min de CPU); build do APK da branch `m3-app-swapchain` (ainda não rodada) |
| Pesadas em RAM | nenhuma sem o jogo (pico medido: 182 MB) |
| Pesadas em disco | NDK (2,4 GB); SDK e Gradle do APK (estimativa de ~3 GB) |
| Longas e retomáveis | análise estática (resultado salvo por arquivo); builds com ninja |
| Leves, não justificam o Colab | suíte Python, conformidade PPU, build do runtime, teste VMX |
| Bloqueadas | lift e compilação do lift (EBOOT); `run_torture.py` e `lift_selftest.py` (compilador proprietário `ppu-lv2-gcc`); renderer Vulkan (GPU Adreno); medições de desempenho (celular) |

## 4. Execuções (commit `dc38f15`)

| ID | Tarefa | Tempo real | CPU | Pico RAM | Resultado | Situação |
|---|---|---|---|---|---|---|
| T1 | Suíte Python do ps3kit (65 scripts) | 97 s | 86% | 172 MB | 33 passam, 2 pulados (só macOS), 30 falham | executado |
| T2 | Conformidade PPU/VMX | 30 s | 95% | 182 MB | **1.533/1.533** checagens passam em x86 (gcc/clang, -O1/-O2) e arm64 NDK (-O1/-O2) sob qemu `cortex-a76`; `selftest.sh` sai com 0 | **validado por execução** |
| T3a | SDL3 3.4.12 para Android arm64 | 80 s | 126% | 127 MB | `libSDL3.so` gerada | só compilado |
| T3b | `android/build.sh` (runtime, 169 alvos) | 51 s | 127% | 124 MB | `libps3kit_android.a` (15,8 MB) | compilado e com teste de link |
| T4 | `vmx_neon_test` (VMX → NEON) | 34 s | 94% | 53 MB | 0 divergências em 3 milhões de vetores por operação, NJ ligado e desligado | validado em qemu; ns/op não vale como benchmark |
| T6 | clang-analyzer (NDK r29) em 167 arquivos | 446 s | 147% | 155 MB | 37 achados | triagem parcial |
| T7 | `prepare.sh --dry-run` e guard do `build_lifted.sh` | < 1 s | — | — | tudo OK até o EBOOT ausente | validado |

O qemu comprova a semântica das instruções ARM, não o desempenho nem a temporização do Cortex-A78 real.

## 5. Problemas encontrados (nenhum foi corrigido)

| # | Problema | Impacto |
|---|---|---|
| 1 | `build-lifted/` fora do `.gitignore` | risco de commitar código compilado do jogo |
| 2 | Passo 3 do `prepare.sh` não é retomável (checa `ppu_recomp.h` em vez de `.lifted-from`) | lift interrompido vira árvore incompleta sem aviso |
| 3 | 30 testes Python falham no Linux: 8 por código só de MSVC (`__declspec`, `SRWLOCK`), 4 por símbolos novos do fork (`spu_lockline_*`, `spu_lifted_probe`), 1 confirmado por `mpnet_*` mais 6 com fixture que não compila, 2 pelo preâmbulo do lifter (`vm_write*_inl`), os demais por outras divergências do fork | testes do host quebrados pelo fork, não pelo Colab |
| 4 | `test_ppu_lift.py` não compila no POSIX (`PPU_LEAN_CONST` indefinido); `selftest.sh` contorna com `-DPPU_LEAN_CONST=` | conformidade só roda via `selftest.sh` |
| 5 | Link do runtime: 17 símbolos indefinidos sem stubs, entre eles `__ImageBase` (símbolo do Windows, em `spu_channels.c:2347` e `spu_drain.c:111`) e `vm_base` | o port precisa fornecê-los |
| 6 | `port_stubs.c` duplica 16 funções `psnr_*` (glob recursivo do commit 4dba99e) | conflito de link |
| 7 | 37 achados do analyzer: 15 em terceiros (stb, at3), 22 no ps3kit (`spu_channels.c:597`, `win32_compat.c:793`, `cellSaveData.c`, `mpnet.c`...) | candidatos, nenhum confirmado como bug |
| 8 | `build_lifted.sh` usa 3 jobs aqui (RAM/4); 3 × 2-4 GB fica perto de 12,67 GB | risco de OOM; usar `LIFT_JOBS=1` ou `2` |
| 9 | `--dry-run` cria diretórios quando o EBOOT existe | menor |

## 6. Gargalos e uso do hardware

- A RAM nunca passou de 182 MB; o gargalo aqui são as **2 vCPUs** (uso de 86% a 147%).
- O limite de 12,67 GB só importa na compilação do código liftado.
- Retomada aproveitada: a análise estática pula arquivos já analisados, o ninja recompila só o que mudou, e nenhum resultado foi sobrescrito.

## 7. Limitações do Colab

- 2 vCPUs são o gargalo; a RAM só importa na compilação do lift.
- Sem swap: estouro de memória aciona o OOM killer.
- A sessão cai por inatividade (cerca de 90 min) ou pelo limite máximo (cerca de 12 h).
- Disco efêmero: é preciso baixar os resultados ou usar o Google Drive.
- Levar o EBOOT ou o código liftado ao Colab ou ao Drive é decisão do dono do projeto, por causa das regras do projeto sobre serviços de terceiros.

## 8. Viabilidade

| Fase | Viabilidade |
|---|---|
| Ferramentas, testes e runtime | alta (já feito) |
| Lift (tradução em Python) | provável; a memória do processo nunca foi medida (estimativa de 3 a 6 GB) |
| Compilação completa do lift | baixa a média numa única sessão; provavelmente exige várias sessões e build persistente |

## 9. Próximos passos (aguardando decisão)

- **A. Sem alterar código:** build do APK (`m3-app-swapchain`); build Linux do ps3kit com `ctest` (`clang -fdeclspec`); build do executor de cenas do renderer (só compilação, sem GPU).
- **B. Correções numa branch nova a partir de `colab/lift-preflight`:** `.gitignore` de `build-lifted/`, retomada do passo 3, conflito `psnr_*`, `__ImageBase` no Android. `PPU_LEAN_CONST` é do ps3kit, que só é modificado com autorização.
- **C. Lift real:** depende de o EBOOT poder entrar no Colab; antes, medir a memória só da etapa de tradução.

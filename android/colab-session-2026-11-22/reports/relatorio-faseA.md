# GoW Ascension Recomp — Android Port
## Fase A: infraestrutura de build (relatório)

Data: 22/11/2026 · Commit-base: `dc38f15` · Branch de trabalho: `colab/phase-a-fixes` (worktree `/root/work/ascension-phaseA`)

**Resumo:** os cinco itens estão corrigidos. O mecanismo de cada um foi validado com testes que falhavam antes e passam depois. A medição real do lift e da compilação continua dependendo do EBOOT.

## 1. Segurança e rastreabilidade

| Item | Situação |
|---|---|
| Commit-base | `dc38f15` confirmado: igual a `colab/lift-preflight`, `android/lift-pipeline` e `origin/android/lift-pipeline`; sem commits novos no remoto; working tree limpo |
| Isolamento | branch nova `colab/phase-a-fixes` criada a partir de `dc38f15` num worktree separado; `colab/lift-preflight` intacta |
| PS3Kit | idêntico ao commit-base (0 linhas de diferença; arquivos gerados pelos testes removidos) |
| Commit, merge, push | nenhum |
| Resultados da rodada 1 | cópia somente leitura em `/root/work/colab-station/preserved`, SHA256 conferido |
| Patch da Fase A | `/root/work/colab-station/phaseA/phaseA-vs-dc38f15.patch` (SHA256 `881c4238…fe3db2`) |
| Arquivos alterados | `.gitignore`, `android/host/port_stubs.c`, `android/lift/build_lifted.sh`, `android/lift/prepare.sh` (95 inserções, 51 remoções) |
| Arquivos novos | `android/lift/lift_stage.sh`, `android/lift/mem_guard.sh`, `android/lift/tests/test_stage.sh`, `android/lift/tests/test_mem_guard.sh`, `android/tests/check_link.sh` |

## 2. Resultado por item

| Item | Problema | Causa raiz | Alteração | Testes | Estado |
|---|---|---|---|---|---|
| **A1** | `build-lifted/` não era ignorado | **comprovada:** os dois scripts criam a pasta na raiz e nenhuma regra a cobria | `.gitignore`: `/build-lifted*/` | antes: `?? build-lifted/...`; depois: 4 casos ignorados, `git add -A` só pega `.gitignore`, regras antigas intactas; nenhum arquivo dessa pasta estava rastreado | **CORRIGIDO E VALIDADO** |
| **A2** | passo 3 não retomável | **comprovada:** o header é gravado antes dos `.cpp`, e 4 dos 6 hooks regravam os chunks no lugar, sem `.tmp` (achado novo). O passo 5 (SPU) tinha o mesmo defeito | `lift_stage.sh`: staging → checagem → manifesto SHA1 → carimbo → promoção atômica, guardando o resultado anterior em `.prev`; passos 3 e 5 do `prepare.sh` usam essa biblioteca | `test_stage.sh`: **38/38** (detalhes na seção 3). O caso "antes" foi demonstrado: o gate original aceita a árvore interrompida | **CORRIGIDO E VALIDADO** (mecanismo); com o lifter real: **BLOQUEADO** (EBOOT) |
| **A3** | 16 `psnr_*` duplicadas | **comprovada:** o `psnr.c` é a implementação real e multiplataforma, já dentro do runtime; os stubs eram redundantes, tinham assinaturas variádicas incompatíveis e um comentário falso | removidos os 16 stubs de `android/host/port_stubs.c` (código do port, não do ps3kit) | build limpo + link completo com `--whole-archive --no-undefined`: antes 16 duplicadas; depois **0 duplicadas e 0 indefinidas** com os stubs de bring-up; as 17 `psnr_*` vêm do `psnr.c.o`; nenhuma outra sobreposição entre as 3 bibliotecas | **CORRIGIDO E VALIDADO** |
| **A4** | 3 jobs no Colab | **comprovada:** o padrão era RAM/4 = 3 | `mem_guard.sh`. `build_lifted.sh` usa 1 job por padrão; 2 ou mais só com pico medido e margem. Vigia de RAM que para a build antes do OOM. Marcador `.build-complete`. Logs de memória por build. Recusa diretório de build não ignorado dentro do repositório | `test_mem_guard.sh`: **33/33** (detalhes na seção 3) | **CORRIGIDO E VALIDADO** (mecanismo); valores reais de memória: **BLOQUEADO** (EBOOT) |
| **A5** | `--dry-run` alterava o estado | **comprovada:** `mkdir` de 4 diretórios (incluindo `src/spu`) e `mktemp` | todas as criações condicionadas ao modo real | comparação do estado completo antes e depois: o original criou 5 diretórios; o corrigido não cria nada | **CORRIGIDO E VALIDADO** |

## 3. Detalhes dos testes

**`test_stage.sh` (38/38):** interrupções reais por SIGKILL, sem chance de limpeza:
- no header, no meio dos chunks, dentro dos hooks (chunk truncado) e entre os dois `rename` da promoção;
- a retomada gera um resultado idêntico byte a byte a uma execução sem interrupção;
- uma segunda execução sem mudanças é pulada sem tocar nos arquivos (idempotente);
- 8 tipos de árvore inconsistente são detectados: chunk truncado, chunk ausente, `.tmp` sobrando, sem carimbo, árvore antiga sem manifesto, entradas diferentes, só header, log do lifter sem relatório de chunks;
- o resultado anterior completo é preservado como `.prev`, e um incompleto como `.incomplete`, nunca apagado.

**`test_mem_guard.sh` (33/33):**
- planejamento de jobs: 1 job por padrão, 2 recusado sem medição, recusado quando a margem não fecha, aceito quando fecha;
- parada controlada em ≤ 5 s, sem processos órfãos, inclusive filhos em outros grupos ou sessões;
- RSS de um clang real medido (104 MB);
- integração com o `build_lifted.sh` numa árvore sintética rotulada (dois arquivos triviais, não código do jogo): parada pelo vigia, `.build-complete` removido e retomada bem-sucedida.

**Regressão geral:**

| Verificação | Resultado |
|---|---|
| Sintaxe (`bash -n`) dos 8 scripts | OK |
| shellcheck (erros) | nenhum; avisos: 0 antes e 0 depois |
| `selftest.sh` a partir da árvore corrigida | passa |
| Conformidade PPU arm64 (qemu cortex-a76) | **1.533/1.533** |
| `--dry-run` sem EBOOT | comportamento original mantido (mensagem, saída 0) |
| Contador `oom_kill` do Colab | 0 (nenhum processo morto por falta de memória) |

## 4. Bugs encontrados nos meus próprios testes (corrigidos)

| Bug | Correção |
|---|---|
| O manifesto se incluía na própria lista; nenhuma árvore recém-selada verificava | manifesto calculado antes de qualquer arquivo de selo existir |
| A primeira versão do vigia deixava processos órfãos (o ninja coloca cada compilador num grupo de processos próprio) | o vigia percorre a árvore de processos inteira antes de sinalizar |
| O vigia esperava 10 s por um processo zumbi | zumbis deixaram de contar como vivos |

## 5. Riscos que continuam

- O passo 1 (extração dos job modules) ainda é controlado por um único arquivo. É rápido, mas não está protegido.
- O `prepare.ps1` do Windows tem o mesmo defeito do passo 3. Não foi alterado porque não pode ser testado aqui.
- O hash das ferramentas no carimbo cobre só os imports diretos. `--force` continua disponível.
- Árvores antigas sem manifesto serão refeitas, e a antiga fica guardada como `.incomplete`. No pior caso, o disco guarda 3 cópias de ~1,3 GB.
- O limite de jobs vale também para a compilação SPU, que fica mais lenta.
- A amostragem de memória do vigia é de 1 s.
- Os 17 símbolos indefinidos do runtime continuam pendentes (item B3).

## 6. Decisões antes da Fase B

1. **Commit:** registrar a Fase A como um commit local em `colab/phase-a-fixes`, sem push, como ponto de retorno antes das mudanças da Fase B?
2. **B2 (`PPU_LEAN_CONST`):** a solução será procurada primeiro fora do ps3kit, na configuração de compilação ou na camada do port. Se a única correção adequada for no gerador do teste (`ps3kit/tools`), o trabalho para e pede autorização.

# Lume

> Lume carrega o contexto para que a pessoa não precise carregar o sistema.

Lume é um runtime contextual pessoal, local e C++26-first. O primeiro protótipo
implementa uma relação pequena, mas completa: ele preserva uma expressão humana,
deriva uma intenção quando há base suficiente, observa o tempo, decide entre falar
e permanecer em silêncio, aceita adiamento e explica sua decisão.

O princípio arquitetural central é:

> O modelo de linguagem fala com a pessoa; o Lume decide sobre o sistema.

Um provedor de linguagem só pode devolver candidatos e formular texto. O core
valida o candidato, resolve referências temporais, registra autoridade e
proveniência e é o único componente que altera o estado canônico. A integração
HTTP OpenAI-compatible usa um Ollama ou `llama-server` local quando disponível.
O fallback determinístico continua funcionando caso o servidor não exista, falhe
ou devolva uma proposta inválida.

## Executar

Requer CMake 3.25+ e um compilador com modo C++26. A integração local é compilada
quando `libcurl` e `nlohmann/json.hpp` estão disponíveis; use
`-DLUME_WITH_LOCAL_LLM=OFF` para um build sem essas dependências.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Uma conversa interativa começa com:

```bash
./build/lume
```

### Interface web

Depois de compilar o runtime, a experiência web conectada começa com:

```bash
cd web
npm install
npm run dev:local
```

Abra `http://localhost:3000`. A interface conversa com o mesmo core e estado da
CLI por uma ponte exclusivamente local. Nenhum contexto é enviado a um serviço
hospedado. `npm run dev` inicia apenas uma prévia local, sem estado canônico.

Para validar também o contrato entre a interface e o runtime:

```bash
cd web
npm test
npm run test:local
```

Para ver qual camada linguística foi selecionada:

```bash
./build/lume doctor
```

Para instalar o serviço Ollama isolado do Lume e preparar o modelo recomendado:

```bash
./scripts/setup-local-llm.sh
./build/lume doctor
```

O script usa `qwen2.5:3b` por padrão. Para escolher outro modelo já compatível:

```bash
LUME_SETUP_MODEL=qwen2.5:7b ./scripts/setup-local-llm.sh
```

Em CPU, a primeira interação também carrega o modelo e pode levar dezenas de
segundos; as seguintes reutilizam o processo aquecido. Se o prazo configurado for
excedido, o Lume responde pelo fallback sem perder a expressão original.

Por padrão o Lume procura primeiro seu serviço dedicado em
`http://127.0.0.1:11435/v1` e depois o Ollama convencional em
`http://127.0.0.1:11434/v1`. Ele descobre o primeiro modelo por `/v1/models` e
mantém todo o tráfego em loopback. Para `llama-server` ou outra configuração local:

```bash
export LUME_LLM_URL=http://127.0.0.1:8080/v1
export LUME_LLM_MODEL=meu-modelo-local
export LUME_LLM_TIMEOUT_MS=60000
```

`LUME_LLM=off` força o fallback. URLs que não sejam loopback são rejeitadas para
que contexto pessoal não saia da máquina por configuração acidental.

Em máquinas onde a detecção automática de GPU do Ollama falha, o projeto inclui
duas opções em `contrib/ollama`: um serviço de usuário isolado na porta 11435 e um
drop-in para corrigir o serviço global. Essas são configurações da máquina, não
exigências do runtime.

Para remover somente o serviço dedicado:

```bash
systemctl --user disable --now lume-ollama.service
rm ~/.config/systemd/user/lume-ollama.service
systemctl --user daemon-reload
```

O ciclo completo de intenções e orquestração de planos também pode ser observado deterministicamente pela CLI:

```bash
STATE=/tmp/lume-demo.ledger
./build/lume --state "$STATE" --at 2026-09-21T18:00 say \
  "Amanhã de manhã quero trabalhar no artigo."
./build/lume --state "$STATE" --at 2026-09-21T18:01 plan \
  "Organiza minha manhã"
./build/lume --state "$STATE" --at 2026-09-21T18:02 apply-plan 4
./build/lume --state "$STATE" --at 2026-09-22T09:00 observe
./build/lume --state "$STATE" --at 2026-09-22T09:01 observe
./build/lume --state "$STATE" --at 2026-09-22T09:02 reply \
  "Sim, mas daqui a uma hora."
./build/lume --state "$STATE" --at 2026-09-22T10:02 observe --explain
./build/lume --state "$STATE" inspect
```

A persistência do Lume opera sobre um **Event Ledger em SQLite com WAL (Write-Ahead Logging)** com validação estrita (`LUME-HARDENING-001`), transações ACID, locks reentrantes thread-safe, separação entre autoridade e classes epistêmicas ([LUME-BOOTSTRAP-001](docs/spec/LUME-BOOTSTRAP-001.md)), detecção de planos obsoletos e uma experiência factual em duas superfícies ([LUME-EXPERIENCE-001](docs/spec/LUME-EXPERIENCE-001.md)).

Para executar o monitoramento residente do relógio e disparos de automações:

```bash
./build/lume daemon --interval-sec 30
```

## Limites honestos desta versão

O fallback reconhece expressões explícitas como “amanhã de manhã quero …” e pedidos estruturados de planejamento (“organiza minha manhã”, “organiza minha semana”). O modelo local entende variações linguísticas, mas o core C++ aceita apenas formas estruturadas validadas contra schema estrito. Nenhuma proposta é aplicada sem consentimento explícito registrado no Ledger.

## Licença

O Lume é distribuído sob a [GNU General Public License v3.0](LICENSE), exclusivamente na versão 3 (`GPL-3.0-only`).

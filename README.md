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
proveniência e é o único componente que altera o estado canônico. Nesta primeira
versão existe um provedor determinístico pequeno. A interface `LanguageProvider`
é o ponto de encaixe para um modelo local via `llama.cpp`; o fallback continua
funcionando caso esse modelo não exista, falhe ou devolva uma proposta inválida.

## Executar

Requer CMake 3.25+ e um compilador com modo C++26.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Uma conversa interativa começa com:

```bash
./build/lume
```

O ciclo completo também pode ser observado deterministicamente pela CLI:

```bash
STATE=/tmp/lume-demo.state
./build/lume --state "$STATE" --at 2026-09-21T18:00 say \
  "Amanhã de manhã quero trabalhar no artigo."
./build/lume --state "$STATE" --at 2026-09-22T09:00 observe
./build/lume --state "$STATE" --at 2026-09-22T09:01 observe
./build/lume --state "$STATE" --at 2026-09-22T09:02 reply \
  "Sim, mas daqui a uma hora."
./build/lume --state "$STATE" --at 2026-09-22T10:02 observe --explain
./build/lume --state "$STATE" inspect
```

A segunda observação equivalente produz `NO_INTERACTION`: silêncio é uma decisão,
não uma ausência de implementação.

Por padrão, o estado fica em `$XDG_STATE_HOME/lume/state.lume` ou
`~/.local/state/lume/state.lume`. `--state` e `LUME_STATE_FILE` permitem isolar
experimentos.

## Limites honestos desta versão

O fallback reconhece intencionalmente apenas a construção “amanhã de manhã quero
…”. Expressões fora dessa gramática são preservadas literalmente, mas não viram
uma intenção acionável. Isso mantém incompletude como estado válido e evita
simular compreensão. O próximo incremento natural é um adaptador local de
`llama.cpp` que implemente o mesmo contrato, com schema estrito, timeout e retorno
automático ao fallback.


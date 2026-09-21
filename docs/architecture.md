# Fronteiras do Lume

```text
Pessoa
  ↕
LanguageProvider              linguagem, interpretação candidata, formulação
  ↕ candidatos / texto
Assistant (core)              validação, contexto, decisão, autoridade
  ↕ entidades aceitas
Store                         fatos, continuidade, proveniência, witness
```

## Contrato de autoridade

`LanguageProvider::interpret` retorna `InterpretationCandidate`. Isso é uma
proposta sem autoridade. O core aceita apenas tipos, restrições temporais e nível
de confiança conhecidos. Propostas desconhecidas são rejeitadas; a expressão
original continua preservada.

O contexto entregue ao provedor é uma projeção mínima (`ContextProjection`), não
o estado completo. Uma formulação vazia ou inválida pode ser substituída por texto
determinístico do core.

## Estado factual

Cada intenção aponta para a expressão que a originou e registra:

- texto/assunto derivado;
- janela temporal resolvida pelo core;
- precisão;
- autoridade (`user`);
- origem da interpretação;
- status e última interação.

Cada interação registra a razão factual e a origem da formulação. `lume inspect`
expõe essa projeção em JSON; `lume why` explica a interação mais recente.

## Próxima fronteira: `llama.cpp`

O adaptador local deve:

1. receber somente a expressão e a projeção mínima necessária;
2. produzir um candidato validado contra schema fechado;
3. operar com prazo e cancelamento;
4. nunca receber uma referência mutável ao `Store`;
5. retornar ao `DeterministicLanguage` em falha, timeout ou saída inválida.

Modelo, prompt e versão deverão integrar a proveniência. Trocar o modelo não muda
a ontologia nem a autoridade do core.


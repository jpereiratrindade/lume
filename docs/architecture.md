# Arquitetura e Fronteiras do Lume

## 1. Visão Sistêmica

```text
                       PESSOA
                          ↕
                  INTERAÇÃO NATURAL
                          ↕
                  LanguageProvider
                          │
                  candidatos / texto
                          ↓
                 ┌─────────────────┐
                 │   LUME CORE     │
                 │                 │
                 │ contexto        │
                 │ autoridade      │
                 │ decisão         │
                 │ planejamento    │
                 │ relevância      │
                 └────────┬────────┘
                          │
                       eventos
                          ↓
                 ┌─────────────────┐
                 │ EVENT LEDGER    │
                 │ append-only     │
                 └────────┬────────┘
                          │
                       replay
                          ↓
                     PROJEÇÕES
                    ↙    ↓    ↘
                 CLI    WEB   lumed
```

---

## 2. Contratos Fortes de Fronteira

### 2.1. LanguageProvider (O Modelo Propõe; o Lume Decide)
- **Schema Fechado e Loopback**: O provedor linguístico (ex: LLM local via `127.0.0.1`) é executado em loopback isolado e devolve candidatos estritos (`InterpretationCandidate`, `PlanProposalCandidate`, `FormulationResult`).
- **Sem Acesso ao Estado**: O modelo nunca recebe referências mutáveis ao estado ou ao Ledger.
- **Fallback Determinístico**: Circuit breaker ativo e fallback transparente para `DeterministicLanguage` em caso de timeout, falha ou violação de esquema.
- **Proveniência Preservada**: Algoritmos e heurísticas registram suas fontes originais (`deterministic-planner`, `core-fallback`, etc.).

### 2.2. Ledger Imutável e Integridade Concorrente
- **Append-Only & Replay**: O estado canônico é derivado por replay determinístico da sequência ordenada de registros de eventos.
- **Gestão Monotônica Exclusiva de Sequência**: O `Ledger` atribui números de sequência crescentes estritos. Callers nunca atribuem sequence numbers manuais.
- **Concorrência Transacional e Multithread Safe**: Operações mutantes (`say`, `observe`, `reply`, `plan`, `apply_plan`, `discard_plan`, `tick`) executam sob transações atômicas `with_exclusive_lock`, com lock de arquivo e reentrância thread-safe com notificação entre threads (`condition_variable`).
- **Rejeição Estrita (`REJECT`)**: Corrupções de linha, contagem incorreta de campos, tipos desconhecidos ou sequências fora de ordem disparam exceções imediatas, garantindo que o estado factual nunca seja fabricado.
- **Permissões POSIX**: Diretórios criados com `0700` e arquivos/locks com `0600`.

### 2.3. Matriz de Autoridade e Epistemologia
- **Autoridade**: `user` (pessoa humana), `core` (motor deliberativo), `user_policy` (rotina autorizada).
- **Epistemologia**: `user_declared` (literalidade da declaração), `derived` (inferência estruturada a partir de fato primário), `observed` (observação factual de ambiente/horário), `proposed` (rascunho de plano aguardando aprovação).
- **Contexto Estruturado**: `ContextItem` distingue fatos de intenções ainda sem janela temporal. Esses itens preservam expressão de origem, confiança e proveniência, mas não entram no motor de atenção até que exista base temporal validada.

### 2.4. Planejamento Ontológico e Detecção de Obsoleto (Staleness)
- **Preservação de Intenções**: Aplicar um plano aloca `allocated_plan_start/end` sem apagar a declaração temporal original do usuário.
- **Não-Sobreposição**: Blocos alocados respeitam intervalos e nunca se sobrepõem no mesmo horizonte.
- **Detecção de Staleness**: `PlanProposal` armazena `as_of_sequence` e `basis_intentions_digest`. Se o contexto das intenções mudar antes da aplicação, o sistema rejeita com `PLAN_STALE` e orienta a geração de uma nova proposta.

### 2.5. Atenção Factual (Attention Engine)
- Projeção factual computada pelo Core (`compute_attention_candidate`) baseada na janela ativa atual e ausência de interação recente.
- A interface gráfica consome essa projeção factual sem heurísticas fictícias.

---

## 3. Experiência Humana em Duas Superfícies
1. **Agora**: silêncio legível, presença pertinente ou intervenção justificada; diálogo como entrada principal.
2. **Sob cuidado**: revisão e correção sob demanda. Orquestração e exploração permanecem recolhidas até serem pedidas.

Os estados de atenção (`silêncio → presença → intervenção`) são independentes das capacidades (`presença → orquestração → exploração`). A arquitetura pode ganhar complexidade sem transferi-la para a superfície primária.

---

## 4. Daemon Residente (`lumed`)
- Subcomando `lume daemon [--interval-sec N]` ou serviço residente para monitoramento contínuo do relógio local, avaliação de condições de disparo de automações (`tick`) e proatividade sem necessidade de interação manual.

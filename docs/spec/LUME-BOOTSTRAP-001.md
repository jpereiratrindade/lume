# LUME-BOOTSTRAP-001: Princípios Fundacionais e Arquitetura do Lume

## 1. Visão do Sistema
O **Lume** é um assistente contextual local e um runtime pessoal proativo. Ele não é um chatbot convencional nem uma lista passiva de tarefas. O sistema foi concebido para coexistir no ambiente do usuário, observando transições temporais, preservando expressões em sua literalidade e agindo apenas dentro da autoridade explicitamente concedida.

---

## 2. Princípios Canônicos

### 2.1. O Modelo Propõe; o Lume Decide
Provedores de linguagem (sejam heurísticos ou Large Language Models locais) operam estritamente como geradores de candidatos estruturados via esquemas fechados em loopback (`127.0.0.1`).
O modelo linguístico **nunca** possui referência mutável ao estado ou autoridade para modificar diretamente entidades do sistema. Toda mutação de estado requer validação ontológica e decisão pelo Core em C++26.

### 2.2. Incompletude Válida e Serenidade
O assistente não força o usuário a preencher campos desnecessários. Uma expressão vaga como *"Amanhã quero voltar nisso"* é perfeitamente válida:
1. É registrada no ledger imediatamente como expressão declarada pelo usuário (`user_declared`).
2. O Lume reconhece a recepção sem ansiedade.
3. A ausência de parâmetros exatos não é tratada como erro, mas como um estado contextual preservado.

### 2.3. Primazia Factual e Rejeição Estrita
O Lume nunca inventa fatos, métricas fictícias, semanas artificiais ou conexões imaginárias:
- Na ausência de intenções, o planejador afirma que o horizonte está livre.
- Na ausência de dados analíticos suficientes, o motor analítico declara ausência de amostra em vez de renderizar dashboards decorativos.
- Qualquer corrupção ou inconsistência no Ledger append-only resulta em rejeição estrita (`REJECT`), nunca em coerção silenciosa ou fabricação de estado.

---

## 3. Matriz Epistêmica e de Autoridade

| Dimensão | Valores Canônicos | Significado no Domínio |
| :--- | :--- | :--- |
| **Autoridade** | `user` | Ação ou consentimento direto da pessoa humana. |
| | `core` | Ação deliberada pelo motor de decisão do Lume. |
| | `user_policy` | Automação configurada pelo usuário com permissão prévia. |
| **Classe Epistêmica** | `user_declared` | Literalidade do que a pessoa declarou (ex: expressão textual). |
| | `derived` | Entidade estruturada deduzida logicamente a partir de um fato primário. |
| | `observed` | Fato observado diretamente no ambiente ou tempo de execução. |
| | `proposed` | Estrutura proposta para validação ou consentimento humano (ex: planos). |
| | `policy_defined` | Regra ou rotina com execução condicional autorizada. |

---

## 4. Ledger Imutável e Integridade Concorrente
O armazenamento de longo prazo é append-only:
- Sequências são monotônicas e geridas com exclusividade pelo `Ledger`.
- Operações atômicas de leitura-projeção-validação-escrita são protegidas por locks com reentrância por thread (`FileLockGuard`).
- As permissões no sistema de arquivos seguem restrições POSIX rigorosas (`0700` para diretórios, `0600` para dados e locks).

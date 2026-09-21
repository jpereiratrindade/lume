# LUME-EXPERIENCE-001: Presença Simples e Contexto sob Cuidado

## 1. Visão Geral
A experiência do Lume é concebida como uma presença contextual, não como um aplicativo que a pessoa precisa administrar. Seu princípio de produto é:

> **O Lume carrega o contexto para que a pessoa não precise carregar o sistema.**

A complexidade pode crescer atrás da interface, mas só se torna visível quando revisão, correção ou consentimento exigem isso.

---

## 2. Dois Eixos Independentes

Estados de atenção e capacidades não formam uma única hierarquia.

```text
POLÍTICA DE ATENÇÃO                 CAPACIDADES
Silêncio                           Presença
Presença pertinente                Orquestração
Intervenção justificada            Exploração
```

- **Silêncio, presença e intervenção** definem quando e com que intensidade o Lume aparece.
- **Presença, orquestração e exploração** definem o que o Lume é capaz de fazer.
- Uma orquestração pode acontecer silenciosamente. Uma exploração iniciada pela pessoa não é uma intervenção.

### Superfície Primária: Agora
- **Propósito**: O momento imediato. Foco sereno, abertura de diálogo e visibilidade do estado de atenção atual.
- **Contrato de Atenção Factual**: A interface só anuncia que algo é pertinente agora se o Core emitir um `AttentionCandidate` factual cuja janela temporal coincida com o momento presente. Na ausência de contexto ativo para a hora atual, a superfície exibe tranquilidade:
  > *"Por enquanto, nada pede tua atenção."*
- **Entrada Única**: A linguagem natural é a porta principal. Atalhos de capacidade não competem com o diálogo.
- **Silêncio Legível**: Na ausência de relevância, a interface diz apenas: *"Por enquanto, nada pede tua atenção."*

### Superfície Secundária: Sob Cuidado
- **Propósito**: Revisar ou corrigir, sob demanda, aquilo que a pessoa não queria perder de vista.
- **Linguagem Humana**: A superfície não expõe `intents`, Ledger, autoridade ou proveniência como linguagem principal.
- **Ações Mínimas**: Resolver e adiar permanecem visíveis; ajustes, exclusão e explicações ficam em aprofundamento.
- **Capacidades Recolhidas**: Organizar, rotinas, conexões e padrões existem, mas não ocupam a navegação primária.

### Orquestração
- **Propósito**: Organizar um horizonte em blocos quando a pessoa pede ou aceita uma sugestão pertinente.
- **Separação de Navegação e Mutação**: Abrir a aba de planejamento é um ato de navegação puramente observacional; não cria eventos nem gera planos automaticamente.
- **Consentimento Explícito**: O usuário aprova propostas clicando em *"Usar este plano"*, momento em que o Core registra o consentimento no Ledger e atualiza os blocos sem apagar a declaração original do usuário.

### Exploração
- **Automatizar**: Visibilidade das rotinas com autoridade concedida (`suggest_only`, `prepare_proposal`). Permite disparo manual de avaliação (`Tick`) e histórico auditável de notificações emitidas.
- **Conectar**: Registro factual de participantes do ecossistema local (Host C++26, LLM em loopback isolado `127.0.0.1`, ausência de conexões externas não autorizadas).
- **Analisar**: Espaço dedicado à extração de evidências e padrões contextuais reais. Não inventa gráficos quando a base de dados ainda está em fase inicial de observação.

---

## 3. Contratos Contra Regressão de Linguagem
- Evitar jargões técnicos de backend na superfície primária de uso cotidiano (ex: preferir *"Usar este plano"* a *"Aplicar proposta ao Ledger"*; preferir *"LOCAL E PRESENTE"* a *"Runtime local C++ ativo"*).
- Manter detalhes de depuração e inspeção causal acessíveis via aprofundamento (botão *"Por que agora?"* / `inspect`).
- Não apresentar contagens, filtros, dashboards ou ações rápidas no “Agora” sem uma razão contextual concreta.
- Toda nova função deve passar pelo teste: *"Isso exige que a pessoa administre o Lume ou permite que o Lume ajude a pessoa a administrar sua realidade?"*
- Quando ambas forem possíveis, escolher a menor superfície capaz de preservar compreensão, correção e consentimento.

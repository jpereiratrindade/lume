# LUME-EXPERIENCE-001: As Três Profundidades da Experiência Humana no Lume

## 1. Visão Geral
A experiência de interação do Lume é concebida não como um aplicativo utilitário frenético, mas como um espaço calmo e contextual de convivência. A interface visual se organiza em **três profundidades espaciais**, evitando a sobrecarga cognitiva e garantindo factualidade integral.

---

## 2. As Três Profundidades

```text
               1. PRESENÇA (Agora)
           Diálogo calmo · Foco presente · Relevância factual
                        ↓
             2. ORQUESTRAÇÃO (Planejar)
           Projeção temporal · Alocação de intenções · Consentimento
                        ↓
             3. EXPLORAÇÃO (Automatizar, Conectar, Analisar)
           Capacidades ativas · Autoridade concedida · Padrões verificáveis
```

### Profundidade 1: Presença (Superfície "Agora")
- **Propósito**: O momento imediato. Foco sereno, abertura de diálogo e visibilidade do estado de atenção atual.
- **Contrato de Atenção Factual**: A interface só anuncia que algo *"merece atenção agora"* se o Core emitir um `AttentionCandidate` factual cuja janela temporal coincida com o momento presente. Na ausência de intenções ativas para a hora atual, a superfície exibe tranquilidade:
  > *"Está tudo tranquilo por enquanto. Nenhuma intenção declarada requer atenção imediata."*
- **Ações Rápidas**: Entrada de linguagem natural e atalhos rápidos para sintetizar propostas de planejamento.

### Profundidade 2: Orquestração (Superfície "Planejar")
- **Propósito**: Visão ampla do horizonte (manhã ou semana) onde intenções reais são organizadas em blocos estruturados.
- **Separação de Navegação e Mutação**: Abrir a aba de planejamento é um ato de navegação puramente observacional; não cria eventos nem gera planos automaticamente.
- **Consentimento Explícito**: O usuário aprova propostas clicando em *"Usar este plano"*, momento em que o Core registra o consentimento no Ledger e atualiza os blocos sem apagar a declaração original do usuário.

### Profundidade 3: Exploração (Superfícies "Automatizar", "Conectar", "Analisar")
- **Automatizar**: Visibilidade das rotinas com autoridade concedida (`suggest_only`, `prepare_proposal`). Permite disparo manual de avaliação (`Tick`) e histórico auditável de notificações emitidas.
- **Conectar**: Registro factual de participantes do ecossistema local (Host C++26, LLM em loopback isolado `127.0.0.1`, ausência de conexões externas não autorizadas).
- **Analisar**: Espaço dedicado à extração de evidências e padrões contextuais reais. Não inventa gráficos quando a base de dados ainda está em fase inicial de observação.

---

## 3. Contratos Contra Regressão de Linguagem
- Evitar jargões técnicos de backend na superfície primária de uso cotidiano (ex: preferir *"Usar este plano"* a *"Aplicar proposta ao Ledger"*; preferir *"LOCAL E PRESENTE"* a *"Runtime local C++ ativo"*).
- Manter detalhes de depuração e inspeção causal acessíveis via aprofundamento (botão *"Por que agora?"* / `inspect`).

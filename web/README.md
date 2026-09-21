# Lume Web

A superfície web do Lume mantém a conversa como centro e apresenta contexto,
razões e proveniência apenas quando a pessoa pede mais profundidade.

## Experiência local conectada

Compile primeiro o runtime na raiz do repositório. Depois:

```bash
cd web
npm install
npm run dev:local
```

Abra `http://localhost:3000`. Esse comando inicia a interface e uma ponte presa a
`127.0.0.1:4141`. A ponte executa o binário diretamente, sem shell, aceita somente
origens locais conhecidas e mantém o C++ como única autoridade sobre o estado.

Por padrão, a interface usa o mesmo estado da CLI. Para isolar uma sessão:

```bash
LUME_WEB_STATE_FILE=/tmp/lume-web.state npm run dev:local
```

Para usar outras portas locais, configure as duas antes de iniciar; a interface,
a ponte e a política de origem serão ajustadas juntas:

```bash
LUME_WEB_PORT=3100 LUME_BRIDGE_PORT=4242 npm run dev:local
```

## Prévia local independente

```bash
npm run dev
```

Sem a ponte, a interface entra em modo de prévia. Esse modo demonstra a
experiência, mas não persiste estado canônico nem se apresenta como o runtime
real.

As duas formas de execução são exclusivamente locais. O projeto não depende de
um serviço hospedado e não envia o contexto pessoal para fora da máquina.

## Validação

```bash
npm test
npm run test:local
```

O segundo comando requer `../build/lume` e valida a comunicação real entre a
interface e o runtime.

A interface é construída com vinext apenas para execução e validação local.

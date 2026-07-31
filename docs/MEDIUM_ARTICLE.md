# **winget no seu bolso: portando o gerenciador de pacotes da Microsoft para Android, sem emulação**

**Subtítulo:** Como um subset real do `winget-cli` da Microsoft passou a rodar nativamente no Termux — sem proot, sem chroot, sem Wine, sem root.

Existe uma pergunta simples que a maioria descarta rápido demais: "dá pra rodar winget no Android?" A resposta óbvia é não — winget é um binário Windows, amarrado a Win32, dependente do ecossistema .NET da Microsoft. Mas "óbvio" nem sempre é "verdadeiro". `winget-termux` é a prova de que dava, sim, pra portar um subconjunto real e funcional do winget para ARM64/bionic, compilando com clang, sem camada de emulação nenhuma.

## **1. Contexto do tema**

`winget-cli` é o gerenciador de pacotes oficial do Windows 10/11 — resolve manifestos YAML, indexa pacotes em SQLite, baixa instaladores, verifica hash, instala. É código aberto (MIT), o que abre uma porta que poucos exploram: nada impede portar a lógica central para outro sistema, desde que se troque tudo que depende de Win32.

Termux roda em Android, sobre bionic libc (não glibc), sem systemd, sem raiz, sem os caminhos de arquivo que qualquer software Linux/Windows tradicional assume. É um ambiente hostil pra portar código C++ que nasceu pensando em `HKEY_LOCAL_MACHINE` e `C:\Program Files`.

## **2. O problema ou a oportunidade**

O caminho fácil seria emulação: Wine, proot, chroot, uma VM. Funciona, mas carrega custo — performance, complexidade, dependência de camadas que podem quebrar a qualquer atualização do host. A oportunidade real era outra: pegar a lógica de manifesto/índice/instalador do winget-cli — que não é Win32-específica no núcleo — e reescrever só o que toca sistema operacional: os backends de instalação, o resolvedor de caminho, a integração com symlink.

O resultado: `winget_real_cli`, um binário ARM64 nativo, compilado com clang, linkado contra sqlite3/libyaml/jsoncpp/OpenSSL/libcurl — as mesmas bibliotecas que qualquer projeto C++ sério usa no Linux, rodando direto no bionic do Termux.

## **3. O que isso significa na prática**

Hoje o projeto cobre a superfície real de comando do winget: `install`, `uninstall`, `upgrade` (individual ou `--all`), `search`, `show`, `list` (com filtro `--upgrade-available`), `pin`/`unpin`, `export`/`import` de setup inteiro, `source add/remove/update/list/export`, `install-url` pra instalar direto de uma URL sem precisar de catalog, `hash`/`validate` pra quem quer criar manifesto próprio, `download` pra baixar sem instalar, `--info` e `complete`.

Por trás disso, decisões de engenharia nada triviais: um sistema de lock por pacote (`flock`) pra evitar corrida entre install/uninstall/upgrade concorrentes; sanitização de path/alias/nome de source contra path traversal; correção de zip-slip na extração de arquivos; self-heal automático de instalação órfã (quando o processo crasha no meio de um install); um catálogo curado de 19 ferramentas ARM64 reais — `fzf`, `ripgrep`, `btop`, `zoxide`, `starship`, `age`, entre outras — cada uma baixada, instalada e executada de verdade antes de entrar no repositório, sem hash placeholder, sem URL não testada.

## **4. Minha visão / análise**

O ponto mais interessante do projeto não é "conseguimos rodar winget no Android" — é o processo de descobrir, na marra, onde a arquitetura Windows vaza pra dentro de um código que parecia genérico. Symlink em vez de atalho `.lnk`. `flock` em vez de mutex nomeado do Win32. Zip-slip é o mesmo bug em qualquer SO, mas o jeito de fechar depende de `std::filesystem::weakly_canonical` disponível só em C++17+. E teve limite real: nem todo binário ARM64 do GitHub Releases roda em Android — libc glibc não funciona, e até binários Go tropeçam no seccomp do Android rejeitando syscalls como `faccessat2`. A régua do projeto desde o início foi simples e inegociável: só entra no catálogo o que rodou de verdade, na tela, produzindo saída real. Isso descartou binários que pareciam certos no papel — hash batendo, download funcionando — e falharam só na hora de executar.

## **5. Caminhos possíveis**

O projeto não tenta ser "winget 100% igual ao Windows" — isso incluiria msstore, instaladores MSI/EXE nativos, DSC (`winget configure`), nada disso faz sentido em ARM64 Termux. O caminho é continuar sendo o gerenciador certo pro nicho certo: ferramenta CLI que existe no GitHub Releases mas ainda não chegou no repositório `pkg` do Termux, instalável com um comando, sem esperar empacotamento oficial, sem depender de mirror.

Próximos passos honestos: mais entradas no catálogo (só as que passarem no teste real), CI rodando de verdade a cada PR, e manter a regra que sustentou o projeto até aqui — nenhuma feature entra sem rodar contra payload real, sem teste automatizado, sem verificação em dispositivo.

## **Fechamento**

`winget-termux` não é sobre recriar o Windows dentro do Android. É sobre pegar uma ideia boa — manifesto declarativo, índice SQLite, verificação de hash — e mostrar que ela sobrevive fora do ecossistema que a criou, sem gambiarra de emulação. A descoberta histórica aqui não foi tecnológica exótica, foi disciplina: testar tudo de verdade, descartar o que não roda, documentar o que ficou pra trás e por quê.

**Você já bateu numa parede parecida — código que parecia amarrado a uma plataforma, mas não estava de verdade?**

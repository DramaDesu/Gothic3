# Gothic 3 — improvement project

Улучшение Gothic 3 (боёвка, рендер, стриминг) через нативные хуки в движок Genome,
поверх Community Patch 1.75.14 + Parallel Universe Patch 1.1.1.

## Почему хуки, а не порт
Геймплей G3 — компилированный C++ (`Script_Game.dll` поверх `Game.dll`), скриптовой VM нет,
зато DLL движка экспортируют почти все символы (Engine 9185, Game 8569), движок сам грузит
любой `scripts/Script_*.dll`, а шейдеры — редактируемые `.fx` в `Materials.pak`.
Полная разведка: [docs/research-2026-08.html](docs/research-2026-08.html).

## Состав
- `mcp/` — MCP-сервер для агентного управления живой игрой (запуск/heartbeat/entity/goto/
  spawn/property/скриншоты/логи) через Script_RemoteControl.dll (ZeroMQ+protobuf, порт 5555).
  Протокол: [mcp/PROTOCOL.md](mcp/PROTOCOL.md).
- `docs/` — исследования и заметки.

## Рабочее окружение (не в репо)
Рядом клонируются: [gothic3sdk](https://github.com/georgeto/gothic3sdk),
[gothic3sdk-examples](https://github.com/georgeto/gothic3sdk-examples),
[форк Jackydima](https://github.com/Jackydima/gothic3sdk) (evade-механики),
[g3dit](https://github.com/georgeto/g3dit), [G3Archive](https://github.com/gekonnn/G3Archive),
[Script_G3Fixes](https://github.com/fyryNy/Script_G3Fixes).
Сборка примеров: `cmake -S gothic3sdk-examples -B gothic3sdk-examples/build -G "Visual Studio 17 2022" -A Win32`.

Игра: Steam (v1.6 по умолчанию!) + ручной CP 1.75.14 (worldofgothic.de) + PUP 1.1.1.
PUP поимённо подавляет Script_CrashReport/FontScale/MouseDrag (их функции встроены в него).

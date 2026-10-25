# SysWidget 2.0 — архитектура

## Модули (src/)

```
src/
├── main.cpp              — точка входа: сервисы, окно, цикл сообщений
├── common.h              — утилиты, константы, Trace (SYSWIDGET_TRACE=1)
├── jsonlite.h            — мини-JSON (парсер/сериализатор, без зависимостей)
├── content.h             — модель строк виджета, конфиг, ApplyCommand (протокол пайпа)
│
├── system/
│   ├── sampler.h         — Sampler: CPU (GetSystemTimes) / RAM (GlobalMemoryStatusEx)
│   └── metrics.h         — MetricsService: std::jthread + stop_token, двойной буфер
│                          AppStateSnapshot, публикация в SharedState + WM_APP_DATA_UPDATED
├── ipc/
│   ├── pipe_server.h     — управляющий пайп \.\pipe\SysWidget (NDJSON), 3 инстанса
│   ├── mcp.h             — MCP-мост (stdio JSON-RPC 2.0, tools/list, tools/call)
│   └── agents.h          — AgentService: OVERLAPPED-сервер \.\pipe\syswidget_agents_ipc,
│                          потокобезопасный unordered_map<string, AgentState>
├── graphics/
│   ├── wic.h             — WIC-фабрика + декодирование любых картинок → ID2D1Bitmap
│   └── wic_gif.h         — WicAnimatedGif: GIF через IWICBitmapDecoder, композитинг
│                          кадров с полной семантикой Disposal (0/1/2/3), задержки из
│                          /grctlext/Delay, кадры → аппаратные ID2D1Bitmap, advance()
├── layout/
│   └── layout.h          — движок разметки: ILayoutElement (Measure/Arrange/Render),
│                          concept LayoutElement, Stack(V/H) с flex-весами, Text, Bar,
│                          Gif (aspect-fit + якоря), AgentCard (пульс/мигание статуса)
├── config/
│   └── theme.h           — RenderTheme: тема из %APPDATA%\SysWidget\theme.json
│                          (цвета, радиусы, шрифты) поверх нативных дефолтов
└── ui/
    ├── card_window.h     — окно-оверлей: glue к панели задач / panel-режим (свободный
    │                      ресайз WM_NCHITTEST, drag за шапку), tray, flyout, z-watchdog
    └── settings_window.h — нативное окно настроек (пресеты, строки, цвета, прозрачность)
```

## Потоки и данные

```
[MetricsService jthread] ── 1 Hz ──> AppStateSnapshot (double buffer, mutex)
        │                                  │
        └─ PostMessage(WM_APP_DATA_UPDATED) ┘   ← UI-поток никогда не сэмплирует
                                                   и не блокируется

[AgentService jthread ×2] <── JSON ── агенты (Claude/Codex/Gemini/...)
        │  (OVERLAPPED ConnectNamedPipe + WaitForMultipleObjects(stopEvent))
        └─ PostMessage(WM_APP_DATA_UPDATED)

[UI thread]  message loop → dirty → layout tree: Measure(availW,availH,dpiScale·userScale)
             → Arrange → Render(ID2D1RenderTarget) → ULW/DComp present
```

- **Синхронизация**: SharedState под SRWLOCK (короткие секции); снапшоты метрик —
  двойной буфер под мелким mutex; агент — map под std::mutex. UI-поток получает
  данные только через неблокирующий PostMessage.
- **Рендер**: Direct2D/DirectWrite (D2D1Factory1). Бэкенды: `gdi` — layered window
  (WS_EX_LAYERED + UpdateLayeredWindow, D2D DC RenderTarget → premultiplied DIB),
  `dcomp` — D3D11 + DirectComposition + swap chain с альфой. Выбор — config.renderer.

## Panel-режим (mode: "panel")

Свободно перемещаемое/ресайзимое окно: `WM_NCHITTEST` (зоны 7 px по краям + HTCAPTION
на шапке), `WM_GETMINMAXINFO` (мин. 240×150), позиция/размер сохраняются в конфиг.
Контент собирается layout-деревом: шапка (CPU/RAM) → карточки агентов → GIF-маскот
(якорь/масштаб из конфига). Агент в статусе `offline` исключается из снапшота —
стек пересчитывается без «дыр».

## Статусы агентов

| статус | рамка/индикатор | поведение |
|---|---|---|
| `working` | `#38BDF8` | пульс альфы 0.4→1.0 (sin, период 1.2 c) |
| `done` | `#22C55E` | статичный зелёный |
| `attention` | `#EF4444` | мигание ~0.7 c вкл / 0.9 c выкл |
| `offline` | — | не рендерится |

## Тема (theme.json)

Необязательный `%APPDATA%\SysWidget\theme.json`: `background_color`, `card_background`,
`card_stroke`, `track`, `text_primary`, `text_secondary`, `accent`, `warn`, `critical`,
`corner_radius_window`, `corner_radius_cards`, `font_family`, `font_size`,
`agents.{working,done,attention,card_background}` — формат `#RRGGBB[AA]`.
Без файла — нативные дефолты, следующая за системной светлой/тёмной темой.

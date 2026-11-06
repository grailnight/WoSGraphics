# Дым из труб (Chimney smoke)

Часть рендерера `winmm.dll`. Вкладка меню «Дым», настройки в `[ChimneySmoke]` файла
`WoS_VolumetricClouds/clouds_menu.ini`.

## Почему так, а не через ассеты

- В игре есть трубы `s_rf_chimneya/b/c` (b стоит в 71 ячейке) и заводская `s_rf_smokestacka`
  (≈104 м), и даже готовый эффект `fx_smk_chimneya.wrap.efxb` в `megacity.pcapk`. Но **ни одна
  сущность на него не ссылается** — эффект вырезан. Дымит только разрушенная труба
  (`s_rf_chimneya_dam.ent` → `fx_smk_vent`).
- Размещение (`megacity.ens`, `.ent` ячеек) — дамп памяти с указателями; прицепить эффект к
  целой трубе там нельзя.

## Как работает

1. **Поиск труб на отрисовке.** Позиции статических мешей хранятся в SHORT4N и распаковываются
   в VS константами `gDecompPosScale/gDecompPosBias` = `bbox/offset` **модели**. Эти шесть float —
   отпечаток модели. Регистры `world`/`gDecompPos*` берутся из CTAB каждого VS (у разных VS они
   разные). Совпадение с таблицей `chimney_emitters.h` + мировая матрица из `world` (абсолютные
   координаты, Y вверх) дают мировые точки дымоходов. Ни текстуры, ни VB не блокируются.
2. **Реестр.** Точки запоминаются на `keepSeconds` (120 с), поэтому шлейф не пропадает, когда
   сама труба отсечена. Дубликаты (LOD, тени) сливаются по расстоянию.
3. **Отрисовка** на bloom-отрисовке (после SSR воды, до лучей/тумана): для ближайших
   `MaxPlumes` шлейфов — прямоугольник на экране, raymarch объёма по глубине INTZ, смешение
   `ONE, SRCALPHA` в линейный HDR. Свет — солнце из таблицы света (как у лучей) + `fogColour`
   как фон.

## Файлы

| Файл | Что это |
|---|---|
| `chimney_smoke.h` | модуль `WosChimneySmoke` (Observe / BloomDraw / статус) |
| `chimney_smoke.hlsl` → `.cso` | шейдер шлейфа, `python compile.py` |
| `chimney_emitters.h` | генерируется `python build_emitters.py` из мешей |
| `extracted/G14/` | `chimneyb*`, `smokestacka*` из G14.PCPACK (остальные — в `WoS_VolumetricClouds/extracted`) |
| `integrate.py` | однократная вставка в `integration/wos_clouds_menu.cpp` (бэкап рядом) |
| `gpu_test.cpp` | HAL-тест шейдера: небо, стена-заслон, два шлейфа → `gpu_test.png` |
| `install.ps1` | бэкап живой `winmm.dll` в `WoS_Deferred/backups` и установка |
| `chimney_smoke_status.json` | пишется игрой раз в секунду: найденные трубы, совпадения, ошибки |

## Проверено / не проверено

- Проверено: шейдер на реальном GPU (`gpu_test.exe`): окклюзия стеной, отсутствие NaN, форма.
  DLL собирается, `verify_install.py` valid, все 19 маркеров на месте.
- **Не проверено в игре**: реальное число найденных труб, яркость относительно неба, стоимость.
  Первое, что смотреть: `chimney_smoke_status.json` → `matched_draws`, `emitters_known`.
  Если `vs_layouts_usable` > 0, а `matched_draws` = 0, — отпечатки не совпали (например, движок
  шлёт не модельный box) — это первое подозрение.

## Откат

Скопировать бэкап из `install.json` → `backup` обратно в `winmm.dll`, либо просто выключить
галочку «Chimney smoke» (Enabled=0 в `[ChimneySmoke]`).

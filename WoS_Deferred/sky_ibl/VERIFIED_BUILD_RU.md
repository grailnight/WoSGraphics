# Установленная сборка на проверенной базе

18 сентября: установлена `a181225cee81a8d2298d6e92a8796acde1a87f08ffc03869c447b715aeabcee8`.
Игровая проверка ещё ожидается. Настройки пользователя сохранены.

База восстановлена из исходников и проверена сравнением с рабочей DLL:
исполняемые секции совпадают побайтно, различается только дата сборки.
Полная копия эффектов: `B:/Games/_Codex_Backups/WoS_effects_current_20260918_004633`.
Копия перед этой установкой и все 103 входа сборки: `backup/verified_20260918_011113`.

Изменены только три файла эталонной цепочки: integration/deferred_runtime.h,
integration/wos_clouds_menu.cpp, integration/deferred_control.h. Остальные зависимости
сверены с эталоном. Старый prepare_integration.py заблокирован; сборка идёт напрямую
через build_integration.ps1. Не использовать prepare_candidate.py и старую candidate DLL.

Добавлено: cosine diffuse irradiance, GGX sky prefilter, split-sum DFG,
Fresnel/roughness/F0, загрузка параметров PBR из существующего INI.
Небо предыдущего кадра допускается только для той же сцены, поскольку sky-pass поздний.
SSGI, shadow caches, TAA, water SSR, дождь и volumetric сохранились в исходной цепочке.

F3 → PBR sky lighting: переключатель, сила рассеянного света и отражений,
параметры адаптации старых материалов. `python control_sky.py 0` / `1` переключает
только новый свет через встроенный API. `full_deferred_status.json` содержит sky_ibl.

GPU: RTX 3060, 99 сценариев, 2423 проверки, включая OFF/no-op, alpha, AO, fog,
профили неба, ограничения свежести/сцены. Парсер: 8 сценариев. Доказательства:
verified_validation.json, verified_install.json, verified_integration.patch.

Это PBR-свет неба поверх сохранённого legacy residual: старые baked/environment
компоненты пока не отделены. Roughness/metalness оцениваются из старых материалов;
небо не включает отражения зданий и облаков. Это ещё не полная замена всех материалов
и всех источников света физически откалиброванным рендерером.

# WoSGraphics

Исходники графического мода для Spider-Man: Web of Shadows (PC, D3D9, x86).
Рендерер — прокси `winmm.dll`, который перехватывает устройство D3D9 игры и добавляет
deferred-освещение, SSGI, каскадные тени с PCSS, объёмный свет, облака, IBL неба,
TAA / SMAA / DLAA, SSR воды, пар и дым.

Репозиторий лежит прямо в папке игры: `.gitignore` по умолчанию игнорирует всё и
пропускает только исходники графики. Файлы игры, другие моды, сборки, захваты и бэкапы
не отслеживаются.

## Что где

| Путь | Что |
|---|---|
| `WoS_Deferred/full_deferred_staged/integration/` | рендерер: `wos_clouds_menu.cpp` (хуки устройства, меню) и заголовки модулей |
| `WoS_Deferred/full_deferred_staged/build_integration.ps1` | сборка `winmm.dll` |
| `WoS_Deferred/full_deferred_staged/verify_install.py` | проверка установки после сборки (обязательна) |
| `WoS_Deferred/full_deferred/` | шейдеры deferred-прохода (`*.hlsl`) и настройки по умолчанию |
| `WoS_Deferred/{cloud_lowres,taa_native,smaa_native,dlss_native,sky_ibl,...}/` | модули, которые компилируются в `winmm.dll` |
| `WoS_Water/` | хуки воды и SSR воды |
| `WoS_DLSS/` | x64-хост DLAA и общий IPC-заголовок |
| `WoS_VolumetricClouds/tools/imgui` | Dear ImGui, submodule |

## Сборка

1. `git submodule update --init`
2. Visual Studio 2026 (v18) с x86-инструментами. Путь к `vcvars32.bat` задан в `build_integration.ps1`.
3. Python 3.
4. Заголовки Vulkan 1.4.363.0 в `WoS_Research/external/vulkan_headers_1.4.363.0/include`.
   Нужны только заголовки: DLAA работает с `VkDevice` DXVK.
5. `powershell -File WoS_Deferred/full_deferred_staged/build_integration.ps1`, затем `verify_install.py`.

Include-пути пока абсолютные (`B:/Games/Spider Man - Web of Shadows/...`).

## Чего здесь нет

- **Байткод шейдеров самой игры.** Он нужен сборке, но генерируется локально из своей
  копии игры: `WoS_Water/native_assets.h` (`WoS_Water/build_native.py`) и
  `integration/native/WoS_Deferred/rsm_flux_mrt_staged/shader_assets.h`.
- Скомпилированные `.cso`, набор G-buffer-вариантов материалов игры, DLL и любые
  данные, извлечённые из игры.
- NVIDIA DLSS SDK.

## Сторонний код

Dear ImGui (MIT), SMAA (MIT, `smaa_native/third_party`), фрагменты Filament
(Apache-2.0, `full_deferred/third_party/filament`). Лицензия для собственного кода
мода пока не выбрана.

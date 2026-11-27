# Сборка и выпуск

## Инструменты

Нужны Windows, PowerShell, Visual Studio Build Tools с компонентом C++ x86/x64, Windows 10 или 11 SDK с x86-библиотеками, Git и Python 3.10+ для упаковки. Скрипт находит MSVC через `vswhere` и выбирает новейшие установленные x86-инструменты и SDK. Плагин использует C++17 и статическую C/C++ runtime.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\HP2Display\build.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\HP2WSFix\build.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\HP2Display\build.ps1 -Verify -GameRoot "D:\Games\HP2"
```

Первые две команды собирают HP2Display и HP2WSFix 2.5. Результаты появляются в `out/HP2Display.asi` и `out/HP2WSFix25-verified/HP2WSFix.asi`. `d3d8.dll` берётся в готовом виде из `third_party/Ultimate-ASI-Loader/`. Тесты HP2Display используют определённый по SHA-256 EXE и `d3d8.dll` из `-GameRoot`. Стенд работает с временной копией обёртки и использует полноэкранный режим 1920×1080. Закройте игру перед запуском стенда.

## Пакет из тега

После фиксации исходников создайте тег версии и выполните:

```powershell
python .\package_release.py v0.2.0
```

Скрипт получает дерево тега через `git archive`, собирает оба ASI и проверяет SHA-256 загрузчика. В `dist/` появляются игровой ZIP, его файл `.sha256` и архив исходников. `BUILD-INFO.txt` в игровом ZIP содержит тег, полный хеш коммита и контрольную сумму каждого файла пакета. Версии компилятора и SDK могут влиять на SHA-256 собранного ASI.

Перед публикацией проверьте архив по [матрице тестов](docs/TESTING.md). На GitHub создайте релиз от проверенного тега, приложите игровой ZIP и его SHA-256, используйте текст из [CHANGELOG.md](CHANGELOG.md). Руководство для внешней площадки хранится в `docs/STEAM-GUIDE.md`.

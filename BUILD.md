# Сборка и выпуск

## Инструменты

Нужны Windows, PowerShell, Visual Studio Build Tools с компонентом C++ x86/x64, Windows 10 или 11 SDK с x86-библиотеками, Git и Python 3.10+ для упаковки. Скрипт находит MSVC через `vswhere` и выбирает новейшие установленные x86-инструменты и SDK. Плагин использует C++17 и статическую C/C++ runtime.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\HP2Display\build.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\HP2Display\build.ps1 -Verify -GameRoot "D:\Games\HP2"
```

Первая команда пишет `out/HP2Display.asi`. Для другого пути используйте `-OutputPath`. Тесты берут определённый по SHA-256 EXE и `d3d8.dll` из `-GameRoot`. Проверочный стенд работает с временной копией обёртки и использует реальный полноэкранный режим 1920×1080. Закройте игру перед запуском стенда.

## Пакет из тега

После фиксации исходников создайте тег версии и выполните:

```powershell
python .\package_release.py v0.1.0
```

Скрипт получает дерево тега через `git archive`, собирает ASI из этого дерева и кладёт в ZIP только заранее перечисленные файлы. Выходные файлы — `dist/HP2Mod-v0.1.0-win32.zip` и его `.sha256`. Внутри `BUILD-INFO.txt` содержит тег, полный хеш коммита и контрольную сумму каждого файла пакета. Версии компилятора и SDK могут влиять на SHA-256 собранного ASI. На публикацию передаётся тот же ZIP, который использовался при проверке.

Перед публикацией проверьте архив по [матрице тестов](docs/TESTING.md). На GitHub создайте релиз от проверенного тега, приложите игровой ZIP и его SHA-256, используйте текст из [CHANGELOG.md](CHANGELOG.md). Руководство для внешней площадки хранится в `docs/STEAM-GUIDE.md`.

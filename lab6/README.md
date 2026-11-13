# Лабораторная работа № 6 — мониторинг изменений значений

Вариант 6. **Мониторинг изменений значений** (аналог `RegNotifyChangeKeyValue()`).

Работа выполнена для Ubuntu/Linux на C++17. Вместо реестра Windows используется
учебное файловое хранилище с той же иерархией ключей и значений, а вместо
`RegNotifyChangeKeyValue()` — механизм `inotify` с ожиданием через `poll()`.

## Индивидуальные параметры

- `STUDENT_ID = 028`
- `SEED = 1000 + 28 * 97 = 3716`
- `RUN_ID` — идентификатор запуска, по умолчанию `1`
- Учебный ключ:
  `HKEY_CURRENT_USER/Software/OSiSP/Lab06/Student_028/Run_<RUN_ID>`

## Назначение программы

Утилита `lab06_regwatch` умеет:

1. Создавать учебную область и детерминированный набор значений.
2. Создавать, изменять, удалять и перечислять значения.
3. Ожидать изменения значений в выбранном ключе, повторно читать состояние,
   сравнивать его с предыдущим снимком и записывать найденные отличия в журнал.
4. Корректно завершаться по тайм-ауту, по достижении лимита уведомлений или
   по сигналу `SIGINT`/`SIGTERM`.

Запись и удаление разрешены только внутри области
`HKEY_CURRENT_USER/Software/OSiSP/Lab06/Student_028/Run_*`.
Попытка изменить любой другой ключ завершается ошибкой `write denied`.

## Соответствие Windows API и Linux-аналогов

| Windows API | Аналог в работе |
|---|---|
| `RegOpenKeyEx()` | проверка существования каталога ключа |
| `RegCreateKeyEx()` | `std::filesystem::create_directories()` |
| `RegQueryValueEx()` | чтение файла `*.val` |
| `RegSetValueEx()` | атомарная запись файла значения |
| `RegEnumValue()` | обход файлов `*.val` |
| `RegDeleteValue()` | `unlink()` |
| `RegDeleteKeyEx()` | `std::filesystem::remove()` |
| `RegDeleteTree()` | `std::filesystem::remove_all()` |
| `RegNotifyChangeKeyValue()` | `inotify_add_watch()` + `poll()` |
| `RegCloseKey()` | закрытие дескрипторов в конце функции |
| `GetLastError()` | `errno` и `strerror(errno)` |
| `QueryPerformanceCounter()` | `clock_gettime(CLOCK_MONOTONIC)` |
| `GetTickCount64()` | `std::chrono::steady_clock` |

## Хранилище

Корень по умолчанию: `~/.osip_lab06_registry` (переопределяется `--root`).
Ключ — каталог, значение — файл `<имя>.val`:

```text
REG_SZ
some text
```

Поддерживаются типы `REG_SZ`, `REG_EXPAND_SZ`, `REG_DWORD`, `REG_QWORD`,
`REG_MULTI_SZ`, `REG_BINARY`. Символы `\`, перевод строки и `\r` экранируются.
Значение записывается во временный файл и переименовывается, поэтому монитор
никогда не читает частично записанный файл. Временные файлы начинаются с точки
и игнорируются при чтении снимка.

## Требования к окружению

- Ubuntu 22.04/24.04 или совместимый Linux с `inotify`
- GCC 11+ или Clang 14+
- CMake 3.16+
- Python 3.10+ для тестов, экспериментов и графиков
- Python-пакеты `matplotlib` (для графиков)
- Graphviz `dot` (только для перерисовки диаграмм)
- Typst (только для сборки отчёта)

## Сборка

```bash
cmake -S lab6 -B lab6/build -DCMAKE_BUILD_TYPE=Release
cmake --build lab6/build -j
```

Или из каталога `lab6`:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Быстрый запуск

Создать учебный ключ с 100 значениями и фиксированным SEED:

```bash
./build/lab06_regwatch init \
  --student-id 028 --run-id 1 --count 100 --seed 3716 \
  --log-file logs/init.log
```

Изменить значение:

```bash
./build/lab06_regwatch set \
  --student-id 028 --run-id 1 \
  --name Item_000 --type REG_SZ --value "new text"
```

Посмотреть содержимое:

```bash
./build/lab06_regwatch list --student-id 028 --run-id 1
```

Запустить наблюдение (терминал 1):

```bash
./build/lab06_regwatch monitor \
  --student-id 028 --run-id 1 \
  --timeout-ms 15000 --max-events 1 --debounce-ms 60 \
  --ready-file /tmp/lab06.ready --log-file logs/monitor.log
```

После появления `/tmp/lab06.ready` изменить значение (терминал 2):

```bash
./build/lab06_regwatch set \
  --name Item_000 --value "changed" --student-id 028 --run-id 1
```

В журнале появятся строки `REG_NOTIFY`, `REG_DIFF` и `REG_MODIFY` со старым и
новым значением.

## Параметры запуска

Общие параметры:

- `--root DIR` — корень файлового хранилища;
- `--student-id ID` — номер студента, по умолчанию `028`;
- `--run-id ID` — номер запуска, по умолчанию `1`;
- `--key KEY` — полный путь ключа (по умолчанию текущий `Run_*`);
- `--log-file FILE` — файл журнала;
- `--quiet` — не дублировать журнал в стандартный вывод.

`init`:

- `--count N` — сколько значений создать;
- `--seed N` — зерно генератора, по умолчанию `3716`.

`set`:

- `--name NAME` — имя значения (обязательно);
- `--type TYPE` — тип значения, по умолчанию `REG_SZ`;
- `--value TEXT` — данные значения.

`delete`:

- `--name NAME` — удалить значение;
- `--tree` — удалить ключ со всеми подключами.

`monitor`:

- `--timeout-ms N` — завершиться через N мс (`0` — без тайм-аута);
- `--max-events N` — завершиться после N обнаруженных изменений
  (`0` — без ограничения);
- `--debounce-ms N` — окно объединения серии событий;
- `--ready-file FILE` — создать файл после регистрации наблюдения.

## Журналирование

Формат строки:

```text
[2026-11-11 10:59:16.284] MONITOR event=REG_DIFF added=0 removed=0 changed=1
```

Каждая операция пишется в журнал с временем, типом, объектом и результатом.
Для монитора дополнительно фиксируются:

- `REG_NOTIFY` — событие `inotify`, маска событий и время обнаружения
  (`ts_us`, микросекунды эпохи);
- `REG_DIFF` — количество добавленных, удалённых и изменённых значений,
  а также время повторного чтения (`read_us`);
- `REG_ADD`, `REG_DEL`, `REG_MODIFY`, `REG_KEY_ADD`, `REG_KEY_DEL` — детали
  различий.

## Обработка ошибок

Проверяются результаты `inotify_init1()`, `inotify_add_watch()`, `poll()`,
`read()`, `open()`, `write()`, `fsync()`, `close()`, `rename()`, `unlink()`,
`mkstemp()`, `clock_gettime()`. При ошибке выводятся имя функции, операция,
код `errno` и описание из `strerror()`.

Обрабатываются, в частности:

- отсутствующий ключ при мониторинге;
- запись или удаление вне учебной области;
- неизвестный тип значения;
- некорректное число для `REG_DWORD`/`REG_QWORD`;
- невозможность открыть файл журнала;
- удаление наблюдаемого ключа (`MONITOR_TARGET_REMOVED`);
- переполнение очереди `inotify` (`IN_Q_OVERFLOW`);
- завершение по сигналу.

Все дескрипторы закрываются, временные файлы удаляются, динамическая память
освобождается стандартными контейнерами.

## Тестирование

```bash
ctest --test-dir build --output-on-failure
```

Проверки находятся в `tests/run_checks.py` и охватывают:

1. создание ключа и перечисление значений;
2. создание, изменение и удаление значения;
3. запрет записи вне учебной области;
4. неизвестный тип и некорректный `REG_DWORD`;
5. отсутствующий ключ;
6. обнаружение изменения одного значения;
7. обнаружение изменения пяти значений за одну серию;
8. обнаружение удаления значения.

## Эксперименты

```bash
python3 scripts/run_experiments.py
python3 scripts/plot_results.py
```

Скрипт выполняет по три запуска для каждой конфигурации:

- `values_1`, `values_10`, `values_100`, `values_500` — изменение одного
  значения при разном числе значений в ключе;
- `change_1`, `change_5`, `change_10` — изменение одного, пяти и десяти
  значений;
- `delete_1` — удаление значения.

Используется фиксированный `SEED = 3716`. Для каждой серии измеряются задержка
уведомления, время повторного чтения и количество обнаруженных изменений.
Сырые данные: `data/raw/experiment_raw.csv`, сводка:
`data/raw/summary.csv`, журналы: `data/raw/logs/`.

Графики:

- `data/plots/read_time_vs_values.png` — время чтения от числа значений;
- `data/plots/detection_latency.png` — задержка обнаружения;
- `data/plots/detected_changes.png` — обнаруженные изменения и уведомления.

## Диаграммы

- `diagrams/architecture.dot` — архитектура приложения;
- `diagrams/activity_monitor.dot` — алгоритм монитора.

Перерисовка:

```bash
python3 scripts/render_diagrams.py
```

## Структура проекта

```text
lab6/
├── src/            # исходный код C++
├── tests/          # проверки CTest/Python
├── scripts/        # эксперименты, графики, диаграммы
├── data/raw/       # сырые данные и журналы
├── data/plots/     # графики PNG/PDF
├── diagrams/       # редактируемые Graphviz-схемы и изображения
├── report/         # отчёт Typst, PDF и промпты ИИ
├── docs/           # план выполнения работы
├── CMakeLists.txt
└── README.md
```

## Ограничения и безопасность

- Настоящий реестр Windows не используется и не изменяется.
- Все изменения выполняются только в каталоге учебной области.
- Наблюдение устанавливается только для собственного учебного ключа.
- Нет перехвата оконных сообщений, внедрения DLL и доступа к чужим процессам.
- Секреты, учётные данные и системные дампы в репозиторий не помещаются.

## Отчёт

- `report/report.typ` — исходник отчёта;
- `report/report.pdf` — собранный PDF;
- `report/ai_prompts.md` — тексты промптов для раздела об использовании ИИ.

Сборка отчёта:

```bash
cd lab6
typst compile --root . report/report.typ report/report.pdf
```

## Репозиторий Git

- URL: <https://github.com/Dima-Vasilenko/osisp>
- Каталог: `lab6/`
- Ветка: `main`

# Экспериментальные данные

Все данные получены программой `lab04_sync` и сохранены скриптом
`scripts/run_experiments.py`. Для каждой конфигурации выполнено по три запуска.
Подробные характеристики среды находятся в `environment.txt`.

## Конфигурации

| Имя | Серия | N | Циклы | Режим | Обдумывание, мс | Еда, мс |
|---|---|---:|---:|---|---|---|
| `standard_fifo` | standard | 5 | 100 | fifo | 1–2 | 20–40 |
| `individual_fifo` | individual | 5 | 240 | fifo | 1–2 | 20–40 |
| `individual_scan` | individual | 5 | 240 | scan | 1–2 | 20–40 |
| `load_n3_fifo` | load | 3 | 240 | fifo | 1–2 | 20–40 |
| `load_n5_fifo` | load | 5 | 240 | fifo | 1–2 | 20–40 |
| `load_n7_fifo` | load | 7 | 240 | fifo | 1–2 | 20–40 |

Для всех запусков: `seed=3716`, `student_id=28`, `timeout=30000 мс`,
`max_duration=180000 мс`.

## Файлы

- `results.csv` — сводка по каждому запуску;
- `waits.csv` — все времена ожидания заявок;
- `raw/runs/` — stdout и CSV ожиданий каждого запуска;
- `plots/` — графики в PNG и PDF;
- `environment.txt` — сведения о среде выполнения.

## Средние результаты

| Конфигурация | Время, мс | Пропускная способность, оп/с | Ср. ожидание, мс | Макс. ожидание, мс | Обходы |
|---|---:|---:|---:|---:|---:|
| `standard_fifo` | 8670.1 | 57.7 | 54.3 | 84.3 | 0 |
| `individual_fifo` | 20764.8 | 57.8 | 54.6 | 88.4 | 0 |
| `individual_scan` | 18259.7 | 65.7 | 43.2 | 193.8 | 280.7 |
| `load_n3_fifo` | 21551.0 | 33.4 | 58.1 | 78.3 | 0 |
| `load_n5_fifo` | 20698.4 | 58.0 | 54.3 | 86.5 | 0 |
| `load_n7_fifo` | 20762.6 | 80.9 | 54.6 | 91.5 | 0 |

## Воспроизведение

```bash
cmake -S lab04-sync -B lab04-sync/build -DCMAKE_BUILD_TYPE=Release
cmake --build lab04-sync/build -j
python3 lab04-sync/scripts/run_experiments.py
python3 lab04-sync/scripts/plot_results.py
```

Скрипты перезаписывают `results.csv`, `waits.csv`, `raw/` и `plots/`.

# MARS CLI

## Что это

`mars` это консольная утилита для измерения характеристик CPU cache.

Программа:

- читает CLI-аргументы
- загружает YAML-конфигурацию
- настраивает логирование
- запускает измерение L1d/L2/L3
- печатает итоговую сводку

---

## Сборка

Пример локальной сборки:

```bash
cmake -S . -B build
cmake --build build
```

После этого исполняемый файл обычно находится здесь:

[`build/src/cli/mars`](/home/rach/rAch-kaplin/M.A.R.S./build/src/cli/mars)

---

## Запуск через Docker

В корне репозитория есть готовые файлы:

- [`Dockerfile`](/home/rach/rAch-kaplin/M.A.R.S./Dockerfile)
- [`docker-compose.yml`](/home/rach/rAch-kaplin/M.A.R.S./docker-compose.yml)
- [`docker.md`](/home/rach/rAch-kaplin/M.A.R.S./docker.md)

### Сборка образа

```bash
docker build -t mars-cli:local .
```

### Базовый запуск через Docker

```bash
docker run --rm mars-cli:local
```

### Запуск с явным конфигом и логом в файл

```bash
mkdir -p logs

docker run --rm \
  -v "$(pwd)/config:/opt/mars/config:ro" \
  -v "$(pwd)/logs:/opt/mars/logs" \
  mars-cli:local \
  --config config/mars_example.yaml \
  --log-level debug \
  --log-file logs/docker.log
```

### Строгий режим через Docker

Этот режим нужен, если вы хотите дать контейнеру больше шансов на корректную работу:

- `bind_cpu`
- `realtime_priority`
- `lock_frequency`
- low-level PMU/perf measurements

Команда:

```bash
docker run --rm \
  --cpuset-cpus="0" \
  --privileged \
  --security-opt seccomp=unconfined \
  --ulimit rtprio=99 \
  --ulimit memlock=-1 \
  -v "$(pwd)/config:/opt/mars/config:ro" \
  -v "$(pwd)/logs:/opt/mars/logs" \
  -v /sys:/sys:rw \
  mars-cli:local \
  --config config/mars_example.yaml
```

---

## Запуск через Docker Compose

### Сборка

```bash
docker compose build
```

### Обычный запуск

```bash
docker compose run --rm mars
```

### Строгий запуск

```bash
docker compose run --rm mars-strict
```

### Выбрать другой CPU для strict-режима

```bash
MARS_CPUSET=3 docker compose run --rm mars-strict
```

Полное объяснение Docker-конфигурации, прав и тестовых команд вынесено в [`doc/docker.md`](/home/rach/rAch-kaplin/M.A.R.S./docker.md).

---

## Базовый запуск

Запуск с конфигом по умолчанию:

```bash
./build/src/cli/mars
```

Запуск с явным конфигом:

```bash
./build/src/cli/mars --config config/mars_example.yaml
```

Запуск с подробным логом:

```bash
./build/src/cli/mars --log-level debug
```

Запуск без вывода summary:

```bash
./build/src/cli/mars --no-summary
```

Запуск без логов в консоль:

```bash
./build/src/cli/mars --no-console
```

Запуск с логом в файл:

```bash
./build/src/cli/mars --log-file mars.log
```

Комбинированный пример:

```bash
./build/src/cli/mars \
  --config config/mars_example.yaml \
  --log-level info \
  --log-file mars.log
```

---

## CLI аргументы

Поддерживаемые аргументы определены в [`include/app/config.hpp`](/home/tiomik/projectX/M.A.R.S./include/app/config.hpp).

| Опция | Короткая форма | Значение | По умолчанию | Описание |
|---|---|---|---|---|
| `--config` | `-c` | путь к YAML-файлу | `config/mars_example.yaml` | Путь к конфигурации измерения |
| `--log-level` | нет | `trace`, `debug`, `info`, `warning`, `warn`, `error`, `err`, `critical`, `off` | `info` | Уровень логирования |
| `--log-file` | нет | путь к файлу | пусто | Дополнительно писать лог в файл |
| `--no-console` | нет | флаг | `false` | Отключить логирование в консоль |
| `--no-summary` | нет | флаг | `false` | Не печатать итоговую summary |

### Как задавать аргументы

Опции со значением можно передавать так:

```bash
./build/src/cli/mars --config config/mars_example.yaml
./build/src/cli/mars -c config/mars_example.yaml
./build/src/cli/mars --log-level debug
./build/src/cli/mars --log-file mars.log
```

Флаги без значения передаются просто наличием:

```bash
./build/src/cli/mars --no-console
./build/src/cli/mars --no-summary
```

Можно комбинировать:

```bash
./build/src/cli/mars -c config/mars_example.yaml --log-level trace --log-file trace.log --no-summary
```

---

## Уровни логирования

| Значение | Что означает |
|---|---|
| `trace` | Максимально подробный лог |
| `debug` | Отладочный лог |
| `info` | Обычный рабочий лог |
| `warning` / `warn` | Только предупреждения и ошибки |
| `error` / `err` | Только ошибки |
| `critical` | Только критические ошибки |
| `off` | Полностью выключить логирование |

Примеры:

```bash
./build/src/cli/mars --log-level info
./build/src/cli/mars --log-level debug
./build/src/cli/mars --log-level off
```

---

## Конфигурационный файл

Формат конфигурации загружается в [`src/app/config_loader.cpp`](/home/tiomik/projectX/M.A.R.S./src/app/config_loader.cpp).

Файл должен содержать корневой раздел:

```yaml
probe:
  ...
```

Пример рабочего файла:

[`config/mars_example.yaml`](/home/tiomik/projectX/M.A.R.S./config/mars_example.yaml)

Полный пример:

```yaml
probe:
  levels:
    - l1
    - l2
    - l3

  limits:
    l1_max: 128KiB
    l2_max: 2MiB
    l3_max: 128MiB

  measurement:
    cache_min_lines: 16
    seed: 48815
    warmup_iterations: 4
    precision: 64
    target_accesses: 2000000
    min_iterations: 1
    max_iterations: 1000

  environment:
    bind_cpu: null
    realtime_priority: false
    lock_frequency: false
```

---

## Структура YAML

### `probe.levels`

Список уровней cache, которые нужно измерять.

Поддерживаемые значения:

| Значение | Смысл |
|---|---|
| `l1` | L1 data cache |
| `l1d` | То же, что `l1` |
| `l2` | L2 cache |
| `l3` | L3 cache |

Пример:

```yaml
probe:
  levels:
    - l1
    - l2
```

Если указать неподдерживаемое значение, загрузка завершится ошибкой.

### `probe.limits`

Максимальные размеры диапазона поиска границы cache.

| Параметр | Тип | Описание |
|---|---|---|
| `l1_max` | размер | Верхняя граница для поиска L1 |
| `l2_max` | размер | Верхняя граница для поиска L2 |
| `l3_max` | размер | Верхняя граница для поиска L3 |

Пример:

```yaml
probe:
  limits:
    l1_max: 128KiB
    l2_max: 2MiB
    l3_max: 128MiB
```

### `probe.measurement`

Параметры алгоритма измерения.

| Параметр | Тип | Описание |
|---|---|---|
| `cache_min_lines` | число / размер | Минимальный стартовый размер в линиях cache |
| `seed` | целое без знака | Seed для случайной перестановки списка |
| `warmup_iterations` | число | Количество прогревочных проходов |
| `precision` | число / размер | Точность бинарного уточнения границы |
| `target_accesses` | число / размер | Целевое число обращений в одном тесте |
| `min_iterations` | число / размер | Нижняя граница числа итераций |
| `max_iterations` | число / размер | Верхняя граница числа итераций |

Пример:

```yaml
probe:
  measurement:
    cache_min_lines: 16
    seed: 48815
    warmup_iterations: 4
    precision: 64
    target_accesses: 2000000
    min_iterations: 1
    max_iterations: 1000
```

### `probe.environment`

Параметры окружения измерения.

| Параметр | Тип | Описание |
|---|---|---|
| `bind_cpu` | `null` или integer | Привязать поток к конкретному CPU |
| `realtime_priority` | boolean | Поднять приоритет процесса |
| `lock_frequency` | boolean | Попытаться зафиксировать частоту CPU |

Пример:

```yaml
probe:
  environment:
    bind_cpu: 0
    realtime_priority: true
    lock_frequency: true
```

Если не нужна привязка к ядру:

```yaml
probe:
  environment:
    bind_cpu: null
```

---

## Формат размеров

Некоторые параметры YAML принимают размеры с суффиксами.

Поддерживаются:

| Формат | Пример |
|---|---|
| байты | `64`, `64b` |
| килобайты | `128k`, `128kb`, `128KiB` |
| мегабайты | `2m`, `2mb`, `2MiB` |
| гигабайты | `1g`, `1gb`, `1GiB` |

Замечания:

- регистр не важен
- значения парсятся как положительное целое
- в коде `kb`, `mb`, `gb` тоже трактуются как степени двойки

Примеры:

```yaml
l1_max: 128KiB
l2_max: 2MiB
precision: 64
target_accesses: 2000000
```

---

## Что переопределяется через CLI, а что через YAML

| Источник | Что задает |
|---|---|
| CLI | путь к конфигу, логирование, вывод summary |
| YAML | параметры измерения cache |

То есть:

- `--config` выбирает YAML-файл
- `--log-level`, `--log-file`, `--no-console` управляют логированием
- `--no-summary` отключает печать итоговой сводки
- все параметры `probe.*` читаются только из YAML

---

## Примеры сценариев

### Измерить только L1 и L2

```yaml
probe:
  levels:
    - l1
    - l2
```

Запуск:

```bash
./build/mars --config config/custom.yaml
```

### Запустить с логом в файл

```bash
./build/mars --config config/mars_example.yaml --log-file mars.log
```

### Запустить без summary, но с debug-логом

```bash
./build/mars --log-level debug --no-summary
```

### Запустить с фиксацией CPU и повышенным приоритетом

```yaml
probe:
  environment:
    bind_cpu: 0
    realtime_priority: true
    lock_frequency: true
```

Запуск:

```bash
./build/mars --config config/strict.yaml
```

---

Такой конфиг тоже валиден: остальные параметры будут взяты из значений по умолчанию в коде.

---

## Полезные пути

| Что | Путь |
|---|---|
| Точка входа CLI | [`src/cli/main.cpp`](/home/tiomik/projectX/M.A.R.S./src/cli/main.cpp) |
| Парсер CLI-опций | [`include/app/config.hpp`](/home/tiomik/projectX/M.A.R.S./include/app/config.hpp) |
| Загрузка YAML | [`src/app/config_loader.cpp`](/home/tiomik/projectX/M.A.R.S./src/app/config_loader.cpp) |
| Пример конфига | [`config/mars_example.yaml`](/home/tiomik/projectX/M.A.R.S./config/mars_example.yaml) |

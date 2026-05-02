# Docker для M.A.R.S.

## Что дают Docker и Docker Compose

Docker не гарантирует, что микроархитектурные бенчмарки будут одинаково точными на любой машине. Контейнер использует CPU и kernel хоста, поэтому на результаты всё равно влияют:

- модель процессора
- настройки governor и turbo на хосте
- права на `perf_event_open`
- возможность писать в `/sys`
- ограничения по `sched_setscheduler`, `mlockall` и `cpuset`

Поэтому в репозитории добавлены два сценария:

- `mars` — portable-режим без расширенных прав
- `mars-strict` — режим для более точных замеров с расширенными правами

## Разбор Dockerfile

Файл [`Dockerfile`](/home/rach/rAch-kaplin/M.A.R.S./Dockerfile) делает следующее:

1. Берёт базовый образ `ubuntu:24.04`.
2. Устанавливает системные зависимости через `apt`.
3. Копирует исходники в `/opt/mars`.
4. Запускает `cmake` и собирает проект.
5. Делает [`build/src/cli/mars`](/home/rach/rAch-kaplin/M.A.R.S./build/src/cli/mars) точкой входа контейнера.

### Какие зависимости ставятся

Зависимости тянутся из стандартных `apt`-репозиториев Ubuntu:

- `build-essential` — компилятор и базовые инструменты сборки
- `cmake` — генерация и конфигурация проекта
- `ninja-build` — быстрый backend для сборки
- `pkg-config` — вспомогательный инструмент поиска библиотек
- `git` — полезен для сборочных сценариев и диагностики
- `ca-certificates` — корректная работа HTTPS внутри контейнера
- `libcli11-dev` — библиотека CLI-аргументов
- `libspdlog-dev` — логирование
- `libyaml-cpp-dev` — чтение YAML-конфигов
- `libasmjit-dev` — генерация машинного кода для низкоуровневых тестов
- `libpfm4-dev` — доступ к именованным PMU events через perfmon

Эти пакеты соответствуют тому, что требует ваш [`CMakeLists.txt`](/home/rach/rAch-kaplin/M.A.R.S./CMakeLists.txt:1):

- `find_package(CLI11 CONFIG REQUIRED)`
- `find_package(spdlog CONFIG REQUIRED)`
- `find_package(yaml-cpp CONFIG REQUIRED)`
- `find_package(asmjit REQUIRED)`
- `libpfm4` ищется через `find_path` и `find_library`

### Что делают `ENTRYPOINT` и `CMD`

В Dockerfile прописано:

```dockerfile
ENTRYPOINT ["/opt/mars/build/src/cli/mars"]
CMD ["--config", "config/mars_example.yaml"]
```

Это означает:

- контейнер по умолчанию запускает сам бинарник `mars`
- если не передавать аргументы, он использует `--config config/mars_example.yaml`
- если передать свои аргументы, они будут добавлены к `ENTRYPOINT`

Пример:

```bash
docker run --rm mars-cli:local --log-level debug
```

## Разбор docker-compose.yml

Файл [`docker-compose.yml`](/home/rach/rAch-kaplin/M.A.R.S./docker-compose.yml) описывает два сервиса.

### Сервис `mars`

Это обычный режим запуска без повышенных привилегий.

Что он делает:

- собирает образ из текущего репозитория
- монтирует локальную папку `./config` в контейнер как `/opt/mars/config`
- монтирует локальную папку `./logs` в контейнер как `/opt/mars/logs`
- запускает `mars` с `config/mars_example.yaml`

### Сервис `mars-strict`

Это режим для более “честных” замеров, когда пользователь готов дать контейнеру дополнительные права.

В нём есть несколько важных настроек.

#### `cpuset: "${MARS_CPUSET:-0}"`

Ограничивает контейнер конкретным CPU. По умолчанию это CPU `0`.

Если хотите другой CPU:

```bash
MARS_CPUSET=3 docker compose run --rm mars-strict
```

Важно:

- если в YAML у вас `bind_cpu: 0`, а контейнер ограничен CPU `3`, то это потенциальное несовпадение
- в строгом режиме лучше держать `bind_cpu` и `MARS_CPUSET` согласованными

#### `privileged: true`

Даёт контейнеру расширенные права.

Это самое широкое из использованных разрешений, и оно добавлено намеренно, потому что ваш проект может:

- писать в `/sys/devices/system/cpu/.../cpufreq/...`
- менять governor
- трогать turbo boost
- работать с низкоуровневыми механизмами, которые часто упираются в ограничения контейнера

Если потом захотите ужесточить профиль, можно будет попробовать перейти на более узкий набор `cap_add`, но для первого рабочего варианта `privileged` надёжнее.

#### `security_opt: ["seccomp=unconfined"]`

У Docker часто есть seccomp-ограничения на низкоуровневые системные вызовы. Для PMU и `perf_event_open` это может быть критично.

Эта настройка убирает стандартный seccomp-профиль Docker для контейнера.

#### `ulimits`

```yaml
ulimits:
  rtprio: 99
  memlock:
    soft: -1
    hard: -1
```

Зачем это нужно:

- `rtprio: 99` позволяет поднимать realtime priority
- `memlock: -1` убирает лимит на закрепление памяти

Это напрямую связано с вашим кодом:

- `pthread_setschedparam(..., SCHED_FIFO, ...)`
- `mlockall(MCL_CURRENT | MCL_FUTURE)`

#### `- /sys:/sys:rw`

Пробрасывает sysfs хоста в контейнер с правом записи.

Нужно, потому что ваш код работает с:

- `/sys/devices/system/cpu/cpuX/cpufreq/scaling_governor`
- `/sys/devices/system/cpu/cpufreq/boost`
- `/sys/devices/system/cpu/intel_pstate/no_turbo`

Без этого `lock_frequency` и отключение turbo обычно не сработают.

## Когда использовать какой режим

Используйте `mars`, если нужно:

- просто собрать и запустить проект
- дать пользователю простой onboarding
- проверить CLI, конфиги и базовый пайплайн

Используйте `mars-strict`, если нужно:

- тестировать `bind_cpu`
- тестировать `realtime_priority`
- тестировать `lock_frequency`
- проверять PMU-события и низкоуровневые измерения в более строгом окружении

## Как собрать и запустить

### 1. Сборка образа через Docker

```bash
docker build -t mars-cli:local .
```

### 2. Обычный запуск через Docker

```bash
docker run --rm mars-cli:local
```

### 3. Запуск с явным конфигом и debug-логом

```bash
docker run --rm \
  -v "$(pwd)/config:/opt/mars/config:ro" \
  -v "$(pwd)/logs:/opt/mars/logs" \
  mars-cli:local \
  --config config/mars_example.yaml \
  --log-level debug \
  --log-file logs/docker-run.log
```

### 4. Строгий запуск через Docker

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

### 5. Сборка через Docker Compose

```bash
docker compose build
```

### 6. Обычный запуск через Docker Compose

```bash
docker compose run --rm mars
```

### 7. Строгий запуск через Docker Compose

```bash
docker compose run --rm mars-strict
```

### 8. Строгий запуск через Docker Compose на другом CPU

```bash
MARS_CPUSET=3 docker compose run --rm mars-strict
```

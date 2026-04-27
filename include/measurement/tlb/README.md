# TLB Benchmark

## 1. Какую задачу рассматриваем

В этой секции мы оцениваем ёмкость `TLB` для трансляции виртуальных адресов в физические.

Нас интересуют два уровня:

- `L1 DTLB`
- `L2 TLB / STLB`

Идея эксперимента такая:

- процессор читает данные из большого числа разных виртуальных страниц;
- пока все трансляции помещаются в `TLB`, доступ остаётся дешёвым;
- когда число страниц становится слишком большим, latency растёт;
- по первым двум устойчивым перегибам можно оценить размеры `L1 DTLB` и `L2/STLB`.

Важно: benchmark измеряет именно поведение data-side translation, а не читает аппаратные спецификации напрямую.

## 2. Какой результат хотим получить

По итогам benchmark должен дать две основные метрики:

- `L1 DTLB estimate: N pages`
- `L2/STLB estimate: M pages`

Дополнительно эти же значения переводятся в coverage в байтах:

- `N * page_size`
- `M * page_size`

Пример итогового вывода:

```text
L1 DTLB estimate: 64 pages (~262144 bytes coverage)
L2/STLB estimate: 512 pages (~2097152 bytes coverage)
```

Основная внутренняя метрика, по которой строится это решение:

- `median cycles per access`

Именно по росту `median cycles/access` при увеличении числа страниц детектор находит точки перегиба.

## 3. Какой алгоритм используется

### Общая схема

Алгоритм состоит из трёх частей:

1. подготовить набор страниц и workload;
2. измерить latency для разных размеров рабочего множества;
3. найти два первых устойчивых скачка latency.

### Подготовка workload

Benchmark:

- строит sweep по числу страниц: `1, 2, 4, 8, ... max_pages`;
- выделяет память под расширенный пул страниц;
- размещает по одному `PageNode` в каждой странице;
- разносит offset внутри страницы по разным cache line, чтобы не ловить ложные `L1D` conflicts;
- делает `pretouch`, чтобы не мерить page faults;
- прогревает instruction path до основного sweep.

Это реализовано в:

- [tlb_measurer.hpp](/home/rach/rAch-kaplin/M.A.R.S./include/measurement/tlb/tlb_measurer.hpp:150)

Ключевые helper-функции:

- `build_page_counts()`
- `allocate_mapping()`
- `page_node_views()`
- `pretouch_pages()`
- `warm_instruction_path()`

### Как выполняется измерение

Для каждой точки `pages = N` benchmark:

1. случайно перемешивает подготовленный пул страниц;
2. берёт первые `N` страниц как рабочее множество;
3. связывает их в циклический список;
4. делает warm-up;
5. запускает горячий цикл pointer chasing;
6. считает `cycles/access`;
7. повторяет это несколько раз и сохраняет `min / median / mean / max`.

Горячий цикл делает только зависимые загрузки:

```cpp
cursor = cursor->next;
```

Это важно, потому что:

- следующий адрес зависит от предыдущего чтения;
- предвыборка и параллелизм памяти влияют меньше;
- измерение лучше отражает именно latency translation + access.

Это реализовано в:

- `measure_point()`
- `warmup()`
- `measure_cycles()`

Код:

- [tlb_measurer.hpp](/home/rach/rAch-kaplin/M.A.R.S./include/measurement/tlb/tlb_measurer.hpp:401)

### Как находятся L1 и L2

После того как кривая собрана, benchmark:

1. строит сглаженную версию `median cycles/access`;
2. считает baseline по первым точкам;
3. ищет первый устойчивый скачок как кандидат в `L1 DTLB`;
4. после небольшого зазора ищет второй устойчивый скачок как кандидат в `L2/STLB`.

Чтобы не ловить слишком ранний шум, точки меньше `64 pages` не рассматриваются как кандидат на `L1`.

Это реализовано в:

- `detect_boundaries()`
- `moving_average()`
- `find_jump()`

Код:

- [tlb_measurer.hpp](/home/rach/rAch-kaplin/M.A.R.S./include/measurement/tlb/tlb_measurer.hpp:441)

### Какие параметры задаёт пользователь

Секция конфига специально минимальная:

```yaml
benchmarks:
  tlb:
    enabled: true
    measurement:
      max_pages: 4096
      iterations: 2000000
      huge_pages: false
```

Смысл полей:

- `max_pages` — максимальная точка sweep;
- `iterations` — сколько зависимых обращений делать в одном замере;
- `huge_pages` — использовать ли `2 MiB` huge pages вместо обычных `4 KiB`.

Парсинг:

- [config_loader.cpp](/home/rach/rAch-kaplin/M.A.R.S./src/app/config_loader.cpp:538)

Все остальные параметры benchmark зафиксированы в коде как константы, чтобы методика оставалась стабильной и не ломалась случайной настройкой.

### Что именно печатается в summary

Итоговая печать делает только две оценки:

- `L1 DTLB estimate`
- `L2/STLB estimate`

Код:

- [summary_printer.hpp](/home/rach/rAch-kaplin/M.A.R.S./include/measurement/core/summary_printer.hpp:10)

## Краткий вывод

Эта секция решает конкретную задачу: по кривой `latency vs number of pages` оценить ёмкость `L1 DTLB` и `L2/STLB`.

В коде это выражено так:

- `measure()` собирает точки;
- `measure_point()` и `measure_cycles()` строят сам эксперимент;
- `detect_boundaries()` превращает кривую в две итоговые оценки;
- `SummaryPrinter` печатает результат пользователю.

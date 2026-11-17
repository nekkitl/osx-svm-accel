# Набор исходников QEMU

База: `f9587d4045c67cd0d8d8bdcd5d0bb5b6b395b63c`, <https://github.com/qemu/qemu>. [manifest.json](../../qemu/manifest.json) фиксирует каждый путь, upstream SHA-256 (null для добавлений) и SHA-256 публикационной копии. В 51 файле — все tracked modifications и добавления AMDVM из референсного checkout. Полное дерево QEMU не вендорится.

`tools/prepare-qemu.py` проверяет точный Git HEAD, отклоняет посторонние tracked changes и валидирует все исходные/публикационные hash до копирования. Повторное применение идемпотентно. `tools/build-qemu.sh` собирает Darwin x86_64, amdsvm, Cocoa и SLIRP. Загруженные исходники и сборки игнорируются.

| Файлы / область | Назначение |
| --- | --- |
| `accel/amdsvm/amdsvm.c` | регистрация, общий экспериментальный профиль, workers, RUN/KICK, dispatch и lifecycle карты памяти VM |
| `amdsvm-vm-provider.c/.h` | VM-scoped transport, capabilities, handles/generations vCPU, map и destroy |
| `amdsvm-provider-lifecycle.c/.h` | legacy single-vCPU lifecycle старой диагностики |
| `amdsvm-firmware-dispatch.c/.h` | проверка классов выходов и callbacks эмуляции |
| `target/i386/amdsvm/amdsvm-state.c/.h` | CPU/XSTATE mapping, INIT/SIPI, эмуляция перехватов и MMIO |
| `amdsvm-event.h`, `amdsvm-msr.h` | retained guest events и модель guest MSR |
| `amdsvm-memory*` | FlatView, ROM aliases, ABI maps, типы памяти из MTRR |
| `target/i386/emulate/*`, `target/i386/hvf/*` | совместное использование x86 helpers с сохранением upstream notices |
| `hw/display/vga.c`, `system/ioport.c` | ограниченная диагностика device I/O из референсной разработки |
| Meson/Kconfig/options, `svm.h` | регистрация, IOKit, build integration и закреплённый SLIRP fallback |
| `tests/unit/test-amdsvm-*`, unit Meson | восемь офлайн-тестовых программ |

Публичный флаг `confirm-experimental-run=on` выбирает VM API с 1–8 vCPU и 256 МиБ–16 ГиБ. Изменения относительно v5 в [public-interface.diff](../../qemu/public-interface.diff). QEMU использует shared RUN, если провайдер объявил эту возможность. Архивный кандидат собран с поддержкой PAUSE и native CR3. Опции shared RUN и native RDTSC при его сборке выключены.

Frontend не выбирает boot payload, ISO или kernel command line гостя. Byte matchers конечных ROM остаются отдельными диагностическими режимами. См. [сборку и запуск](build-and-run.md), [манифест](../../MANIFEST.json).

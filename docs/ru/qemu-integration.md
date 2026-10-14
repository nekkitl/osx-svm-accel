# Набор исходников QEMU

База: `f9587d4045c67cd0d8d8bdcd5d0bb5b6b395b63c`, <https://github.com/qemu/qemu>. [manifest.json](../../qemu/manifest.json) фиксирует каждый путь, upstream SHA-256 (null для добавлений) и SHA-256 публикационной копии. В 40 файлах — все tracked modifications и добавления AMDVM из референсного checkout. Полное дерево QEMU не вендорится.

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
| `tests/unit/test-amdsvm-*`, unit Meson | шесть офлайн-тестовых программ |

Отличия публичного frontend от аппаратно проверенного бинарника: `confirm-alpine-smp` заменён на `confirm-experimental-run`, этот флаг выбирает VM API, допускаются 1–2 vCPU / 256 МиБ–4 ГиБ вместо двух точных размеров RAM. World-switch, обработка выходов и исправление AP HLT-флага сохранены. Более широкий диапазон допуска и другие ОС не доказаны прежними тестами.

Frontend не выбирает boot payload, ISO или kernel command line гостя. Byte matchers конечных ROM остаются отдельными диагностическими режимами. См. [сборку и запуск](build-and-run.md), [манифест](../../MANIFEST.json).

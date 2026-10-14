# Архитектура

```text
Модель устройств QEMU + отдельный worker каждого vCPU
             |
      VM-scoped IOUserClient ABI
             |
 AMDVMProbe.kext: владелец pinned RAM / контексты vCPU
             |
    NPT + VMCB + GPR/XSTATE world switch
             |
         AMD SVM / VMRUN
```

Provider получает архитектурное состояние x86 и карту гостевой физической памяти. Он не определяет гостевую ОС. QEMU сохраняет эмуляцию устройств, CPUID policy, модель guest MSR и диспетчеризацию VMEXIT. Неподдерживаемые операции явно отклоняются; выбор другого ISO не добавляет отсутствующие возможности.

Соединение согласует legacy single-vCPU mode либо VM-scoped API. VM владеет закреплённой картой памяти; у каждого vCPU независимы CPU/event state и NPT. RUN/KICK generations, admission и ownership запрещают одновременный RUN одного vCPU и согласуют teardown с незавершёнными операциями. При замене карты памяти sibling vCPU выводятся из RUN до commit. Механизмы покрыты тестами и короткими аппаратными прогонами; общая безопасность всех гонок/отказов не доказана.

VMCB хранит только часть состояния гостя. Остальные GPR и отдельный guest XSTATE переносятся программно. Kernel wrapper сохраняет проверяемую границу хоста, использует physical interrupt exits и восстанавливает SVM ownership. FPU context хоста, CPU migration, IF/GIF и kernel exports входят в контракт исполнения и не являются особенностями конкретной гостевой ОС.

LAPIC INIT/SIPI проходят через штатную x86-модель QEMU. Принятый startup сбрасывает прежний HLT-флаг акселератора; INIT также очищает retained guest events. Иначе HLT SeaBIOS мог блокировать последующий Linux AP trampoline.

Публичный `confirm-experimental-run` включает VM API и принимает обычные `-m`/`-smp` в пределах 1–2 vCPU и 256 МиБ–4 ГиБ. Конечные ROM и no-RUN диагностика имеют отдельные проверки. Возможности provider и подготовка памяти проверяются независимо от политики frontend.

См. [интеграцию QEMU](qemu-integration.md), [матрицу](hardware.md), [риски](risks.md), отдельное [предложение AMD-HVF](amd-hvf-proposal.md).

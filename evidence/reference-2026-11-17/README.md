# Runtime evidence for the 2026-11-17 snapshot

Reference host: Ryzen 7 5700X, macOS Tahoe 26.0.1. Time is measured from QEMU start to Alpine's serial `login:` prompt. These runs answer whether a particular guest booted and how long that boot took. They do not measure CPU throughput or certify every admitted resource profile.

## AMDVM boot history

| Date | Profile | Build pair / change | Time to login |
| --- | --- | --- | ---: |
| Oct 18 | 8 vCPU / 4 GiB | Before shared NPT | 56.13 s |
| Oct 21 | 8 vCPU / 4 GiB | Shared NPT, provider E6F8 | 34.73 s |
| Oct 26 | 8 vCPU / 4 GiB | A46E / QEMU b32e, shared RUN active | 20.221 s |
| Oct 29 | 8 vCPU / 8, 12, 16 GiB | ACEDE / QEMU 9378, one observation per size | 36.096 / 44.762 / 55.885 s |

The first and third observations are about **2.78× apart in time to login**. Builds and option sets changed across runs. The comparison does not isolate the gain of any individual optimization. The RAM-map refresh window was 19.48 s on Oct 18 and 4.04 s on Oct 21 at 8 vCPU / 4 GiB.

## Later rerun and controls

Oct 30, B77E provider plus QEMU e5a7. Each attempt used a fresh disposable overlay. TCG controls at 12 and 16 GiB used the same QEMU and base disk.

| Guest profile | AMDVM login | TCG login |
| --- | ---: | ---: |
| 1 vCPU / 256 MiB | No login, early guest #PF | — |
| 8 vCPU / 4 GiB | 28.523 s | — |
| 8 vCPU / 8 GiB | 35.629 s | — |
| 8 vCPU / 12 GiB | No login, early guest #PF | 21.004 s |
| 8 vCPU / 16 GiB | No login, early guest #PF | 20.738 s |

The recorded attempts completed stop/quit and preserved the base disk. A separate 12-GiB run with PAUSE and native CR3 disabled reached login at 43.069 s. That outcome was inconsistent across the recorded attempts. A later 0F03 candidate also failed to reach login at 1 vCPU / 256 MiB and 8 vCPU / 4 GiB. The guest reported a not-present page fault while the subsequent guest page-table walk found a present mapping. The cause is open.

A separate Oct 29 control series with QEMU d900 reached login under TCG in 18.549 s at 1 vCPU / 256 MiB, then 16.884, 16.471, 16.659 and 17.899 s at 8 vCPU / 4, 8, 12 and 16 GiB. Those numbers belong to that series. They cannot be mixed with the Oct 30 controls to form paired speedups.

## Windows 10

Windows 10 22H2 completed installation on the reference host. On Oct 25 the installed system booted from a QCOW2 overlay to the desktop with Q35, OVMF, an EPYC-Milan-v1 guest CPU model, 8 vCPUs and 4 GiB. QMP stop and quit succeeded, QEMU exited with status 0, and the base disk hash was unchanged. This run used provider `3FCFF7ED-D56F-32DB-AEBC-4EB064FBBB0F` and QEMU SHA-256 `74814c63440abcc9696e5496b0cc752120ec89b33680d969c0d9a8958ccf36a8`. A separate later run reached desktop and then hit a `MEMORY MANAGEMENT` bugcheck. Long-run stability remains open.

The packaged E71D bundle is an unsigned reproduction of the Oct 30 diagnostic build. macOS declined to load the signed lab copy pending approval. E71D has no hardware guest run on record. See [release manifest](../../MANIFEST.json) and [selected run metadata](runs.json).

## Замеры и ограничения

На Ryzen 7 5700X с macOS Tahoe 26.0.1 время до приглашения входа Alpine на 8 vCPU / 4 ГиБ сократилось с 56,13 с 18 октября до 34,73 с после перехода на shared NPT и до 20,221 с 26 октября. Это примерно в 2,78 раза между первым и последним замерами. Между прогонами менялись сборки и флаги, поэтому цифры показывают динамику проекта. Вклад отдельных оптимизаций по ним определить нельзя.

В матрице 30 октября связка B77E и QEMU e5a7 дошла до входа за 28,523 с на 4 ГиБ и за 35,629 с на 8 ГиБ при 8 vCPU. Профили с 12 и 16 ГиБ, а также 1 vCPU / 256 МиБ остановились на раннем `#PF`. Контрольный TCG с тем же QEMU и образом загрузился на 12 и 16 ГиБ за 21,004 и 20,738 с. В более ранних одиночных прогонах AMDVM вход на 12 и 16 ГиБ был зафиксирован. Более поздние результаты показывают, что эта конфигурация пока работает неповторяемо.

Windows 10 22H2 прошла полную установку на референсном хосте. В прогоне 25 октября установленная система загрузилась с виртуального диска до рабочего стола с Q35, OVMF, 8 vCPU и 4 ГиБ. Остановка и выход через QMP прошли успешно, базовый диск сохранил контрольную сумму. В отдельном более позднем прогоне после появления рабочего стола произошёл сбой `MEMORY MANAGEMENT`. Длительная стабильность ещё требует проверки. В архиве лежит диагностический E71D для разбора раннего сбоя Linux. Сборка завершилась успешно. Аппаратный запуск этого кандидата пока не зафиксирован.

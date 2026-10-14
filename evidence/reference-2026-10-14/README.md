# Reference evidence / Референсные evidence

Recorded on 2026-10-14, Ryzen 7 5700X, macOS 26.0.1 (25A362). Provider UUID 7A07BC16-52EE-33C5-B239-50E6913D73E1; original unsigned binary SHA-256 82980e357394a349661f0d8e0f693c1466e87f73cbff7746ef1d4935e81713aa. The publication provider rebuild matches that binary byte for byte.

- [summary.json](summary.json): hardware SVM, two online guest cores, 256 MiB; reference QEMU UUID/hash and short workload/stop/cleanup result.
- [guest-cpu-state.txt](guest-cpu-state.txt): online=0-1, nproc=2, one socket, two core IDs, Linux reports two CPUs.
- [guest-core-workload.txt](guest-core-workload.txt): two finite 64-MiB SHA256 jobs with taskset affinities 0 and 1; [result](core-workload-result.json).
- [host-before.txt](host-before.txt), [host-after.txt](host-after.txt): matching recorded SVM MSRs on 16 host CPUs; quarantine counters zero.
- [stop-result.json](stop-result.json): QMP paused acknowledgement in 1.626 ms.
- [tcg-control.json](tcg-control.json): same two-core/256-MiB disk boot under TCG.
- [alpine-4g-before-ap-fix.json](alpine-4g-before-ap-fix.json): earlier 4-GiB Alpine boot; CPU1 offline. [Guest output](alpine-4g-before-ap-fix-cpus.txt).
- [finite-smp-4g-result.txt](finite-smp-4g-result.txt): separate two-vCPU/4-GiB finite ROM, both CPUs execute and yield; this is not an Alpine SMP result.

These selected logs are reference observations, not a complete security audit or a long stability study. Historical single-core sessions are summarized in the hardware matrix and screenshot; their full development logs are not bundled. The original hardware QEMU frontend had a guest-named admission flag. The publication frontend is generic and has separate build/no-RUN validation: [MANIFEST.json](../../MANIFEST.json), [interface delta](../../qemu/public-interface.diff).

В summary и логах различаются реальные online CPU, настроенные vCPU и тип прогона. Сравниваются конкретные SVM MSR и quarantine counters, не все регистры хоста. Обобщённый публикационный frontend проверяется отдельно от прежнего аппаратного бинарника. Полные приватные журналы, образы ОС и бинарники не входят в этот пакет.

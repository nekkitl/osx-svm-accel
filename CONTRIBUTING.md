# Contributing

AMDVM is an experimental AMD SVM provider and QEMU accelerator. Contributions to the provider, QEMU integration, documentation and reproducible hardware reports are welcome. Guest images and VM disks are not part of this repository.

## Report a result or problem

Use the issue templates for a hardware result, build problem or feature request. English and Russian reports are welcome. The hardware template follows the fields in [Hardware reports](docs/en/hardware.md#hardware-reports) ([Русский](docs/ru/hardware.md)). Report unavailable values as `unknown` and checks you did not perform as `not checked`.

Include the first failure and exact reproduction steps. Distinguish the source revision, built candidate, installed bundle, loaded kext UUID and actual hardware execution. A build or prelaunch check does not establish successful VMRUN; a configured vCPU count does not establish online guest CPUs. An error-induced shutdown does not establish working stop/kick.

Attach relevant text logs and panic reports with credentials, keys, personal information and private host paths removed. Preserve instruction addresses, UUIDs, hashes, error codes and the order of events needed to understand the failure. Do not upload guest disks, installation media, SDK/KDK files or third-party binaries.

## Submit a change

1. Describe the problem, intended behavior and affected layer before expanding the implementation.
2. Keep the patch focused and preserve existing ABI layouts and version negotiation. Explain any compatibility change explicitly.
3. Use the pinned QEMU revision and overlay workflow in [Build and run](docs/en/build-and-run.md). Keep upstream license notices and document the origin of imported code in [THIRD_PARTY.md](THIRD_PARTY.md).
4. Add or update relevant offline checks for behavior changes. Record the commands and results in the pull request, including failures and checks not performed.
5. Update English and Russian documentation when changing user-visible behavior or limitations. Write each prose paragraph on one source line; keep commands, lists and tables structured.
6. Summarize what changed, why, evidence obtained, remaining limitations and any rollback implications. Do not include generated build directories or binaries in the patch.

## Checks and hardware boundary

```sh
make provider
make checks
make qemu
make qemu-checks
```

`make provider` builds the default no-RUN provider. These commands do not install or load a kext or execute a guest. See [Build and run](docs/en/build-and-run.md) for the experimental build, signing, manual installation and rollback procedure. State which checks apply to your change; hardware access is not required for documentation contributions.

Hardware execution is a separate operator decision on a suitable test host. Read [Risks](docs/en/risks.md), retain a known artifact and rollback path, and verify the loaded UUID before execution. Report only observed results. Do not disable validation to turn a failing case into a passing result, and do not treat successful restoration of SVM MSRs as proof of all host state.

Original contributions use GPL-2.0-or-later; upstream-derived files retain their applicable notices and licenses. See [LICENSE](LICENSE) and [THIRD_PARTY.md](THIRD_PARTY.md).

## Кратко по-русски

Issues и pull requests можно писать по-русски. Для аппаратного отчёта используйте шаблон и поля из [hardware.md](docs/ru/hardware.md). Разделяйте сборку, установку, фактически загруженный UUID и аппаратный результат; указывайте первый отказ, реальные online CPU гостя, stop/cleanup и непроведённые проверки. Удаляйте секреты и личные пути из журналов. Делайте узкие изменения, сохраняйте ABI и лицензии, обновляйте EN/RU документацию. Один абзац — одна строка исходника. Сборка и офлайн-проверки не требуют аппаратного RUN; загрузка kext и VMRUN выполняются отдельно оператором по инструкции с возможностью отката.

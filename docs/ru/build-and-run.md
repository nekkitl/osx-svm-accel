# Сборка, запуск и откат

Сначала прочтите [риски](risks.md). Команды выполняются из корня репозитория на x86_64 macOS. Для аппаратного исполнения нужен тестовый хост с SVM/NPT. Установите Xcode Command Line Tools, Python 3, Git, Ninja, pkg-config, GLib и Pixman; их executables/pkg-config должны быть доступны в PATH. Configure QEMU создаёт Python environment и получает закреплённые subprojects; может понадобиться сеть. В репозитории нет прошивок, образов ОС, SDK/KDK или сторонних бинарников.

## 1. Сборка и проверки

```sh
make provider-experimental
make checks
make qemu
make qemu-checks
```
`provider-experimental` включает legacy entry и VM-scoped RUN/KICK; результат лежит в `svm-kext/build/experimental/`. Обычный `make provider` оставляет RUN закрытым. Эти цели не устанавливают/загружают kext и не вызывают VMRUN. QEMU checkout `f9587d4045c67cd0d8d8bdcd5d0bb5b6b395b63c` помещается в игнорируемый `qemu-src/`; поверх применяется проверенный по checksum набор исходников. Результат сборки — `qemu-build/`.

Необязательный аудит импортов: задайте `XNU_KPI_EXPORTS` как путь к соответствующему списку экспортов и выполните одной командой: `python3 svm-kext/audit_kpi_imports.py svm-kext/build/experimental/AMDVMProbe.kext/Contents/MacOS/AMDVMProbe "$XNU_KPI_EXPORTS"`. AMDVM не требует собирать собственный XNU. Аудит списка экспортов не доказывает разрешение символов работающим ядром.

## 2. Подпись, идентичность и ручная установка

Перед заменой загруженного provider закройте существующих гостей и проверьте stop/cleanup.

Ниже показана ad-hoc подпись для проверки целостности. Она не даёт доверенного одобрения kext; используйте подходящие identity и политику загрузки своего хоста. Пакет не изменяет SIP/AMFI и boot policy.

```sh
codesign --force --sign - svm-kext/build/experimental/AMDVMProbe.kext
codesign --verify --strict --verbose=4 svm-kext/build/experimental/AMDVMProbe.kext
xcrun dwarfdump --uuid svm-kext/build/experimental/AMDVMProbe.kext/Contents/MacOS/AMDVMProbe
shasum -a 256 svm-kext/build/experimental/AMDVMProbe.kext/Contents/MacOS/AMDVMProbe
```
Подпись может изменить SHA-256; фиксируйте окончательный артефакт после неё. Манифест отдельно указывает исходный аппаратно проверенный бинарник. Сохраните путь `backup_dir` ниже, чтобы он оставался известен после reboot.

```sh
backup_dir=$(mktemp -d /var/tmp/amdvm-rollback.XXXXXX)
echo "$backup_dir"
if [ -d /Library/Extensions/AMDVMProbe.kext ]; then
  sudo ditto /Library/Extensions/AMDVMProbe.kext "$backup_dir/AMDVMProbe.kext"
fi
sudo ditto svm-kext/build/experimental/AMDVMProbe.kext /Library/Extensions/AMDVMProbe.kext
sudo chown -R root:wheel /Library/Extensions/AMDVMProbe.kext
sudo chmod -R a+rX /Library/Extensions/AMDVMProbe.kext
sudo kmutil print-diagnostics --bundle-path /Library/Extensions/AMDVMProbe.kext
sudo kmutil load -p /Library/Extensions/AMDVMProbe.kext
```
При запросе macOS одобрите именно это расширение в System Settings и перезагрузитесь. Копирование bundle само по себе не обновляет Auxiliary Kernel Collection. После reboot сравните загруженный UUID с кандидатом:

```sh
kmutil showloaded -b com.nekkit.amdvm.probe --show loaded
xcrun dwarfdump --uuid /Library/Extensions/AMDVMProbe.kext/Contents/MacOS/AMDVMProbe
sudo svm-kext/build/experimental/amdvm-probe
sudo svm-kext/build/experimental/amdvm-probe --vm-negotiate
```
Эти команды probe не выполняют VMRUN. При несовпадении UUID/ABI или неразрешённых символах сначала устраните конкретную ошибку загрузки. Версии SDK/KDK и фактические kernel exports могут отличаться между хостами.

## 3. Свои образы и ресурсы

Используйте собственный установочный ISO и QCOW2. Пример задаёт 2 ядра и 256 МиБ; публичная политика допускает 1–2 vCPU и 256 МиБ–4 ГиБ. Provider может отклонить подготовку памяти из-за ресурсов или типов памяти. Устанавливайте `confirm-no-kdp-watchpoints` только при известном выполнении условия.

```sh
./qemu-build/qemu-img create -f qcow2 guest.qcow2 8G
# Set VM_ISO to the ISO you selected before running this command.
: "${VM_ISO:?Set VM_ISO to your installation ISO}"
QMP_SOCKET=/tmp/amdvm-qmp-$$.sock
sudo ./qemu-build/qemu-system-x86_64 \
  -accel amdsvm,confirm-experimental-run=on,confirm-no-kdp-watchpoints=on \
  -machine pc -m 256M -smp cpus=2,sockets=1,cores=2,threads=1 \
  -L ./qemu-src/pc-bios -display cocoa \
  -drive file=guest.qcow2,format=qcow2,if=virtio \
  -cdrom "$VM_ISO" -boot once=d \
  -vga none -device virtio-gpu-pci,xres=1280,yres=800 \
  -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
  -qmp unix:"$QMP_SOCKET",server=on,wait=off
```
Для последующих загрузок с диска уберите `-cdrom` и `-boot once=d`. Акселератор не ограничивает образы Alpine, но пока проверена только эта ОС. Пример использует VirtIO: другой ОС могут потребоваться драйверы; также возможен неподдерживаемый CPU/device path. Акселератор не подставляет kernel arguments, клавиши загрузчика или команды внутри гостя.

## 4. Наблюдаемая остановка и cleanup

В другом терминале укажите тот же сохранённый путь QMP socket. Для обычного использования предпочтительно выключение гостевой ОС. Для контролируемого pause/teardown:

```sh
sudo python3 tools/qmp.py --socket "$QMP_SOCKET" status
sudo python3 tools/qmp.py --socket "$QMP_SOCKET" stop
sudo python3 tools/qmp.py --socket "$QMP_SOCKET" quit
sudo svm-kext/build/experimental/amdvm-probe
```
Helper запрещает quit для работающей VM и проверяет paused после stop. При отказе/таймауте stop сохраните диагностику: убийство процесса не гарантирует восстановления. Сравните SVM MSR с pre-run snapshot и проверьте нулевые quarantine counters. Это проверка конкретных свойств, не полного восстановления всех регистров хоста.

## 5. Откат provider

Сначала закройте гостей с проверкой stop/cleanup. Если предыдущий bundle был сохранён, восстановите его по записанному `backup_dir`:

```sh
sudo ditto "$backup_dir/AMDVMProbe.kext" /Library/Extensions/AMDVMProbe.kext
sudo chown -R root:wheel /Library/Extensions/AMDVMProbe.kext
sudo chmod -R a+rX /Library/Extensions/AMDVMProbe.kext
sudo kmutil load -p /Library/Extensions/AMDVMProbe.kext
```
Выполните запрошенное системой одобрение/reboot и проверьте загруженный UUID. Если предыдущего provider не было, используйте процедуру удаления для своей версии macOS. Не оставляйте дубликаты bundle identifier в Extensions.

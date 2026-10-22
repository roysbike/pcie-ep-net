# pcie-ep-net (openmiop)

[English](README.md) · [Русский](README.ru.md)

**Ethernet между платами Mixtile Blade 3 внутри Mixtile Cluster Box через
встроенный PCIe-коммутатор шасси.** Каждый блейд получает сетевой
интерфейс `omi0`. Кадр от одного блейда к другому — это DMA-запись из
памяти отправителя прямо в PCIe BAR получателя: он проходит через
коммутатор и не попадает в BMC Cluster Box.

Реализация называется **openmiop** (интерфейс `omi0`, сигнатура в
протоколе `OMI1`). Это независимая реализация, не драйвер MIOP от
Mixtile; кода и прошивок Mixtile в ней нет. Лицензия: GPL-2.0-or-later
([LICENSE](LICENSE)).

**Текущий релиз: [v0.6.1](https://github.com/roysbike/pcie-ep-net/releases/tag/v0.6.1)**
(2026-10-21), входит в OpenMIOP Stack v0.6.1.
Release notes: [v0.6.1](docs/release-notes/v0.6.1.md) ·
[v0.6.0](docs/release-notes/v0.6.0.md) · [CHANGELOG.md](CHANGELOG.md).

## Содержание

* [OpenMIOP Stack](#openmiop-stack)
* [Возможности](#возможности)
* [Оборудование](#оборудование)
* [Как это работает](#как-это-работает)
* [Требования](#требования)
* [Установка](#установка)
* [Проверка](#проверка)
* [Настройка](#настройка)
* [Обновление и смешанные версии](#обновление-и-смешанные-версии)
* [Производительность](#производительность)
* [Известные ограничения](#известные-ограничения)
* [Диагностика](#диагностика)
* [Сборка](#сборка)
* [Структура репозитория](#структура-репозитория)

## OpenMIOP Stack

Mixtile поставляет Cluster Box со своей прошивкой BMC и своим сетевым
драйвером для блейдов (MIOP). **OpenMIOP Stack** — независимая открытая
замена из трёх репозиториев, которые выпускаются вместе:

| Репозиторий | Что даёт | Stack v0.6.1 |
| --- | --- | --- |
| **pcie-ep-net** (этот) | драйвер блейда `openmiop-ep` (`omi0`), модуль BMC `omi-rc`, модули платы Blade 3 | [v0.6.1](https://github.com/roysbike/pcie-ep-net/releases/tag/v0.6.1) |
| [mixtile-talos](https://github.com/roysbike/mixtile-talos) | Talos Linux (Kubernetes) для Blade 3 со встроенным openmiop | [v0.6.1](https://github.com/roysbike/mixtile-talos/releases/tag/v0.6.1) (Talos v1.14.4) |
| [mixtile-clusterbox-mt7620a-openwrt](https://github.com/roysbike/mixtile-clusterbox-mt7620a-openwrt) | OpenWrt для BMC: питание и консоли блейдов, сторона BMC в openmiop, сетевой край кластера, веб-интерфейс, обновления | [v0.4.0](https://github.com/roysbike/mixtile-clusterbox-mt7620a-openwrt/releases/tag/v0.4.0) (OpenWrt 25.12.6) |

Протокол Stack v0.6.1: кадры OpenMIOP v4 между блейдами с возможностями,
которые согласуются для каждого соединения (RX doorbell, несколько
очередей, checksum offload); push-режим, gateway pull и zero-copy в
сторону BMC.

## Возможности

* **Быстрая сеть между блейдами.** 7,8-8,0 Гбит/с на один TCP-поток,
  ~11 Гбит/с на четыре потока между двумя блейдами, ~25 Гбит/с суммарно,
  когда отправляют все четыре блейда. MTU 9000 (до 9246).
* **Обычный Ethernet для ОС.** `omi0` — обычный сетевой интерфейс Linux:
  ARP, IPv4, IPv6 с neighbour discovery, broadcast и multicast, изучение
  MAC-адресов (работает за Linux bridge, с CNI Kubernetes, VXLAN/Geneve).
* **BMC в том же сегменте.** BMC — `10.20.0.1` в `omi0`; он выводит
  блейды в LAN и интернет (~220-280 Мбит/с, предел — процессор BMC).
* **Несколько очередей и прерывания.** 4 пары очередей с RSS, своё
  прерывание RX doorbell и NAPI на каждую очередь, потоки отправки и
  прерывания eDMA на ядрах Cortex-A76.
* **Checksum offload между блейдами.** Получатель принимает кадры как
  `CHECKSUM_PARTIAL`, как у virtio-net; данные проходят только по
  PCIe-линкам (защищены LCRC) и через DRAM.
* **Переживает сбросы.** Перезагрузка блейда, перезагрузка модуля и
  перезагрузка BMC восстанавливаются сами: handshake выхода при выгрузке,
  эпохи узлов, восстановление после сброса линка, блейды переподключаются
  друг к другу без BMC.
* **Безопасно для root complex.** В push-режиме BMC никогда не читает BAR
  блейда, поэтому пропавший блейд не может подвесить PCIe-фабрику.
* **Стабильная идентичность.** Номер узла = слот коммутатора; MAC-адрес
  выводится из серийного номера RK3588.
* **Обновление по одному узлу.** Возможности согласуются для каждой пары,
  поэтому блейды на разных релизах (v0.1-v0.6) работают друг с другом.
* **Модули платы Blade 3** (`drivers/blade3`): `blade3-nvme-recover`
  возвращает M.2 NVMe после сброса PCIe в Cluster Box, `blade3-leds`
  управляет индикаторами портов 2.5G.

## Оборудование

```
        BMC Cluster Box (MT7620A, OpenWrt)
        модуль omi-rc, omi0 10.20.0.1
                   |
              PCIe Gen1 x1   (управление + шлюз в LAN)
                   |
            PCIe-коммутатор ASMedia ASM2824   <- трафик блейд-блейд остаётся здесь
         /        /          \          \
    Gen3 x2   Gen3 x2      Gen3 x2    Gen3 x2
       |         |            |          |
     слот 1    слот 2       слот 3     слот 4     Mixtile Blade 3 (RK3588)
     узел 3    узел 2       узел 0     узел 1     PCIe endpoint, openmiop-ep, omi0 10.20.0.x
```

* **Mixtile Cluster Box:** шасси на четыре Blade 3. Его BMC (MediaTek
  MT7620A, MIPS 580 МГц) включает и выключает блейды, даёт доступ к их
  последовательным консолям и является PCIe root complex шасси.
  Коммутатор ASMedia ASM2824 Gen3 соединяет каждый слот (Gen3 x2) и BMC
  (Gen1 x1).
* **Mixtile Blade 3:** Rockchip RK3588 (4× Cortex-A76 + 4× Cortex-A55),
  eMMC, M.2 NVMe, два порта Ethernet 2.5G. Его контроллер PCIe 3.0 x4
  разделён на две половины: линии 0-1 идут на коммутатор Cluster Box
  (endpoint, этот драйвер), линии 2-3 — на M.2 NVMe (host).

Без Cluster Box PCIe-фабрики нет, поэтому отдельному Blade 3
`openmiop-ep` не нужен.

## Как это работает

* **Сторона блейда.** `openmiop-ep` переводит контроллер PCIe3 RK3588
  `fe150000` в режим PCIe endpoint (PCI ID `1d87:4f4d`) с одним BAR на
  16 МиБ и регистрирует `omi0`. Он программирует контроллер напрямую
  (DBI, iATU, eDMA) и не использует PCI endpoint framework ядра.
* **Сторона BMC.** Модуль ядра `omi-rc` находит endpoint'ы за
  коммутатором, восстанавливает их BAR, command и MPS, нумерует по слоту,
  публикует таблицу пиров (эпоха, адрес BAR, MAC каждого узла) в каждый
  BAR и сам подключает BMC к `omi0`. Push-режим: блейды записывают своё
  состояние в память BMC, BMC никогда не читает блейд
  ([docs/push-mode.md](docs/push-mode.md)). Старый помощник в userspace
  `openmiop-rc` делает то же через sysfs и остаётся запасным вариантом.
* **Путь данных.** eDMA отправителя пишет кадры posted-транзакциями
  `MemWr` в кольцо получателя (своё на каждого отправителя и очередь) в
  его BAR; коммутатор маршрутизирует их по адресу (у ASM2824 нет ACS).
  Затем отправитель пишет слово doorbell, которое получатель отображает
  на прерывание GIC ITS, и оно запускает NAPI очереди (GRO). Кредиты
  возвращаются тоже posted-записями. Endpoint'ы никогда не читают друг
  друга.
* **Шлюз.** Кадры к BMC идут в кольцо шлюза; кадры от BMC блейд сам
  забирает своим eDMA прямо из буферов сокетов BMC (gateway pull,
  zero-copy).

Полное описание (раскладка BAR, управление, порядок памяти, L2):
[docs/architecture.md](docs/architecture.md).

## Требования

| Компонент | Поддерживается / проверено |
| --- | --- |
| Шасси | Mixtile Cluster Box (коммутатор ASM2824, BMC MT7620A), 4 слота |
| Блейд | Mixtile Blade 3 (RK3588), линк Gen3 x2 до коммутатора |
| ОС блейда, проверено на железе | Talos v1.14.4 (ядро 6.18.57-talos) через [mixtile-talos](https://github.com/roysbike/mixtile-talos), основная платформа; Debian 12 с ядром Mixtile 6.1.99 (проверено с v0.1) |
| ОС блейда, сборка в CI | DKMS-пакет на ядрах Debian 12/13 и Ubuntu 22.04 (HWE)/24.04 arm64; любое ядро 6.1+ с заголовками. Прерываниям RX нужен Linux 6.10+ (на старых ядрах опрос) |
| Device tree | `pcie@fe150000` в режиме PCIe endpoint: `openmiop,rk3588-pcie-ep` (mixtile-talos) или `mixtile,miop-ep-rk3588` (U-Boot Mixtile) |
| BMC | прошивка ClusterBox v0.4.0 (OpenWrt 25.12.6, Linux 6.12) или v0.3.0 (OpenWrt 23.05, Linux 5.15) со встроенным `omi-rc` |

## Установка

### 1. BMC

Установите прошивку ClusterBox
[v0.4.0](https://github.com/roysbike/mixtile-clusterbox-mt7620a-openwrt/releases/tag/v0.4.0).
Она загружает `omi-rc` при старте (`kmod-omi-rc`), а если не вышло —
запускает помощник (пакет `openmiop`). Больше на BMC ничего не нужно.

BMC получает `omi0` = 10.20.0.1/24, MTU 9000.

<details>
<summary>Другая прошивка BMC: помощник в userspace</summary>

Возьмите из релиза `openmiop-<version>-clusterbox-bmc-mipsel.tar.gz`
(`openmiop-rc`, `omi-peek`, `openmiop.init`):

```sh
sha256sum -c --ignore-missing SHA256SUMS
tar xzf openmiop-0.6.1-clusterbox-bmc-mipsel.tar.gz
cd openmiop-0.6.1-clusterbox-bmc-mipsel
scp -O openmiop-rc openmiop.init root@<bmc>:/tmp/
ssh root@<bmc> 'cp /tmp/openmiop-rc /usr/bin/openmiop-rc && chmod 755 /usr/bin/openmiop-rc &&
  cp /tmp/openmiop.init /etc/init.d/openmiop && chmod 755 /etc/init.d/openmiop &&
  /etc/init.d/openmiop enable && /etc/init.d/openmiop start'
```

`chmod` после копирования обязателен: копирование поверх существующего
файла в overlay BMC может снять бит исполнения. Лог:
`/tmp/openmiop-rc.log`.
</details>

### 2. Блейды: Talos

Используйте образ или установщик
[mixtile-talos v0.6.1](https://github.com/roysbike/mixtile-talos/releases/tag/v0.6.1).
Драйвер и модули платы встроены как system extension; ничего собирать не
нужно. В README mixtile-talos есть конфигурация машины
(`LinkAliasConfig` для `omi0`, адрес) и порядок обновления.

### 2. Блейды: Debian или Ubuntu

Установите DKMS-пакет из
[релиза](https://github.com/roysbike/pcie-ep-net/releases/tag/v0.6.1).
DKMS собирает модуль под работающее ядро (нужны его заголовки; на образе
Mixtile Debian 12 пакет сам находит `/usr/src/linux-headers-6.1-rockchip`).

```sh
sha256sum -c --ignore-missing SHA256SUMS
sudo apt install ./openmiop-dkms_0.6.1_all.deb
echo 10.20.0.<N>/24 | sudo tee /etc/openmiop.addr    # свой адрес на каждый блейд; .1 — это BMC
sudo reboot
```

Пакет включает `openmiop.service` (он поднимает `omi0`, когда есть
`/etc/openmiop.addr`) и выключает `load-miop.service` от Mixtile: стек
MIOP управляет тем же контроллером и не выгружается на работающей
системе, поэтому нужна перезагрузка. Модули платы из `drivers/blade3` в
DKMS-пакет не входят.

## Проверка

На блейде:

```sh
ip -d link show omi0          # UP, LOWER_UP, mtu 9000
dmesg | grep openmiop         # "node N", "peer M up, 4 queues, doorbell, checksum offload"
ethtool -S omi0               # счётчики, rx_q<n>_packets по очередям
ethtool -l omi0               # пары очередей
ping -c3 10.20.0.1            # BMC
ping -c3 -M do -s 8972 10.20.0.<peer>   # jumbo-кадр другому блейду
```

На Talos: `talosctl -n <node> dmesg | grep openmiop`.

На BMC:

```sh
dmesg | grep omi0             # "push mode", "node N active" для каждого блейда
nodectl status --all          # все блейды включены, LINK Gen3 x2
ping -c3 10.20.0.<blade>
```

С рабочей машины, которая видит всех участников: `scripts/matrix-test.sh`
пингует каждую пару (обычные и jumbo-кадры) и проверяет IPv6 multicast;
`scripts/bench.sh` запускает iperf3 и передачи с проверкой SHA-256.

## Настройка

**Адреса.** Одна /24 на фабрику: BMC — 10.20.0.1, каждый блейд берёт
свой адрес (в лаборатории — последний октет адреса управления блейда).
MTU 9000 у всех участников.

**Параметры драйвера** `openmiop-ep` (`modprobe openmiop-ep name=value`;
на Talos `machine.kernel.modules[].parameters`). Значения по умолчанию —
те, с которыми проверены релизы.

| Параметр | По умолчанию | Что делает |
| --- | --- | --- |
| `queues` | 4 | пары очередей TX/RX (1-4); пиры с одинаковым числом используют отдельное кольцо на очередь |
| `rx_doorbell` | on | прерывание RX от отправителя (Linux 6.10+ с GIC ITS; off: опрос) |
| `tx_irq` | on | ждать завершения eDMA по прерыванию (off: опрос) |
| `tx_cpus` | пусто | CPU для потоков отправки и прерываний eDMA, например `4-7`; пусто: самые мощные ядра (Cortex-A76 на RK3588) |
| `p2p_csum` | on | предлагать и использовать checksum offload между блейдами |
| `gw_pull`, `gw_zc` | on | забирать кадры шлюза BMC своим eDMA; zero-copy, если BMC его предлагает |
| `autonomy` | on | после сброса фабрики переподключаться к прежним пирам, не дожидаясь BMC |
| `use_edma` | on | off: весь трафик через шлюз BMC (только для отладки) |

## Обновление и смешанные версии

* **BMC:** обновите прошивку ClusterBox (веб-интерфейс или
  `mixtile-update`). Блейды переподключатся сами.
* **Talos:** `talosctl upgrade` на соответствующий установщик
  mixtile-talos, по одному узлу.
* **Debian/Ubuntu:** установите новый `.deb` и перезагрузите (или
  `sudo rmmod openmiop_ep && sudo systemctl restart openmiop`). При
  выгрузке модуль выполняет handshake выхода; пиры ждут несколько секунд
  и переподключаются.

Все участники должны говорить на протоколе v4 (версия есть в заголовке).
Блейды с v0.1 по v0.6 работают вместе: прерывания, очереди и checksum
offload согласуются для каждой пары, поэтому при обновлении по одному
узлу каждая пара продолжает работать; offload пара получает, когда на
обоих концах v0.6. `omi-rc` (push-режим) требует v0.3 или новее на всех
блейдах; для старых нужен помощник (`touch /etc/openmiop.use-helper` на
BMC).

## Производительность

Лабораторный Cluster Box, четыре Blade 3 на Talos, MTU 9000, под
нагрузкой кластера:

| Путь | Скорость |
| --- | --- |
| Блейд -> блейд, 1 TCP-поток | 7,8-8,0 Гбит/с, 0 ретрансмиссий |
| Блейд -> блейд, 4 TCP-потока | 11,1-11,3 Гбит/с |
| Все четыре блейда отправляют одновременно | ~25 Гбит/с суммарно (измерено на v0.3) |
| Под -> под через Kube-OVN Geneve, 1 / 4 потока | 5,3-5,6 / 8,6 Гбит/с |
| Блейд <-> LAN через BMC | ~220-280 Мбит/с (процессор BMC) |

Целостность данных проверяется передачами с SHA-256 в каждом релизе. Все
результаты, прошлые релизы и история шлюза:
[docs/performance.md](docs/performance.md).

## Известные ограничения

* **Один поток = одна очередь.** Один TCP-поток использует одну очередь и
  один канал eDMA (7,8-8,0 Гбит/с); несколько потоков расходятся по
  четырём очередям.
* **Нет TSO/GSO.** Стек режет данные по MTU. Checksum offload работает
  только между блейдами, не в сторону BMC. Слоты фиксированные, 10 КиБ,
  поэтому маленькие кадры расходуют место в кольце впустую.
* **Шлюз медленный.** Трафик в LAN проходит через процессор MT7620A:
  ~220-280 Мбит/с. На трафик между блейдами это не влияет.
* **Перезагрузка BMC сбрасывает фабрику.** Все линки блейдов падают, пока
  `omi-rc` снова не загрузится (~150 с на OpenWrt 25.12); друг к другу
  блейды переподключаются сами через ~12 с. На Blade 3 тот же сброс
  попадает на M.2 NVMe (общий PERST#); `blade3-nvme-recover` его
  возвращает. С DRBD 9.3.4 перед плановой перезагрузкой BMC проверьте,
  что `out-of-sync` равен 0
  ([LINBIT/drbd#153](https://github.com/LINBIT/drbd/issues/153)).
* **Только с помощником:** помощник в userspace читает BAR блейдов и
  может подвесить фабрику, если блейд пропал без handshake выхода;
  `omi-rc` блейд никогда не читает. Повторная нумерация от root port
  (только помощник) останавливает весь трафик на ~1-2 с.
* **Не проверено:** больше четырёх блейдов, другие PCIe-коммутаторы, MPS
  кроме 128/256, текущий релиз на Debian на железе.

## Диагностика

| Симптом | Что проверить / как исправить |
| --- | --- |
| `omi0` есть, но нет carrier | На BMC: загружен `omi-rc` (`dmesg \| grep omi0`) или работает помощник (`pgrep openmiop-rc`)? `lspci -d 1d87:4f4d` видит блейд? |
| Блейд виден на BMC без адреса BAR | Помощник: сам перенумерует от root port (не чаще раза в минуту); принудительно — `touch /var/run/openmiop-reenumerate` |
| Пинги до ~100 байт проходят, больше — нет | Расхождение MPS: перезапустите `omi-rc` / помощник, он выставляет один MPS для всех |
| На Talos интерфейс называется `enx…` | Добавьте `LinkAliasConfig` из README mixtile-talos |
| На BMC completion timeout, за коммутатором никто не отвечает | Фабрика подвисла: перезагрузите BMC (питание блейдов остаётся) |
| Низкая скорость | `lspci -vv` на BMC: `LnkSta` Gen3 x2; MTU 9000 на обоих концах; в dmesg у пира `4 queues, doorbell, checksum offload` |

Подробнее: [docs/operations.md](docs/operations.md).

## Сборка

```sh
make -C drivers/openmiop KDIR=<kernel build tree>     # openmiop-ep.ko
make -C drivers/blade3 KDIR=<kernel build tree>       # модули платы (нужно полное дерево сборки: приватный заголовок DesignWare)
make -C userspace                                     # openmiop-rc, omi-peek (mipsel-linux-gnu-gcc)
make -C <openwrt kernel tree> M=$PWD/drivers/omi-rc ARCH=mips CROSS_COMPILE=mipsel-openwrt-linux- modules
```

`openmiop-rc` и `omi-peek` — самостоятельные бинарники MIPS32 soft-float
(у MT7620A нет FPU). Прошивка ClusterBox собирает `omi-rc` как
`kmod-omi-rc`; mixtile-talos собирает `openmiop-ep` и модули платы под
ядро Talos.

GitHub Actions на каждый push: модуль endpoint под `linux-6.1.y` и
`linux-6.18.y`, модули платы, бинарники BMC и установка DKMS-пакета на
Debian 12/13 и Ubuntu 22.04/24.04. Тег `v*` публикует релиз (DKMS `.deb`,
архив для BMC, `SHA256SUMS`, `BUILD-INFO.txt`).

## Структура репозитория

| Путь | Содержимое |
| --- | --- |
| `drivers/openmiop/` | драйвер endpoint блейда `openmiop-ep` и общий заголовок протокола `openmiop.h` |
| `drivers/omi-rc/` | модуль ядра BMC `omi-rc` |
| `drivers/blade3/` | модули платы Blade 3 `blade3-nvme-recover`, `blade3-leds` |
| `userspace/` | помощник BMC `openmiop-rc` и инспектор BAR `omi-peek` |
| `packaging/debian-dkms/` | DKMS-пакет и `openmiop.service` |
| `scripts/` | тесты (`matrix-test.sh`, `bench.sh`, `net-test.sh`), init-скрипты, помощники сборки |
| `docs/` | [архитектура](docs/architecture.md), [push-режим](docs/push-mode.md), [эксплуатация](docs/operations.md), [производительность](docs/performance.md), [release notes](docs/release-notes/), [история](docs/history/) |

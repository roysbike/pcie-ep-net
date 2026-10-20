# pcie-ep-net (openmiop)

[English](README.md) · [Русский](README.ru.md)

Ethernet между платами Mixtile Blade 3 внутри Mixtile Cluster Box через
встроенный PCIe-коммутатор шасси. Каждый блейд получает сетевой интерфейс
`omi0`. Кадры между блейдами — это DMA-записи из памяти одного блейда
прямо в PCIe BAR другого: они проходят через коммутатор и не попадают в
BMC Cluster Box.

Реализация называется **openmiop** (`omi0`, сигнатура в протоколе `OMI1`).
Это независимая реализация, не драйвер MIOP от Mixtile; кода и прошивок
Mixtile в ней нет. Лицензия: GPL-2.0-or-later ([LICENSE](LICENSE)).

Текущий релиз: **v0.5.2** (v0.5.0 и исправления `omi-rc` для Linux 6.1+
и 6.12: сборка и включение портов PCIe-коммутатора; протокол v4 между блейдами; push-режим: BMC
никогда не читает память блейда; gateway pull с zero-copy: блейды сами
забирают трафик от Cluster Box своим eDMA прямо из буферов BMC, примерно
в 3 раза быстрее v0.3; [release notes](docs/release-notes/v0.5.2.md)). Подробности — в [CHANGELOG.md](CHANGELOG.md) и
[совместимых релизах](README.md#compatible-releases).

## Оборудование и для кого это

**Mixtile Cluster Box** — компактное шасси на четыре платы **Mixtile
Blade 3**. Внутри:

* управляющий контроллер (BMC): MediaTek MT7620A (MIPS) с OpenWrt. Он
  включает, выключает и перезагружает блейды, даёт доступ к их
  последовательным консолям и служит корневым комплексом PCIe всего шасси;
* PCIe-коммутатор ASMedia ASM2824 (Gen3): линк Gen3 x2 к каждому слоту и
  линк Gen1 x1 к BMC.

**Mixtile Blade 3** — вычислительная плата на Rockchip RK3588
(4× Cortex-A76 + 4× Cortex-A55) с eMMC и слотом M.2 NVMe. Её контроллер
PCIe 3.0 x4 разделён пополам: линии 0-1 идут к коммутатору Cluster Box,
линии 2-3 — к NVMe. Blade 3 работает и сама по себе, без Cluster Box.

Mixtile поставляет шасси со своей прошивкой BMC и своим сетевым
драйвером для блейдов (MIOP). **OpenMIOP Stack** — независимая открытая
замена для владельцев Cluster Box с платами Blade 3 (или отдельной
Blade 3), которым нужно:

| Что нужно | Репозиторий |
| --- | --- |
| Ethernet между блейдами через PCIe-коммутатор (`omi0`, 7-8 Гбит/с на поток, ~25 Гбит/с при четырёх отправителях) | [pcie-ep-net](https://github.com/roysbike/pcie-ep-net) (этот репозиторий) |
| Talos Linux (Kubernetes) на Blade 3, в Cluster Box или отдельно | [mixtile-talos](https://github.com/roysbike/mixtile-talos) |
| Свежий OpenWrt на BMC: питание и консоли блейдов, обновление прошивки, сторона OpenMIOP на BMC | [mixtile-clusterbox-mt7620a-openwrt](https://github.com/roysbike/mixtile-clusterbox-mt7620a-openwrt) |


## Что входит

* **На блейде:** модуль ядра `openmiop-ep.ko` переводит контроллер PCIe3
  RK3588 (`fe150000`) в режим endpoint (PCI ID `1d87:4f4d`) с одним BAR
  на 16 МиБ и регистрирует сетевой интерфейс `omi0`.
* **На BMC:** модуль ядра `omi-rc` (с v0.3, push-режим) или, как запасной
  вариант, программа `openmiop-rc`. BMC раздаёт блейдам адреса друг друга
  и служит шлюзом в LAN и интернет; его скорость ограничена CPU MT7620A
  (~140–150 Мбит/с к блейду с gateway pull в v0.4, ~90 без него).
* **Модули платы Blade 3** в `drivers/blade3` (входят в extension
  mixtile-talos, в пакет DKMS — нет): `blade3-leds` (светодиоды активности
  портов RTL8125 на 2.5G) и `blade3-nvme-recover` (возвращает M.2 NVMe
  после PERST# от Cluster Box; нужен полный build tree ядра ради приватного
  заголовка DesignWare): `make -C drivers/blade3 KDIR=<kernel build tree>`.

## Установка

| Где | Как |
| --- | --- |
| Cluster Box (BMC) | прошивка [mixtile-clusterbox-mt7620a-openwrt v0.3.0](https://github.com/roysbike/mixtile-clusterbox-mt7620a-openwrt/releases/tag/v0.3.0): `omi-rc` грузится при старте. На другой прошивке — архив `openmiop-<version>-clusterbox-bmc-mipsel.tar.gz`, см. [Install](README.md#install) |
| Blade 3 с Talos | установщик [mixtile-talos v0.6.0](https://github.com/roysbike/mixtile-talos/releases/tag/v0.6.0): модуль уже встроен, собирать ничего не нужно |
| Blade 3 с Debian/Ubuntu | DKMS-пакет из [релиза](https://github.com/roysbike/pcie-ep-net/releases/tag/v0.6.1) (ядро 6.1+, заголовки ядра), затем адрес в `/etc/openmiop.addr` и перезагрузка, см. [Install](README.md#install) |

Сначала обновите до v0.3 все блейды, потом BMC: модуль `omi-rc` не
трогает блейды без push-режима. Адреса: BMC — 10.20.0.1/24, блейды —
10.20.0.x/24 (у каждого свой), MTU 9000.

## Проверка и ограничения

Команды проверки — в [Verify](README.md#verify), результаты тестов — в
[Tested configuration](README.md#tested-configuration-and-results).
Главные ограничения: один поток использует одну очередь (7-8 Гбит/с
между блейдами v0.6, ~4,7 раньше), нет TSO/GSO, контрольные суммы TCP/UDP
не считаются только между блейдами (к BMC считаются), перезагрузка BMC сбрасывает
линки всех блейдов (блейды переподключаются сами через ~12 с). Полный
список — в [Known limitations](README.md#known-limitations).

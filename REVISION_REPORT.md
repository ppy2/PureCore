# Отчёт по ревизии дескрипторов XingCore UAC2 Gadget

## Дата: 2025-11-23

## Проблема
Windows не определяет USB гаджет как аудиоустройство. Устройство распознаётся как "Составное USB устройство", но не появляется в звуковых устройствах.

## Сравнение дескрипторов (Original vs Текущий Gadget)

### ✅ ИСПРАВЛЕНО

#### 1. **Device Descriptor - bDeviceClass**
- **Original:** `0xEF` (Miscellaneous Device)
- **Было:** `0x00` (Use Interface Class)
- **Стало:** `0xEF` ✓
- **Файл:** `ext_tree/board/luckfox/rootfs_overlay/etc/init.d/S98xingcore:54`

#### 2. **Device Descriptor - bDeviceSubClass**
- **Original:** `0x02` (Common Class)
- **Было:** `0x00`
- **Стало:** `0x02` ✓
- **Файл:** `ext_tree/board/luckfox/rootfs_overlay/etc/init.d/S98xingcore:55`

#### 3. **Device Descriptor - bDeviceProtocol**
- **Original:** `0x01` (Interface Association Descriptor)
- **Было:** `0x00`
- **Стало:** `0x01` ✓
- **Файл:** `ext_tree/board/luckfox/rootfs_overlay/etc/init.d/S98xingcore:56`

#### 4. **String Descriptors - Interface Strings**
- **Original:** ВСЕ = `0x00` (пустые)
- **Было:** Заполнены строками (iFunction=0x04, iInterface=0x05-0x0F и т.д.)
- **Стало:** ВСЕ = `0x00` ✓
- **Файл:** `buildroot/output/build/linux-custom/drivers/usb/gadget/function/f_uac2.c:1100-1122`
- **Обнулены:**
  - `iad_desc.iFunction` → 0
  - `std_ac_if_desc.iInterface` → 0
  - `in_clk_src_desc.iClockSource` → 0
  - `out_clk_src_desc.iClockSource` → 0
  - `usb_out_it_desc.iTerminal` → 0
  - `io_in_it_desc.iTerminal` → 0
  - `usb_in_ot_desc.iTerminal` → 0
  - `io_out_ot_desc.iTerminal` → 0
  - `std_as_out_if0_desc.iInterface` → 0
  - `std_as_out_if1_desc.iInterface` → 0
  - `std_as_in_if0_desc.iInterface` → 0
  - `std_as_in_if1_desc.iInterface` → 0
  - Feature Unit `iFeature` → 0

---

## ❌ КРИТИЧЕСКИЕ ПРОБЛЕМЫ (требуют дополнительной работы)

### 1. **Configuration #2 (Vendor-Specific) ОТСУТСТВУЕТ**

**Original.txt имеет ДВЕ конфигурации:**
```
bNumConfigurations: 0x02

Configuration 1: UAC2 Audio
  - wTotalLength: 0x00D5 (213 bytes)
  - bNumInterfaces: 0x02
  - Interface 0: Audio Control
  - Interface 1: Audio Streaming (Alt 0, 1, 2)

Configuration 2: Vendor Specific
  - wTotalLength: 0x0012 (18 bytes)
  - bNumInterfaces: 0x01
  - Interface 0: 0xFF/0xFF/0xFF (Vendor Specific)
  - bNumEndpoints: 0x00 (no endpoints)
```

**Текущий Gadget:**
- bNumConfigurations: `0x01` (только одна конфигурация!)
- Configuration 2 полностью отсутствует

**Почему это критично:**
Windows UAC2 драйвер может требовать ТОЧНОГО соответствия дескрипторов. Отсутствие второй конфигурации может приводить к:
- Отказу в загрузке драйвера
- Неправильной инициализации устройства
- Использованию generic USB driver вместо UAC2

**Решение:**
Требуется создать драйвер `f_vendor.c` или использовать composite framework для добавления второй конфигурации. Это требует:
1. Создание модуля `f_vendor.c` в `drivers/usb/gadget/function/`
2. Модификация `Kconfig` и `Makefile`
3. Модификация скрипта S98xingcore для создания второй конфигурации

---

### 2. **Other Speed Configuration Descriptor НЕПРАВИЛЬНЫЙ**

**Original.txt:**
```
Other Speed Configuration Descriptor:
  - bDescriptorType: 0x07
  - wTotalLength: 0x0012 (18 bytes)
  - bNumInterfaces: 0x01
  - Interface: 0xFF/0xFF/0xFF (Vendor Specific)
```

**Текущий Gadget:**
Копирует UAC2 конфигурацию:
```
  - wTotalLength: 0x00D5 (213 bytes)
  - bNumInterfaces: 0x02
  - Interface: Audio (0x01)
```

**Решение:**
Требуется патч в `drivers/usb/gadget/composite.c` для генерации правильного Other Speed Configuration дескриптора.

---

## ⚠️ ДОПОЛНИТЕЛЬНЫЕ РАЗЛИЧИЯ (не критичные)

### 3. **Device Qualifier Descriptor**
- **Original:** `bDeviceClass=0x00, bDeviceSubClass=0x00, bDeviceProtocol=0x00`
- **Текущий:** Устанавливается автоматически kernel composite driver
- **Статус:** Может требовать проверки

### 4. **wMaxPacketSize для High-Speed**
- **Original:** `0x0B20` (800 bytes x 2 transactions = 1600 bytes/microframe)
- **Текущий:** Зависит от p_srate и устанавливается динамически
- **Статус:** Требует проверки для высоких частот (384kHz+)

### 5. **Feedback Endpoint bInterval**
- **Original:** `0x04` (8 microframes = 1ms)
- **Текущий:** Устанавливается драйвером f_uac2.c
- **Статус:** Требует проверки

---

## 🔧 ПРОДЕЛАННЫЕ ИЗМЕНЕНИЯ

### Файл 1: `f_uac2.c`
**Путь:** `buildroot/output/build/linux-custom/drivers/usb/gadget/function/f_uac2.c`

**Изменения (строки 1100-1122):**
```c
/* XINGCORE EMULATION: All interface strings = 0x00 (empty) */
iad_desc.iFunction = 0;
std_ac_if_desc.iInterface = 0;
in_clk_src_desc.iClockSource = 0;
out_clk_src_desc.iClockSource = 0;
usb_out_it_desc.iTerminal = 0;
io_in_it_desc.iTerminal = 0;
usb_in_ot_desc.iTerminal = 0;
io_out_ot_desc.iTerminal = 0;
std_as_out_if0_desc.iInterface = 0;
std_as_out_if1_desc.iInterface = 0;
std_as_in_if0_desc.iInterface = 0;
std_as_in_if1_desc.iInterface = 0;

if (FUOUT_EN(uac2_opts)) {
    u8 *i_feature = (u8 *)out_feature_unit_desc +
            out_feature_unit_desc->bLength - 1;
    *i_feature = 0;  /* XINGCORE: iFeature = 0x00 */
}
if (FUIN_EN(uac2_opts)) {
    u8 *i_feature = (u8 *)in_feature_unit_desc +
            in_feature_unit_desc->bLength - 1;
    *i_feature = 0;  /* XINGCORE: iFeature = 0x00 */
}
```

### Файл 2: `S98xingcore`
**Путь:** `ext_tree/board/luckfox/rootfs_overlay/etc/init.d/S98xingcore`

**Изменения (строки 54-56):**
```bash
# XingCore: Miscellaneous Device (0xEF/0x02/0x01) для IAD поддержки
echo 0xEF > bDeviceClass      # Miscellaneous Device
echo 0x02 > bDeviceSubClass   # Common Class
echo 0x01 > bDeviceProtocol   # Interface Association Descriptor
```

---

## 📋 ПЛАН ДАЛЬНЕЙШИХ ДЕЙСТВИЙ

### Приоритет 1: Тестирование текущих изменений
1. ✓ Пересобрать ядро с изменениями в f_uac2.c
2. ✓ Обновить скрипт S98xingcore на устройстве
3. ☐ Перезагрузить устройство
4. ☐ Проверить дескрипторы на Windows (USBTreeView)
5. ☐ Проверить определение в Device Manager

### Приоритет 2: Добавление Configuration 2 (если тест 1 не помог)
1. ☐ Создать драйвер f_vendor.c
2. ☐ Добавить CONFIG_USB_F_VENDOR в kernel config
3. ☐ Модифицировать S98xingcore для создания configs/c.2
4. ☐ Пересобрать и протестировать

### Приоритет 3: Исправление Other Speed Configuration (если тест 2 не помог)
1. ☐ Патч composite.c для правильного Other Speed descriptor
2. ☐ Пересобрать и протестировать

---

## 🔍 ДИАГНОСТИКА

### Команды для проверки на устройстве:
```bash
# Проверить текущие дескрипторы
cd /sys/kernel/config/usb_gadget/xingcore
cat bDeviceClass bDeviceSubClass bDeviceProtocol

# Проверить количество конфигураций
ls -d configs/*

# Проверить UAC2 настройки
cd functions/uac2.usb0
cat p_chmask p_srate p_ssize

# Проверить статус USB
dmesg | tail -30 | grep -i "usb\|gadget\|uac"
```

### Команды для проверки на Windows:
1. Скачать USBTreeView: https://www.uwe-sieber.de/usbtreeview_e.html
2. Запустить и найти VID=152A, PID=8852
3. Сравнить дескрипторы с original.txt
4. Проверить Device Manager → Sound, video and game controllers

---

## 📊 ТАБЛИЦА СРАВНЕНИЯ ДЕСКРИПТОРОВ

| Параметр | Original | Было | Стало | Статус |
|----------|----------|------|-------|--------|
| bDeviceClass | 0xEF | 0x00 | 0xEF | ✅ |
| bDeviceSubClass | 0x02 | 0x00 | 0x02 | ✅ |
| bDeviceProtocol | 0x01 | 0x00 | 0x01 | ✅ |
| bNumConfigurations | 0x02 | 0x01 | 0x01 | ❌ |
| iFunction (IAD) | 0x00 | 0x04 | 0x00 | ✅ |
| iInterface (AC) | 0x00 | 0x05 | 0x00 | ✅ |
| iClockSource | 0x00 | 0x07 | 0x00 | ✅ |
| iTerminal (IT) | 0x00 | 0x08 | 0x00 | ✅ |
| iTerminal (OT) | 0x00 | 0x0B | 0x00 | ✅ |
| iFeature | 0x00 | 0x0D | 0x00 | ✅ |
| iInterface (AS Alt0) | 0x00 | 0x0E | 0x00 | ✅ |
| iInterface (AS Alt1) | 0x00 | 0x0F | 0x00 | ✅ |
| Config 2 (Vendor) | Присутствует | Отсутствует | Отсутствует | ❌ |

---

## 💡 ВЫВОДЫ

### Что исправлено:
1. ✅ Device Class изменён на 0xEF/0x02/0x01 (Miscellaneous Device с IAD)
2. ✅ ВСЕ interface string дескрипторы обнулены (iFunction, iInterface, iClockSource, iTerminal, iFeature)

### Что ещё нужно:
1. ❌ Добавить Configuration 2 (vendor-specific, 18 bytes)
2. ❌ Исправить Other Speed Configuration descriptor
3. ⚠️ Проверить wMaxPacketSize для high-speed endpoints
4. ⚠️ Проверить Device Qualifier descriptor

### Ожидаемый результат после текущих изменений:
Windows должен:
1. Распознать устройство как "XingCore USB Hi-Resolution Audio"
2. Использовать встроенный UAC2 драйвер (usbaudio.sys)
3. Показать устройство в Sound Settings

**Если устройство всё ещё не работает после текущих изменений, потребуется добавить Configuration 2.**

---

## 📝 ПРИМЕЧАНИЯ

1. Изменения в f_uac2.c будут потеряны при `make linux-dirclean`. Рекомендуется создать патч и добавить в `ext_tree/patches/`.
2. Скрипт S98xingcore сохранится после пересборки.
3. Для создания патча используйте:
   ```bash
   cd buildroot/output/build/linux-custom
   git diff drivers/usb/gadget/function/f_uac2.c > /opt/PureFox/ext_tree/patches/f_uac2_xingcore_strings.patch
   ```

---

**Автор:** Claude Code
**Дата:** 2025-11-23

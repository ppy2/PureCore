# Исправление проблем с распознаванием USB Audio в Windows

## Проблема

После удаления драйверов (libusbK, ASIO) устройство XingCore перестало распознаваться как аудио даже с правильными дескрипторами (0xEF/0x02/0x01).

**Причина:** Windows кэширует информацию о USB устройствах и драйверах. При неправильном удалении драйверов кэш повреждается.

---

## Решение 1: Полная очистка USB кэша (РЕКОМЕНДУЕТСЯ)

### Шаг 1: Удалить все устройства XingCore из системы

1. **Отключить XingCore** от USB
2. Открыть Device Manager (Win+X → Device Manager)
3. View → Show hidden devices (Показать скрытые устройства)
4. Найти и удалить ВСЕ XingCore устройства:
   - Sound, video and game controllers
   - Audio inputs and outputs
   - libusbK USB Devices (если есть)
   - Universal Serial Bus devices
   - Other devices
5. Для каждого устройства:
   - Right-click → Uninstall device
   - ☑ **Delete the driver software for this device**
   - Uninstall

### Шаг 2: Очистить реестр USB устройств

**ВНИМАНИЕ: Создайте точку восстановления перед этим!**

Запустите от **Администратора** PowerShell:

```powershell
# Остановить службы USB
Stop-Service -Name "usbhub" -Force
Stop-Service -Name "usbccgp" -Force

# Удалить кэш USB устройств для VID 152A PID 8852
$usbKey = "HKLM:\SYSTEM\CurrentControlSet\Enum\USB"
Get-ChildItem -Path $usbKey -Recurse |
    Where-Object { $_.Name -match "VID_152A&PID_8852" } |
    Remove-Item -Recurse -Force

# Очистить кэш SetupAPI
Remove-Item "C:\Windows\INF\setupapi.dev.log" -Force -ErrorAction SilentlyContinue
Remove-Item "C:\Windows\INF\setupapi.app.log" -Force -ErrorAction SilentlyContinue

# Перезапустить службы
Start-Service -Name "usbhub"
Start-Service -Name "usbccgp"

Write-Host "USB cache cleared. Please reboot." -ForegroundColor Green
```

### Шаг 3: Перезагрузить компьютер

```powershell
Restart-Computer
```

### Шаг 4: Подключить XingCore

После перезагрузки:
1. Подключить XingCore к USB
2. Windows должен автоматически распознать устройство
3. Установить драйвер UAC2 (должен быть встроенный)
4. Проверить Device Manager → Sound, video and game controllers
5. Должно появиться "XingCore USB Hi-Resolution Audio"

---

## Решение 2: Утилита USBDeview (Быстрый метод)

### Скачать и использовать

1. **Скачать USBDeview:**
   https://www.nirsoft.net/utils/usb_devices_view.html

2. **Запустить от Администратора**

3. **Найти все записи XingCore:**
   - Фильтр: VID=152A, PID=8852
   - Или поиск по "XingCore"

4. **Удалить все найденные записи:**
   - Select all XingCore devices
   - Edit → Uninstall Selected Devices
   - Или: F7 (Uninstall)

5. **Перезагрузить компьютер**

6. **Подключить XingCore**

---

## Решение 3: Очистка драйверов через Pnputil

Запустить от **Администратора** CMD:

```batch
REM Показать все установленные драйвера
pnputil /enum-drivers

REM Найти драйвера XingCore / libusbK
pnputil /enum-drivers | findstr /i "xingcore"
pnputil /enum-drivers | findstr /i "libusbk"

REM Удалить драйвера (замените oemXX.inf на найденные)
pnputil /delete-driver oem42.inf /uninstall /force
pnputil /delete-driver oem43.inf /uninstall /force

REM Перезагрузить
shutdown /r /t 0
```

---

## Решение 4: Ручная очистка реестра (Эксперты)

### ВНИМАНИЕ: Создайте backup реестра!

1. **Открыть regedit** (Win+R → regedit)

2. **Удалить следующие ключи:**

```
HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Enum\USB\VID_152A&PID_8852
HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\DeviceClasses\{6994AD04-93EF-11D0-A3CC-00A0C9223196}
   (Найти и удалить все подключи со ссылками на VID_152A&PID_8852)
```

3. **Удалить драйвер ASIO (если был установлен):**

```
HKEY_LOCAL_MACHINE\SOFTWARE\ASIO\XingCore ASIO
HKEY_CLASSES_ROOT\CLSID\{F8EC58FE-3882-4359-BA60-77B90A5AA813}
```

4. **Удалить кэш libusbK (если был установлен):**

```
HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\libusbK
HKEY_LOCAL_MACHINE\SOFTWARE\libusbK
```

5. **Перезагрузить**

---

## Решение 5: Сброс USB Stack (Радикальный метод)

**ВНИМАНИЕ: Это переустановит ВСЕ USB драйвера!**

Запустить от **Администратора** CMD:

```batch
REM Удалить все USB драйвера
pnputil /enum-drivers | findstr "USB" > usb_drivers.txt
for /f "tokens=2" %%i in ('findstr "oem" usb_drivers.txt') do pnputil /delete-driver %%i /force

REM Переустановить USB stack
pnputil /scan-devices

REM Перезагрузить
shutdown /r /t 0
```

---

## Проверка успешности

После выполнения любого из методов:

### 1. Device Manager

```
Device Manager
  └─ Sound, video and game controllers
      └─ XingCore USB Hi-Resolution Audio  ✅
```

### 2. Device Properties

- Driver Provider: **Microsoft**
- Driver Class: **MEDIA**
- Device Status: **This device is working properly**

### 3. Windows Sound Settings

```
Settings → System → Sound → Output devices
  └─ Speakers (XingCore USB Hi-Resolution Audio)  ✅
```

### 4. USBTreeView

Скачать: https://www.uwe-sieber.de/usbtreeview_e.html

Проверить дескрипторы:
```
Device Descriptor:
  bDeviceClass: 0xEF (Miscellaneous)  ✅
  bDeviceSubClass: 0x02               ✅
  bDeviceProtocol: 0x01 (IAD)         ✅
```

---

## Почему оригинальная плата не распознается?

### Возможные причины:

1. **Windows кэш поврежден**
   - Решение: Методы 1-5 выше

2. **Драйвер libusbK всё ещё привязан к VID/PID**
   - Windows пытается использовать libusbK вместо UAC2
   - Решение: Удалить libusbK полностью (Метод 3)

3. **Composite Device Parent не загружается**
   - Проверить: Device Manager → USB Composite Device
   - Решение: Обновить USB драйвера материнской платы

4. **USB порт в плохом состоянии**
   - Попробовать другой USB порт
   - Попробовать USB 3.0 порт вместо 2.0

5. **Конфликт с другими аудио драйверами**
   - Отключить другие USB аудио устройства
   - Проверить конфликты в Device Manager

---

## Диагностика

### Проверить логи Windows:

```powershell
# Event Viewer - USB errors
Get-WinEvent -LogName System | Where-Object {$_.ProviderName -match "USB"} | Select-Object -First 50

# Посмотреть ошибки драйверов
Get-WinEvent -LogName System | Where-Object {$_.Id -eq 219} | Select-Object -First 20
```

### Проверить dmesg на устройстве:

```bash
ssh root@10.147.20.35
dmesg | grep -i "uac\|usb\|gadget"
```

---

## Дополнительные инструменты

### USBPcap
Перехватывает USB трафик для анализа:
https://desowin.org/usbpcap/

### USB Device Tree Viewer
Показывает полную информацию об USB устройствах:
https://www.uwe-sieber.de/usbtreeview_e.html

### DebugView
Логи драйверов в реальном времени:
https://docs.microsoft.com/en-us/sysinternals/downloads/debugview

---

## Если ничего не помогло

### Последний вариант: Чистая установка Windows

Если USB stack Windows полностью повреждён:

1. Создать точку восстановления
2. Попробовать Repair Install:
   - Скачать Windows ISO
   - Запустить setup.exe с ISO
   - Выбрать "Upgrade" (сохранить файлы)
3. Или переустановить Windows с нуля

---

## Профилактика на будущее

### Правильное удаление драйверов:

1. **Всегда использовать официальные uninstall скрипты**
2. **Не удалять драйвера вручную из system32**
3. **Использовать Device Manager для удаления устройств**
4. **Отключать устройство перед удалением драйвера**
5. **Перезагружать после удаления драйверов**

### Создание backup:

```batch
REM Backup реестра USB
reg export HKLM\SYSTEM\CurrentControlSet\Enum\USB usb_backup.reg

REM Backup драйверов
pnputil /enum-drivers > drivers_backup.txt
```

---

## Итоговый скрипт автоматической очистки

Сохраните как `fix_xingcore_usb.bat` и запустите от **Администратора**:

```batch
@echo off
echo ============================================
echo XingCore USB Audio - Fix Windows Cache
echo ============================================
echo.
echo WARNING: This will remove XingCore USB drivers
echo Press Ctrl+C to cancel, or any key to continue...
pause >nul

echo.
echo Step 1: Removing drivers from driver store...
for /f "tokens=1" %%i in ('pnputil /enum-drivers ^| findstr /i "xingcore libusbk"') do (
    echo Removing %%i...
    pnputil /delete-driver %%i /uninstall /force
)

echo.
echo Step 2: Cleaning registry...
reg delete "HKLM\SYSTEM\CurrentControlSet\Enum\USB\VID_152A&PID_8852" /f >nul 2>&1
reg delete "HKLM\SOFTWARE\ASIO\XingCore ASIO" /f >nul 2>&1
reg delete "HKCR\CLSID\{F8EC58FE-3882-4359-BA60-77B90A5AA813}" /f >nul 2>&1

echo.
echo Step 3: Clearing setupapi logs...
del "C:\Windows\INF\setupapi.dev.log" /f /q >nul 2>&1
del "C:\Windows\INF\setupapi.app.log" /f /q >nul 2>&1

echo.
echo ============================================
echo DONE! Please REBOOT your computer now.
echo After reboot, connect XingCore device.
echo Windows should recognize it automatically.
echo ============================================
echo.
pause
```

Сохраните и запустите!

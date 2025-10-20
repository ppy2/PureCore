$(document).ready(function () {
    // Polling system configuration
    const POLLING_CONFIG = {
        NORMAL_INTERVAL: 3000,    // 3 seconds normal interval
        SWITCHING_INTERVAL: 1000  // 1 second when switching
    };

    // Global variables
    let lastKnownStatus = null;
    let isServiceSwitching = false; // Flag to block updates during switching
    let isVolumeChanging = false; // Flag to block volume updates during user changes
    let isAlsaSwitching = false; // Flag to block ALSA updates during switching
    let statusInterval = null;
    
    // Universal interface update function
    function updateInterfaceFromStatus(data) {
        // Update active service ONLY if switching is not in progress
        if (data.active_service !== undefined && !isServiceSwitching) {
            $('button[data-service]').removeClass('active');
            if (data.active_service) {
                $(`button[data-service="${data.active_service}"]`).addClass('active');
            }
        }
        
        // Update ALSA state ONLY if not switching
        if (data.alsa_state !== undefined && !isAlsaSwitching) {
            updateAlsaUI(data.alsa_state);
        }
        
        // Update volume only if user is not changing it at the moment
        if (!isVolumeChanging) {
            updateVolumeFromStatus(data);
        }
    }
    let previousActiveService = null;


    // Dynamic URL setup based on current host (PRESERVED!)
    var currentHost = window.location.hostname;
    $('button[data-service="aprenderer"] .settings-link').attr('href', 'http://' + currentHost + ':7779');
    $('button[data-service="aplayer"] .settings-link').attr('href', 'http://' + currentHost + ':7778');

    // Language dictionaries (FULLY PRESERVED!)
    window.translations = {
        'ru': {
            'Output': 'Выход:',
            'i2s_settings': 'Настройки I2S',
            'update_firmware': 'Обновить прошивку',
            'update_firmware_short': 'Обновить',
            'firmware_update': 'Обновление прошивки',
            'close': 'Закрыть',
            'update_start': 'Запуск обновления...',
            'update_finish': 'Обновление завершено. Перезагрузите LuckFox',
            'update_error': 'Ошибка при обновлении.',
            'confirm_update': 'Вы уверены, что хотите обновить прошивку?',
            'alsa_error': 'Ошибка при переключении ALSA',
            'service_error': 'Ошибка при переключении сервиса',
            'settings': '',
            'switching_player': 'Переключение плеера...',
            'switching_output': 'Переключение выхода...',
            'usb_dac_missing': 'USB ЦАП не обнаружен.<br>Пожалуйста, подключите USB ЦАП',
            'confirm_reboot': 'Вы уверены, что хотите перезагрузить систему?',
            'confirm_shutdown': 'Вы уверены, что хотите выключить систему?',
            'shutdown_complete': 'Система выключена. Можете отключить питание.',
            'i2s_title': 'I2S Настройки',
            'back_button': '← Назад',
            'mode_title': 'Режим',
            'pll_mode': 'PLL',
            'ext_mode': 'EXT',
            'submode_title': 'Вариант выхода',
            'lr_mode': 'L/R',
            'plr_mode': '±L/±R',
            'std_mode': 'STD',
            'l_mode': 'L',
            'r_mode': 'R',
            'plusl_mode': '±L',
            'plusr_mode': '±R',
            '8ch_mode': '8CH',
            'mclk_title': 'MCLK',
            'warning_attention': 'Внимание!',
            'warning_text1': 'Выход MCLK в режимах PLL и EXT имеет разные настройки (OUTPUT/INPUT).',
            'warning_text2': 'После изменения настроек I2S необходима перезагрузка системы для вступления в силу.',
            'yes_btn': 'Да',
            'cancel_btn': 'Отмена',
            'apply_reboot': 'Применить и перезагрузить',
            'pcm_swap_title': 'PCM Swap',
            'dsd_swap_title': 'DSD Swap'
        },
        'en': {
            'alsa_output': 'ALSA Output:',
            'i2s_settings': 'I2S Settings',
            'update_firmware': 'Update Firmware',
            'update_firmware_short': 'Update',
            'firmware_update': 'Firmware Update',
            'close': 'Close',
            'update_start': 'Starting update...',
            'update_finish': 'Update completed. Please reboot LuckFox',
            'update_error': 'Update error.',
            'confirm_update': 'Are you sure you want to update the firmware?',
            'alsa_error': 'Error switching ALSA',
            'service_error': 'Error switching service',
            'settings': '',
            'switching_player': 'Switching player...',
            'switching_output': 'Switching output...',
            'usb_dac_missing': 'USB DAC not detected.<br>Please connect USB DAC',
            'confirm_reboot': 'Are you sure you want to reboot the system?',
            'confirm_shutdown': 'Are you sure you want to shutdown the system?',
            'shutdown_complete': 'System has been shut down. You can disconnect power.',
            'i2s_title': 'I2S Settings',
            'back_button': '← Back',
            'mode_title': 'Mode',
            'pll_mode': 'PLL',
            'ext_mode': 'EXT',
            'submode_title': 'Output Mode',
            'lr_mode': 'L/R',
            'plr_mode': '±L/±R',
            'std_mode': 'STD',
            'l_mode': 'L',
            'r_mode': 'R',
            'plusl_mode': '±L',
            'plusr_mode': '±R',
            '8ch_mode': '8CH',
            'mclk_title': 'MCLK',
            'warning_attention': 'Warning!',
            'warning_text1': 'MCLK output has different settings in PLL and EXT modes (OUTPUT/INPUT).',
            'warning_text2': 'System reboot is required after changing I2S settings to apply them.',
            'yes_btn': 'Yes',
            'cancel_btn': 'Cancel',
            'apply_reboot': 'Apply & Reboot',
            'pcm_swap_title': 'PCM Swap',
            'dsd_swap_title': 'DSD Swap'
        },
        'de': {
            'alsa_output': 'ALSA Ausgang:',
            'i2s_settings': 'I2S Einstellungen',
            'update_firmware': 'Firmware aktualisieren',
            'update_firmware_short': 'Update',
            'firmware_update': 'Firmware-Update',
            'close': 'Schließen',
            'update_start': 'Update wird gestartet...',
            'update_finish': 'Update abgeschlossen. Bitte LuckFox neu starten',
            'update_error': 'Update-Fehler.',
            'confirm_update': 'Sind Sie sicher, dass Sie die Firmware aktualisieren möchten?',
            'alsa_error': 'Fehler beim Umschalten von ALSA',
            'service_error': 'Fehler beim Umschalten des Dienstes',
            'settings': '',
            'switching_player': 'Player wird gewechselt...',
            'switching_output': 'Ausgang wird gewechselt...',
            'usb_dac_missing': 'USB-DAC nicht erkannt.<br>Bitte USB-DAC anschließen',
            'confirm_reboot': 'Sind Sie sicher, dass Sie das System neu starten möchten?',
            'confirm_shutdown': 'Sind Sie sicher, dass Sie das System herunterfahren möchten?',
            'shutdown_complete': 'System wurde heruntergefahren. Sie können die Stromversorgung trennen.',
            'i2s_title': 'I2S Einstellungen',
            'back_button': '← Zurück',
            'mode_title': 'Modus',
            'pll_mode': 'PLL',
            'ext_mode': 'EXT',
            'submode_title': 'Ausgangsmodus',
            'lr_mode': 'L/R',
            'plr_mode': '±L/±R',
            'std_mode': 'STD',
            'l_mode': 'L',
            'r_mode': 'R',
            'plusl_mode': '±L',
            'plusr_mode': '±R',
            '8ch_mode': '8CH',
            'mclk_title': 'MCLK',
            'warning_attention': 'Achtung!',
            'warning_text1': 'MCLK-Ausgang hat unterschiedliche Einstellungen in PLL- und EXT-Modi (OUTPUT/INPUT).',
            'warning_text2': 'Systemneustart ist erforderlich, nachdem I2S-Einstellungen geändert wurden.',
            'yes_btn': 'Ja',
            'cancel_btn': 'Abbrechen',
            'apply_reboot': 'Anwenden & Neustart',
            'pcm_swap_title': 'PCM Swap',
            'dsd_swap_title': 'DSD Swap'
        },
        'fr': {
            'alsa_output': 'Sortie ALSA:',
            'i2s_settings': 'Paramètres I2S',
            'update_firmware': 'Mettre à jour le firmware',
            'update_firmware_short': 'Mettre à jour',
            'firmware_update': 'Mise à jour du firmware',
            'close': 'Fermer',
            'update_start': 'Démarrage de la mise à jour...',
            'update_finish': 'Mise à jour terminée. Veuillez redémarrer LuckFox',
            'update_error': 'Erreur de mise à jour.',
            'confirm_update': 'Êtes-vous sûr de vouloir mettre à jour le firmware?',
            'alsa_error': 'Erreur lors du changement ALSA',
            'service_error': 'Erreur lors du changement de service',
            'settings': '',
            'switching_player': 'Changement de lecteur...',
            'switching_output': 'Changement de sortie...',
            'usb_dac_missing': 'DAC USB non détecté.<br>Veuillez connecter un DAC USB',
            'confirm_reboot': 'Êtes-vous sûr de vouloir redémarrer le système?',
            'confirm_shutdown': 'Êtes-vous sûr de vouloir arrêter le système?',
            'shutdown_complete': 'Système arrêté. Vous pouvez débrancher l\'alimentation.',
            'i2s_title': 'Paramètres I2S',
            'back_button': '← Retour',
            'mode_title': 'Mode',
            'pll_mode': 'PLL',
            'ext_mode': 'EXT',
            'submode_title': 'Mode de sortie',
            'lr_mode': 'L/R',
            'plr_mode': '±L/±R',
            'std_mode': 'STD',
            'l_mode': 'L',
            'r_mode': 'R',
            'plusl_mode': '±L',
            'plusr_mode': '±R',
            '8ch_mode': '8CH',
            'mclk_title': 'MCLK',
            'warning_attention': 'Attention!',
            'warning_text1': 'La sortie MCLK a des paramètres différents en modes PLL et EXT (OUTPUT/INPUT).',
            'warning_text2': 'Un redémarrage du système est nécessaire après modification des paramètres I2S.',
            'yes_btn': 'Oui',
            'cancel_btn': 'Annuler',
            'apply_reboot': 'Appliquer et redémarrer',
            'pcm_swap_title': 'PCM Swap',
            'dsd_swap_title': 'DSD Swap'
        },
        'zh': {
            'alsa_output': 'ALSA 输出:',
            'i2s_settings': 'I2S 设置',
            'update_firmware': '更新固件',
            'update_firmware_short': '更新',
            'firmware_update': '固件更新',
            'close': '关闭',
            'update_start': '开始更新...',
            'update_finish': '更新完成。请重启 LuckFox',
            'update_error': '更新错误。',
            'confirm_update': '您确定要更新固件吗？',
            'alsa_error': 'ALSA 切换错误',
            'service_error': '服务切换错误',
            'settings': '',
            'switching_player': '正在切换播放器...',
            'switching_output': '正在切换输出...',
            'usb_dac_missing': '未检测到 USB DAC。<br>请连接 USB DAC',
            'confirm_reboot': '您确定要重启系统吗？',
            'confirm_shutdown': '您确定要关闭系统吗？',
            'shutdown_complete': '系统已关闭。您可以断开电源。',
            'i2s_title': 'I2S 设置',
            'back_button': '← 返回',
            'mode_title': '主模式',
            'pll_mode': 'PLL',
            'ext_mode': 'EXT',
            'submode_title': '输出模式',
            'lr_mode': 'L/R',
            'plr_mode': '±L/±R',
            'std_mode': 'STD',
            'l_mode': 'L',
            'r_mode': 'R',
            'plusl_mode': '±L',
            'plusr_mode': '±R',
            '8ch_mode': '8CH',
            'mclk_title': 'MCLK',
            'warning_attention': '注意！',
            'warning_text1': 'MCLK输出在PLL和EXT模式下具有不同的设置（OUTPUT/INPUT）。',
            'warning_text2': '更改I2S设置后需要重启系统才能生效。',
            'yes_btn': '是',
            'cancel_btn': '取消',
            'apply_reboot': '应用并重启',
            'pcm_swap_title': 'PCM Swap',
            'dsd_swap_title': 'DSD Swap'
        }
    };

    // Browser language detection (PRESERVED!)
    window.detectLanguage = function() {
        const lang = navigator.language || navigator.userLanguage;
        if (lang.startsWith('ru')) return 'ru';
        if (lang.startsWith('de')) return 'de';
        if (lang.startsWith('fr')) return 'fr';
        if (lang.startsWith('zh')) return 'zh';
        return 'en';
    }

    // Language application (PRESERVED!)
    window.currentLang = window.detectLanguage();
    // Update I2S settings link - now unified multilingual
    $('#i2s-settings-link').attr('href', 'i2s.php');

    window.applyTranslations = function() {
        $('[data-lang]').each(function() {
            const key = $(this).data('lang');
            if (window.translations[window.currentLang][key] !== undefined) {
                if (key !== 'settings' || !$(this).find('img').length) {
                    $(this).text(window.translations[window.currentLang][key]);
                }
            }
        });
    }

    window.applyTranslations();

    // Adaptive styles (FULLY PRESERVED!)
    function setResponsiveStyles() {
        if (window.innerWidth < 500) {
            $('.player-buttons, .ap-buttons, .streaming-buttons').css({
                'display': 'block',
                'gap': '0'
            });
            $('.player-buttons .btn-custom, .ap-buttons .btn-custom, .streaming-buttons .btn-custom').css({
                'width': '100%',
                'margin-top': '10px',
                'margin-bottom': '0'
            });
            $('.alsa-button').css('padding', '10px');
        } else {
            $('.player-buttons, .ap-buttons, .streaming-buttons').css({
                'display': 'flex',
                'gap': '10px'
            });
            $('.player-buttons .btn-custom, .ap-buttons .btn-custom, .streaming-buttons .btn-custom').css({
                'width': 'calc(50% - 5px)',
                'margin-top': '0',
                'margin-bottom': '0'
            });
            $('.alsa-button').css('padding', '10px 20px');
        }
    }

    setResponsiveStyles();
    $(window).resize(function() {
        setResponsiveStyles();
    });
    
    // Check CSS gap support
    function checkGapSupport() {
        const testElement = document.createElement('div');
        testElement.style.display = 'flex';
        testElement.style.gap = '10px';
        if (testElement.style.gap === '10px') {
            document.documentElement.classList.add('supports-gap');
        }
    }
    checkGapSupport();

    // Force status check on user actions
    function forceStatusCheck() {
        console.log('Принудительная проверка состояния...');
        $.ajax({
            url: 'status_fast.php',
            method: 'GET',
            timeout: 3000,
            dataType: 'json',
            success: function(response) {
                console.log('Принудительная проверка:', response);
                updateInterfaceFromStatus(response);
                lastKnownStatus = response;
            },
            error: function() {
                console.warn('Ошибка принудительной проверки');
            }
        });
    }

    // Track page visibility
    document.addEventListener('visibilitychange', function() {
        if (!document.hidden) {
            // Page became visible - force check
            forceStatusCheck();
        }
    });

    // Spinner control (PRESERVED!)
    function showSpinner(text = null) {
        if (text) {
            $('.spinner-text').text(text);
        }
        $('.spinner-overlay').css('display', 'flex');
    }

    function hideSpinner() {
        $('.spinner-overlay').css('display', 'none');
        // Restore original text
        $('.spinner-text').text(translations[currentLang]['switching_player']);
    }

    // Check USB DAC via forced status check
    function checkUsbDac(successCallback, errorCallback) {
        // Always do fresh check for USB DAC - it can be connected/disconnected
        $.ajax({
            url: 'status_fast.php',
            method: 'GET',
            timeout: 3000,
            dataType: 'json',
            cache: false, // Принудительно отключаем кеширование
            success: function(response) {
                lastKnownStatus = response;
                updateInterfaceFromStatus(response);
                
                if (response.usb_dac) {
                    if (successCallback) successCallback();
                } else {
                    customAlert(translations[currentLang]['usb_dac_missing']);
                    if (errorCallback) errorCallback();
                }
            },
            error: function(xhr, status, error) {
                console.error('AJAX error in checkUsbDac:', status, error, 'Response:', xhr.responseText);
                customAlert(translations[currentLang]['service_error'] + ': ' + status);
                if (errorCallback) errorCallback();
            }
        });
    }

    // Принудительная проверка состояния сервисов (только по требованию)
    function checkActiveService(callback) {
        console.log('Проверка активного сервиса...');
        $.ajax({
            url: 'status_fast.php',
            method: 'GET',
            timeout: 3000,
            dataType: 'json',
            success: function(response) {
                const activeService = response.active_service || '';
                
                lastKnownStatus = response;
                
                // Обновляем UI
                if (activeService !== previousActiveService) {
                    if (previousActiveService !== null && previousActiveService !== activeService) {
                        hideSpinner();
                    }
                    previousActiveService = activeService;
                    $('.btn-custom').removeClass('active');
                    if (activeService) {
                        $(`button[data-service="${activeService}"]`).addClass('active');
                    }
                }
                
                updateAlsaUI(response.alsa_state);
                updateVolumeFromStatus(response);
                
                if (callback) callback(activeService);
            },
            error: function() {
                console.warn('Ошибка проверки состояния');
            }
        });
    }

    // НОВАЯ функция для обновления ALSA UI
    function updateAlsaUI(alsaState) {
        const toggleInput = $('#alsa-toggle');
        const i2sSettingsLink = $('#i2s-settings-link');
        
        $('.alsa-toggle').removeClass('active-i2s');
        
        switch (alsaState) {
            case 'usb':
                toggleInput.prop('checked', false);
                // Отключаем настройки I2S при USB - мгновенно!
                i2sSettingsLink.addClass('no-transition').css({
                    'opacity': '0.3',
                    'pointer-events': 'none',
                    'cursor': 'not-allowed'
                });
                setTimeout(() => i2sSettingsLink.removeClass('no-transition'), 10);
                break;
            case 'i2s':
                toggleInput.prop('checked', true);
                $('.alsa-toggle').addClass('active-i2s');
                // Включаем настройки I2S при I2S - мгновенно!
                i2sSettingsLink.addClass('no-transition').css({
                    'opacity': '1',
                    'pointer-events': 'auto',
                    'cursor': 'pointer'
                });
                setTimeout(() => i2sSettingsLink.removeClass('no-transition'), 10);
                break;
            case 'error':
                console.error('Ошибка при чтении конфигурации ALSA');
                break;
        }
    }

    // Проверка состояния ALSA через принудительную проверку
    function checkAlsaState() {
        // Используем последнее известное состояние
        if (lastKnownStatus && lastKnownStatus.alsa_state) {
            updateAlsaUI(lastKnownStatus.alsa_state);
            return;
        }
        
        // Иначе принудительно проверяем
        checkActiveService();
    }

    // Обработка переключения ALSA toggle
    $('#alsa-toggle').change(function(e) {
        e.preventDefault();
        const checkbox = $(this);
        const isChecked = checkbox.is(':checked');
        const cardType = isChecked ? 'i2s' : 'usb';
        
        // СРАЗУ обновляем UI иконки настроек при клике на toggle!
        updateAlsaUI(cardType);
        
        // Блокируем ALSA обновления во время переключения
        isAlsaSwitching = true;
        
        forceStatusCheck(); // Принудительная проверка при клике

        if (cardType === 'usb') {
            checkUsbDac(
                function() { switchAlsa(cardType); },
                function() { 
                    // При ошибке возвращаем toggle в исходное состояние
                    checkbox.prop('checked', !isChecked);
                    updateAlsaUI(!isChecked ? 'i2s' : 'usb'); // Откатываем UI иконки
                    isAlsaSwitching = false; // Разблокируем обновления
                }
            );
        } else {
            switchAlsa(cardType);
        }
    });

    // Упрощенная функция переключения ALSA
    function switchAlsa(cardType) {
        // UI уже обновлен пользователем (toggle switch), просто отправляем команду
        $.ajax({
            url: 'handle_alsa.php',
            method: 'POST',
            data: { card: cardType },
            timeout: 10000,
            success: function() {
                // UI уже обновлен при клике, просто разблокируем обновления
                setTimeout(() => {
                    isAlsaSwitching = false;
                }, 2000);
            },
            error: function() {
                // При ошибке возвращаем toggle в исходное состояние
                const oppositeState = (cardType === 'usb') ? 'i2s' : 'usb';
                updateAlsaUI(oppositeState);
                isAlsaSwitching = false; // Разблокируем обновления
            }
        });
    }

    // Обработка кликов по кнопкам сервисов (ПОЛНОСТЬЮ СОХРАНЕНА!)
    $('.btn-custom').click(function(e) {
        if ($(e.target).is('a') || $(e.target).is('img')) return true;
        if (!$(this).data('service') || $(this).hasClass('active')) return;
        
        // Блокируем клики если уже идёт переключение
        if (isServiceSwitching) {
            const busyText = {
                'ru': 'Идёт переключение плеера, подождите...',
                'en': 'Player switch in progress, please wait...',
                'de': 'Player-Wechsel läuft, bitte warten...',
                'fr': 'Changement de lecteur en cours, veuillez patienter...',
                'zh': '播放器切换中，请稍候...'
            };
            customAlert(busyText[currentLang] || busyText['en']);
            return;
        }

        const service = $(this).data('service');
        forceStatusCheck(); // Принудительная проверка при клике
        
        // Используем последнее известное состояние ALSA
        const alsaState = lastKnownStatus ? lastKnownStatus.alsa_state : null;
        if (alsaState === 'usb') {
            checkUsbDac(function() { switchPlayerService(service); });
        } else {
            switchPlayerService(service);
        }
    });

    // Функция переключения сервиса с мгновенной активацией кнопки
    function switchPlayerService(service) {
        // Блокируем обновления кнопок во время переключения
        isServiceSwitching = true;
        
        // СРАЗУ делаем кнопку активной для отзывчивости UI
        $('.btn-custom').removeClass('active');
        $(`button[data-service="${service}"]`).addClass('active');
        console.log('Кнопка', service, 'активирована мгновенно, ожидаем запуск сервиса...');
        
        // Переключение сервиса
        
        // Увеличенный таймаут для сервисов с двухэтапным запуском
        const timeoutDuration = (service === 'qobuz') ? 15000 : (service === 'tidalconnect') ? 12000 : 8000;
        
        // Таймаут для деактивации кнопки при неудаче
        let switchingTimeout = setTimeout(() => {
            console.warn('Таймаут переключения на', service, '- деактивируем кнопку');
            isServiceSwitching = false; // Разблокируем обновления
            $('.btn-custom').removeClass('active');
            customAlert(translations[currentLang]['service_error']);
        }, timeoutDuration);
        
        $.ajax({
            url: 'handle_service.php',
            method: 'POST',
            data: { service: service },
            timeout: 15000,
            success: function() {
                console.log('Команда переключения на', service, 'отправлена, начинаем проверку...');
                
                // Сразу начинаем проверку без задержки
                const checkInterval = 500; // 500ms между проверками
                let checkCount = 0;
                const maxChecks = 40; // 20 секунд максимум

                function checkServiceStatusChange() {
                        $.ajax({
                            url: 'status_fast.php', // Используем оптимизированный запрос
                            method: 'GET',
                            timeout: 3000,
                            dataType: 'json',
                            success: function(response) {
                                const activeService = response.active_service || '';
                                checkCount++;
                                
                                console.log('Проверка', checkCount, ': активный сервис =', activeService, ', ожидаемый =', service);
                                
                                if (!activeService) {
                                    console.warn('Нет активных сервисов, продолжаем проверку... (попытка', checkCount, 'из', maxChecks, ')');
                                    if (checkCount >= maxChecks) {
                                        console.error('Сервис', service, 'не поднялся после максимального числа попыток');
                                        clearTimeout(switchingTimeout);
                                        $('.btn-custom').removeClass('active');
                                        customAlert(translations[currentLang]['service_error']);
                                        return;
                                    }
                                    // Продолжаем проверку, кнопка остается активной
                                    setTimeout(checkServiceStatusChange, checkInterval);
                                    return;
                                }
                                
                                if (activeService === service) {
                                    console.log('Успешно переключен на', service);
                                    clearTimeout(switchingTimeout); // Отменяем таймаут неудачи
                                    isServiceSwitching = false; // Разблокируем обновления
                                    lastKnownStatus = response;
                                    updateInterfaceFromStatus(response);
                                    console.log('Сервис переключен успешно');

                                    // Принудительно обновляем данные из system_status.json через 2.5s
                                    // (учитываем задержку обновления status_monitor ~2s)
                                    setTimeout(() => {
                                        console.log('Обновление данных после переключения источника...');
                                        updateStatusFromServer();
                                    }, 2500);
                                } else if (activeService !== service) {
                                    console.log("Активирован сервис " + activeService + " вместо " + service);
                                    clearTimeout(switchingTimeout); // Отменяем таймаут неудачи
                                    isServiceSwitching = false; // Разблокируем обновления
                                    $('.btn-custom').removeClass('active');
                                    $(`button[data-service="${activeService}"]`).addClass('active');
                                    lastKnownStatus = response;
                                    updateInterfaceFromStatus(response);

                                    // Принудительно обновляем данные из system_status.json через 2.5s
                                    // (учитываем задержку обновления status_monitor ~2s)
                                    setTimeout(() => {
                                        console.log('Обновление данных после переключения плеера...');
                                        updateStatusFromServer();
                                    }, 2500);
                                } else if (checkCount >= maxChecks) {
                                    console.error("Тайм-аут при переключении на сервис " + service);
                                    clearTimeout(switchingTimeout);
                                    isServiceSwitching = false; // Разблокируем обновления
                                    $('.btn-custom').removeClass('active');
                                    customAlert(translations[currentLang]['service_error']);
                                } else {
                                    setTimeout(checkServiceStatusChange, checkInterval);
                                }
                            },
                            error: function(xhr, status, error) {
                                console.error('Ошибка проверки состояния:', status, error);
                                clearTimeout(switchingTimeout);
                                isServiceSwitching = false; // Разблокируем обновления
                                $('.btn-custom').removeClass('active');
                                customAlert(translations[currentLang]['service_error']);
                            }
                        });
                    }

                // Запускаем проверку сразу
                checkServiceStatusChange();
            },
            error: function(xhr, status, error) {
                console.error('AJAX error switching to', service, ':', status, error, 'Response:', xhr.responseText);
                clearTimeout(switchingTimeout); // Отменяем таймаут при ошибке AJAX
                isServiceSwitching = false; // Разблокируем обновления
                $('.btn-custom').removeClass('active');
                customAlert(translations[currentLang]['service_error'] + ': ' + status);
            }
        });
    }

    // Инициализация
    checkActiveService(function(activeService) {
        previousActiveService = activeService;
    });
    checkAlsaState();

    // Кастомный confirm диалог
    function customConfirm(message, callback) {
        $('#confirm-message').html(message);
        $('#custom-confirm').addClass('show');
        
        // Обновляем текст кнопок на основе языка
        const yesText = {
            'ru': 'Да',
            'de': 'Ja',
            'fr': 'Oui',
            'zh': '是',
            'en': 'Yes'
        };
        const cancelText = {
            'ru': 'Отмена',
            'de': 'Abbrechen',
            'fr': 'Annuler',
            'zh': '取消',
            'en': 'Cancel'
        };
        $('#confirm-yes').text(yesText[currentLang] || yesText['en']);
        $('#confirm-no').text(cancelText[currentLang] || cancelText['en']);
        
        // Обработчики кнопок
        $('#confirm-yes').off('click').on('click', function() {
            $('#custom-confirm').removeClass('show');
            callback(true);
        });
        
        $('#confirm-no').off('click').on('click', function() {
            $('#custom-confirm').removeClass('show');
            callback(false);
        });
    }
    
    function customAlert(message) {
        $('#alert-message').html(message);
        $('#custom-alert').addClass('show');
        
        // Обновляем текст кнопки на основе языка
        $('#alert-ok').text('OK');
        
        // Обработчик кнопки
        $('#alert-ok').off('click').on('click', function() {
            $('#custom-alert').removeClass('show');
        });
        
        // Закрытие по клику на фон
        $('#custom-alert').off('click').on('click', function(e) {
            if (e.target === this) {
                $('#custom-alert').removeClass('show');
            }
        });
    }

    // ПОЛНОСТЬЮ СОХРАНЕНА функция обновления прошивки!
    $('#update-firmware').click(function(e) {
        
        customConfirm(translations[currentLang]['confirm_update'], function(confirmed) {
            if (!confirmed) return;

        $('#update-log-modal').addClass('show');
        $('#update-log-content').text(translations[currentLang]['update_start'] + "\n");
        let logContent = $('#update-log-content');

        $.ajax({
            url: 'run_update.php',
            method: 'GET',
            xhrFields: {
                onprogress: function(e) {
                    let newText = e.currentTarget.responseText;
                    logContent.append(newText);
                    setTimeout(() => { logContent.scrollTop(logContent.prop("scrollHeight")); }, 100);
                }
            },
            success: function() {
                logContent.html(logContent.html());
                logContent.append('\n<span class="update-finish">' + translations[currentLang]['update_finish'] + '</span>');
            },
            error: function() {
                logContent.append('\n<span class="update-error">' + translations[currentLang]['update_error'] + '</span>');
                logContent.html(logContent.html());
            }
        });
        });
    });

    // СОХРАНЕНА функция закрытия модального окна
    $('.close-modal').click(function() {
        $('#update-log-modal').removeClass('show');
    });

    // Обработчики для reboot и shutdown
    $('#reboot-link').click(function(e) {
        e.preventDefault();
        customConfirm(translations[currentLang]['confirm_reboot'], function(confirmed) {
            if (confirmed) {
            $('.spinner-overlay').addClass('show');
            const rebootText = {
                'ru': 'Перезагрузка...',
                'de': 'Neustart...',
                'fr': 'Redémarrage...',
                'zh': '重启中...',
                'en': 'Rebooting...'
            };
            $('.spinner-text').text(rebootText[currentLang] || rebootText['en']);
            
            $.ajax({
                url: 'reboot.php',
                method: 'POST',
                success: function() {
                    // Ждем восстановления соединения после перезагрузки
                    setTimeout(checkConnectionAfterReboot, 3000);
                },
                error: function() {
                    // Если запрос не прошел, все равно ждем восстановления
                    setTimeout(checkConnectionAfterReboot, 3000);
                }
            });
            }
        });
    });

    $('#shutdown-link').click(function(e) {
        e.preventDefault();
        customConfirm(translations[currentLang]['confirm_shutdown'], function(confirmed) {
            if (confirmed) {
            $('.spinner-overlay').addClass('show');
            const shutdownText = {
                'ru': 'Выключение...',
                'de': 'Herunterfahren...',
                'fr': 'Arrêt en cours...',
                'zh': '关机中...',
                'en': 'Shutting down...'
            };
            $('.spinner-text').text(shutdownText[currentLang] || shutdownText['en']);
            
            $.ajax({
                url: 'shutdown.php',
                method: 'POST',
                success: function() {
                    // Показываем сообщение о завершении через 3 секунды
                    setTimeout(function() {
                        $('.spinner').hide();
                        $('.spinner-text').text(translations[currentLang]['shutdown_complete']);
                    }, 3000);
                },
                error: function() {
                    const errorText = {
                        'ru': 'Ошибка при выключении',
                        'de': 'Fehler beim Herunterfahren',
                        'fr': 'Erreur d\'arrêt',
                        'zh': '关机错误',
                        'en': 'Shutdown error'
                    };
                    customAlert(errorText[currentLang] || errorText['en']);
                    $('.spinner-overlay').removeClass('show');
                }
            });
            }
        });
    });

    // Функция проверки соединения после перезагрузки
    function checkConnectionAfterReboot() {
        $.ajax({
            url: 'status_fast.php',
            method: 'GET',
            timeout: 2000,
            success: function() {
                // Соединение восстановлено, перезагружаем страницу
                location.reload();
            },
            error: function() {
                // Соединение еще не восстановлено, ждем еще
                setTimeout(checkConnectionAfterReboot, 2000);
            }
        });
    }

    // Volume Control
    let volumeSlider = document.getElementById('volume-slider');
    let volumeDisplay = document.getElementById('volume-display');
    let volumeIcon = document.getElementById('volume-icon');
    let isMuted = false;

    // Обновляем громкость из уже полученных данных status_fast.php (НЕ отдельный запрос!)
    function updateVolumeFromStatus(data) {
        // Handle volume and mute control availability separately
        let volumeControlsAvailable = true;
        let muteControlsAvailable = true;
        
        console.log('Volume update:', data.volume, 'available:', data.volume_control_available, 'changing:', isVolumeChanging);
        
        if (data.volume_control_available !== undefined) {
            volumeControlsAvailable = data.volume_control_available;
            if (volumeSlider) {
                volumeSlider.disabled = !volumeControlsAvailable;
                volumeSlider.style.opacity = volumeControlsAvailable ? '1' : '0.4';
                volumeSlider.style.cursor = volumeControlsAvailable ? 'pointer' : 'not-allowed';
                volumeSlider.style.pointerEvents = volumeControlsAvailable ? 'auto' : 'none';
            }
        }
        
        if (data.mute_control_available !== undefined) {
            muteControlsAvailable = data.mute_control_available;
            if (volumeIcon) {
                volumeIcon.style.opacity = muteControlsAvailable ? '1' : '0.4';
                volumeIcon.style.cursor = muteControlsAvailable ? 'pointer' : 'not-allowed';
                volumeIcon.style.pointerEvents = muteControlsAvailable ? 'auto' : 'none';
            }
        }
        
        // Always update volume display if volume data is available
        if (data.volume) {
            if (volumeControlsAvailable) {
                let volume = parseInt(data.volume.replace('%', ''));
                if (volumeSlider && volumeSlider.value != volume) {
                    volumeSlider.value = volume;
                }
                if (volumeDisplay) {
                    volumeDisplay.textContent = volume.toString();
                }
            } else {
                // For non-controllable DACs, show 100 in both display and slider
                if (volumeDisplay) {
                    volumeDisplay.textContent = '100';
                }
                if (volumeSlider && volumeSlider.value != 100) {
                    volumeSlider.value = 100;
                }
            }
        } else {
            // If no volume data available, show 100 (for DACs without volume controls)
            if (volumeDisplay && volumeDisplay.textContent === '--') {
                volumeDisplay.textContent = '100';
            }
            if (volumeSlider && volumeSlider.value != 100) {
                volumeSlider.value = 100;
            }
        }
        
        // Устанавливаем правильную иконку в зависимости от состояния mute
        if (volumeIcon) {
            const newMuted = data.muted || false;
            if (isMuted !== newMuted) {
                isMuted = newMuted;
                if (isMuted) {
                    volumeIcon.src = 'assets/img/mute.svg';
                    const unmuteText = {
                        'ru': 'Включить звук',
                        'de': 'Ton einschalten',
                        'fr': 'Activer le son',
                        'zh': '开启声音',
                        'en': 'Unmute'
                    };
                    volumeIcon.title = unmuteText[currentLang] || unmuteText['en'];
                } else {
                    volumeIcon.src = 'assets/img/volume.svg';
                    const volumeText = {
                        'ru': 'Громкость',
                        'de': 'Lautstärke',
                        'fr': 'Volume',
                        'zh': '音量',
                        'en': 'Volume'
                    };
                    volumeIcon.title = volumeText[currentLang] || volumeText['en'];
                }
            }
        }
    }

    // Set volume
    function setVolume(volume) {
        isVolumeChanging = true; // Блокируем обновления громкости
        
        fetch('volume.php', {
            method: 'POST',
            headers: {'Content-Type': 'application/x-www-form-urlencoded'},
            body: 'action=set_volume&volume=' + volume
        })
        .then(response => response.json())
        .then(data => {
            if (data.disabled) {
                console.log('Volume control disabled:', data.reason);
                isVolumeChanging = false;
                return;
            }
            if (data.success) {
                volumeDisplay.textContent = volume;
            }
            // Разблокируем через 1 секунду, чтобы дать время системе обновиться
            setTimeout(() => {
                isVolumeChanging = false;
            }, 1000);
        })
        .catch(error => {
            console.error('Error setting volume:', error);
            // Разблокируем даже при ошибке
            setTimeout(() => {
                isVolumeChanging = false;
            }, 1000);
        });
    }

    // Toggle mute
    function toggleMute() {
        isVolumeChanging = true; // Блокируем обновления громкости
        
        fetch('volume.php', {
            method: 'POST',
            headers: {'Content-Type': 'application/x-www-form-urlencoded'},
            body: 'action=toggle_mute'
        })
        .then(response => response.json())
        .then(data => {
            if (data.disabled) {
                console.log('Mute control disabled:', data.reason);
                isVolumeChanging = false;
                return;
            }
            if (data.success) {
                isMuted = data.muted;
                if (isMuted) {
                    volumeIcon.src = 'assets/img/mute.svg';
                    const unmuteText = {
                        'ru': 'Включить звук',
                        'de': 'Ton einschalten',
                        'fr': 'Activer le son',
                        'zh': '开启声音',
                        'en': 'Unmute'
                    };
                    volumeIcon.title = unmuteText[currentLang] || unmuteText['en'];
                } else {
                    volumeIcon.src = 'assets/img/volume.svg';
                    const volumeText = {
                        'ru': 'Громкость',
                        'de': 'Lautstärke',
                        'fr': 'Volume',
                        'zh': '音量',
                        'en': 'Volume'
                    };
                    volumeIcon.title = volumeText[currentLang] || volumeText['en'];
                }
            }
            // Разблокируем через 1 секунду
            setTimeout(() => {
                isVolumeChanging = false;
            }, 1000);
        })
        .catch(error => {
            console.error('Error toggling mute:', error);
            // Разблокируем даже при ошибке
            setTimeout(() => {
                isVolumeChanging = false;
            }, 1000);
        });
    }

    // Volume slider event listener
    if (volumeSlider) {
        volumeSlider.addEventListener('input', function() {
            let volume = this.value;
            volumeDisplay.textContent = volume;
            // Блокируем обновления пока пользователь двигает слайдер
            isVolumeChanging = true;
        });

        volumeSlider.addEventListener('change', function() {
            let volume = this.value;
            setVolume(volume); // setVolume сам управляет блокировкой
        });

        // Начальное состояние загрузится через polling
    }

    // Volume icon click listener
    if (volumeIcon) {
        volumeIcon.addEventListener('click', function(e) {
            e.preventDefault();
            toggleMute();
        });
    }
    
    // Очистка интервалов при выходе со страницы
    window.addEventListener('beforeunload', function() {
        if (statusInterval) {
            clearInterval(statusInterval);
        }
    });
    
    // Инициализация polling системы
    function startPolling() {
        // Первая проверка статуса
        forceStatusCheck();
        
        // Регулярный polling каждые 3 секунды
        statusInterval = setInterval(function() {
            if (!isServiceSwitching && !isVolumeChanging) {
                $.ajax({
                    url: 'status_fast.php',
                    method: 'GET',
                    timeout: 3000,
                    dataType: 'json',
                    success: function(response) {
                        updateInterfaceFromStatus(response);
                    }
                });
            }
        }, POLLING_CONFIG.NORMAL_INTERVAL);
    }
    
    // Запускаем polling через 2 секунды после загрузки
    setTimeout(startPolling, 2000);

    // I2S Modal functions
    window.openI2SModal = function() {
        // Load current I2S settings
        $.ajax({
            url: 'i2s.php?action=getStatus',
            method: 'GET',
            dataType: 'json',
            success: function(data) {
                // Set mode toggle
                if (data.mode === 'pll') {
                    $('#modal-mode-pll').prop('checked', true);
                } else {
                    $('#modal-mode-ext').prop('checked', true);
                }
                
                // Set MCLK toggle
                if (data.mclk === '512') {
                    $('#modal-mclk-512').prop('checked', true);
                } else {
                    $('#modal-mclk-1024').prop('checked', true);
                }
                
                // Set PCM swap toggle
                if (data.pcm_swap === '1') {
                    $('#modal-pcm-swap').prop('checked', true);
                } else {
                    $('#modal-pcm-normal').prop('checked', true);
                }
                
                // Set DSD swap toggle
                if (data.dsd_swap === '1') {
                    $('#modal-dsd-swap').prop('checked', true);
                } else {
                    $('#modal-dsd-normal').prop('checked', true);
                }
                
                // Set submode buttons active state
                $('.i2s-submode-btn').removeClass('active');
                $(`.i2s-submode-btn[value="${data.submode}"]`).addClass('active');
                
                // Apply translations
                applyTranslationsToModal();
                
                // Show modal
                $('#i2s-modal').addClass('show');
            },
            error: function() {
                console.error('Failed to load I2S settings');
            }
        });
    };

    window.closeI2SModal = function() {
        $('#i2s-modal').removeClass('show');
    };

    window.confirmRebootI2S = function(event) {
        if (event) {
            event.preventDefault();
        }
        const currentLang = detectLanguage();
        const rebootMessages = {
            'ru': 'Применить настройки I2S и перезагрузить устройство?',
            'en': 'Apply I2S settings and reboot the device?',
            'de': 'I2S-Einstellungen anwenden und Gerät neu starten?',
            'fr': 'Appliquer les paramètres I2S et redémarrer l\'appareil?',
            'zh': '应用I2S设置并重启设备？'
        };
        const message = rebootMessages[currentLang] || rebootMessages['en'];
        
        customConfirm(message, function(confirmed) {
            if (confirmed) {
                applyI2SSettings();
            }
        });
        return false;
    };

    function applyI2SSettings() {
        const formData = new FormData();
        
        // Get selected mode
        const mode = $('input[name="mode"]:checked').val();
        if (mode) formData.append('mode', mode);
        
        // Get selected MCLK
        const mclk = $('input[name="mclk"]:checked').val();
        if (mclk) formData.append('mclk', mclk);
        
        // Get selected submode
        const activeSubmodeBtn = $('.i2s-submode-btn.active');
        if (activeSubmodeBtn.length > 0) {
            formData.append('submode', activeSubmodeBtn.attr('value'));
        }
        
        // Show spinner
        $('.spinner-overlay').addClass('show');
        const currentLang = detectLanguage();
        const rebootingTexts = {
            'ru': 'Применение настроек и перезагрузка...',
            'en': 'Applying settings and rebooting...',
            'de': 'Anwenden der Einstellungen und Neustart...',
            'fr': 'Application des paramètres et redémarrage...',
            'zh': '应用设置并重启中...'
        };
        $('.spinner-text').text(rebootingTexts[currentLang] || rebootingTexts['en']);
        
        // Close modal first
        closeI2SModal();
        
        // Apply settings
        fetch('i2s.php', {
            method: 'POST',
            body: formData
        }).then(() => {
            // Reboot after applying settings
            return fetch('reboot.php', { method: 'POST' });
        }).then(() => {
            setTimeout(checkConnectionAfterReboot, 3000);
        }).catch(() => {
            setTimeout(checkConnectionAfterReboot, 3000);
        });
    }

    function applyTranslationsToModal() {
        const currentLang = detectLanguage();
        const translations = window.translations[currentLang] || window.translations['en'];
        
        $('#i2s-modal [data-lang]').each(function() {
            const key = $(this).data('lang');
            if (translations[key]) {
                $(this).text(translations[key]);
            }
        });
    }

    // I2S Modal event handlers - submit submode immediately like in original i2s.php
    $(document).on('click', '.i2s-submode-btn', function() {
        $('.i2s-submode-btn').removeClass('active');
        $(this).addClass('active');
        
        const formData = new FormData();
        formData.append('submode', $(this).attr('value'));
        
        // Apply submode setting immediately
        fetch('i2s.php', {
            method: 'POST',
            body: formData
        }).then(() => {
            console.log(`Applied submode: ${$(this).attr('value')}`);
        }).catch(error => {
            console.error('Error applying submode:', error);
        });
    });
    
    // Handle toggle switches in modal - submit immediately like in original i2s.php
    $(document).on('change', '#i2s-modal .toggle-input-compact', function() {
        if (this.checked) {
            const formData = new FormData();
            
            if (this.name === 'mode') {
                formData.append('mode', this.value);
            } else if (this.name === 'mclk') {
                formData.append('mclk', this.value);
            } else if (this.name === 'pcm_swap') {
                formData.append('pcm_swap', this.value);
            } else if (this.name === 'dsd_swap') {
                formData.append('dsd_swap', this.value);
            }
            
            // Apply setting immediately
            fetch('i2s.php', {
                method: 'POST',
                body: formData
            }).then(() => {
                // Settings applied successfully
                console.log(`Applied ${this.name}: ${this.value}`);
            }).catch(error => {
                console.error('Error applying setting:', error);
            });
        }
    });

    // Prevent modal from closing when clicking inside modal content
    $(document).on('click', '#i2s-modal .modal-content', function(e) {
        e.stopPropagation();
    });
    
    // Close modal when clicking outside
    $(document).on('click', '#i2s-modal', function() {
        closeI2SModal();
    });

    // I2S settings link handler
    $(document).on('click', '#i2s-settings-link', function(e) {
        e.preventDefault();
        openI2SModal();
        return false;
    });

    // ===== SWIPE TO HIDE BUTTONS FUNCTIONALITY =====
    const HIDDEN_BUTTONS_KEY = 'hiddenButtons';
    let startX = 0;
    let endX = 0;
    const SWIPE_THRESHOLD = 100;

    // Load hidden buttons from localStorage on page load
    function loadHiddenButtons() {
        const hidden = localStorage.getItem(HIDDEN_BUTTONS_KEY);
        if (hidden) {
            try {
                const hiddenArray = JSON.parse(hidden);
                hiddenArray.forEach(service => {
                    $(`button[data-service="${service}"]`).hide();
                });
            } catch (e) {
                console.error('Failed to parse hidden buttons:', e);
            }
        }
    }

    // Save hidden buttons to localStorage
    function saveHiddenButtons() {
        const hiddenButtons = [];
        $('button[data-service]').each(function() {
            if ($(this).is(':hidden')) {
                hiddenButtons.push($(this).data('service'));
            }
        });
        localStorage.setItem(HIDDEN_BUTTONS_KEY, JSON.stringify(hiddenButtons));
    }

    // Handle swipe gestures on player buttons (touch and mouse)
    $('button[data-service]').each(function() {
        const $button = $(this);
        let isDragging = false;
        
        // Touch events
        this.addEventListener('touchstart', function(e) {
            startX = e.changedTouches[0].screenX;
        }, { passive: true });

        this.addEventListener('touchend', function(e) {
            endX = e.changedTouches[0].screenX;
            handleSwipe($button);
        }, { passive: true });

        // Mouse events for desktop
        $button.on('mousedown', function(e) {
            startX = e.screenX;
            isDragging = false;
        });

        $button.on('mousemove', function(e) {
            if (e.buttons === 1) { // Left mouse button is pressed
                isDragging = true;
            }
        });

        $button.on('mouseup', function(e) {
            if (isDragging) {
                endX = e.screenX;
                handleSwipe($button);
                e.preventDefault();
                e.stopPropagation();
            }
            isDragging = false;
        });
    });

    function handleSwipe($button) {
        const swipeDistance = endX - startX;
        
        // Swipe left to hide
        if (swipeDistance < -SWIPE_THRESHOLD) {
            const serviceName = $button.text().trim();
            const currentHost = window.location.hostname;
            const resetUrl = `http://${currentHost}/default.php`;
            
            const hideText = {
                'ru': 'Скрыть кнопку',
                'en': 'Hide button',
                'de': 'Schaltfläche ausblenden',
                'fr': 'Masquer le bouton',
                'zh': '隐藏按钮'
            };
            
            const restoreText = {
                'ru': 'Для восстановления всех кнопок откройте:',
                'en': 'To restore all buttons, visit:',
                'de': 'Um alle Schaltflächen wiederherzustellen, besuchen Sie:',
                'fr': 'Pour restaurer tous les boutons, visitez:',
                'zh': '要恢复所有按钮，请访问：'
            };
            
            const message = `${hideText[currentLang] || hideText['en']} "${serviceName}"?<br><br>${restoreText[currentLang] || restoreText['en']}<br>${resetUrl}`;
            
            customConfirm(message, function(confirmed) {
                if (confirmed) {
                    const buttonHeight = $button.outerHeight(true);
                    
                    // Шаг 1: Фиксируем высоту БЕЗ transition
                    $button.css({
                        'height': buttonHeight + 'px',
                        'overflow': 'hidden'
                    });
                    
                    // Шаг 2: Через 20ms добавляем transition и запускаем обе анимации
                    setTimeout(function() {
                        $button.css({
                            'transition': 'height 1s ease-in-out, margin 1s ease-in-out, opacity 1s ease, transform 1s ease, filter 1s ease'
                        });
                        
                        // Шаг 3: Через 20ms запускаем распыление и схлопывание одновременно
                        setTimeout(function() {
                            $button.addClass('btn-dissolving');
                            $button.css({
                                'height': '0',
                                'margin-top': '0',
                                'margin-bottom': '0'
                            });
                            
                            setTimeout(function() {
                                $button.hide();
                                saveHiddenButtons();
                                checkAndExpandButtons();
                            }, 1000);
                        }, 20);
                    }, 20);
                }
            });
        }
    }

    // Check if button pairs need to expand to full width
    function checkAndExpandButtons() {
        // Only on desktop (width > 500px)
        if (window.innerWidth <= 500) return;
        
        // Check player-buttons container
        const $playerButtons = $('.player-buttons');
        const $visiblePlayerButtons = $playerButtons.find('button[data-service]:visible');
        if ($visiblePlayerButtons.length === 1) {
            $visiblePlayerButtons.css('width', '100%');
        }
        
        // Check streaming-buttons container
        const $streamingButtons = $('.streaming-buttons');
        const $visibleStreamingButtons = $streamingButtons.find('button[data-service]:visible');
        if ($visibleStreamingButtons.length === 1) {
            $visibleStreamingButtons.css('width', '100%');
        }
    }

    // Load hidden buttons on page load
    loadHiddenButtons();
    checkAndExpandButtons();

});
/* Cache bust version: 1753367744 */

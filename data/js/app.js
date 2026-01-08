/* BigDashVesc Web Dashboard JavaScript */
/* WebSocket handling, data buffering, and UI updates */

// Application State
const App = {
    ws: null,
    wsReconnectTimer: null,
    wsReconnectDelay: 2000,
    isConnected: false,
    lastTelemetry: null,
    
    // Initialize the application
    init: function() {
        this.setupTabs();
        this.setupWebSocket();
        this.setupEventListeners();
        this.startPollingFallback();
        
        console.log('[App] BigDashVesc Web Dashboard initialized');
    },
    
    // Tab Navigation
    setupTabs: function() {
        var tabs = document.querySelectorAll('.tab');
        var self = this;
        
        tabs.forEach(function(tab) {
            tab.addEventListener('click', function() {
                var tabId = this.dataset.tab;
                
                // Update tab buttons
                tabs.forEach(function(t) {
                    t.classList.remove('active');
                });
                this.classList.add('active');
                
                // Update tab content
                document.querySelectorAll('.tab-content').forEach(function(content) {
                    content.classList.remove('active');
                });
                document.getElementById('tab-' + tabId).classList.add('active');
            });
        });
    },
    
    // WebSocket Setup
    setupWebSocket: function() {
        var self = this;
        var wsUrl = 'ws://' + window.location.host + '/ws';
        
        try {
            self.ws = new WebSocket(wsUrl);
            
            self.ws.onopen = function() {
                console.log('[WS] Connected');
                self.isConnected = true;
                self.updateConnectionStatus(true);
                self.wsReconnectDelay = 2000;
            };
            
            self.ws.onmessage = function(event) {
                try {
                    var message = JSON.parse(event.data);
                    self.handleMessage(message);
                } catch (e) {
                    console.error('[WS] Parse error:', e);
                }
            };
            
            self.ws.onclose = function() {
                console.log('[WS] Disconnected');
                self.isConnected = false;
                self.updateConnectionStatus(false);
                self.scheduleReconnect();
            };
            
            self.ws.onerror = function(error) {
                console.error('[WS] Error:', error);
                self.ws.close();
            };
        } catch (e) {
            console.error('[WS] Failed to create WebSocket:', e);
            self.scheduleReconnect();
        }
    },
    
    scheduleReconnect: function() {
        var self = this;
        if (self.wsReconnectTimer) {
            clearTimeout(self.wsReconnectTimer);
        }
        
        self.wsReconnectTimer = setTimeout(function() {
            console.log('[WS] Reconnecting...');
            self.setupWebSocket();
        }, self.wsReconnectDelay);
        
        // Exponential backoff up to 30 seconds
        self.wsReconnectDelay = Math.min(self.wsReconnectDelay * 1.5, 30000);
    },
    
    // Handle incoming messages
    handleMessage: function(message) {
        if (message.type === 'telemetry') {
            this.processTelemetry(message.data);
        } else if (message.type === 'status') {
            this.processStatus(message.data);
        } else if (message.type === 'pong') {
            // Heartbeat response
        }
    },
    
    // Process telemetry data
    processTelemetry: function(data) {
        this.lastTelemetry = data;
        this.updateDashboard(data);
    },
    
    // Update dashboard UI
    updateDashboard: function(data) {
        // Battery
        var batteryEl = document.getElementById('batteryPercent');
        var batteryFill = document.getElementById('batteryFill');
        if (batteryEl && batteryFill) {
            batteryEl.textContent = data.batteryPercent;
            batteryFill.style.width = data.batteryPercent + '%';
            if (data.batteryPercent < 20) {
                batteryFill.classList.add('low');
            } else {
                batteryFill.classList.remove('low');
            }
        }
        
        // Cell info
        var cellInfoEl = document.getElementById('cellInfo');
        if (cellInfoEl && data.cellCount > 0) {
            var cellVoltage = (data.voltage / data.cellCount).toFixed(2);
            cellInfoEl.textContent = data.cellCount + 'S @ ' + cellVoltage + 'V/cell';
        }
        
        // Voltage
        this.updateMetric('voltage', data.voltage.toFixed(1));
        
        // Current
        this.updateMetric('currentIn', data.currentIn.toFixed(1));
        
        // Peak current
        var peakEl = document.getElementById('peakCurrent');
        if (peakEl) {
            peakEl.textContent = data.peakCurrent.toFixed(1);
        }
        
        // Power
        var power = data.voltage * data.currentIn;
        this.updateMetric('power', Math.round(power));
        
        // ERPM
        var rpmDisplay = data.rpm;
        if (Math.abs(data.rpm) > 9999) {
            rpmDisplay = (data.rpm / 1000).toFixed(0) + 'k';
        }
        this.updateMetric('rpm', rpmDisplay);
        
        // Duty
        this.updateMetric('duty', (data.duty * 100).toFixed(0));
        
        // Input (PPM/ADC)
        this.updateInputDisplay(data);
        
        // mAh Used
        this.updateMetric('mahUsed', Math.round(data.ampHours * 1000));
        
        // Wh Used
        this.updateMetric('whUsed', data.wattHours.toFixed(1));
        
        // Temperatures (convert C to F)
        var tempFetF = this.celsiusToFahrenheit(data.tempFet);
        var tempMotorF = this.celsiusToFahrenheit(data.tempMotor);
        
        var tempFetEl = document.getElementById('tempFet');
        if (tempFetEl) {
            tempFetEl.textContent = Math.round(tempFetF);
            tempFetEl.className = 'metric-value';
            if (data.tempFet >= 90) {
                tempFetEl.classList.add('accent-red');
            } else if (data.tempFet >= 80) {
                tempFetEl.classList.add('accent-yellow');
            } else {
                tempFetEl.classList.add('accent-green');
            }
        }
        
        var tempMotorEl = document.getElementById('tempMotor');
        if (tempMotorEl) {
            tempMotorEl.textContent = Math.round(tempMotorF);
            tempMotorEl.className = 'metric-value';
            if (data.tempMotor >= 100) {
                tempMotorEl.classList.add('accent-yellow');
            }
        }
        
        // Warnings
        this.updateWarnings(data);
    },
    
    updateMetric: function(id, value) {
        var el = document.getElementById(id);
        if (el) {
            el.textContent = value;
        }
    },
    
    // Update Input (PPM/ADC) display
    updateInputDisplay: function(data) {
        var labelEl = document.getElementById('inputLabel');
        var valueEl = document.getElementById('inputValue');
        var barFill = document.getElementById('inputBarFill');
        
        if (!labelEl || !valueEl || !barFill) return;
        
        var inputValue = 0;
        var inputType = 'Input';
        var hasValidInput = false;
        
        // Prefer PPM if valid, otherwise use ADC
        if (data.ppmValid) {
            inputValue = data.ppmValue;
            inputType = 'PPM';
            hasValidInput = true;
        } else if (data.adcValid) {
            inputValue = data.adcValue;
            inputType = 'ADC';
            hasValidInput = true;
        }
        
        labelEl.textContent = inputType;
        
        if (hasValidInput) {
            var pctValue = Math.round(inputValue * 100);
            valueEl.textContent = pctValue;
            
            // Update bar fill
            if (inputValue >= 0) {
                var width = inputValue * 50;
                barFill.style.left = '50%';
                barFill.style.width = width + '%';
                barFill.classList.remove('reverse');
            } else {
                var absWidth = Math.abs(inputValue) * 50;
                barFill.style.left = (50 - absWidth) + '%';
                barFill.style.width = absWidth + '%';
                barFill.classList.add('reverse');
            }
        } else {
            valueEl.textContent = '--';
            barFill.style.width = '0%';
        }
    },
    
    celsiusToFahrenheit: function(c) {
        return (c * 9 / 5) + 32;
    },
    
    // Update warning banner
    updateWarnings: function(data) {
        var banner = document.getElementById('warningBanner');
        var text = document.getElementById('warningText');
        
        if (!banner || !text) return;
        
        if (data.faultCode > 0) {
            text.textContent = 'FAULT: ' + data.faultString;
            banner.classList.add('show');
        } else if (data.tempFet >= 90) {
            text.textContent = 'WARNING: FET Temperature Critical!';
            banner.classList.add('show');
        } else if (data.tempFet >= 80) {
            text.textContent = 'WARNING: FET Temperature High';
            banner.classList.add('show');
        } else if (data.batteryPercent < 10) {
            text.textContent = 'WARNING: Battery Low!';
            banner.classList.add('show');
        } else {
            banner.classList.remove('show');
        }
    },
    
    // Update connection status indicator
    updateConnectionStatus: function(connected) {
        var dot = document.getElementById('statusDot');
        var text = document.getElementById('statusText');
        
        if (dot && text) {
            if (connected) {
                dot.classList.add('connected');
                dot.classList.remove('connecting');
                text.textContent = 'Connected';
            } else {
                dot.classList.remove('connected');
                dot.classList.add('connecting');
                text.textContent = 'Reconnecting...';
            }
        }
    },
    
    processStatus: function(data) {
        var dot = document.getElementById('statusDot');
        var text = document.getElementById('statusText');
        var deviceName = document.getElementById('deviceName');
        var uptime = document.getElementById('uptime');
        var freeHeap = document.getElementById('freeHeap');
        
        if (dot && text) {
            if (data.connected) {
                dot.classList.add('connected');
                dot.classList.remove('connecting');
                text.textContent = data.deviceName || 'Connected';
            } else {
                dot.classList.remove('connected');
                text.textContent = data.state || 'Disconnected';
            }
        }
        
        if (deviceName) {
            deviceName.textContent = data.deviceName || '--';
        }
        
        if (uptime) {
            var mins = Math.floor(data.uptime / 60);
            var secs = data.uptime % 60;
            uptime.textContent = mins + 'm ' + secs + 's';
        }
        
        if (freeHeap) {
            freeHeap.textContent = Math.round(data.freeHeap / 1024) + ' KB';
        }
        
        // Update settings page
        var settingsHeap = document.getElementById('settingsHeap');
        var settingsPsram = document.getElementById('settingsPsram');
        
        if (settingsHeap) {
            settingsHeap.textContent = Math.round(data.freeHeap / 1024) + ' KB';
        }
        if (settingsPsram && data.freePsram) {
            settingsPsram.textContent = Math.round(data.freePsram / 1024 / 1024) + ' MB';
        }
    },
    
    // Polling fallback when WebSocket is not available
    pollTimer: null,
    
    startPollingFallback: function() {
        var self = this;
        
        self.pollTimer = setInterval(function() {
            // Only poll if WebSocket is disconnected
            if (!self.isConnected) {
                self.pollTelemetry();
            }
            // Always poll status periodically
            self.pollStatus();
        }, 1000);
    },
    
    pollTelemetry: function() {
        var self = this;
        
        fetch('/api/telemetry')
            .then(function(response) {
                return response.json();
            })
            .then(function(data) {
                if (data && data.valid) {
                    self.processTelemetry(data);
                }
            })
            .catch(function(error) {
                // Silently fail
            });
    },
    
    pollStatus: function() {
        var self = this;
        
        fetch('/api/status')
            .then(function(response) {
                return response.json();
            })
            .then(function(data) {
                self.processStatus(data);
            })
            .catch(function(error) {
                // Silently fail
            });
    },
    
    // Event Listeners
    setupEventListeners: function() {
        var self = this;
        
        // VESC Reboot button
        var rebootBtn = document.getElementById('rebootVesc');
        if (rebootBtn) {
            rebootBtn.addEventListener('click', function() {
                if (confirm('Are you sure you want to reboot the VESC?')) {
                    fetch('/api/vesc/reboot', { method: 'POST' })
                        .then(function(response) {
                            return response.json();
                        })
                        .then(function(data) {
                            if (data.success) {
                                Toast.show('VESC reboot command sent', 'success');
                            } else {
                                Toast.show('Failed: ' + data.error, 'error');
                            }
                        })
                        .catch(function(error) {
                            Toast.show('Error: ' + error, 'error');
                        });
                }
            });
        }
    }
};

// Toast notification system
var Toast = {
    container: null,
    
    init: function() {
        this.container = document.getElementById('toastContainer');
    },
    
    show: function(message, type) {
        if (!this.container) {
            this.init();
        }
        
        type = type || 'info';
        
        var toast = document.createElement('div');
        toast.className = 'toast ' + type;
        toast.textContent = message;
        
        this.container.appendChild(toast);
        
        // Auto remove after 4 seconds
        setTimeout(function() {
            toast.style.opacity = '0';
            toast.style.transform = 'translateX(100%)';
            setTimeout(function() {
                if (toast.parentNode) {
                    toast.parentNode.removeChild(toast);
                }
            }, 300);
        }, 4000);
    }
};

// Initialize when DOM is ready
document.addEventListener('DOMContentLoaded', function() {
    App.init();
});

#!/usr/bin/env python3
"""
Build, upload, and monitor tool for BasiliskII ESP32.

Usage:
    python3 tools/build_upload_monitor.py          # Build, upload, and monitor
    python3 tools/build_upload_monitor.py build    # Build only
    python3 tools/build_upload_monitor.py upload   # Upload only
    python3 tools/build_upload_monitor.py monitor  # Monitor only
"""

import sys
import os
import subprocess
import serial
import time

# Configuration
SERIAL_PORT = '/dev/cu.usbmodem211401'
BAUD_RATE = 115200
MONITOR_TIMEOUT = 60  # seconds

def get_project_root():
    """Get the project root directory."""
    script_dir = os.path.dirname(os.path.abspath(__file__))
    return os.path.dirname(script_dir)

def run_pio_command(cmd, cwd=None):
    """Run a PlatformIO command."""
    if cwd is None:
        cwd = get_project_root()
    
    full_cmd = ['pio', 'run'] + cmd
    print(f"Running: {' '.join(full_cmd)}")
    print("-" * 60)
    
    result = subprocess.run(full_cmd, cwd=cwd)
    return result.returncode == 0

def build():
    """Build the project."""
    print("\n=== Building ===\n")
    return run_pio_command([])

def upload():
    """Upload firmware to device."""
    print("\n=== Uploading ===\n")
    return run_pio_command(['-t', 'upload'])

def monitor():
    """Monitor serial output."""
    print("\n=== Monitoring Serial Output ===")
    print(f"Port: {SERIAL_PORT}, Baud: {BAUD_RATE}")
    print(f"Timeout: {MONITOR_TIMEOUT}s")
    print("Press Ctrl+C to exit\n")
    print("-" * 60)
    
    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
        
        # Reset device via DTR toggle
        ser.dtr = False
        time.sleep(0.3)
        ser.dtr = True
        
        print("Device reset, waiting for output...\n")
        
        start_time = time.time()
        while True:
            # Check timeout
            if MONITOR_TIMEOUT > 0 and (time.time() - start_time) > MONITOR_TIMEOUT:
                print(f"\n[Monitor ended after {MONITOR_TIMEOUT}s timeout]")
                break
            
            # Read and print serial data
            if ser.in_waiting:
                try:
                    line = ser.readline().decode('utf-8', errors='replace').strip()
                    if line:
                        print(line)
                except Exception as e:
                    print(f"[Read error: {e}]")
            else:
                time.sleep(0.01)
                
        ser.close()
        return True
        
    except serial.SerialException as e:
        print(f"[Serial error: {e}]")
        return False
    except KeyboardInterrupt:
        print("\n[Monitor stopped by user]")
        if 'ser' in locals():
            ser.close()
        return True

def main():
    """Main entry point."""
    args = sys.argv[1:] if len(sys.argv) > 1 else ['all']
    
    os.chdir(get_project_root())
    
    for arg in args:
        if arg == 'build':
            if not build():
                print("\nBuild failed!")
                sys.exit(1)
        elif arg == 'upload':
            if not upload():
                print("\nUpload failed!")
                sys.exit(1)
        elif arg == 'monitor':
            monitor()
        elif arg == 'all':
            if not build():
                print("\nBuild failed!")
                sys.exit(1)
            if not upload():
                print("\nUpload failed!")
                sys.exit(1)
            monitor()
        else:
            print(f"Unknown command: {arg}")
            print(__doc__)
            sys.exit(1)
    
    print("\nDone!")

if __name__ == '__main__':
    main()

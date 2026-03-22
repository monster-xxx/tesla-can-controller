#!/bin/bash

# Tesla CAN Controller Development Environment Setup Script
# This script sets up the development environment for all three devices

set -e

echo "🚗 Tesla CAN Controller Development Environment Setup"
echo "====================================================="

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo "⚠️  Please run as root or with sudo"
    exit 1
fi

# Update system
echo "📦 Updating system packages..."
apt-get update
apt-get upgrade -y

# Install required packages
echo "📦 Installing development tools..."
apt-get install -y \
    git wget curl build-essential \
    python3 python3-pip python3-venv \
    cmake ninja-build ccache \
    libusb-1.0-0 libssl-dev \
    screen minicom

# Install Python packages
echo "📦 Installing Python packages..."
pip3 install --upgrade pip
pip3 install \
    esptool \
    pyserial \
    python-can \
    cantools \
    bleak \
    colorama \
    tqdm

# Create development directory
DEV_DIR="$HOME/tesla-can-dev"
echo "📁 Creating development directory: $DEV_DIR"
mkdir -p "$DEV_DIR"
cd "$DEV_DIR"

# Clone ESP-IDF
echo "🔧 Installing ESP-IDF..."
if [ ! -d "esp-idf" ]; then
    git clone --recursive https://github.com/espressif/esp-idf.git
fi
cd esp-idf
git checkout v5.1.2
./install.sh esp32,esp32s3,esp32c6
source export.sh
cd ..

# Clone Tesla DBC files
echo "📊 Downloading Tesla DBC files..."
if [ ! -d "tesla-dbc" ]; then
    git clone https://github.com/joshwardell/model3dbc.git tesla-dbc
fi

# Clone M5Dial BSP
echo "🖥️  Installing M5Dial BSP..."
if [ ! -d "esp-bsp" ]; then
    git clone https://github.com/espressif/esp-bsp.git
fi
cd esp-bsp
git submodule update --init --recursive
cd ..

# Clone LVGL
echo "🎨 Installing LVGL..."
if [ ! -d "lvgl" ]; then
    git clone https://github.com/lvgl/lvgl.git
fi

# Create project symlinks
echo "🔗 Setting up project symlinks..."
PROJECT_DIR="/root/.openclaw/workspace/tesla-can-controller"
if [ -d "$PROJECT_DIR" ]; then
    ln -sf "$PROJECT_DIR" "$DEV_DIR/tesla-can-controller"
    echo "✅ Project linked to: $DEV_DIR/tesla-can-controller"
fi

# Install CAN tools
echo "🔌 Installing CAN tools..."
apt-get install -y can-utils
pip3 install can-isotp

# Create udev rules for CAN devices
echo "⚙️  Setting up udev rules..."
cat > /etc/udev/rules.d/99-can.rules << EOF
# CAN devices
KERNEL=="can[0-9]*", GROUP="dialout", MODE="0660"
KERNEL=="vcan[0-9]*", GROUP="dialout", MODE="0660"

# USB CAN adapters
SUBSYSTEM=="usb", ATTRS{idVendor}=="1d50", ATTRS{idProduct}=="606f", MODE="0666"
SUBSYSTEM=="usb", ATTRS{idVendor}=="0c72", ATTRS{idProduct}=="000c", MODE="0666"
SUBSYSTEM=="usb", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="5740", MODE="0666"
EOF

udevadm control --reload-rules
udevadm trigger

# Create virtual CAN interface for testing
echo "🔄 Creating virtual CAN interface..."
modprobe vcan
ip link add dev vcan0 type vcan
ip link set up vcan0

# Install debugging tools
echo "🐛 Installing debugging tools..."
apt-get install -y gdb-multiarch openocd
pip3 install pygdbmi

# Create environment setup script
echo "📝 Creating environment setup script..."
cat > "$DEV_DIR/setup_env.sh" << 'EOF'
#!/bin/bash

# Tesla CAN Controller Environment Setup
export TESLA_DEV_DIR="$HOME/tesla-can-dev"
export IDF_PATH="$TESLA_DEV_DIR/esp-idf"
export BSP_PATH="$TESLA_DEV_DIR/esp-bsp"
export LVGL_PATH="$TESLA_DEV_DIR/lvgl"
export TESLA_DBC_PATH="$TESLA_DEV_DIR/tesla-dbc"

# Source ESP-IDF
if [ -f "$IDF_PATH/export.sh" ]; then
    source "$IDF_PATH/export.sh"
    echo "✅ ESP-IDF environment loaded"
fi

# Add tools to PATH
export PATH="$TESLA_DEV_DIR/tools:$PATH"

# Project aliases
alias build-device-a="cd $TESLA_DEV_DIR/tesla-can-controller/software/firmware/device_a_m5dial && idf.py set-target esp32s3 && idf.py build"
alias build-device-b="cd $TESLA_DEV_DIR/tesla-can-controller/software/firmware/device_b_can_reader && idf.py set-target esp32c6 && idf.py build"
alias build-device-c="cd $TESLA_DEV_DIR/tesla-can-controller/software/firmware/device_c_ambilight && idf.py set-target esp32 && idf.py build"

alias flash-device-a="cd $TESLA_DEV_DIR/tesla-can-controller/software/firmware/device_a_m5dial && idf.py -p /dev/ttyUSB0 flash"
alias flash-device-b="cd $TESLA_DEV_DIR/tesla-can-controller/software/firmware/device_b_can_reader && idf.py -p /dev/ttyUSB1 flash"
alias flash-device-c="cd $TESLA_DEV_DIR/tesla-can-controller/software/firmware/device_c_ambilight && idf.py -p /dev/ttyUSB2 flash"

alias monitor-device-a="cd $TESLA_DEV_DIR/tesla-can-controller/software/firmware/device_a_m5dial && idf.py -p /dev/ttyUSB0 monitor"
alias monitor-device-b="cd $TESLA_DEV_DIR/tesla-can-controller/software/firmware/device_b_can_reader && idf.py -p /dev/ttyUSB1 monitor"
alias monitor-device-c="cd $TESLA_DEV_DIR/tesla-can-controller/software/firmware/device_c_ambilight && idf.py -p /dev/ttyUSB2 monitor"

echo "🚗 Tesla CAN Controller development environment ready!"
echo "Available commands:"
echo "  build-device-a/b/c   - Build firmware for each device"
echo "  flash-device-a/b/c   - Flash firmware to device"
echo "  monitor-device-a/b/c - Monitor device serial output"
EOF

chmod +x "$DEV_DIR/setup_env.sh"

# Create test scripts
echo "🧪 Creating test scripts..."
mkdir -p "$DEV_DIR/tools"

# CAN test script
cat > "$DEV_DIR/tools/test_can.sh" << 'EOF'
#!/bin/bash

# CAN bus test script
INTERFACE=${1:-vcan0}

echo "Testing CAN interface: $INTERFACE"

# Check if interface exists
if ! ip link show "$INTERFACE" > /dev/null 2>&1; then
    echo "Error: CAN interface $INTERFACE not found"
    exit 1
fi

# Bring interface up
sudo ip link set "$INTERFACE" up type can bitrate 500000
sudo ip link set "$INTERFACE" txqueuelen 1000

echo "CAN interface $INTERFACE configured at 500kbps"

# Test transmission
echo "Sending test message..."
cansend "$INTERFACE" 123#DEADBEEF

echo "Listening for messages (Ctrl+C to stop)..."
candump "$INTERFACE"
EOF

chmod +x "$DEV_DIR/tools/test_can.sh"

# BLE test script
cat > "$DEV_DIR/tools/test_ble.py" << 'EOF'
#!/usr/bin/env python3

import asyncio
from bleak import BleakScanner, BleakClient

async def scan_ble_devices():
    """Scan for BLE devices"""
    print("Scanning for BLE devices...")
    devices = await BleakScanner.discover(timeout=5.0)
    
    print(f"Found {len(devices)} devices:")
    for device in devices:
        print(f"  {device.address}: {device.name} - RSSI: {device.rssi}dB")
    
    return devices

async def test_ble_connection(address):
    """Test connection to a BLE device"""
    print(f"Connecting to {address}...")
    
    try:
        async with BleakClient(address) as client:
            print(f"Connected to {address}")
            
            # Get services
            services = await client.get_services()
            print(f"Found {len(services.services)} services:")
            
            for service in services.services:
                print(f"  Service: {service.uuid}")
                for char in service.characteristics:
                    print(f"    Characteristic: {char.uuid}")
    
    except Exception as e:
        print(f"Connection failed: {e}")

if __name__ == "__main__":
    # Scan for devices
    devices = asyncio.run(scan_ble_devices())
    
    # Try to connect to first device
    if devices:
        asyncio.run(test_ble_connection(devices[0].address))
EOF

chmod +x "$DEV_DIR/tools/test_ble.py"

# DBC parser test
cat > "$DEV_DIR/tools/test_dbc.py" << 'EOF'
#!/usr/bin/env python3

import cantools
import can
import sys

def test_dbc_parsing(dbc_file):
    """Test DBC file parsing"""
    print(f"Loading DBC file: {dbc_file}")
    
    try:
        db = cantools.database.load_file(dbc_file)
        
        print(f"Loaded DBC database:")
        print(f"  Version: {db.version}")
        print(f"  Messages: {len(db.messages)}")
        print(f"  Nodes: {len(db.nodes)}")
        
        # Show first 5 messages
        print("\nFirst 5 messages:")
        for i, msg in enumerate(db.messages[:5]):
            print(f"  {i+1}. {msg.name} (ID: 0x{msg.frame_id:X})")
            print(f"     Length: {msg.length} bytes")
            print(f"     Signals: {len(msg.signals)}")
            
            # Show first 3 signals
            for sig in msg.signals[:3]:
                print(f"       - {sig.name}: {sig.start}|{sig.length}@{sig.byte_order}")
        
        # Test encoding/decoding
        print("\nTesting encoding/decoding...")
        if db.messages:
            test_msg = db.messages[0]
            print(f"Testing message: {test_msg.name}")
            
            # Create test data
            data = {}
            for sig in test_msg.signals:
                if sig.initial is not None:
                    data[sig.name] = sig.initial
                elif sig.minimum is not None:
                    data[sig.name] = sig.minimum
                else:
                    data[sig.name] = 0
            
            # Encode
            encoded = test_msg.encode(data)
            print(f"Encoded data: {encoded.hex()}")
            
            # Decode
            decoded = test_msg.decode(encoded)
            print(f"Decoded data: {decoded}")
    
    except Exception as e:
        print(f"Error loading DBC file: {e}")
        sys.exit(1)

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: test_dbc.py <dbc_file>")
        sys.exit(1)
    
    test_dbc_parsing(sys.argv[1])
EOF

chmod +x "$DEV_DIR/tools/test_dbc.py"

# Create build script
cat > "$DEV_DIR/tools/build_all.sh" << 'EOF'
#!/bin/bash

# Build all Tesla CAN Controller firmware

set -e

echo "🔨 Building Tesla CAN Controller firmware..."
echo "============================================"

# Source environment
if [ -f "$HOME/tesla-can-dev/setup_env.sh" ]; then
    source "$HOME/tesla-can-dev/setup_env.sh"
fi

# Build Device A (M5Dial)
echo ""
echo "📱 Building Device A (M5Dial)..."
cd "$TESLA_DEV_DIR/tesla-can-controller/software/firmware/device_a_m5dial"
idf.py set-target esp32s3
idf.py build
echo "✅ Device A build complete"

# Build Device B (CAN Reader)
echo ""
echo "🔌 Building Device B (CAN Reader)..."
cd "$TESLA_DEV_DIR/tesla-can-controller/software/firmware/device_b_can_reader"
idf.py set-target esp32c6
idf.py build
echo "✅ Device B build complete"

# Build Device C (Ambient Light)
echo ""
echo "💡 Building Device C (Ambient Light)..."
cd "$TESLA_DEV_DIR/tesla-can-controller/software/firmware/device_c_ambilight"
idf.py set-target esp32
idf.py build
echo "✅ Device C build complete"

echo ""
echo "🎉 All firmware builds completed successfully!"
echo ""
echo "Flash commands:"
echo "  flash-device-a   # Flash Device A"
echo "  flash-device-b   # Flash Device B"
echo "  flash-device-c   # Flash Device C"
EOF

chmod +x "$DEV_DIR/tools/build_all.sh"

# Create documentation
echo "📚 Creating documentation..."
cat > "$DEV_DIR/README.md" << 'EOF'
# Tesla CAN Controller Development Environment

## Quick Start

1. **Setup Environment**:
   ```bash
   source ~/tesla-can-dev/setup_env.sh
   ```

2. **Build Firmware**:
   ```bash
   build-all
   ```

3. **Flash Devices** (adjust serial ports as needed):
   ```bash
   flash-device-a   # Device A (M5Dial) on /dev/ttyUSB0
   flash-device-b   # Device B (CAN Reader) on /dev/ttyUSB1
   flash-device-c   # Device C (Ambient Light) on /dev/ttyUSB2
   ```

4. **Monitor Output**:
   ```bash
   monitor-device-a
   ```

## Project Structure

```
tesla-can-dev/
├── esp-idf/          # ESP-IDF framework
├── esp-bsp/          # M5Dial BSP
├── lvgl/             # LVGL graphics library
├── tesla-dbc/        # Tesla DBC files
├── tesla-can-controller/ -> /root/.openclaw/workspace/tesla-can-controller
└── tools/            # Development tools
```

## Development Tools

### CAN Tools
- `test_can.sh` - Test CAN interface
- `candump` - Monitor CAN traffic
- `cansend` - Send CAN messages

### BLE Tools
- `test_ble.py` - Scan and test BLE devices
- `bleak` - Python BLE library

### DBC Tools
- `test_dbc.py` - Test DBC file parsing
- `cantools` - Python CAN tools

## Testing

### Virtual CAN Testing
```bash
# Create virtual CAN interface
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set up vcan0

# Test communication
./tools/test_can.sh vcan0
```

### BLE Testing
```bash
# Scan for BLE devices
python3 tools/test_ble.py
```

### DBC Testing
```bash
# Test DBC parsing
python3 tools/test_dbc.py tesla-dbc/model3.dbc
```

## Debugging

### Serial Monitor
```bash
# Monitor device serial output
idf.py -p /dev/ttyUSB0 monitor
```

### GDB Debugging
```bash
# Start OpenOCD
openocd -f board/esp32s3-builtin.cfg

# Connect GDB
gdb-multiarch build/device_a_m5dial.elf
```

## Useful Commands

```bash
# Build specific device
build-device-a
build-device-b
build-device-c

# Clean build
idf.py fullclean

# Menuconfig
idf.py menuconfig

# Size analysis
idf.py size
idf.py size-components
idf.py size-files
```

## Troubleshooting

### Common Issues

1. **Permission denied on serial port**:
   ```bash
   sudo usermod -a -G dialout $USER
   # Log out and back in
   ```

2. **ESP-IDF not found**:
   ```bash
   source ~/tesla-can-dev/esp-idf/export.sh
   ```

3. **CAN interface not found**:
   ```bash
   sudo modprobe vcan
   sudo ip link add dev vcan0 type vcan
   sudo ip link set up vcan0
   ```

## Resources

- [ESP-IDF Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/)
- [LVGL Documentation](https://docs.lvgl.io/latest/)
- [Tesla DBC Repository](https://github.com/joshwardell/model3dbc)
- [M5Dial Documentation](https://docs.m5stack.com/en/core/m5dial)
- [CAN Tools Documentation](https://github.com/linux-can/can-utils)

## Support

For issues and questions:
1. Check the troubleshooting section
2. Review ESP-IDF documentation
3. Check project GitHub issues
4. Contact
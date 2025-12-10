# RP2350 (Pico 2 W) Dual-Boot FTP Server & Amiga SPI Bridge

Firmware for the Raspberry Pi Pico 2 W providing both an Amiga SPI bridge (bare-metal mode) and a WiFi FTP server (FreeRTOS mode) with mode switching via button press.

## Features

### Dual-Boot System

The Pico 2 W boots into two different modes:

- **Bare-Metal Mode (Default)**: Amiga SPI Bridge for SD card access
  - Fast, low-latency SPI bridge between Amiga parallel port and SD card
  - Exclusive interrupt handler for ~200-300ns response time
  - PIO-based activity LED mirroring
  - Default mode on normal power-on

- **FreeRTOS Mode**: WiFi FTP Server for remote file management
  - Full-featured FTP server over WiFi
  - Manage SD card contents remotely via any FTP client
  - Multi-client support (up to 8 simultaneous connections)
  - High-performance file transfers

**Mode Switching**: Hold the mode switch button (GPIO13) for 3 seconds to toggle between modes. The system will perform a clean watchdog reboot to switch modes.

### FTP Server Features

The FTP server implements all essential FTP operations:

**File Operations:**
- Upload files (STOR)
- Download files (RETR)
- Delete files (DELE)
- Rename files (RNFR/RNTO)
- Get file size (SIZE)
- Get/set file timestamp (MDTM/MFMT)

**Directory Operations:**
- List directories (LIST, MLSD, NLST)
- Navigate directories (CWD, CDUP, PWD)
- Create directories (MKD/XMKD)
- Remove directories (RMD/XRMD)
- Rename directories (RNFR/RNTO)

**Connection Management:**
- Authentication (USER/PASS)
- Passive mode (PASV)
- Binary/ASCII mode (TYPE)
- Keepalive (NOOP)
- Feature negotiation (FEAT)

**Special Features:**
- **Timestamp Preservation**: Original file modification times are preserved during upload (RFC 3659 MFMT)
- **Multi-client Support**: Up to 8 simultaneous FTP connections
- **RAM Buffering**: Efficient transfers with smart buffering (≤256KB files use RAM, larger files stream)
- **Empty Directory Support**: Proper handling of empty directory listings

## Hardware Requirements

- Raspberry Pi Pico 2 W
- SD card (FAT32 formatted)
- Mode switch button connected to GPIO13 (available on carrier board underside)
- Amiga parallel port connection (see main project for hardware details)

## Build Requirements

### Prerequisites

- **arm-none-eabi-gcc toolchain**: Required for cross-compiling
  - Install via your package manager or download from ARM
  - Add the bin directory to your PATH early (before other paths)
  
  Example PATH on macOS:
```bash
  PATH=/opt/local/bin:/Applications/ARM/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:/opt/X11/bin
```

- **CMake**: Build system (version 3.20 or later)
- **Git**: For cloning repository and submodules
- **Pico SDK**: Automatically fetched by CMake

### WiFi Credentials Setup

**Important**: No pre-built UF2 file is provided because everyone needs to build with their own WiFi credentials.

1. Create a file named `wifi_credentials.cmake` in the `rp2350/` folder
2. Add your WiFi credentials:
```cmake
# WiFi credentials (required)
set(WIFI_SSID "YourNetworkName")
set(WIFI_PASSWORD "YourWiFiPassword")

# FTP credentials (optional - defaults to pico/pico)
set(FTP_USER "admin")
set(FTP_PASSWORD "YourSecurePassword")
```

3. **Security**: Add `wifi_credentials.cmake` to your `.gitignore` to avoid committing credentials!

### Build Instructions
```bash
# Clone the repository
git clone https://github.com/jbilander/amiga-par-to-spi-adapter.git
cd amiga-par-to-spi-adapter/rp2350

# Initialize FatFS submodule
cd lib/fatfs/
git submodule update --init
cd ../..

# Create build directory and compile
mkdir build
cd build
cmake ..
make -j4
```

The compiled firmware `par_spi_ftp_server.uf2` will be created in the `build/` directory.

### Installing Firmware

1. Hold the BOOTSEL button on the Pico 2 W
2. Connect USB cable to your computer
3. Release BOOTSEL - Pico appears as USB mass storage device
4. Copy `par_spi_ftp_server.uf2` to the Pico
5. Pico automatically reboots and starts in bare-metal mode

## Usage

### Bare-Metal Mode (Amiga SPI Bridge)

- **Default mode** on normal power-on
- Provides fast SPI bridge for Amiga SD card access
- Use as normal with your Amiga SD card driver
- LED indicates activity
- No network activity in this mode

### FreeRTOS Mode (WiFi FTP Server)

1. **Switch to FreeRTOS mode**:
   - Hold mode switch button (GPIO13) for 3 seconds
   - LED will change blinking pattern
   - System performs clean reboot into FTP mode

2. **Connect to FTP server**:
   - Find Pico's IP address in serial console (see Debugging section)
   - Use any FTP client (FileZilla recommended)
   - Default credentials: `pico` / `pico` (unless overridden in wifi_credentials.cmake)

3. **Return to Bare-Metal mode**:
   - Hold mode switch button for 3 seconds
   - System reboots back to Amiga SPI bridge mode

### FileZilla Configuration

For optimal performance and proper timestamp preservation, configure FileZilla:

1. **Transfer Mode**:
   - Menu: `Transfer` → `Transfer Type` → Select **Binary**
   - Critical for file integrity!

2. **Timestamp Preservation**:
   - Menu: `Transfer` → `Preserve timestamps of transferred files`
   - Check this option to maintain original file dates
   - Uses MFMT command (RFC 3659) automatically

3. **Connection**:
   - Host: `ftp://192.168.x.x` (your Pico's IP)
   - Username: `pico` (or your custom username)
   - Password: `pico` (or your custom password)
   - Port: `21`

## Debugging

### Serial Console

View real-time logs via USB serial connection:
```bash
# macOS/Linux
screen /dev/cu.usbmodem141201 115200

# Or use screen with your specific device
screen /dev/ttyACM0 115200
```

**Exit screen**: Press `Ctrl+A` then `K`, confirm with `Y`

### Enable Detailed FTP Debugging

For troubleshooting FTP server issues, enable debug logging:

1. Open `ftp_server.c`
2. Find the line (near the top):
```c
   #define FTP_DEBUG 0
```
3. Change to:
```c
   #define FTP_DEBUG 1
```
4. Rebuild and reflash

**Debug output includes:**
- All FTP commands received
- File operations (open/close/read/write)
- Data connection status
- Transfer progress
- Error conditions

**Note**: Debug output is verbose! Only enable when troubleshooting.

### LED Indicators

**Bare-Metal Mode**:
- Activity mirroring from Amiga operations

**FreeRTOS Mode**:
- Solid ON: Initializing
- Medium blink (500ms): Connecting to WiFi
- Slow blink (1000ms): Connected, FTP server ready
- Fast blink (200ms): Error (WiFi init or connection failed)

## FTP Server Performance

**Optimization**:
- Small files (≤256KB): Buffered in RAM for single SD write
- Large files (>256KB): 64KB streaming buffer for memory efficiency
- Supports up to 8 simultaneous client connections

## Default Credentials

**FTP Login** (can be overridden in wifi_credentials.cmake):
- Username: `pico`
- Password: `pico`

**Security Note**: Change default credentials for production use! Add custom credentials to `wifi_credentials.cmake`:
```cmake
set(FTP_USER "admin")
set(FTP_PASSWORD "YourSecurePassword123")
```

## Technical Details

- **FTP Protocol**: RFC 959 (core FTP) + RFC 3659 extensions (MLSD, MFMT)
- **Network Stack**: lwIP with threadsafe background WiFi (pico_cyw43_arch_lwip_threadsafe_background)
- **RTOS**: FreeRTOS SMP with dual-core affinity
  - Core 0: WiFi management task
  - Core 1: FTP server task
- **Filesystem**: FatFS with SD card via SPI
- **SPI Speed**: 
  - Slow: 400 KHz (initialization)
  - Fast: 15 MHz (normal operation)
- **TCP Configuration**:
  - Send buffer: 24 × TCP_MSS
  - Window: 32 × TCP_MSS
  - Segments: 256

## Known Limitations

- **FTP is unencrypted**: Credentials and data transmitted in plain text
  - Only use on trusted networks
  - Consider VPN for remote access
- **FAT Timestamp Limitations**: 
  - Year range: 1980-2107
  - Precision: 2 seconds (rounded down)
- **No FTPS/SFTP**: Secure FTP variants not implemented
- **No PORT mode**: Only PASV (passive mode) supported
  - Standard for modern FTP clients
  - No issues with NAT/firewalls

## Troubleshooting

### WiFi Connection Issues

**Symptoms**: Fast blinking LED, "Failed to connect" in serial log

**Solutions**:
1. Check WiFi credentials in `wifi_credentials.cmake`
2. Ensure 2.4GHz WiFi network (Pico 2 W doesn't support 5GHz)
3. Check SSID/password for typos
4. Verify network is not hidden or using unusual security
5. Check serial log for specific error messages

### FTP Connection Issues

**Symptoms**: Cannot connect, timeouts, authentication failures

**Solutions**:
1. Verify Pico's IP address from serial console
2. Check FTP credentials (default: pico/pico)
3. Ensure FileZilla is in **Binary** mode
4. Try disabling firewall temporarily
5. Check router allows FTP (port 21)
6. Enable FTP_DEBUG for detailed logs

### File Transfer Issues

**Symptoms**: Corrupted files, wrong sizes, failed transfers

**Solutions**:
1. **Set FileZilla to Binary mode** (most common issue!)
2. Verify SD card is FAT32 formatted
3. Check SD card health
4. Ensure sufficient free space on SD card
5. Try single file transfer first (rule out multi-client issues)

### Timestamp Issues

**Symptoms**: Files show wrong dates after upload

**Solutions**:
1. Enable "Preserve timestamps" in FileZilla Transfer menu
2. Verify MFMT in FEAT response (enable FTP_DEBUG)
3. Check source file has valid timestamp (1980-2107)
4. Remember 2-second precision limitation

### Mode Switch Issues

**Symptoms**: Button press doesn't switch modes

**Solutions**:
1. Hold button for full 3 seconds
2. Check GPIO13 connection
3. Watch serial console for "Button held" message
4. Verify pull-up resistor on GPIO13

## Project Structure
```
rp2350/
├── main.c                  # Boot manager, mode selection
├── par_spi.c               # Bare-metal Amiga SPI bridge
├── ftp_server.c            # FTP server implementation
├── ftp_types.h             # FTP data structures
├── ftp_server.h            # FTP server API
├── main.h                  # Common definitions
├── util.c/h                # Utility functions
├── CMakeLists.txt          # Build configuration
├── lib/
│   └── fatfs/              # FatFS filesystem (submodule)
├── include/
│   └── FreeRTOSConfig.h    # FreeRTOS configuration
└── build/                  # Build output directory
```

## Contributing

This is part of the [amiga-par-to-spi-adapter](https://github.com/jbilander/amiga-par-to-spi-adapter) project. Contributions welcome!

## License

See main project repository for license information.

## Credits

- **Bare-metal SPI bridge**: Based on Niklas Ekström's RP2040 implementation
- **Hardware design**: jbilander
- **FTP server**: Enhanced implementation with RFC 3659 extensions
- **FreeRTOS integration**: Dual-core SMP with WiFi management

## Version History

- **v18-complete-fix** (2025-12-09): Complete FTP server with all essential commands
  - All essential FTP operations implemented
  - MFMT timestamp preservation (RFC 3659)
  - Multi-client support (8 simultaneous)
  - Fixed empty directory timeout bug
  - Configurable credentials via wifi_credentials.cmake
  - Conditional Amiga IRQ signaling (mode switch only)

## Support

For issues, questions, or contributions, please use the [GitHub Issues](https://github.com/jbilander/amiga-par-to-spi-adapter/issues) page.

---

**Happy file transferring!** 📁✨

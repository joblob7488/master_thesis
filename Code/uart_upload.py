import sys
import time
import serial


BAUD_RATE       = 19200
BOOT_TIMEOUT    = 2.0   # seconds to wait for bootloader response after 'u'
UPLOAD_TIMEOUT  = 10.0  # seconds to wait for 'OK' after sending the binary
AWAIT_STRING    = b"Awaiting neorv32_exe.bin"
OK_STRING       = b"OK"


def read_until(port: serial.Serial, token: bytes, timeout: float) -> bytes:
    """Read from port until token is found or timeout expires. Returns all bytes read."""
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        chunk = port.read(port.in_waiting or 1)
        if chunk:
            buf += chunk
            if token in buf:
                return buf
    return buf


def main():
    if len(sys.argv) != 3:
        print("Upload and execute application image via UART to the NEORV32 bootloader.")
        print("Reset the processor before starting the upload.\n")
        print("Usage:   python neorv32_uart_upload.py <serial port> <neorv32_exe.bin>")
        print("Example: python neorv32_uart_upload.py COM3 path/to/neorv32_exe.bin")
        sys.exit(0)

    port_name = sys.argv[1]
    bin_path  = sys.argv[2]

    # --- open and configure serial port ---
    try:
        port = serial.Serial(
            port=port_name,
            baudrate=BAUD_RATE,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=1,
            xonxoff=False,
            rtscts=False,
            dsrdtr=False,
        )
    except serial.SerialException as e:
        print(f"Error opening port {port_name}: {e}")
        sys.exit(1)

    print(f"Opened {port_name} at {BAUD_RATE} baud.")

    # --- abort autoboot sequence ---
    port.write(b" ")
    port.flush()
    time.sleep(0.1)
    port.reset_input_buffer()

    # --- send upload command ---
    port.write(b"u")
    port.flush()

    # --- wait for bootloader to signal it is ready ---
    print("Waiting for bootloader...")
    response = read_until(port, AWAIT_STRING, BOOT_TIMEOUT)

    if AWAIT_STRING not in response:
        print("Bootloader response error!")
        print("Reset the processor before starting the upload.")
        print(f"Received: {response!r}")
        port.close()
        sys.exit(1)

    # --- send the executable ---
    try:
        with open(bin_path, "rb") as f:
            exe_data = f.read()
    except OSError as e:
        print(f"Error reading {bin_path}: {e}")
        port.close()
        sys.exit(1)

    print(f"Uploading {len(exe_data)} bytes...", end="", flush=True)
    port.write(exe_data)
    port.flush()

    # --- wait for OK ---
    response = read_until(port, OK_STRING, UPLOAD_TIMEOUT)

    if OK_STRING not in response:
        print(" FAILED!")
        print(f"Received: {response!r}")
        port.close()
        sys.exit(1)

    print(" OK")

    # --- start execution ---
    print("Starting application...")
    port.write(b"e")
    port.flush()

    port.close()
    sys.exit(0)


if __name__ == "__main__":
    main()
import argparse
import serial
import struct
import sys
import time

CHUNK_SIZE  = 4096   # must match C #define
BAUD_RATE   = 921600
ACK         = 0x79
NAK         = 0x6E
TIMEOUT_S   = 30     # seconds to wait for ACK after erase (sectors take time)


def wait_ack(ser: serial.Serial, timeout: float = TIMEOUT_S) -> bool:
    
    ser.timeout = timeout
    while True:
        resp = ser.read(1)
        if not resp:
            print("ERROR: Timeout waiting for ACK.")
            return False
        b = resp[0]
        if b == ACK:
            return True
        if b == NAK:
            print("ERROR: NAK received from device.")
            return False
        try:
            print(chr(b), end="", flush=True)
        except:
            pass


def send_model(port: str, baud: int, model_path: str):

    with open(model_path, "rb") as f:
        model_data = f.read()

    model_size  = len(model_data)
    num_chunks  = (model_size + CHUNK_SIZE - 1) // CHUNK_SIZE

    print(f"Model      : {model_path}")
    print(f"Size       : {model_size} bytes ({model_size/1024/1024:.2f} MB)")
    print(f"Chunks     : {num_chunks} x {CHUNK_SIZE} bytes")
    print(f"Port       : {port} @ {baud} baud")
    print()

    with serial.Serial(port, baud, timeout=5) as ser:
        time.sleep(0.3)
        ser.reset_input_buffer()

        print("Sending model size...")
        ser.write(struct.pack("<I", model_size))

        num_sectors = (model_size + 64*1024 - 1) // (64*1024)
        erase_timeout = max(30, num_sectors * 3)  # 3s per sector, min 30s
        print(f"Waiting for erase ({num_sectors} sectors, up to {erase_timeout}s)...")

        if not wait_ack(ser, timeout=erase_timeout):
            sys.exit(1)
        print("Erase done. Starting transfer...\n")
        time.sleep(0.1)

        start_time = time.time()

        for chunk_idx in range(num_chunks):
            offset = chunk_idx * CHUNK_SIZE
            chunk  = model_data[offset : offset + CHUNK_SIZE]

            # Pad last chunk to page boundary (256 bytes)
            if len(chunk) % 256 != 0:
                chunk = chunk + b"\xff" * (256 - len(chunk) % 256)

            ser.write(chunk)

            # Wait for ACK — device ACKs after programming the chunk to flash
            if not wait_ack(ser, timeout=5.0):
                print(f"Failed on chunk {chunk_idx + 1}/{num_chunks}")
                sys.exit(1)
            time.sleep(0.05)
            
            # Progress
            done     = chunk_idx + 1
            pct      = done / num_chunks * 100
            elapsed  = time.time() - start_time
            rate     = (done * CHUNK_SIZE) / elapsed / 1024 if elapsed > 0 else 0
            eta      = (num_chunks - done) * CHUNK_SIZE / (rate * 1024) if rate > 0 else 0
            bar      = "#" * (done * 40 // num_chunks)
            print(f"\r  [{bar:<40}] {pct:5.1f}%  {rate:6.1f} KB/s  ETA {eta:5.0f}s",
                  end="", flush=True)

        elapsed = time.time() - start_time
        print(f"\n\nTransfer complete in {elapsed:.1f}s "
              f"({model_size/elapsed/1024:.1f} KB/s average).")
        print(f"Model written to flash at offset 0x00100000.")


def main():
    parser = argparse.ArgumentParser(description="Send TM model to NEORV32 flash writer")
    parser.add_argument("model",   help="Path to raw model binary file")
    parser.add_argument("--port",  default="COM3", help="Serial port")
    parser.add_argument("--baud",  type=int, default=BAUD_RATE, help="Baud rate")
    args = parser.parse_args()
    send_model(args.port, args.baud, args.model)


if __name__ == "__main__":
    main()
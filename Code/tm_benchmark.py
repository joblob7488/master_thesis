import argparse
import csv
import os
import re
import serial
import struct
import time

BAUD_RATE        = 921600
FEATURES         = 784
POS_CHUNKS       = 25
BYTES_PER_IMAGE  = POS_CHUNKS * 4   # 100 bytes
IMAGES_PER_DIGIT = 20
CPU_FREQ_HZ      = 100_000_000      # 100 MHz, used to convert cycles to ms


def pack_image(bits: list) -> bytes:
    assert len(bits) == FEATURES
    words = []
    for i in range(POS_CHUNKS):
        word = 0
        for b in range(32):
            bit_idx = i * 32 + b
            if bit_idx < FEATURES and bits[bit_idx] == 1:
                word |= (1 << b)
        words.append(word)
    return struct.pack(f"<{POS_CHUNKS}I", *words)


def load_example(data_dir: str, digit: int, index: int):
    folder = os.path.join(data_dir, str(digit))
    if not os.path.isdir(folder):
        raise FileNotFoundError(f"Folder '{folder}' not found. Run sort_mnist.py first.")

    files = sorted([f for f in os.listdir(folder) if f.endswith(".txt")])
    if index >= len(files):
        raise IndexError(f"Only {len(files)} examples for digit {digit}, requested index {index}")

    path = os.path.join(folder, files[index])
    with open(path, "r") as f:
        tokens = f.read().strip().split()

    if len(tokens) != FEATURES:
        raise ValueError(f"Expected {FEATURES} values in {path}, got {len(tokens)}")

    return [int(t) for t in tokens], files[index]


def wait_ready(ser: serial.Serial):
    print("Waiting for device ready...")
    buf = b""
    while b"Ready." not in buf:
        chunk = ser.read(64)
        if chunk:
            buf += chunk
            print(chunk.decode("ascii", errors="replace"), end="", flush=True)
    print("\nDevice ready.\n")


def send_image_and_get_result(ser: serial.Serial, packed: bytes):
    ser.write(packed)

    # Read raw prediction byte (sent as pred+1)
    pred_byte = None
    while pred_byte is None:
        b = ser.read(1)
        if not b:
            raise TimeoutError("Timeout waiting for prediction byte")
        val = b[0]
        if 1 <= val <= 10:
            pred_byte = val - 1

    # Read RESULT line — skip empty lines and \r
    while True:
        line = b""
        while True:
            c = ser.read(1)
            if not c:
                raise TimeoutError("Timeout waiting for RESULT line")
            if c == b'\n':
                break
            line += c

        line_str = line.decode("ascii", errors="replace").strip()
        if not line_str:
            continue  # skip empty lines

        m = re.search(r"RESULT\s+(\d+)\s+(\d+)", line_str)
        if not m:
            raise ValueError(f"Unexpected line from device: '{line_str}'")

        return pred_byte, int(m.group(1))


def run_benchmark(port: str, baud: int, data_dir: str, mode: str, log_path: str, summary_path: str):

    total_images = IMAGES_PER_DIGIT * 10
    print(f"=== TM Inference Benchmark ===")
    print(f"Mode     : {mode}")
    print(f"Port     : {port} @ {baud} baud")
    print(f"Data     : {data_dir}")
    print(f"Images   : {IMAGES_PER_DIGIT} per digit x 10 digits = {total_images} total")
    print(f"CSV log  : {log_path}")
    print(f"Summary  : {summary_path}")
    print()

    # Pre-load all test examples
    print("Loading test examples...")
    test_set = []
    for digit in range(10):
        for idx in range(IMAGES_PER_DIGIT):
            bits, filename = load_example(data_dir, digit, idx)
            packed = pack_image(bits)
            test_set.append({
                "digit":    digit,
                "filename": filename,
                "packed":   packed,
            })
    print(f"Loaded {len(test_set)} examples.\n")

    results = []

    with serial.Serial(port, baud, timeout=30) as ser:
        wait_ready(ser)

        print(f"Running {total_images} inferences...\n")
        start_time = time.time()

        for i, item in enumerate(test_set):
            digit    = item["digit"]
            filename = item["filename"]
            packed   = item["packed"]

            pred, cycles = send_image_and_get_result(ser, packed)
            correct = (pred == digit)

            ms = cycles / CPU_FREQ_HZ * 1000

            results.append({
                "digit":    digit,
                "filename": filename,
                "pred":     pred,
                "correct":  1 if correct else 0,
                "cycles":   cycles,
                "ms":       f"{ms:.3f}",
            })

            marker = "correct" if correct else "incorrect"
            print(f"  [{i+1:3d}/{total_images}] digit={digit} pred={pred} {marker}  cycles={cycles}  ({ms:.3f} ms)")

        elapsed = time.time() - start_time

    # Save CSV log
    with open(log_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["digit", "filename", "pred", "correct", "cycles", "ms"])
        writer.writeheader()
        writer.writerows(results)

    # Build summary lines
    summary_lines = []
    summary_lines.append(f"{'='*60}")
    summary_lines.append(f"SUMMARY ({mode})")
    summary_lines.append(f"{'='*60}")
    summary_lines.append(f"{'Digit':<8} {'Correct':<10} {'Accuracy':<12} {'Avg Cycles':<16} {'Avg ms':<10}")
    summary_lines.append(f"{'-'*60}")

    total_correct = 0
    total_cycles  = 0

    for digit in range(10):
        digit_results = [r for r in results if r["digit"] == digit]
        correct  = sum(r["correct"] for r in digit_results)
        cycles   = [r["cycles"] for r in digit_results]
        avg_cyc  = sum(cycles) / len(cycles)
        avg_ms   = avg_cyc / CPU_FREQ_HZ * 1000
        total_correct += correct
        total_cycles  += sum(cycles)
        summary_lines.append(
            f"  {digit:<6} {correct}/{IMAGES_PER_DIGIT:<7}  "
            f"{correct/IMAGES_PER_DIGIT*100:6.1f}%     "
            f"{avg_cyc:>12.0f}     {avg_ms:>8.3f}"
        )

    summary_lines.append(f"{'-'*60}")
    overall_acc = total_correct / total_images * 100
    avg_cycles  = total_cycles / total_images
    avg_ms_all  = avg_cycles / CPU_FREQ_HZ * 1000
    summary_lines.append(
        f"  {'ALL':<6} {total_correct}/{total_images:<5}  "
        f"{overall_acc:6.1f}%     "
        f"{avg_cycles:>12.0f}     {avg_ms_all:>8.3f}"
    )
    summary_lines.append(f"")
    summary_lines.append(f"Total time : {elapsed:.1f}s")
    summary_lines.append(f"Throughput : {total_images/elapsed:.1f} inferences/s")
    summary_lines.append(f"CSV log    : {log_path}")
    summary_lines.append(f"Summary    : {summary_path}")

    # Print summary
    print()
    for line in summary_lines:
        print(line)

    # Save summary to txt
    with open(summary_path, "w") as f:
        f.write("\n".join(summary_lines) + "\n")


def main():
    parser = argparse.ArgumentParser(description="NEORV32 TM inference benchmark")
    parser.add_argument("--port",  default="COM3",         help="Serial port")
    parser.add_argument("--baud",  type=int, default=BAUD_RATE, help="Baud rate")
    parser.add_argument("--data",  default="mnist_sorted", help="Sorted MNIST folder")
    parser.add_argument("--mode",  default="noncfu",       help="noncfu or cfu")
    args = parser.parse_args()

    log_path     = f"benchmark_{args.mode}.csv"
    summary_path = f"benchmark_{args.mode}_summary.txt"

    run_benchmark(args.port, args.baud, args.data, args.mode, log_path, summary_path)


if __name__ == "__main__":
    main()
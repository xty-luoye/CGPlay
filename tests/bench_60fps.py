"""Benchmark 4K 60fps and 8K 60fps playback performance."""
import subprocess, os, time, json
from pathlib import Path

MEDIA = Path(__file__).parent / "media"
CGPLAY = r"C:\Users\1\Desktop\RVLite\build_win_full\bin\Release\CGPlay.exe"


def get_mem_gb():
    try:
        import psutil
        for p in psutil.process_iter(["name", "memory_info"]):
            if p.info["name"] == "CGPlay.exe":
                return p.info["memory_info"].rss / (1024**3)
    except:
        pass
    return 0


def test(label, path, desc):
    print(f"\n{'='*60}")
    print(f"[{label}] {desc}")
    mb = path.stat().st_size // (1024**2)
    print(f"Size: {mb} MB | {path.stat().st_size // (1024**3)} GB")
    print(f"{'='*60}")

    os.system("taskkill /f /im CGPlay.exe 2>nul")
    time.sleep(1)

    t0 = time.time()
    proc = subprocess.Popen([CGPLAY, str(path)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(5)
    startup = round(time.time() - t0, 2)
    m0 = get_mem_gb()
    ok = proc.poll() is None
    print(f"  Startup: {startup}s | Mem: {m0:.1f} GB | {'ALIVE' if ok else 'DEAD'}")

    if not ok:
        return {"label": label, "status": "CRASHED", "startup_s": startup}

    time.sleep(12)
    m1 = get_mem_gb()
    ok = proc.poll() is None
    print(f"  Play 12s: Mem={m1:.1f} GB (d+{m1-m0:.2f}) | {'ALIVE' if ok else 'DEAD'}")

    time.sleep(5)
    m2 = get_mem_gb()
    ok = proc.poll() is None
    print(f"  End: Mem={m2:.1f} GB (d+{m2-m0:.2f}) | {'ALIVE' if ok else 'DEAD'}")

    proc.terminate()
    try:
        proc.wait(timeout=3)
    except:
        proc.kill()

    return {
        "label": label, "desc": desc,
        "status": "OK" if ok else "CRASHED_DURING",
        "startup_s": startup,
        "mem_start_gb": round(m0, 2),
        "mem_play_gb": round(m1, 2),
        "mem_end_gb": round(m2, 2),
        "mem_growth_gb": round(m2 - m0, 2),
    }


results = [
    test("4K 60fps", MEDIA / "4k_60fps.mp4", "3840x2160 H.265 10bit 60fps"),
    test("8K 60fps", MEDIA / "8k_60fps.mp4", "7680x4320 H.265 60fps"),
]

print(f"\n{'='*70}")
print("BENCHMARK: 4K/8K 60fps")
print(f"{'='*70}")
print(f"{'Test':<12} {'Startup':>8} {'Mem Start':>10} {'Mem End':>10} {'Growth':>8} {'Status':>8}")
print("-" * 60)
for r in results:
    s = "OK" if r["status"] == "OK" else "FAIL"
    print(f"{r['label']:<12} {r['startup_s']:>7}s {r['mem_start_gb']:>8.1f} GB {r['mem_end_gb']:>8.1f} GB {r['mem_growth_gb']:>+6.1f} GB {s:>8}")

path = MEDIA / "bench_60fps.json"
with open(path, "w") as f:
    json.dump(results, f, indent=2, default=str)
print(f"\nSaved: {path}")
print(f"{'='*70}")

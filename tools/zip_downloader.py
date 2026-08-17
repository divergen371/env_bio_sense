#!/usr/bin/env python3
import argparse
import urllib.request
import urllib.error
import json
import os
import sys
import concurrent.futures
import threading

# スレッドセーフなprint用ロック
print_lock = threading.Lock()

def safe_print(*args, **kwargs):
    with print_lock:
        print(*args, **kwargs)

def download_file(base_url, dest_dir, fname):
    name = fname.lstrip("/")
    dest_path = os.path.join(dest_dir, name)
    dl_url = f"{base_url}/download?file={name}"
    
    safe_print(f"[{name}] Starting download...")
    
    try:
        req = urllib.request.Request(dl_url)
        with urllib.request.urlopen(req, timeout=15) as response, open(dest_path, 'wb') as out_file:
            total_length = response.getheader('Content-Length')
            
            dl = 0
            while True:
                chunk = response.read(8192 * 4) # 少し大きめのチャンク
                if not chunk:
                    break
                dl += len(chunk)
                out_file.write(chunk)
                
            if total_length:
                safe_print(f"[{name}] Download complete: {dl / 1024 / 1024:.2f} MB")
            else:
                safe_print(f"[{name}] Download complete: {dl / 1024 / 1024:.2f} MB (unknown total)")
                
        return True, name
    except Exception as e:
        safe_print(f"[{name}] Error downloading: {e}")
        return False, name

def main():
    parser = argparse.ArgumentParser(description="Download ZIP archives from EnvBioSense ESP32 in parallel")
    parser.add_argument("--host", default="192.168.4.1", help="ESP32 IP address or hostname")
    parser.add_argument("--dest", default=".", help="Destination directory")
    parser.add_argument("--workers", type=int, default=3, help="Number of parallel downloads")
    args = parser.parse_args()

    base_url = f"http://{args.host}"
    
    safe_print(f"Connecting to {base_url}/api/files...")
    try:
        req = urllib.request.Request(f"{base_url}/api/files")
        with urllib.request.urlopen(req, timeout=5) as response:
            data_bytes = response.read()
            data = json.loads(data_bytes.decode('utf-8'))
    except Exception as e:
        safe_print(f"Error connecting to ESP32: {e}")
        sys.exit(1)
        
    files = data.get("files", [])
    zip_files = [f["name"] for f in files if f["name"].endswith(".zip")]
    
    if not zip_files:
        safe_print("No ZIP files found on the device.")
        sys.exit(0)
        
    safe_print(f"Found {len(zip_files)} ZIP file(s):")
    for i, fname in enumerate(zip_files):
        safe_print(f"  - {fname}")
        
    if not os.path.exists(args.dest):
        os.makedirs(args.dest)
        
    safe_print(f"\nStarting parallel download of {len(zip_files)} file(s) with {args.workers} workers...")
    
    success_count = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as executor:
        # submit tasks
        futures = {executor.submit(download_file, base_url, args.dest, fname): fname for fname in zip_files}
        
        for future in concurrent.futures.as_completed(futures):
            success, name = future.result()
            if success:
                success_count += 1
                
    safe_print(f"\nAll downloads finished! Successfully downloaded {success_count} out of {len(zip_files)} files.")

if __name__ == "__main__":
    main()

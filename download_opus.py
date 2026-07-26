import requests, zipfile, io, os, shutil
r = requests.get("https://components.espressif.com/api/components/esphome/micro-opus", timeout=20)
data = r.json()
latest = data["versions"][0]
url = latest["url"]
print(f"version {latest['version']}, url: {url}")
print(f"deps: {[(d.get('namespace'),d.get('name'),d.get('spec'),d.get('source')) for d in latest.get('dependencies',[])]}")

dest = "D:/UGit/ai-mirror/Firmware/esp32_ai_mirror/managed_components/esphome__micro-opus"
for attempt in range(8):
    try:
        resp = requests.get(url, timeout=60)
        print(f"download attempt {attempt}: {resp.status_code} len={len(resp.content)}")
        if resp.status_code == 200 and len(resp.content) > 1000:
            z = zipfile.ZipFile(io.BytesIO(resp.content))
            if os.path.exists(dest):
                shutil.rmtree(dest)
            os.makedirs(dest, exist_ok=True)
            z.extractall(dest)
            print(f"extracted to {dest}")
            print("files:", z.namelist()[:30])
            break
    except Exception as e:
        print(f"attempt {attempt}: err {e}")

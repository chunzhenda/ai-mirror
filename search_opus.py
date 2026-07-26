import requests
url = "https://components-file.espressif.com/components/esphome/micro-opus/0.4.1/readme.md"
for attempt in range(4):
    try:
        r = requests.get(url, timeout=20)
        print(f"attempt {attempt}: {r.status_code}")
        if r.status_code == 200:
            print(r.text[:3500])
            break
    except Exception as e:
        print(f"attempt {attempt}: err {e}")

# list files in the component via registry
print("\n=== micro-opus files ===")
r2 = requests.get("https://components.espressif.com/api/components/esphome/micro-opus/0.4.1", timeout=20)
print(r2.text[:1500])

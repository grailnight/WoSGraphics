"""Toggle sky lighting through the installed renderer's bounded control API."""
from pathlib import Path
import argparse,json,time,os
p=argparse.ArgumentParser();p.add_argument('enabled',type=int,choices=[0,1]);a=p.parse_args()
root=Path(__file__).resolve().parents[1];active=root/'full_deferred'
request=time.time_ns()//1000000
tmp=active/'control.sky.tmp';tmp.write_text(f'[Control]\nRequestId={request}\nSkyIBL={a.enabled}\n',encoding='ascii');os.replace(tmp,active/'control.ini')
for _ in range(150):
    try:
        status=json.loads((active/'control_status.json').read_text())
        if status.get('request_id')==request:
            assert status['applied'] and not status['error'],status
            print(json.dumps(status));break
    except (OSError,ValueError):pass
    time.sleep(.1)
else:raise SystemExit('No response from renderer')

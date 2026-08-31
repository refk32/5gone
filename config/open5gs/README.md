# Open5GS native install

PLMN **001-01** must match `config/srsran/gnb_20mhz.yml`.

Install:
```bash
bash scripts/install-open5gs.sh
```

Config paths:
- **apt install:** `/etc/open5gs/*.yaml`
- **source install:** `$ROOT/build/open5gs/etc/open5gs/`

Start:
```bash
bash scripts/start-5g-native.sh core
bash scripts/add-test-subscriber.sh
```

No Docker — MongoDB runs natively on host.


# Prepare

```bash
sudo ./dsm-scripts/config.sh
./dsm-scripts/config_memdev.sh cxl
numactl --cpunodebind=0 ./build/simulate.sh
```
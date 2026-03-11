# Hyperparameter Report

## Current Summary
- Best Success: 12.50%
- Best Full SLA: 12.50%
- Last Success: 12.50%
- Last SLA: 12.50%
- Last Full SLA: 12.50%
- Last Avg Algorithm Latency: 75.43 ms
- Last P95 Algorithm Latency: 0.00 ms
- Success Trend (last ~12 epochs): 0.000 / epoch
- Full SLA Trend (last ~12 epochs): 0.000 / epoch
- Last Top Failures: []

## Recommended Next Config
- `epochs`: 1 -> **11**
- `rel_curr_end_epoch`: None -> **70**
- `rel_curr_strict_ratio`: None -> **0.76**
- `rel_curr_strict_ramp_ratio`: None -> **0.25**
- `rel_curr_min_scale`: None -> **0.72**
- `max_data_files`: None -> **14**

## Next Run Command
```bash
python -m ground_training.train --epochs 11 --max_data_files 14 --rel_curr_end_epoch 70 --rel_curr_min_scale 0.72 --rel_curr_strict_ratio 0.76 --rel_curr_strict_ramp_ratio 0.25
```

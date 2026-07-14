# Paper plot scripts (vendored from p3os-paper)

These files are copied from `/mnt/disk1/yjs/p3os-paper/eval/` so artifact
evaluation figures match the paper. Prefer editing the paper repo and
re-copying here if the published style changes.

| File | Paper source |
|------|----------------|
| `config.py` | `eval/config.py` |
| `memory_fig_style.py` | `eval/memory_fig_style.py` |
| `memory_custom_colors.json` | `eval/memory_custom_colors.json` |
| `state_partition.py` | `eval/state_partition.py` |
| `recovery_performance_single.py` | `eval/recovery_performance_single.py` |
| `ipc_cdf/plot_cdf_all.py` | `eval/ipc_cdf/plot_cdf_all.py` |
| `ipc_cdf/plot_breakdown_all.py` | `eval/ipc_cdf/plot_breakdown_all.py` |
| `malloc/plot_combined_allocator_figure.py` | `eval/malloc/plot_combined_allocator_figure.py` |

AE experiment `plot.py` / `parse_and_plot.py` scripts only adapt log/CSV paths
and call into this package; drawing logic stays here.

import matplotlib.pyplot as plt
import os
from pathlib import Path

SYSTEM_NAME = 'Starfish'
draw_jpg = False
draw_pdf = True

def draw_fig(name: str, out_dir=None):
  """Save ``name.eps`` (and optional pdf/jpg) under ``out_dir`` (default cwd).

  Matches p3os-paper/eval/config.py; ``out_dir`` is the only AE addition so
  artifact-evaluation can place figures next to experiment outputs.
  """
  base = Path(out_dir) if out_dir is not None else Path('.')
  base.mkdir(parents=True, exist_ok=True)
  tmp = base / 'tmpfig'
  if not tmp.exists():
    tmp.mkdir(parents=True, exist_ok=True)
  if draw_jpg:
    plt.savefig(str(tmp / f'{name}.jpg'), dpi=1200, format='jpg', bbox_inches='tight')
  if draw_pdf:
    plt.savefig(str(tmp / f'{name}.pdf'), dpi=1200, format='pdf', bbox_inches='tight')
  plt.savefig(str(base / f'{name}.eps'), dpi=1200, format='eps', bbox_inches='tight')
  # Also keep a flat PDF beside the EPS for AE gather (paper used tmpfig/).
  if draw_pdf:
    plt.savefig(str(base / f'{name}.pdf'), dpi=1200, format='pdf', bbox_inches='tight')

#!/usr/bin/env python3
"""107 探針的相容入口；改跑涵蓋其 fork 情境的完整 TMR 矩陣。"""
from pathlib import Path
import runpy

if __name__ == '__main__':
    runpy.run_path(str(Path(__file__).with_name('probe_tmr_matrix.py')), run_name='__main__')

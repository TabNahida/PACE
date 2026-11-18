#!/usr/bin/env python3
"""Render before/after gateway evidence. The homepage selects epoll, not best-of-backends."""
import argparse
import json
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('input', type=Path)
p.add_argument('output', type=Path)
p.add_argument('--all-backends', action='store_true')
args = p.parse_args()
data = json.loads(args.input.read_text())
backends = ['baseline', 'epoll', 'io_uring', 'nginx'] if args.all_backends else ['baseline', 'epoll', 'nginx']
labels = {'baseline': 'PACE before · epoll', 'epoll': 'PACE now · epoll', 'io_uring': 'PACE now · io_uring', 'nginx': 'nginx 1.30.6'}
colors = {'baseline': '#929ab9', 'epoll': '#70dfc3', 'io_uring': '#b1a6ff', 'nginx': '#6a9cd1'}
scenarios = [('static', 'Static files'), ('static_cached', 'Static · cached'), ('proxy', 'Reverse proxy'), ('proxy_cached', 'Proxy · cached')]
plt.rcParams.update({'font.family': 'DejaVu Sans', 'svg.fonttype': 'none', 'font.size': 10,
                     'text.color': '#e7ecf5', 'axes.labelcolor': '#c4cddd', 'xtick.color': '#aebbd2', 'ytick.color': '#e7ecf5'})
fig, axes = plt.subplots(1, 2, figsize=(13, 7.2), facecolor='#0d1420', sharex=True)
maximum = max(s['requests_per_second']['max'] for s in data['summaries']) / 1000
width = .18 if args.all_backends else .22
for ax, workers in zip(axes, (1, 4)):
    ax.set_facecolor('#0d1420')
    for index, backend in enumerate(backends):
        stats = [next(s['requests_per_second'] for s in data['summaries'] if (s['scenario'], s['workers'], s['backend']) == (scenario, workers, backend)) for scenario, _ in scenarios]
        positions = [group + (index-(len(backends)-1)/2)*width for group in range(4)]
        values = [s['median']/1000 for s in stats]
        errors = [[(s['median']-s['min'])/1000 for s in stats], [(s['max']-s['median'])/1000 for s in stats]]
        ax.barh(positions, values, width*.84, xerr=errors, color=colors[backend], label=labels[backend],
                error_kw={'ecolor': '#e7ecf5', 'capsize': 2, 'elinewidth': .8})
        for y, value, stat in zip(positions, values, stats):
            ax.text(stat['max']/1000+maximum*.018, y, f'{value:.1f}', va='center', color=colors[backend], fontsize=9)
    ax.set_title(f'{workers} frontend worker' + ('s' if workers > 1 else ''), loc='left', fontsize=15, fontweight='bold', pad=18)
    ax.set_yticks(range(4), [label for _, label in scenarios]); ax.invert_yaxis()
    ax.set_xlabel('Thousand requests/s · higher is better', labelpad=12)
    ax.set_xlim(0, maximum*1.20); ax.set_axisbelow(True); ax.grid(axis='x', color='#243148', linewidth=.7)
    ax.spines[['top', 'right', 'bottom', 'left']].set_visible(False)
    ax.tick_params(axis='both', length=0, pad=7)
fig.suptitle('HTTP gateways · measured on Linux', x=.04, y=.97, ha='left', fontsize=22, fontweight='bold')
fig.text(.96, .96, '2026-11-03', ha='right', fontsize=10, color='#aebbd2')
fig.legend(*axes[0].get_legend_handles_labels(), loc='upper center', bbox_to_anchor=(.5, .91), ncol=len(backends), frameon=False, fontsize=10)
fig.text(.04, .07, '32 connections · 4 KiB bodies · 5 rotating trials · local Linux / WSL2, shared host', fontsize=10, color='#aebbd2')
fig.text(.04, .035, 'Bars: medians · whiskers: trial min–max, not confidence intervals · full latency and raw data linked below', fontsize=9, color='#aebbd2')
fig.subplots_adjust(left=.14, right=.98, top=.79, bottom=.19, wspace=.40)
args.output.parent.mkdir(parents=True, exist_ok=True)
fig.savefig(args.output, facecolor=fig.get_facecolor())

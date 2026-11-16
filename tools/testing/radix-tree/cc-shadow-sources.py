#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
"""Extract actual workingset, truncate and list-LRU functions for the fixture."""
import re
import sys
from pathlib import Path


def extract(source, name):
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    lambda m: re.sub(r'[^\n]', ' ', m[0]), source, flags=re.S)
    pattern = rf'\b{re.escape(name)}\s*\([^;{{}}()]*\)\s*(?:__must_hold\([^)]*\)\s*)?\{{'
    matches = list(re.finditer(pattern, masked))
    if not matches:
        raise ValueError(f'missing function: {name}')
    # Duplicate definitions are the CONFIG_MEMCG / !CONFIG_MEMCG alternatives.
    match = matches[-1]
    start = source.rfind('\n\n', 0, match.start()) + 2
    end = match.end()
    depth = 1
    while depth:
        depth += (masked[end] == '{') - (masked[end] == '}')
        end += 1
    return source[start:end]


mode, filename = sys.argv[1:]
source = Path(filename).read_text()
if mode == 'lru':
    names = ('lock_list_lru', 'unlock_list_lru', 'lru_shrinker_id',
             'list_lru_memcg_aware', 'lock_list_lru_of_memcg',
             'list_lru_lock', 'list_lru_unlock', '__list_lru_add',
             '__list_lru_del', 'list_lru_add', 'list_lru_add_obj',
             'list_lru_del', 'list_lru_del_obj', 'list_lru_isolate',
             '__list_lru_walk_one')
elif mode == 'truncate':
    names = []
    if re.search(r'\bcc_clear_shadow_entries\s*\(', source):
        names.append('cc_clear_shadow_entries')
    names.extend(('clear_shadow_entries', 'truncate_folio_batch_exceptionals'))
elif mode == 'swap':
    names = ('folio_batch_remove_exceptionals',)
elif mode == 'mapping':
    names = ('mapping_shrinkable',)
else:
    names = ['workingset_update_node', 'cc_workingset_update_node']
    if re.search(r'\bcc_workingset_clear_node\s*\(', source):
        names.append('cc_workingset_clear_node')
    names.append('shadow_lru_isolate')
print('\n\n'.join(extract(source, name) for name in names))

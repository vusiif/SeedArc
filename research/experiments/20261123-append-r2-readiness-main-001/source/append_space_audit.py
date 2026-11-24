# SPDX-License-Identifier: GPL-3.0-or-later
"""Audit v4's separable plan objective; no compression and no new hypotheses tested.

The constants describe append_common.hpp r1 / append4_study.cpp::PlanEvaluator.
Counts are upper bounds for independent group work, not elapsed time predictions.
"""
import argparse
import hashlib
import json
from pathlib import Path


def audit(raw):
    records = [json.loads(line) for line in raw.read_text(encoding='utf-8').splitlines()]
    searches = [r for r in records if r.get('label') == 'search' and r.get('returncode') == 0]
    # No dictionary => T has no meaning; three nonzero sizes each have two T values.
    options = 1 + 3 * 2
    domains = [(1, 1)] + [(k, 4) for k in range(2, 9)]
    return dict(
        candidate_revision='append v4 / seed_revision=1',
        objective='dictionaries+data+key_table+seed_record',
        canonical_plan_count=options + sum(rules * options**k for k, rules in domains[1:]),
        grouping_domain_count=sum(rules for k, rules in domains),
        independent_group_work_upper_bound=sum(k * rules * options for k, rules in domains),
        full_input_encoding_pass_upper_bound=sum(rules * options for k, rules in domains),
        reasoning='For fixed rule and k, membership is independent of D/T. Each nonempty group can choose its smallest dictionary+data result independently. Key-table and seed-record overhead are fixed within that domain. Canonical k=1 uses rule 0. Empty/duplicate groups and cache sharing can reduce actual work.',
        limitations=[
            'This is an exact-optimum construction for the present separable objective, not an implemented exhaustive solver.',
            'Up to 203 full-input encoding passes plus dictionary training can still be impractical on large input. A finite upper bound does not mean affordable.',
            'Observed cache hits are not global search-space coverage. No denominator for unique member-set keys was enumerated here.',
            'The cancelled pilot has missing repetitions and an interrupted child; observations are descriptive only.',
            'No Seed-value conclusion, performance comparison or large-scale acceptance follows from these counts.'
        ],
        raw_sha256=hashlib.sha256(raw.read_bytes()).hexdigest(),
        observed_searches=[dict(rep=r['rep'], pool=r['pool'], stage=r['stage'], variant=r['variant'],
                                 **{k:r['result'].get(k) for k in ['plan_evaluations', 'group_evaluations',
                                      'group_cache_hits', 'budget_bytes', 'budget_spent_bytes', 'stop']})
                           for r in searches])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--raw', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = audit(args.raw)
    if args.output.exists():
        raise FileExistsError('do not overwrite an existing audit')
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k:result[k] for k in ['canonical_plan_count', 'grouping_domain_count',
                       'independent_group_work_upper_bound', 'full_input_encoding_pass_upper_bound']}
                     | {'observed_searches':len(result['observed_searches'])}))


if __name__ == '__main__':
    main()

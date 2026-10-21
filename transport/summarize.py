import csv
import statistics
import sys
from collections import defaultdict

prefix = sys.argv[1] if len(sys.argv) > 1 else 'results'
groups = defaultdict(list)
with open(prefix + '_throughput.csv', newline='') as f:
    for r in csv.DictReader(f):
        groups[(r['body_bytes'], r['mode'])].append(r)
with open(prefix + '_throughput_summary.csv', 'w', newline='') as f:
    out = csv.writer(f)
    out.writerow(['body_bytes', 'mode', 'repeats', 'median_delivered_mmsg_s', 'min_delivered_mmsg_s', 'max_delivered_mmsg_s', 'median_missing_pct', 'total_missing', 'median_process_cpu_cores', 'median_sender_cpu_cores', 'median_receiver_cpu_cores'])
    for (size, mode), rows in groups.items():
        values = [float(r['received_mmsg_s']) for r in rows]
        out.writerow([size, mode, len(rows), statistics.median(values), min(values), max(values), statistics.median(100 * int(r['missing']) / int(r['sent']) for r in rows), sum(int(r['missing']) for r in rows), statistics.median(float(r['process_cpu_cores']) for r in rows), statistics.median(float(r['sender_cpu_s']) / float(r['delivery_seconds']) for r in rows), statistics.median(float(r['receiver_cpu_s']) / float(r['delivery_seconds']) for r in rows)])
groups.clear()
with open(prefix + '_rtt.csv', newline='') as f:
    for r in csv.DictReader(f):
        groups[(r['body_bytes'], r['mode'])].append(r)
with open(prefix + '_rtt_summary.csv', 'w', newline='') as f:
    out = csv.writer(f)
    out.writerow(['body_bytes', 'mode', 'repeats', 'median_repeat_p50_us', 'median_repeat_p95_us', 'median_repeat_p99_us', 'worst_repeat_p99_us', 'total_timeouts'])
    for (size, mode), rows in groups.items():
        out.writerow([size, mode, len(rows), statistics.median(float(r['p50_us']) for r in rows), statistics.median(float(r['p95_us']) for r in rows), statistics.median(float(r['p99_us']) for r in rows), max(float(r['p99_us']) for r in rows), sum(int(r['timeouts']) for r in rows)])
try:
    groups.clear()
    with open(prefix + '_paced.csv', newline='') as f:
        for r in csv.DictReader(f):
            groups[r['target_msg_s']].append(r)
    with open(prefix + '_paced_summary.csv', 'w', newline='') as f:
        out = csv.writer(f)
        out.writerow(['target_msg_s', 'repeats', 'median_received_msg_s', 'median_received_of_offered_pct', 'median_schedule_missed_pct', 'median_accepted_missing_pct', 'median_arrival_minus_planned_p50_us', 'median_arrival_minus_planned_p99_us', 'median_actual_send_to_arrival_p99_us', 'median_sender_lateness_p99_us'])
        for rate, rows in groups.items():
            out.writerow([rate, len(rows), statistics.median(float(r['received_msg_s']) for r in rows), statistics.median(100 * int(r['received']) / int(r['offered_slots']) for r in rows), statistics.median(100 * int(r['source_schedule_missed']) / int(r['offered_slots']) for r in rows), statistics.median(100 * int(r['accepted_minus_received']) / max(1, int(r['accepted'])) for r in rows), statistics.median(float(r['arrival_minus_planned_p50_us']) for r in rows), statistics.median(float(r['arrival_minus_planned_p99_us']) for r in rows), statistics.median(float(r['arrival_minus_actual_p99_us']) for r in rows), statistics.median(float(r['sender_lateness_p99_us']) for r in rows)])
except FileNotFoundError:
    pass
